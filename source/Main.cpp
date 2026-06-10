#include "plugin.h"
#include "CPad.h"
#include "CPlayerPed.h"
#include "CVehicle.h"
#include "cDMAudio.h"
#include "CMenuManager.h"
#include "CHud.h"
#include "CFont.h"
#include "CControllerConfigManager.h"
#include "CCutsceneMgr.h"
#include "CCamera.h"
#include "bass.h"
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <algorithm>
#include <cstdlib>
#include <ctime>
#include <cmath>
#include <thread>
#include <atomic>
#include <mutex>
#include <map>
#include <windows.h>

using namespace plugin;

// Declared in Switch.cpp
extern bool gSwitchNext;
extern bool gSwitchPrev;

// Declared in RadioVehicles.cpp
int GetStationForVehicle(CVehicle* pVehicle);
void OnPlayerExitVehicle(CVehicle* pVehicle);
bool IsNoRadioVehicle(int modelId);

// Game resolution addresses
static DWORD* const pResWidth = (DWORD*)0xA0FD04;
static DWORD* const pResHeight = (DWORD*)0xA0FD08;

struct RadioStation {
    std::string name;
    std::string file;
};

std::vector<RadioStation> stations;
static std::vector<std::string> gMp3Files;
static int gMp3StationIndex = -1;

std::string gGameFolder;
std::string gScriptsFolder;
std::ofstream gLog;
bool gAmbientRadioEnabled = false;

static HSTREAM gStream = 0;
bool gWasInVehicle = false;

// True while the player is inside a vehicle (updated every frame). Switch.cpp
// reads this to decide whether on-foot interior ambient audio is allowed through.
bool gPlayerInVehicle = false;

// The vehicle instance the player is currently in (or was just in). Updated every
// frame while inside a vehicle and used on exit to save the station to the correct
// instance — m_pVehicle is already null on the exit frame (instantly so when
// knocked off a bike), so without this the station would be saved to a null key
// and the vehicle would forget it after the 5s timer expired.
static CVehicle* gActiveVehicle = nullptr;
static bool gBassReady = false;
static bool gWasPaused = false;
static unsigned char gLastVolume = 0;

// Audio ducking: dim the radio under mission dialogue / the mission-passed tune,
// like the original game. gDuckFactor (1.0 = full, RADIO_DUCK_LEVEL = ducked)
// ramps smoothly toward its target each frame; UpdateVolume applies it.
extern std::atomic<DWORD> gMissionPassedDuckUntil; // set by the SCM hook (Switch.cpp)
extern std::atomic<DWORD> gDialogueDuckUntil;      // refreshed per mission-dialogue line
static float gDuckFactor = 1.0f;
static float gLastDuckApplied = 1.0f;
static const float RADIO_DUCK_LEVEL = 0.35f; // radio volume while ducked (35%)
unsigned char* const pMusicVolume = (unsigned char*)0x86965A;
int gCurrentStation = -1;

// VC's "Radio Volume" slider (m_nPrefsMusicVolume @ 0x86965A) ranges 0..127.
// Map it to BASS's 0.0..1.0 range, where 1.0 = the audio file's own level with
// NO amplification, so the new radio sits near the game's baseline loudness
// instead of being roughly doubled (the old code divided by 64, which amplified
// up to ~2x). Tuning knob: increase MUSIC_VOLUME_MAX to make the radio quieter
// overall, decrease it to make it louder.
const float MUSIC_VOLUME_MAX = 127.0f;
float RadioVolume(unsigned int pref)
{
    float v = (float)pref / MUSIC_VOLUME_MAX;
    if (v < 0.0f) v = 0.0f;
    if (v > 1.0f) v = 1.0f;
    return v;
}

// SCM opcode 041E can request a specific playback position (timecode in ms) for
// the original VC stations. When PlayLoadedStation plays the matching station it
// seeks to gForcedSeekMs instead of the synced radio clock. One-shot: consumed
// on the next PlayLoadedStation. gForcedSeekStation == -1 means "no forced seek".
static int gForcedSeekStation = -1;
static double gForcedSeekMs = 0.0;

// Global radio clock
double gRadioTime = 0.0;
static DWORD gLastTick = 0;

// Initial random offset baked into gRadioTime at launch. The nine original VC
// stations subtract this so they play on a clock that starts at 0 (= the top of
// the broadcast, like stock VC -> a fresh game opens Flash FM on Billie Jean).
// Added / custom stations keep the offset so they feel "already playing".
static double gRadioStartOffset = 0.0;

// Optional per-station start offset in ms, from the INI [STARTOFFSET] section.
// Lets you open a station at a specific point in its broadcast (e.g. Flash FM on
// Billie Jean). Keyed by station index; absent = no offset.
std::map<int, double> gStationStartOffsetMs;

// Start offsets are meant to recreate the *new-game intro* (e.g. Flash FM opening
// on Billie Jean). Once the player loads a save this session, we stop applying
// them so a loaded save resumes the radio naturally instead of always replaying
// the intro song. Reset only by relaunching the game.
static bool gStartOffsetsActive = true;

// New game vs loaded save. The nine original VC stations open at the top of the
// broadcast (clock 0) ONLY on a brand-new game. After a save load they must
// resume at a randomized "already playing" spot like added stations, not replay
// from the top — otherwise relaunching and loading a save always restarts the
// track from 0 (because the originals' clock is "time since launch", which is ~0
// right after launch). Cleared together with gStartOffsetsActive on any load.
static bool gOriginalsFromTop = true;

// Per-station drift anchor, computed the first time an offset station plays this
// session: it opens exactly at the configured offset and then advances naturally,
// so announcements and re-tunes resume where it was instead of snapping back to
// the offset. Only ever touched on the main thread (player radio).
static std::map<int, double> gStationAnchorDelta;

// Parse "M:SS" / "M:SS.mmm" or a plain seconds value into milliseconds.
static double ParseTimecodeMs(const std::string& s)
{
    size_t colon = s.find(':');
    if (colon != std::string::npos) {
        double mins = atof(s.substr(0, colon).c_str());
        double secs = atof(s.substr(colon + 1).c_str());
        return (mins * 60.0 + secs) * 1000.0;
    }
    return atof(s.c_str()) * 1000.0; // bare number = seconds
}

// Shared playback position (ms, before wrapping) for a given station index.
// applyStartOffset must be true only for the player's own car radio (called on
// the main thread). The ambient/traffic radio passes false, so the intro cue
// stays on the player's radio and the anchor map is only ever touched on the
// main thread.
double StationTimelineMs(int index, bool applyStartOffset)
{
    // Indices 0..8 are the nine original Vice City stations (the INI lists them
    // in VC's canonical order). Index 9 and up are added/custom stations.
    double base = (index >= 0 && index < 9 && gOriginalsFromTop)
        ? (gRadioTime - gRadioStartOffset) // originals on a NEW GAME: clock starts at 0 (top of broadcast)
        : gRadioTime;                       // added stations, and ALL stations after a save load: randomized "already playing" offset

    if (applyStartOffset && gStartOffsetsActive) {
        auto off = gStationStartOffsetMs.find(index);
        if (off != gStationStartOffsetMs.end()) {
            // Anchor on the station's FIRST play this session: it opens exactly at
            // the configured offset (independent of how long the intro cutscenes
            // ran), then drifts forward naturally — so an announcement or a re-tune
            // resumes where it was instead of snapping back to the offset.
            auto anc = gStationAnchorDelta.find(index);
            if (anc == gStationAnchorDelta.end())
                anc = gStationAnchorDelta.emplace(index, off->second - base).first;
            base += anc->second;
        }
    }
    return base;
}

// MP3 player state — protected by gMp3Mutex
static std::mutex gMp3Mutex;
static double gMp3SeekMs = 0.0;
static std::string gMp3FilePath = "";
static double gMp3RandomOffset = 0.0;
static double gMp3TotalDuration = 0.0;

// Background loading — gLoadedBuffer protected by gLoadMutex
static std::mutex gLoadMutex;
static std::vector<BYTE> gLoadedBuffer;
static std::atomic<bool> gBufferReady(false);
static std::atomic<bool> gLoadingInProgress(false);
static int gLoadingStation = -1;

// Station display + debounce
static bool gWaitingToPlay = false;
static int gLastNativeStation = -1;
static bool gPoliceRadioPlaying = false;
int gPendingStation = -1;
static DWORD gLastSwitchTick = 0;
static const DWORD SWITCH_DEBOUNCE_MS = 500;
static std::string gStationNameToShow = "";
static DWORD gStationNameTimer = 0;
static const DWORD STATION_NAME_DURATION = 3000;

// Radio announcements (SCM opcode 057D). gPendingAnnouncement is set by the
// hook in Switch.cpp (0 = bclosed, 1 = bopen, -1 = none). gQueuedAnnouncement
// holds a request until the player is actually in a vehicle (matching the
// game's native behaviour of waiting until you're driving).
extern std::atomic<int> gPendingAnnouncement;
static bool gAnnouncementPlaying = false;
static int gQueuedAnnouncement = -1;

// Set by the SCM hook (Switch.cpp) when a mission forces a station via opcode
// 041E SET_RADIO_CHANNEL. Applied to the BASS radio in gameProcessEvent.
extern std::atomic<int> gPendingScmStation;
extern std::atomic<int> gPendingScmStationTime; // param 2: timecode ms, or -1

// True while the player is in a vehicle listed in the [NORADIO] section.
static bool gNoRadioVehicle = false;

// SFX
static HSTREAM gSfxTuningStream = 0;
static HSTREAM gSfxStaticStream = 0;
static bool gSfxLoaded = false;

// SDT entry structure — GTA VC PC: 20 bytes each
struct SdtEntry {
    DWORD offset;
    DWORD size;
    DWORD sampleRate;
    DWORD loopStart;
    DWORD loopEnd;
};

// 343 = tuning click, 344-355 = static sounds
static int gStaticSoundIndex = 0;
static std::vector<BYTE> gWavTuning;
static std::vector<BYTE> gWavStatic[12];

static void BuildWav(const std::vector<BYTE>& pcm, DWORD sampleRate, std::vector<BYTE>& out)
{
    DWORD dataSize = (DWORD)pcm.size();
    out.resize(44 + dataSize);
    BYTE* p = out.data();

    auto write4 = [&](DWORD v) { memcpy(p, &v, 4); p += 4; };
    auto write2 = [&](WORD v) { memcpy(p, &v, 2); p += 2; };

    memcpy(p, "RIFF", 4); p += 4;
    write4(36 + dataSize);
    memcpy(p, "WAVE", 4); p += 4;
    memcpy(p, "fmt ", 4); p += 4;
    write4(16);
    write2(1);
    write2(1);
    write4(sampleRate);
    write4(sampleRate * 2);
    write2(2);
    write2(16);
    memcpy(p, "data", 4); p += 4;
    write4(dataSize);
    memcpy(p, pcm.data(), dataSize);
}

static HSTREAM CreateRawStream(const std::vector<BYTE>& wavBuf, bool loop)
{
    if (wavBuf.empty()) return 0;
    HSTREAM s = BASS_StreamCreateFile(TRUE, wavBuf.data(), 0, (QWORD)wavBuf.size(),
        loop ? BASS_SAMPLE_LOOP : 0);
    if (!s) {
        gLog << "SFX: stream error " << BASS_ErrorGetCode() << std::endl;
        gLog.flush();
    }
    return s;
}

static void LoadAllSfx()
{
    std::string sdtPath = gGameFolder + "audio\\SFX.SDT";
    std::string rawPath = gGameFolder + "audio\\SFX.RAW";

    std::ifstream sdt(sdtPath, std::ios::binary);
    if (!sdt.is_open()) {
        gLog << "SFX: cannot open SFX.SDT" << std::endl;
        gLog.flush();
        return;
    }
    std::ifstream raw(rawPath, std::ios::binary);
    if (!raw.is_open()) {
        gLog << "SFX: cannot open SFX.RAW" << std::endl;
        gLog.flush();
        return;
    }

    auto loadOne = [&](int index, std::vector<BYTE>& wavOut) -> bool {
        SdtEntry entry;
        sdt.seekg(index * sizeof(SdtEntry));
        sdt.read((char*)&entry, sizeof(SdtEntry));
        if (entry.size == 0) {
            gLog << "SFX: entry " << index << " has zero size" << std::endl;
            return false;
        }
        std::vector<BYTE> pcm(entry.size);
        raw.seekg(entry.offset);
        raw.read((char*)pcm.data(), entry.size);
        BuildWav(pcm, entry.sampleRate, wavOut);
        gLog << "SFX: sound " << index << " loaded — " << entry.size << " bytes @ " << entry.sampleRate << " Hz" << std::endl;
        return true;
        };

    bool ok = loadOne(343, gWavTuning);
    for (int i = 0; i < 12; i++)
        loadOne(344 + i, gWavStatic[i]);

    sdt.close();
    raw.close();

    gSfxLoaded = ok;
    gLog << "SFX loaded: " << (ok ? "yes" : "partial/failed") << std::endl;
    gLog.flush();
}

static void PlayTuningSound()
{
    if (!gSfxLoaded || gWavTuning.empty()) return;

    if (gSfxTuningStream) {
        BASS_ChannelStop(gSfxTuningStream);
        BASS_StreamFree(gSfxTuningStream);
        gSfxTuningStream = 0;
    }

    gSfxTuningStream = CreateRawStream(gWavTuning, false);
    if (gSfxTuningStream) {
        BASS_ChannelSetAttribute(gSfxTuningStream, BASS_ATTRIB_VOL, RadioVolume(*pMusicVolume));
        BASS_ChannelPlay(gSfxTuningStream, FALSE);
    }
}

static void StartStaticSound()
{
    if (!gSfxLoaded) return;

    if (gSfxStaticStream) {
        BASS_ChannelStop(gSfxStaticStream);
        BASS_StreamFree(gSfxStaticStream);
        gSfxStaticStream = 0;
    }

    int pick = gStaticSoundIndex % 12;
    gStaticSoundIndex = (gStaticSoundIndex + 1) % 12;
    if (gWavStatic[pick].empty()) return;

    gSfxStaticStream = CreateRawStream(gWavStatic[pick], true);
    if (gSfxStaticStream) {
        BASS_ChannelSetAttribute(gSfxStaticStream, BASS_ATTRIB_VOL, RadioVolume(*pMusicVolume));
        BASS_ChannelPlay(gSfxStaticStream, FALSE);
    }
}

static void StopStaticSound()
{
    if (gSfxStaticStream) {
        BASS_ChannelStop(gSfxStaticStream);
        BASS_StreamFree(gSfxStaticStream);
        gSfxStaticStream = 0;
    }
}

static void TurnRadioOff()
{
    if (gStream) {
        BASS_ChannelStop(gStream);
        BASS_StreamFree(gStream);
        gStream = 0;
    }
    StopStaticSound();
    {
        std::lock_guard<std::mutex> lock(gLoadMutex);
        gLoadedBuffer.clear();
    }
    gBufferReady = false;
    gCurrentStation = -1;
    gWaitingToPlay = false;
    gStationNameToShow = "Radio Off";
    gStationNameTimer = GetTickCount();
    gLog << "Radio off" << std::endl;
    gLog.flush();
}

static void StopRadio()
{
    if (gStream) {
        BASS_ChannelStop(gStream);
        BASS_StreamFree(gStream);
        gStream = 0;
    }
}

// Fully tear down the radio and reset all per-session state. Used when the game
// isn't in normal play (loading screen / no player) or when a save or restart is
// loaded from the menu, so audio never bleeds over a load and the next session
// starts clean.
static void StopAndResetRadio()
{
    StopRadio();
    StopStaticSound();
    {
        std::lock_guard<std::mutex> lock(gLoadMutex);
        gLoadedBuffer.clear();
    }
    gBufferReady = false;
    gWaitingToPlay = false;
    gWasInVehicle = false;
    gCurrentStation = -1;
    gPendingStation = -1;
    gLastSwitchTick = 0;
    gAnnouncementPlaying = false;
    gQueuedAnnouncement = -1;
    gPoliceRadioPlaying = false;
    gNoRadioVehicle = false;
    gForcedSeekStation = -1;
    gPendingScmStation.exchange(-1);
    gPendingScmStationTime.exchange(-1);
    gStationNameToShow = "";
}

static bool IsADF(const std::string& path)
{
    std::string ext = path.size() >= 4 ? path.substr(path.size() - 4) : "";
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
    return ext == ".adf";
}

static void UpdateVolume()
{
    unsigned char vol = *pMusicVolume;
    float bassVol = RadioVolume(vol) * gDuckFactor;
    bool prefChanged = (vol != gLastVolume);
    bool duckChanged = (fabsf(gDuckFactor - gLastDuckApplied) > 0.001f);
    if (!prefChanged && !duckChanged)
        return;
    gLastVolume = vol;
    gLastDuckApplied = gDuckFactor;
    if (gStream)
        BASS_ChannelSetAttribute(gStream, BASS_ATTRIB_VOL, bassVol);
    if (gSfxStaticStream)
        BASS_ChannelSetAttribute(gSfxStaticStream, BASS_ATTRIB_VOL, bassVol);
    if (gSfxTuningStream)
        BASS_ChannelSetAttribute(gSfxTuningStream, BASS_ATTRIB_VOL, bassVol);
}

static std::vector<std::string> ScanMp3Folder(const std::string& folder)
{
    std::vector<std::string> files;
    std::string searchPath = folder + "mp3\\*.mp3";
    WIN32_FIND_DATAA fd;
    HANDLE hFind = FindFirstFileA(searchPath.c_str(), &fd);
    if (hFind == INVALID_HANDLE_VALUE)
        return files;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
            files.push_back(folder + "mp3\\" + fd.cFileName);
    } while (FindNextFileA(hFind, &fd));
    FindClose(hFind);
    std::sort(files.begin(), files.end());
    return files;
}

static void EnsureMp3Durations()
{
    static bool loaded = false;
    if (loaded) return;

    double total = 0.0;
    for (const auto& f : gMp3Files) {
        HSTREAM s = BASS_StreamCreateFile(FALSE, f.c_str(), 0, 0, 0);
        if (s) {
            QWORD bytes = BASS_ChannelGetLength(s, BASS_POS_BYTE);
            double d = BASS_ChannelBytes2Seconds(s, bytes) * 1000.0;
            total += d > 0 ? d : 1.0;
            BASS_StreamFree(s);
        }
        else {
            total += 1.0;
        }
    }

    loaded = true;

    {
        std::lock_guard<std::mutex> lock(gMp3Mutex);
        gMp3TotalDuration = total;
        if (total > 0.0)
            gMp3RandomOffset = fmod((double)(rand() % 1000000), total);
    }

    gLog << "MP3 durations loaded, total: " << (DWORD)(total / 1000.0) << "s" << std::endl;
    gLog.flush();
}

static void GetMp3PlayerPosition(double radioTime, int& fileIndex, double& seekMs)
{
    if (gMp3Files.empty()) { fileIndex = 0; seekMs = 0; return; }

    EnsureMp3Durations();

    double total, offset;
    {
        std::lock_guard<std::mutex> lock(gMp3Mutex);
        total = gMp3TotalDuration;
        offset = gMp3RandomOffset;
    }

    if (total <= 0.0) { fileIndex = 0; seekMs = 0; return; }

    double pos = fmod(radioTime + offset, total);

    for (int i = 0; i < (int)gMp3Files.size(); i++) {
        HSTREAM s = BASS_StreamCreateFile(FALSE, gMp3Files[i].c_str(), 0, 0, 0);
        double dur = 1.0;
        if (s) {
            QWORD bytes = BASS_ChannelGetLength(s, BASS_POS_BYTE);
            double d = BASS_ChannelBytes2Seconds(s, bytes) * 1000.0;
            if (d > 0) dur = d;
            BASS_StreamFree(s);
        }
        if (pos < dur) {
            fileIndex = i;
            seekMs = pos;
            return;
        }
        pos -= dur;
    }
    fileIndex = 0;
    seekMs = 0;
}

static void LoadStationThread(int index)
{
    if (index == gMp3StationIndex) {
        int fileIndex;
        double seekMs;
        GetMp3PlayerPosition(gRadioTime, fileIndex, seekMs);

        {
            std::lock_guard<std::mutex> lock(gMp3Mutex);
            gMp3FilePath = gMp3Files[fileIndex];
            gMp3SeekMs = seekMs;
        }

        gLog << "Thread: MP3 PLAYER -> " << gMp3Files[fileIndex] << " at " << (DWORD)seekMs << " ms" << std::endl;
        gLog.flush();

        {
            std::lock_guard<std::mutex> lock(gLoadMutex);
            gLoadedBuffer.clear();
        }
        gBufferReady = true;
        gLoadingInProgress = false;
        return;
    }

    std::string fullPath = gGameFolder + stations[index].file;
    gLog << "Thread: loading " << fullPath << std::endl;
    gLog.flush();

    std::vector<BYTE> buf;

    if (IsADF(fullPath))
    {
        std::ifstream f(fullPath, std::ios::binary);
        if (!f.is_open()) {
            gLog << "Thread: cannot open file!" << std::endl;
            gLog.flush();
            gLoadingInProgress = false;
            return;
        }

        buf = std::vector<BYTE>(
            std::istreambuf_iterator<char>(f),
            std::istreambuf_iterator<char>()
        );

        for (auto& b : buf)
            b ^= 0x22;
    }

    gLog << "Thread: loaded " << buf.size() << " bytes" << std::endl;
    gLog.flush();

    {
        std::lock_guard<std::mutex> lock(gLoadMutex);
        gLoadedBuffer = std::move(buf);
    }
    gBufferReady = true;
    gLoadingInProgress = false;
}

static void StartLoadingStation(int index)
{
    if (gLoadingInProgress)
        return;

    gBufferReady = false;
    gLoadingInProgress = true;
    gLoadingStation = index;
    gWaitingToPlay = true;
    std::thread(LoadStationThread, index).detach();
}

static void PlayLoadedStation(int index)
{
    StopRadio();
    StopStaticSound();
    gWaitingToPlay = false;

    if (index == gMp3StationIndex) {
        std::string filePath;
        double seekMs;
        {
            std::lock_guard<std::mutex> lock(gMp3Mutex);
            filePath = gMp3FilePath;
            seekMs = gMp3SeekMs;
        }
        gStream = BASS_StreamCreateFile(FALSE, filePath.c_str(), 0, 0, BASS_SAMPLE_LOOP);
        if (!gStream) {
            gLog << "BASS MP3 error: " << BASS_ErrorGetCode() << std::endl;
            gLog.flush();
            return;
        }
        QWORD seekBytes = BASS_ChannelSeconds2Bytes(gStream, seekMs / 1000.0);
        BASS_ChannelSetPosition(gStream, seekBytes, BASS_POS_BYTE);
        gLog << "MP3 PLAYER: " << filePath << " at " << (DWORD)seekMs << " ms" << std::endl;
        gLog.flush();
    }
    else {
        std::vector<BYTE> localBuffer;
        {
            std::lock_guard<std::mutex> lock(gLoadMutex);
            localBuffer = std::move(gLoadedBuffer);
            gLoadedBuffer.clear();
        }

        if (!localBuffer.empty()) {
            gStream = BASS_StreamCreateFile(TRUE, localBuffer.data(), 0, localBuffer.size(), BASS_SAMPLE_LOOP);
            if (!gStream) {
                gLog << "BASS error: " << BASS_ErrorGetCode() << std::endl;
                gLog.flush();
                return;
            }
            {
                std::lock_guard<std::mutex> lock(gLoadMutex);
                gLoadedBuffer = std::move(localBuffer);
            }
        }
        else {
            std::string fullPath = gGameFolder + stations[index].file;
            gStream = BASS_StreamCreateFile(FALSE, fullPath.c_str(), 0, 0, BASS_SAMPLE_LOOP);
            if (!gStream) {
                gLog << "BASS error: " << BASS_ErrorGetCode() << std::endl;
                gLog.flush();
                return;
            }
        }

        QWORD totalBytes = BASS_ChannelGetLength(gStream, BASS_POS_BYTE);
        double totalMs = BASS_ChannelBytes2Seconds(gStream, totalBytes) * 1000.0;
        if (totalMs > 0.0) {
            // Honor an SCM-requested timecode only for the station it was set for
            // (an original VC station); everything else rides the synced clock.
            double seekMs;
            if (index == gForcedSeekStation && gForcedSeekMs >= 0.0)
                seekMs = fmod(gForcedSeekMs, totalMs);
            else
                seekMs = fmod(StationTimelineMs(index, true), totalMs);
            gForcedSeekStation = -1; // one-shot, consumed
            QWORD seekBytes = BASS_ChannelSeconds2Bytes(gStream, seekMs / 1000.0);
            BASS_ChannelSetPosition(gStream, seekBytes, BASS_POS_BYTE);
            gLog << "[" << stations[index].name << "] seek: " << (DWORD)seekMs << " / " << (DWORD)totalMs << " ms" << std::endl;
            gLog.flush();
        }
    }

    unsigned char vol = *pMusicVolume;
    gLastVolume = vol;
    BASS_ChannelSetAttribute(gStream, BASS_ATTRIB_VOL, RadioVolume(vol));
    BASS_ChannelPlay(gStream, FALSE);
    gCurrentStation = index;
    if (index >= 0 && index < (int)stations.size()) {
        gStationNameToShow = stations[index].name;
        gStationNameTimer = GetTickCount();
    }
}

static void LoadINI()
{
    char path[MAX_PATH];
    HMODULE hModule = NULL;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
        GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        (LPCSTR)&LoadINI, &hModule);
    GetModuleFileNameA(hModule, path, MAX_PATH);
    gScriptsFolder = std::string(path);
    gScriptsFolder = gScriptsFolder.substr(0, gScriptsFolder.find_last_of("\\/") + 1);

    std::string noSlash = gScriptsFolder.substr(0, gScriptsFolder.size() - 1);
    gGameFolder = noSlash.substr(0, noSlash.find_last_of("\\/") + 1);

    std::string iniPath = gScriptsFolder + "NorthstarRadio.ini";
    std::string logPath = gScriptsFolder + "NorthstarRadio.log";

    gLog.open(logPath);
    gLog << "Game folder: " << gGameFolder << std::endl;
    gLog << "Scripts folder: " << gScriptsFolder << std::endl;

    std::ifstream iniBin(iniPath, std::ios::binary);
    if (!iniBin.is_open()) {
        gLog << "ERROR: NorthstarRadio.ini not found" << std::endl;
        return;
    }
    std::vector<BYTE> rawBytes((std::istreambuf_iterator<char>(iniBin)),
        std::istreambuf_iterator<char>());
    iniBin.close();

    std::string iniContent;

    if (rawBytes.size() >= 2 && rawBytes[0] == 0xFF && rawBytes[1] == 0xFE) {
        const wchar_t* wstr = (const wchar_t*)(rawBytes.data() + 2);
        int wlen = (int)((rawBytes.size() - 2) / 2);
        int needed = WideCharToMultiByte(CP_UTF8, 0, wstr, wlen, nullptr, 0, nullptr, nullptr);
        iniContent.resize(needed);
        WideCharToMultiByte(CP_UTF8, 0, wstr, wlen, &iniContent[0], needed, nullptr, nullptr);
        gLog << "INI: detected UTF-16 LE, converted automatically" << std::endl;
    }
    else if (rawBytes.size() >= 2 && rawBytes[0] == 0xFE && rawBytes[1] == 0xFF) {
        std::vector<BYTE> swapped(rawBytes.begin() + 2, rawBytes.end());
        for (size_t i = 0; i + 1 < swapped.size(); i += 2)
            std::swap(swapped[i], swapped[i + 1]);
        const wchar_t* wstr = (const wchar_t*)swapped.data();
        int wlen = (int)(swapped.size() / 2);
        int needed = WideCharToMultiByte(CP_UTF8, 0, wstr, wlen, nullptr, 0, nullptr, nullptr);
        iniContent.resize(needed);
        WideCharToMultiByte(CP_UTF8, 0, wstr, wlen, &iniContent[0], needed, nullptr, nullptr);
        gLog << "INI: detected UTF-16 BE, converted automatically" << std::endl;
    }
    else if (rawBytes.size() >= 3 && rawBytes[0] == 0xEF && rawBytes[1] == 0xBB && rawBytes[2] == 0xBF) {
        iniContent.assign(rawBytes.begin() + 3, rawBytes.end());
        gLog << "INI: detected UTF-8 BOM, stripped automatically" << std::endl;
    }
    else if (rawBytes.size() >= 2 && rawBytes[0] == '{' && rawBytes[1] == '\\') {
        gLog << "ERROR: NorthstarRadio.ini is saved as RTF format." << std::endl;
        gLog << "Please open it in Notepad (not WordPad) and save as plain text." << std::endl;
        gLog.flush();
        return;
    }
    else {
        iniContent.assign(rawBytes.begin(), rawBytes.end());
    }

    std::istringstream ini(iniContent);

    auto trim = [](std::string& s) {
        size_t start = s.find_first_not_of(" \t\r\n");
        size_t end = s.find_last_not_of(" \t\r\n");
        s = (start == std::string::npos) ? "" : s.substr(start, end - start + 1);
        };

    bool inStationsSection = false;
    bool inSettingsSection = false;
    bool inOffsetSection = false;
    std::map<std::string, double> pendingOffsets; // station name (lower) -> start offset ms
    std::string line;
    while (std::getline(ini, line))
    {
        size_t comment = line.find('#');
        if (comment != std::string::npos)
            line = line.substr(0, comment);

        trim(line);
        if (line.empty())
            continue;

        if (line[0] == '[') {
            std::string header = line;
            std::transform(header.begin(), header.end(), header.begin(), ::tolower);
            trim(header);
            inStationsSection = (header == "[stations]");
            inSettingsSection = (header == "[settings]");
            inOffsetSection = (header == "[startoffset]");
            continue;
        }

        // [STARTOFFSET]: "Station Name | M:SS"  (open that station at this point)
        if (inOffsetSection) {
            size_t sep = line.find('|');
            if (sep == std::string::npos)
                continue;
            std::string name = line.substr(0, sep);
            std::string tc = line.substr(sep + 1);
            trim(name);
            trim(tc);
            if (name.size() >= 2 && name.front() == '"' && name.back() == '"')
                name = name.substr(1, name.size() - 2);
            std::transform(name.begin(), name.end(), name.begin(), ::tolower);
            if (!name.empty() && !tc.empty())
                pendingOffsets[name] = ParseTimecodeMs(tc);
            continue;
        }

        if (inSettingsSection) {
            size_t eq = line.find('=');
            if (eq != std::string::npos) {
                std::string key = line.substr(0, eq);
                std::string val = line.substr(eq + 1);
                trim(key);
                trim(val);
                std::transform(key.begin(), key.end(), key.begin(), ::tolower);
                if (key == "ambientradio")
                    gAmbientRadioEnabled = (val == "1");
            }
            continue;
        }

        if (!inStationsSection)
            continue;

        size_t sep = line.find('|');
        if (sep == std::string::npos)
            sep = line.find('\t');
        if (sep == std::string::npos)
            continue;

        RadioStation station;
        station.name = line.substr(0, sep);
        station.file = line.substr(sep + 1);
        trim(station.name);
        trim(station.file);

        if (station.name.empty() || station.file.empty())
            continue;

        if (station.name.size() >= 2 && station.name.front() == '"' && station.name.back() == '"')
            station.name = station.name.substr(1, station.name.size() - 2);

        stations.push_back(station);
        gLog << "Station " << stations.size() << ": [" << station.name << "] -> [" << station.file << "]" << std::endl;
    }

    gLog << "Total stations loaded: " << stations.size() << std::endl;

    gMp3Files = ScanMp3Folder(gGameFolder);
    if (!gMp3Files.empty()) {
        gMp3StationIndex = (int)stations.size();
        RadioStation mp3Station;
        mp3Station.name = "MP3 PLAYER";
        mp3Station.file = "";
        stations.push_back(mp3Station);
        gLog << "MP3 PLAYER station added with " << gMp3Files.size() << " files" << std::endl;
    }

    // Resolve [STARTOFFSET] entries (matched by station name) to station indices.
    if (!pendingOffsets.empty()) {
        for (int i = 0; i < (int)stations.size(); i++) {
            std::string lname = stations[i].name;
            std::transform(lname.begin(), lname.end(), lname.begin(), ::tolower);
            auto it = pendingOffsets.find(lname);
            if (it != pendingOffsets.end()) {
                gStationStartOffsetMs[i] = it->second;
                gLog << "Start offset: [" << stations[i].name << "] -> " << (DWORD)it->second << " ms" << std::endl;
            }
        }
    }

    gLog.flush();
}

class NoRadioPlugin
{
public:
    NoRadioPlugin()
    {
        LoadINI();

        srand((unsigned int)time(NULL));
        double randomMs = (double)((((DWORD)rand() << 15) | (DWORD)rand()) % 7200000U);
        gRadioTime = randomMs;
        gRadioStartOffset = randomMs; // originals play from (gRadioTime - this) = 0 at launch
        gLastTick = GetTickCount();
        gLog << "Radio time start: " << (DWORD)gRadioTime << " ms (offset " << (DWORD)gRadioStartOffset
             << "; original stations start at 0)" << std::endl;
        gLog.flush();

        Events::initGameEvent.Add([]()
            {
                // If this game-init was triggered by loading a save, disable the
                // intro start-offset so Flash FM resumes naturally — only a brand-new
                // game should open on Billie Jean. m_bWantToLoad is reliably set here
                // at init time (the per-frame check missed it because the game loop is
                // suspended during the load). Done conservatively: we only ever disable,
                // so a new game (m_bWantToLoad == false) is untouched and the intro works.
                if (FrontEndMenuManager.m_bWantToLoad) {
                    gStartOffsetsActive = false;
                    gOriginalsFromTop = false; // loaded save: originals resume at a random spot, not the top
                }

                if (!gBassReady) {
                    if (BASS_Init(-1, 44100, 0, 0, NULL)) {
                        gBassReady = true;
                    }
                    else if (BASS_ErrorGetCode() == 8) {
                        gBassReady = true;
                    }
                    gLog << "BASS ready: " << gBassReady << std::endl;
                    gLog.flush();

                    LoadAllSfx();

                    if (gBassReady && !stations.empty())
                        StartLoadingStation(0);
                }
            });

        Events::gameProcessEvent.Add([]()
            {
                DWORD now = GetTickCount();
                gRadioTime += (double)(now - gLastTick);
                gLastTick = now;

                // A save load / restart was requested from the menu: the world is
                // being rebuilt, so keep the radio fully torn down and run no other
                // logic until it's done. Without this the radio resumes over the
                // loading screen (and could restart from a stale "in vehicle" state).
                if (FrontEndMenuManager.m_bWantToLoad || FrontEndMenuManager.m_bWantToRestart) {
                    // Loading a save (not a fresh new game) means the intro is over —
                    // stop applying [STARTOFFSET] so loaded saves resume the radio
                    // naturally instead of always replaying the intro song.
                    if (FrontEndMenuManager.m_bWantToLoad) {
                        gStartOffsetsActive = false;
                        gOriginalsFromTop = false; // loaded save: originals resume at a random spot, not the top
                    }
                    StopAndResetRadio();
                    gWasPaused = false;
                    return;
                }

                CPlayerPed* pPlayer = FindPlayerPed();
                if (!pPlayer) {
                    // No player ped (loading screen / teardown) — ensure silence and
                    // don't leave a "was paused" flag that would resume a stale stream.
                    StopAndResetRadio();
                    gWasPaused = false;
                    return;
                }

                // A change of player ped (after the first one this session) means the
                // world was rebuilt — a save was loaded or the game restarted. Once the
                // intro start-offset has already been applied, stop applying it so a
                // loaded save resumes the radio naturally instead of replaying the intro
                // song. (m_bWantToLoad isn't reliably observable here because the game
                // loop is suspended during the load.)
                {
                    static CPlayerPed* sLastPed = nullptr;
                    if (sLastPed && sLastPed != pPlayer && !gStationAnchorDelta.empty()) {
                        gStartOffsetsActive = false;
                        gOriginalsFromTop = false;
                        gStationAnchorDelta.clear();
                    }
                    sLastPed = pPlayer;
                }

                // Pause the new radio while the pause menu is open (avoids clashing
                // with VC's own radio preview in the audio-settings screen) OR while
                // a cutscene / scripted scene is playing — otherwise the radio keeps
                // blaring under a mission cutscene you triggered from a vehicle.
                // ms_running covers theatrical cutscenes; the camera's widescreen
                // (cinematic) bars cover scripted in-mission camera scenes. The new
                // Radio Volume level is applied on the way out (UpdateVolume below).
                bool inCutscene = CCutsceneMgr::ms_running || TheCamera.m_bWideScreenOn;
                bool isPaused = FrontEndMenuManager.m_bMenuActive || inCutscene;
                if (isPaused && !gWasPaused) {
                    if (gStream) BASS_ChannelPause(gStream);
                    if (gSfxStaticStream) BASS_ChannelPause(gSfxStaticStream);
                    gWasPaused = true;
                }
                else if (!isPaused && gWasPaused) {
                    UpdateVolume();
                    if (gStream) BASS_ChannelPlay(gStream, FALSE);
                    if (gSfxStaticStream) BASS_ChannelPlay(gSfxStaticStream, FALSE);
                    gWasPaused = false;
                }
                if (isPaused) return;

                CVehicle* pVehicle = pPlayer->m_pVehicle;
                bool inVehicle = pPlayer->m_bInVehicle;
                gPlayerInVehicle = inVehicle;

                // Remember the current vehicle so we can save its station on exit
                // (m_pVehicle is null on the exit frame, and the instant you're
                // knocked off a bike).
                if (inVehicle && pVehicle)
                    gActiveVehicle = pVehicle;

                // ===== No-radio vehicles ([NORADIO] section): total silence =====
                // Checked before everything else so it overrides music, police
                // radio and announcements alike.
                if (inVehicle && pVehicle && IsNoRadioVehicle(pVehicle->m_nModelIndex)) {
                    if (!gNoRadioVehicle) {
                        StopRadio();
                        StopStaticSound();
                        gNoRadioVehicle = true;
                        gWasInVehicle = true;        // suppress the seated-detection path
                        gPoliceRadioPlaying = false;
                        gCurrentStation = -1;
                        gPendingStation = -1;
                        gLastSwitchTick = 0;
                        gWaitingToPlay = false;
                        gBufferReady = false;
                        gAnnouncementPlaying = false;
                        gStationNameToShow = "";     // no banner
                        gLog << "No-radio vehicle (model " << pVehicle->m_nModelIndex << "): radio disabled" << std::endl;
                        gLog.flush();
                    }
                    // Swallow any input or queued announcement so nothing can turn it on.
                    gSwitchNext = false;
                    gSwitchPrev = false;
                    gPendingAnnouncement.exchange(-1);
                    gQueuedAnnouncement = -1;
                    gPendingScmStation.exchange(-1);
                    gPendingScmStationTime.exchange(-1);
                    UpdateVolume();
                    return;
                }

                if (inVehicle && pVehicle && pVehicle->IsLawEnforcementVehicle()) {
                    if (!gPoliceRadioPlaying) {
                        StopRadio();
                        StopStaticSound();
                        std::string policePath = gGameFolder + "audio\\police.mp3";
                        gStream = BASS_StreamCreateFile(FALSE, policePath.c_str(), 0, 0, BASS_SAMPLE_LOOP);
                        if (gStream) {
                            unsigned char vol = *pMusicVolume;
                            gLastVolume = vol;
                            BASS_ChannelSetAttribute(gStream, BASS_ATTRIB_VOL, RadioVolume(vol));
                            BASS_ChannelPlay(gStream, FALSE);
                            gLog << "Police radio started" << std::endl;
                            gLog.flush();
                        }
                        gPoliceRadioPlaying = true;
                    }
                    UpdateVolume();
                    return;
                }

                if (gPoliceRadioPlaying) {
                    StopRadio();
                    gPoliceRadioPlaying = false;
                    gWasInVehicle = false;
                    gCurrentStation = -1;
                    gWaitingToPlay = false;
                }

                if (inVehicle)
                    DMAudio.SetRadioInCar(10);

                // Detect SCM opcode station changes via native radio byte
                if (inVehicle && pVehicle) {
                    int nativeStation = *(BYTE*)((BYTE*)pVehicle + 0x23C);
                    if (nativeStation != 10 && nativeStation != gLastNativeStation && gWasInVehicle) {
                        if (nativeStation >= 0 && nativeStation < (int)stations.size()) {
                            gLastNativeStation = nativeStation;
                            StopRadio();
                            {
                                std::lock_guard<std::mutex> lock(gLoadMutex);
                                gLoadedBuffer.clear();
                            }
                            gBufferReady = false;
                            gPendingStation = -1;
                            gLastSwitchTick = 0;
                            gCurrentStation = nativeStation;
                            StartLoadingStation(nativeStation);
                            gLog << "SCM: station set to [" << stations[nativeStation].name << "]" << std::endl;
                            gLog.flush();
                        }
                    }
                    // Force native station byte back to 10 (radio off) every frame after reading.
                    // This prevents the native audio system and HUD from acting on any value
                    // the game may have written here due to mouse wheel or other input.
                    *(BYTE*)((BYTE*)pVehicle + 0x23C) = 10;
                }
                else {
                    gLastNativeStation = -1;
                }

                DWORD pedState = *(DWORD*)((BYTE*)pPlayer + 0x244);
                bool isSeated = (pedState == 0x32);

                if (isSeated && !gWasInVehicle && pVehicle) {
                    gWasInVehicle = true;
                    int station = GetStationForVehicle(pVehicle);
                    StopRadio();
                    {
                        std::lock_guard<std::mutex> lock(gLoadMutex);
                        gLoadedBuffer.clear();
                    }
                    gBufferReady = false;
                    gCurrentStation = station;
                    StartLoadingStation(station);
                }

                if (gWasInVehicle && gWaitingToPlay && gBufferReady) {
                    PlayLoadedStation(gLoadingStation);
                }

                // ===== Radio announcements (opcode 057D: 0 = bclosed, 1 = bopen) =====
                {
                    int req = gPendingAnnouncement.exchange(-1);
                    if (req == 0 || req == 1)
                        gQueuedAnnouncement = req;
                }

                if (gAnnouncementPlaying) {
                    if (!inVehicle) {
                        // Player left the car mid-announcement: stop it and let the
                        // normal "exited vehicle" logic below take over.
                        StopRadio();
                        gAnnouncementPlaying = false;
                        gQueuedAnnouncement = -1;
                    }
                    else if (!gStream || BASS_ChannelIsActive(gStream) == BASS_ACTIVE_STOPPED) {
                        // Announcement finished: resume the station from the live
                        // radio clock (it kept advancing, so we rejoin "in progress").
                        gAnnouncementPlaying = false;
                        gSwitchNext = false;
                        gSwitchPrev = false;
                        if (gCurrentStation >= 0) {
                            StopRadio();
                            {
                                std::lock_guard<std::mutex> lock(gLoadMutex);
                                gLoadedBuffer.clear();
                            }
                            gBufferReady = false;
                            StartLoadingStation(gCurrentStation);
                            gLog << "Announcement finished, resuming station" << std::endl;
                            gLog.flush();
                        }
                        UpdateVolume();
                        return;
                    }
                    else {
                        // Still playing: freeze radio controls until it ends, exactly
                        // like the native announcement ("radio cannot be switched").
                        gSwitchNext = false;
                        gSwitchPrev = false;
                        UpdateVolume();
                        return;
                    }
                }
                else if (gQueuedAnnouncement != -1 && gWasInVehicle && inVehicle) {
                    int ann = gQueuedAnnouncement;
                    gQueuedAnnouncement = -1;
                    std::string annFile = gGameFolder + "audio\\" + (ann == 0 ? "BCLOSED.mp3" : "BOPEN.mp3");

                    StopRadio();
                    StopStaticSound();
                    {
                        std::lock_guard<std::mutex> lock(gLoadMutex);
                        gLoadedBuffer.clear();
                    }
                    gBufferReady = false;
                    gWaitingToPlay = false;
                    gPendingStation = -1;
                    gLastSwitchTick = 0;
                    gSwitchNext = false;
                    gSwitchPrev = false;

                    gStream = BASS_StreamCreateFile(FALSE, annFile.c_str(), 0, 0, 0); // not looped
                    if (gStream) {
                        unsigned char vol = *pMusicVolume;
                        gLastVolume = vol;
                        BASS_ChannelSetAttribute(gStream, BASS_ATTRIB_VOL, RadioVolume(vol));
                        BASS_ChannelPlay(gStream, FALSE);
                        gAnnouncementPlaying = true;
                        gStationNameToShow = ""; // announcements never show a station banner
                        gLog << "Announcement playing: " << annFile << std::endl;
                        gLog.flush();
                        UpdateVolume();
                        return;
                    }
                    else {
                        gLog << "Announcement: BASS error " << BASS_ErrorGetCode() << " (" << annFile << ")" << std::endl;
                        gLog.flush();
                        // File missing/unreadable: fall through to normal radio.
                    }
                }

                // ===== Mission-scripted radio change (SCM opcode 041E) =====
                // main.scm forces a specific station during some missions. The
                // hook in Switch.cpp captured the requested index; apply it here.
                // The request is left pending (load, not consumed) until we're
                // actually in a car and not mid-load, so it survives the frame the
                // player first sits down (when the default station is still loading).
                {
                    int scmStation = gPendingScmStation.load();
                    if (scmStation >= 0 && inVehicle && gWasInVehicle && !gLoadingInProgress) {
                        int scmTime = gPendingScmStationTime.load();
                        gPendingScmStation = -1;     // consume now that we can act on it
                        gPendingScmStationTime = -1;
                        // 0..8 are the original VC stations (same order as the INI).
                        // 9/10 mean "off" in VC, and we never map 041E to MP3 PLAYER.
                        if (scmStation <= 8 && scmStation < (int)stations.size()
                            && scmStation != gCurrentStation) {
                            StopRadio();
                            {
                                std::lock_guard<std::mutex> lock(gLoadMutex);
                                gLoadedBuffer.clear();
                            }
                            gBufferReady = false;
                            gPendingStation = -1;
                            gLastSwitchTick = 0;
                            gCurrentStation = scmStation;
                            // Original VC stations honor the script's exact timecode
                            // when it gave one (param 2 >= 0); otherwise (and for any
                            // added stations) we ride the synced clock.
                            if (scmTime >= 0) {
                                gForcedSeekStation = scmStation;
                                gForcedSeekMs = (double)scmTime;
                                gLog << "SCM 041E: radio -> [" << stations[scmStation].name
                                     << "] @ " << scmTime << " ms (scripted timecode)" << std::endl;
                            }
                            else {
                                gForcedSeekStation = -1;
                                gLog << "SCM 041E: radio -> [" << stations[scmStation].name
                                     << "] (synced clock)" << std::endl;
                            }
                            StartLoadingStation(scmStation);
                            gLog.flush();
                        }
                        // station 9/10 (off), out of range, or unchanged: nothing to do
                    }
                }

                // MP3 PLAYER: advance to next file when current ends
                if (gCurrentStation == gMp3StationIndex && gStream) {
                    if (BASS_ChannelIsActive(gStream) == BASS_ACTIVE_STOPPED) {
                        StopRadio();
                        {
                            std::lock_guard<std::mutex> lock(gLoadMutex);
                            gLoadedBuffer.clear();
                        }
                        gBufferReady = false;
                        StartLoadingStation(gMp3StationIndex);
                    }
                }

                if (gSwitchNext || gSwitchPrev) {
                    bool goNext = gSwitchNext;
                    gSwitchNext = false;
                    gSwitchPrev = false;

                    if (gWasInVehicle) {
                        int next;
                        if (gCurrentStation == -1 && gPendingStation == -1) {
                            next = goNext ? 0 : (int)stations.size() - 1;
                        }
                        else {
                            int base = gPendingStation != -1 ? gPendingStation : gCurrentStation;
                            next = base + (goNext ? 1 : -1);
                        }

                        PlayTuningSound();

                        if (next < 0 || next >= (int)stations.size()) {
                            gPendingStation = -1;
                            gLastSwitchTick = 0;
                            StopStaticSound();
                            TurnRadioOff();
                        }
                        else {
                            if (!gSfxStaticStream)
                                StartStaticSound();

                            StopRadio();

                            gPendingStation = next;
                            gLastSwitchTick = GetTickCount();
                            gStationNameToShow = stations[next].name;
                            gStationNameTimer = GetTickCount();
                            gLog << "Pending: [" << stations[next].name << "]" << std::endl;
                            gLog.flush();
                        }
                    }
                }

                if (gPendingStation != -1 && gLastSwitchTick != 0) {
                    if (GetTickCount() - gLastSwitchTick >= SWITCH_DEBOUNCE_MS) {
                        int next = gPendingStation;
                        gPendingStation = -1;
                        gLastSwitchTick = 0;
                        {
                            std::lock_guard<std::mutex> lock(gLoadMutex);
                            gLoadedBuffer.clear();
                        }
                        gBufferReady = false;
                        gCurrentStation = next;
                        StartLoadingStation(next);
                        gLog << "Loading: [" << stations[next].name << "]" << std::endl;
                        gLog.flush();
                    }
                }

                if (!inVehicle && gWasInVehicle) {
                    OnPlayerExitVehicle(gActiveVehicle ? gActiveVehicle : pVehicle);
                    gActiveVehicle = nullptr;
                    StopRadio();
                    StopStaticSound();
                    gStationNameToShow = "";
                    gWasInVehicle = false;
                    gNoRadioVehicle = false;
                    gCurrentStation = -1;
                    gPendingStation = -1;
                    gLastSwitchTick = 0;
                    gWaitingToPlay = false;
                    gPendingScmStation.exchange(-1);
                    gPendingScmStationTime.exchange(-1);
                    gForcedSeekStation = -1;
                }

                // ===== Audio ducking: dim the radio under mission dialogue (opcode
                // 03D1, refreshed per line) and the mission-passed tune (opcode 0394),
                // like the original game. Both deadlines are set by the SCM hook. =====
                {
                    DWORD nowTick = GetTickCount();
                    bool duck = gStream && (gDialogueDuckUntil.load() > nowTick
                                            || gMissionPassedDuckUntil.load() > nowTick);
                    float target = duck ? RADIO_DUCK_LEVEL : 1.0f;
                    gDuckFactor += (target - gDuckFactor) * 0.18f; // smooth ramp in/out
                    if (gDuckFactor > 0.999f) gDuckFactor = 1.0f;
                    if (gDuckFactor < RADIO_DUCK_LEVEL) gDuckFactor = RADIO_DUCK_LEVEL;
                }

                UpdateVolume();
            });

        Events::drawHudEvent.Add([]()
            {
                // Re-pin the native vehicle radio to OFF right before the HUD draws.
                // A fast-scroll race can briefly set a real native station between our
                // per-frame cleanup passes; clamping here, just before the HUD renders,
                // stops the stock radio-name banner flashing top-center behind ours.
                if (gPlayerInVehicle && gActiveVehicle) {
                    BYTE* ns = (BYTE*)((BYTE*)gActiveVehicle + 0x23C);
                    if (*ns != 10)
                        *ns = 10;
                }

                if (gStationNameToShow.empty()) return;
                if (GetTickCount() - gStationNameTimer > STATION_NAME_DURATION) {
                    gStationNameToShow = "";
                    return;
                }

                float resW = (float)*pResWidth;
                float resH = (float)*pResHeight;

                // Scale relative to a 1920px-wide baseline, then bump ~10% so the
                // station name reads a touch larger. (scale = 1.10 at 1920px,
                // scaling proportionally at lower/higher resolutions.)
                float scale = (resW / 1920.0f) * 1.10f;

                wchar_t wname[256];
                AsciiToUnicode(gStationNameToShow.c_str(), wname);

                CFont::SetCentreOff();
                CFont::SetRightJustifyOff();
                CFont::SetJustifyOff();
                // Explicitly clear background state — if another HUD element
                // (weapon name, zone, etc.) left SetBackground on, it bleeds
                // into our render and draws an unwanted black box.
                CFont::SetBackgroundOff();
                CFont::SetBackGroundOnlyTextOff();

                CFont::SetFontStyle(FONT_HEADING);
                CFont::SetScale(scale, scale * 1.5f);
                CFont::SetProportional(true);

                // GetStringWidth reflects the current scale, so centering stays correct.
                float textWidth = CFont::GetStringWidth(wname, true);
                float centerX = (resW * 0.5f) - (textWidth * 0.5f);
                float posY = resH * 0.05f;

                // Shadow offset scales with font so it stays visually proportional.
                // To change text color: edit the CRGBA below.
                //   White  (VC default): CRGBA(255, 255, 255, 255)
                //   Silver/grey:         CRGBA(192, 192, 192, 255)
                //   Gold:                CRGBA(255, 200,  50, 255)
                CFont::SetColor(CRGBA(0, 0, 0, 200));
                CFont::PrintString(centerX + 2.0f * scale, posY + 2.0f * scale, wname);

                CFont::SetColor(CRGBA(255, 255, 255, 255));
                CFont::PrintString(centerX, posY, wname);
            });
    }
} noRadioPlugin;
