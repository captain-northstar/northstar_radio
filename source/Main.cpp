#include "plugin.h"
#include "CPad.h"
#include "CPlayerPed.h"
#include "CVehicle.h"
#include "cDMAudio.h"
#include "CMenuManager.h"
#include "CHud.h"
#include "CFont.h"
#include "CControllerConfigManager.h"
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
#include <windows.h>

using namespace plugin;

// Declared in Switch.cpp
extern bool gSwitchNext;
extern bool gSwitchPrev;

// Declared in RadioVehicles.cpp
int GetStationForVehicle(CVehicle* pVehicle);
void OnPlayerExitVehicle(CVehicle* pVehicle);

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
static bool gBassReady = false;
static bool gWasPaused = false;
static unsigned char gLastVolume = 0;
unsigned char* const pMusicVolume = (unsigned char*)0x86965A;
int gCurrentStation = -1;

// Global radio clock
double gRadioTime = 0.0;
static DWORD gLastTick = 0;

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
        BASS_ChannelSetAttribute(gSfxTuningStream, BASS_ATTRIB_VOL, (*pMusicVolume) / 64.0f);
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
        BASS_ChannelSetAttribute(gSfxStaticStream, BASS_ATTRIB_VOL, (*pMusicVolume) / 64.0f);
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

static bool IsADF(const std::string& path)
{
    std::string ext = path.size() >= 4 ? path.substr(path.size() - 4) : "";
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
    return ext == ".adf";
}

static void UpdateVolume()
{
    unsigned char vol = *pMusicVolume;
    if (vol == gLastVolume)
        return;
    gLastVolume = vol;
    if (gStream)
        BASS_ChannelSetAttribute(gStream, BASS_ATTRIB_VOL, vol / 64.0f);
    if (gSfxStaticStream)
        BASS_ChannelSetAttribute(gSfxStaticStream, BASS_ATTRIB_VOL, vol / 64.0f);
    if (gSfxTuningStream)
        BASS_ChannelSetAttribute(gSfxTuningStream, BASS_ATTRIB_VOL, vol / 64.0f);
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
            double seekMs = fmod(gRadioTime, totalMs);
            QWORD seekBytes = BASS_ChannelSeconds2Bytes(gStream, seekMs / 1000.0);
            BASS_ChannelSetPosition(gStream, seekBytes, BASS_POS_BYTE);
            gLog << "[" << stations[index].name << "] seek: " << (DWORD)seekMs << " / " << (DWORD)totalMs << " ms" << std::endl;
            gLog.flush();
        }
    }

    unsigned char vol = *pMusicVolume;
    gLastVolume = vol;
    BASS_ChannelSetAttribute(gStream, BASS_ATTRIB_VOL, vol / 64.0f);
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
        gLastTick = GetTickCount();
        gLog << "Radio time start: " << (DWORD)gRadioTime << " ms" << std::endl;
        gLog.flush();

        Events::initGameEvent.Add([]()
            {
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

                CPlayerPed* pPlayer = FindPlayerPed();
                if (!pPlayer)
                    return;

                bool isPaused = FrontEndMenuManager.m_bMenuActive;
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

                if (inVehicle && pVehicle->IsLawEnforcementVehicle()) {
                    if (!gPoliceRadioPlaying) {
                        StopRadio();
                        StopStaticSound();
                        std::string policePath = gGameFolder + "audio\\police.mp3";
                        gStream = BASS_StreamCreateFile(FALSE, policePath.c_str(), 0, 0, BASS_SAMPLE_LOOP);
                        if (gStream) {
                            unsigned char vol = *pMusicVolume;
                            gLastVolume = vol;
                            BASS_ChannelSetAttribute(gStream, BASS_ATTRIB_VOL, vol / 64.0f);
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
                }
                else {
                    gLastNativeStation = -1;
                }

                DWORD pedState = *(DWORD*)((BYTE*)pPlayer + 0x244);
                bool isSeated = (pedState == 0x32);

                if (isSeated && !gWasInVehicle) {
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
                    OnPlayerExitVehicle(pVehicle);
                    StopRadio();
                    StopStaticSound();
                    gStationNameToShow = "";
                    gWasInVehicle = false;
                    gCurrentStation = -1;
                    gPendingStation = -1;
                    gLastSwitchTick = 0;
                    gWaitingToPlay = false;
                }

                UpdateVolume();
            });

        Events::drawHudEvent.Add([]()
            {
                if (gStationNameToShow.empty()) return;
                if (GetTickCount() - gStationNameTimer > STATION_NAME_DURATION) {
                    gStationNameToShow = "";
                    return;
                }

                float resW = (float)*pResWidth;
                float resH = (float)*pResHeight;

                wchar_t wname[256];
                AsciiToUnicode(gStationNameToShow.c_str(), wname);

                CFont::SetCentreOff();
                CFont::SetRightJustifyOff();
                CFont::SetJustifyOff();

                CFont::SetFontStyle(FONT_HEADING);
                CFont::SetScale(1.0f, 1.5f);
                CFont::SetProportional(true);

                float textWidth = CFont::GetStringWidth(wname, true);
                float centerX = (resW * 0.5f) - (textWidth * 0.5f);
                float posY = resH * 0.05f;

                CFont::SetColor(CRGBA(0, 0, 0, 255));
                CFont::PrintString(centerX + 2.0f, posY + 2.0f, wname);

                CFont::SetColor(CRGBA(255, 255, 255, 255));
                CFont::PrintString(centerX, posY, wname);
            });
    }
} noRadioPlugin;
