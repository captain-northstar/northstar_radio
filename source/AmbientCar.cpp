#include "plugin.h"
#include "CPlayerPed.h"
#include "CPed.h"
#include "CVehicle.h"
#include "CVector.h"
#include "CMenuManager.h"
#include "CPools.h"
#include "CCamera.h"
#include "CCutsceneMgr.h"
#include "bass.h"
#include "bass_fx.h"
#include <string>
#include <vector>
#include <map>
#include <fstream>
#include <algorithm>
#include <cmath>
#include <thread>
#include <atomic>
#include <mutex>

using namespace plugin;

// Declared in Main.cpp
struct RadioStation {
    std::string name;
    std::string file;
};
extern std::vector<RadioStation> stations;
extern double gRadioTime;
extern std::string gGameFolder;
extern std::ofstream gLog;
extern bool gWasInVehicle;
static unsigned char* const pMusicVolume = (unsigned char*)0x86965A;

// Declared in RadioVehicles.cpp
extern std::map<int, std::vector<int>> gVehicleStationMap;
bool IsNoRadioVehicle(int modelId);

// Declared in Main.cpp — maps the 0..127 radio-volume slider to BASS 0.0..1.0.
float RadioVolume(unsigned int pref);
// Declared in Main.cpp — shared playback position; original stations start at 0,
// added stations use the randomized offset.
double StationTimelineMs(int index, bool applyStartOffset);

// [SETTINGS] AmbientRadio3D (defined/read in Main.cpp): pan + doppler drive-by
// simulation. When off, the old flat volume-only behaviour is used.
extern bool gAmbient3DEnabled;

// Ambient stream — only touched on main thread
static HSTREAM gAmbientStream = 0;
int gAmbientStation = -1;
CVehicle* gAmbientVehicle = nullptr;
static float gAmbientLastVol = -1.0f;
static DWORD gAmbientLastVolTick = 0;

// Identity of the vehicle the ambient stream belongs to, for the per-frame
// stale-pointer check: gAmbientVehicle is only safe to dereference after
// verifying the pool slot still holds this exact pointer with this model
// (vehicles despawn at any time; the pool recycles slots).
static int gAmbientVehicleIdx = -1;    // index into CPools::ms_pVehiclePool
static int gAmbientVehicleModel = -1;  // model id at the time we latched it

// 3D drive-by state (only used when gAmbient3DEnabled).
static float gAmbientBaseFreq = 0.0f;     // stream's natural sample rate
static bool  gAmbientUseTempoFreq = false;// tempo stream: BASS_ATTRIB_TEMPO_FREQ
static float gAmbientDoppler = 1.0f;      // smoothed doppler ratio
static float gAmbientPan = 0.0f;          // smoothed stereo pan [-1..1]

// The two muffle-EQ bands (800 Hz / 3.5 kHz). Live handles are main-thread only;
// the Ready pair is filled by the loader thread under gAmbientMutex and moved to
// live alongside the stream. Handles die with the stream (BASS_StreamFree), so
// they are just zeroed on stop. The gains are updated per frame with distance:
// brighter when the car is close, duller far away — this is also what makes the
// stereo pan audible (directional hearing needs mid/high frequencies, and the
// old fixed -18/-28 dB muffle stripped almost all of them).
static HFX gAmbientEqLow = 0, gAmbientEqHigh = 0;
static HFX gAmbientReadyEqLow = 0, gAmbientReadyEqHigh = 0;

// Background loading
// gAmbientBuf: protected by gAmbientMutex
// gAmbientReadyStream: fully prepared stream, handed to main thread
static std::mutex gAmbientMutex;
static std::vector<BYTE> gAmbientBuf;
static HSTREAM gAmbientReadyStream = 0;
static std::atomic<bool> gAmbientStreamReady(false);
static std::atomic<bool> gAmbientLoadingInProgress(false);
static int gAmbientLoadingStation = -1;

// Distance thresholds
static const float HEAR_DISTANCE = 30.0f;  // max range, volume = 0 here
static const float MIN_START_DISTANCE = 10.0f;  // volume = max here
static const float START_DISTANCE = 25.0f;  // stream only starts if player is beyond this

// 3D drive-by tunables
static const float AMBIENT_PAN_MAX = 0.95f;       // max stereo pan (1.0 = fully one ear)
static const float AMBIENT_PAN_RELAX = 4.0f;      // pan eases to centre only inside this distance
static const float AMBIENT_DOPPLER_FACTOR = 2.0f; // exaggeration; 1.0 = strict physics (too subtle at city speeds)
static const float AMBIENT_EQ_NEAR = 8.0f;        // at/below this distance: brightest (least muffled)
static const float AMBIENT_EQ_FAR = 25.0f;        // at/beyond this distance: fully muffled

static void StopAmbientStream()
{
    if (gAmbientStream) {
        BASS_ChannelStop(gAmbientStream);
        BASS_StreamFree(gAmbientStream);
        gAmbientStream = 0;
    }
    // Also free any ready-but-unplayed stream
    {
        std::lock_guard<std::mutex> lock(gAmbientMutex);
        if (gAmbientReadyStream) {
            BASS_StreamFree(gAmbientReadyStream);
            gAmbientReadyStream = 0;
        }
        gAmbientReadyEqLow = 0;
        gAmbientReadyEqHigh = 0;
        gAmbientBuf.clear();
    }
    gAmbientEqLow = 0;
    gAmbientEqHigh = 0;
    gAmbientStation = -1;
    gAmbientVehicle = nullptr;
    gAmbientVehicleIdx = -1;
    gAmbientVehicleModel = -1;
    gAmbientLastVol = -1.0f;
    gAmbientBaseFreq = 0.0f;
    gAmbientUseTempoFreq = false;
    gAmbientDoppler = 1.0f;
    gAmbientPan = 0.0f;
    gAmbientStreamReady = false;
}

static bool IsADF(const std::string& path)
{
    if (path.size() < 4) return false;
    std::string ext = path.substr(path.size() - 4);
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
    return ext == ".adf";
}

static float DistanceSq(const CVector& a, const CVector& b)
{
    float dx = a.x - b.x;
    float dy = a.y - b.y;
    float dz = a.z - b.z;
    return dx * dx + dy * dy + dz * dz;
}

static const float AMBIENT_MAX_VOL = 1.0f;

static float CalcVolume(float distance)
{
    if (distance >= HEAR_DISTANCE)      return 0.0f;
    if (distance <= MIN_START_DISTANCE) return AMBIENT_MAX_VOL;
    float range = HEAR_DISTANCE - MIN_START_DISTANCE;
    float t = (HEAR_DISTANCE - distance) / range;
    return t * t * AMBIENT_MAX_VOL;
}

// Everything heavy runs here — file I/O, decoding, stream creation, FX setup
static void AmbientLoadThreadImpl(int stationIndex)
{
    if (stationIndex < 0 || stationIndex >= (int)stations.size()) {
        gAmbientLoadingInProgress = false;
        return;
    }

    std::string fullPath = gGameFolder + stations[stationIndex].file;
    HSTREAM stream = 0;

    gLog << "Ambient: thread loading " << fullPath << std::endl;
    gLog.flush();

    if (IsADF(fullPath)) {
        std::ifstream f(fullPath, std::ios::binary);
        if (!f.is_open()) {
            gLog << "Ambient: cannot open " << fullPath << std::endl;
            gLog.flush();
            gAmbientLoadingInProgress = false;
            return;
        }

        // Pre-size and read in one shot rather than growing via istreambuf_iterator
        // (which briefly needs ~2x the file size). These ADFs run 100-180 MB in
        // Reviced; the spike could exhaust the 32-bit address space. bad_alloc here
        // is caught by the AmbientLoadThread wrapper so the game survives.
        std::vector<BYTE> buf;
        f.seekg(0, std::ios::end);
        std::streamoff len = f.tellg();
        f.seekg(0, std::ios::beg);
        if (len > 0) {
            buf.resize((size_t)len);
            f.read(reinterpret_cast<char*>(buf.data()), len);
            buf.resize((size_t)f.gcount());
        }
        f.close();

        for (size_t i = 0; i < buf.size(); i++)
            buf[i] ^= 0x22;

        stream = BASS_StreamCreateFile(TRUE, buf.data(), 0,
            (QWORD)buf.size(), BASS_STREAM_DECODE | BASS_SAMPLE_LOOP);

        if (!stream) {
            gLog << "Ambient: BASS error " << BASS_ErrorGetCode() << std::endl;
            gLog.flush();
            gAmbientLoadingInProgress = false;
            return;
        }

        // Keep buffer alive under lock
        std::lock_guard<std::mutex> lock(gAmbientMutex);
        gAmbientBuf = std::move(buf);
    }
    else {
        stream = BASS_StreamCreateFile(FALSE, fullPath.c_str(), 0, 0,
            BASS_STREAM_DECODE | BASS_SAMPLE_LOOP);

        if (!stream) {
            gLog << "Ambient: BASS error " << BASS_ErrorGetCode() << std::endl;
            gLog.flush();
            gAmbientLoadingInProgress = false;
            return;
        }
    }

    // Seek to gRadioTime
    QWORD totalBytes = BASS_ChannelGetLength(stream, BASS_POS_BYTE);
    double totalMs = BASS_ChannelBytes2Seconds(stream, totalBytes) * 1000.0;
    if (totalMs > 0.0) {
        double seekMs = fmod(StationTimelineMs(stationIndex, false), totalMs);
        QWORD seekBytes = BASS_ChannelSeconds2Bytes(stream, seekMs / 1000.0);
        BASS_ChannelSetPosition(stream, seekBytes, BASS_POS_BYTE);
    }

    // Wrap with BASS_FX for EQ support
    HSTREAM fxStream = BASS_FX_TempoCreate(stream, BASS_FX_FREESOURCE);

    // Apply low-pass EQ — muffled car sound. Zero-init matters: BASS_BFX_PEAKEQ's
    // first field is lBand (the band index), which the old code left uninitialized.
    // Starts at the "far" (fully muffled) gains; the per-frame 3D update re-shapes
    // both bands with distance once the stream is playing.
    BASS_BFX_PEAKEQ eq = {};
    eq.lBand = 0;
    eq.lChannel = BASS_BFX_CHANALL;
    eq.fBandwidth = 2.5f;
    eq.fQ = 0.0f;

    HFX hFx = BASS_ChannelSetFX(fxStream, BASS_FX_BFX_PEAKEQ, 1);
    eq.fCenter = 800.0f;
    eq.fGain = -18.0f;
    BASS_FXSetParameters(hFx, &eq);

    HFX hFx2 = BASS_ChannelSetFX(fxStream, BASS_FX_BFX_PEAKEQ, 2);
    eq.fCenter = 3500.0f;
    eq.fGain = -28.0f;
    BASS_FXSetParameters(hFx2, &eq);

    // Hand the ready stream (and its EQ handles) to the main thread
    {
        std::lock_guard<std::mutex> lock(gAmbientMutex);
        gAmbientReadyStream = fxStream;
        gAmbientReadyEqLow = hFx;
        gAmbientReadyEqHigh = hFx2;
    }

    gLog << "Ambient: ready [" << stations[stationIndex].name << "]" << std::endl;
    gLog.flush();

    gAmbientStreamReady = true;
    gAmbientLoadingInProgress = false;
}

// Detached background thread — same silent-crash hazard as the player-radio loader:
// an uncaught std::bad_alloc while decoding a large ADF into gAmbientBuf would
// std::terminate the game with no error dialog. Catch everything so a failed
// ambient load just gives up quietly and the game keeps running.
static void AmbientLoadThread(int stationIndex)
{
    try {
        AmbientLoadThreadImpl(stationIndex);
    }
    catch (const std::exception& e) {
        gLog << "Ambient: load aborted (" << e.what() << ") — skipped" << std::endl;
        gLog.flush();
        gAmbientStreamReady = false;
        gAmbientLoadingInProgress = false;
    }
    catch (...) {
        gAmbientStreamReady = false;
        gAmbientLoadingInProgress = false;
    }
}

static void StartAmbientLoad(int stationIndex, CVehicle* pVehicle, int poolIdx)
{
    if (gAmbientLoadingInProgress) return;

    // The candidate pointer comes from a pool scan up to 100ms old — verify the
    // pool slot still holds this exact vehicle BEFORE latching it (it may have
    // despawned; the old code got away with a stale pointer only because it
    // never dereferenced it, which the 3D update below now does every frame).
    auto* pool = CPools::ms_pVehiclePool;
    if (!pVehicle || !pool || poolIdx < 0 || poolIdx >= pool->m_nSize) {
        gLog << "Ambient: start rejected (no valid pool slot, idx " << poolIdx << ")" << std::endl;
        gLog.flush();
        return;
    }
    if (pool->IsFreeSlotAtIndex(poolIdx) || pool->GetAt(poolIdx) != pVehicle) {
        gLog << "Ambient: start rejected (vehicle despawned before latch)" << std::endl;
        gLog.flush();
        return;
    }

    StopAmbientStream();
    gAmbientLoadingStation = stationIndex;
    gAmbientVehicle = pVehicle;
    gAmbientVehicleIdx = poolIdx;
    gAmbientVehicleModel = pVehicle->m_nModelIndex;
    gAmbientLoadingInProgress = true;
    gAmbientStreamReady = false;

    gLog << "Ambient: loading [" << stations[stationIndex].name << "] for model "
         << gAmbientVehicleModel << std::endl;
    gLog.flush();

    std::thread(AmbientLoadThread, stationIndex).detach();
}

// The ONLY safe way to touch gAmbientVehicle: returns it only if its pool slot
// is still occupied by this exact pointer with the model we latched (otherwise
// the vehicle despawned / the slot was recycled — never dereference then).
static CVehicle* GetValidAmbientVehicle()
{
    if (!gAmbientVehicle || gAmbientVehicleIdx < 0) return nullptr;
    auto* pool = CPools::ms_pVehiclePool;
    if (!pool || gAmbientVehicleIdx >= pool->m_nSize) return nullptr;
    if (pool->IsFreeSlotAtIndex(gAmbientVehicleIdx)) return nullptr;
    CVehicle* v = pool->GetAt(gAmbientVehicleIdx);
    if (v != gAmbientVehicle) return nullptr;
    if (v->m_nModelIndex != gAmbientVehicleModel) return nullptr;
    return v;
}

// Per-frame spatial update for the playing ambient stream: live-position volume,
// stereo pan that follows the car relative to the CAMERA (so left/right matches
// what the player sees), and a doppler pitch shift from the radial closing speed
// — the classic pass-by sweep. Falls back to the old flat volume-only update
// when AmbientRadio3D = 0. Stops the stream if the vehicle despawned.
static void UpdateAmbientSpatial(const CVector& playerPos, float gameVol)
{
    CVehicle* v = GetValidAmbientVehicle();
    if (!v) {
        StopAmbientStream();
        return;
    }

    CVector vehPos = v->GetPosition();
    float distance = sqrtf(DistanceSq(playerPos, vehPos));
    float vol = CalcVolume(distance) * gameVol;

    if (!gAmbient3DEnabled) {
        // Legacy behaviour: volume only, throttled to 100ms.
        DWORD now = GetTickCount();
        if (now - gAmbientLastVolTick >= 100) {
            gAmbientLastVolTick = now;
            if (fabsf(vol - gAmbientLastVol) > 0.005f) {
                BASS_ChannelSetAttribute(gAmbientStream, BASS_ATTRIB_VOL, vol);
                gAmbientLastVol = vol;
            }
        }
        return;
    }

    // Volume every frame from the live position — a smooth fade as it drives by.
    if (fabsf(vol - gAmbientLastVol) > 0.002f) {
        BASS_ChannelSetAttribute(gAmbientStream, BASS_ATTRIB_VOL, vol);
        gAmbientLastVol = vol;
    }

    // --- Stereo pan, from the camera's view direction ---
    // Horizontal camera-right = at x world-up(0,0,1) = (at.y, -at.x, 0); pan is
    // the car direction projected onto it, at nearly full strength (a car passing
    // on your right IS nearly all in your right ear). Relaxed to centre only when
    // it is practically on top of us.
    const CVector& camPos = TheCamera.m_CameraMatrix.pos;
    const CVector& camAt = TheCamera.m_CameraMatrix.at;
    float rx = camAt.y, ry = -camAt.x;
    float rlen = sqrtf(rx * rx + ry * ry);
    float dx = vehPos.x - camPos.x, dy = vehPos.y - camPos.y;
    float dlen = sqrtf(dx * dx + dy * dy);
    float targetPan = 0.0f;
    if (rlen > 0.05f && dlen > 1.0f) {
        targetPan = ((dx * rx + dy * ry) / (rlen * dlen)) * AMBIENT_PAN_MAX;
        if (distance < AMBIENT_PAN_RELAX)
            targetPan *= distance / AMBIENT_PAN_RELAX;
    }
    gAmbientPan += (targetPan - gAmbientPan) * 0.25f; // smooth camera cuts/jitter
    BASS_ChannelSetAttribute(gAmbientStream, BASS_ATTRIB_PAN, gAmbientPan);

    // --- Distance + direction EQ ---
    // The muffle opens up as the car approaches (a distant radio is nearly all
    // bass; up close you hear the actual music). Restoring the mid/high content
    // near the player is also what makes the pan localizable — you cannot tell
    // where a subwoofer is. On top of that, cars BEHIND the camera get an extra
    // treble dip (up to -8 dB), the standard two-speaker front/rear cue.
    if (gAmbientEqLow && gAmbientEqHigh) {
        float t = (distance - AMBIENT_EQ_NEAR) / (AMBIENT_EQ_FAR - AMBIENT_EQ_NEAR);
        if (t < 0.0f) t = 0.0f;
        if (t > 1.0f) t = 1.0f;
        float gainLow = -6.0f - 12.0f * t;    // 800 Hz:   -6 dB near .. -18 dB far
        float gainHigh = -10.0f - 18.0f * t;  // 3.5 kHz: -10 dB near .. -28 dB far
        if (dlen > 1.0f) {
            float flen = sqrtf(camAt.x * camAt.x + camAt.y * camAt.y);
            if (flen > 0.05f) {
                float facing = (dx * camAt.x + dy * camAt.y) / (dlen * flen); // 1 = ahead, -1 = behind
                if (facing < 0.0f)
                    gainHigh += 8.0f * facing; // up to -8 dB extra when behind the camera
            }
        }
        BASS_BFX_PEAKEQ eq = {};
        eq.lBand = 0;
        eq.lChannel = BASS_BFX_CHANALL;
        eq.fBandwidth = 2.5f;
        eq.fQ = 0.0f;
        eq.fCenter = 800.0f;
        eq.fGain = gainLow;
        BASS_FXSetParameters(gAmbientEqLow, &eq);
        eq.fCenter = 3500.0f;
        eq.fGain = gainHigh;
        BASS_FXSetParameters(gAmbientEqHigh, &eq);
    }

    // --- Doppler ---
    // f = f0 * (c + listener closing speed) / (c - source closing speed), using
    // horizontal radial components. GTA move speed is units(=m) per physics frame
    // at 50 fps, so x50 for m/s. The closing speeds are exaggerated by
    // AMBIENT_DOPPLER_FACTOR — strict physics at city speeds is only a +-4-5%
    // shift, too subtle to register; films and games all cheat this up. Applied
    // via the tempo stream's sample rate (pitch AND playback rate shift together
    // — physically what doppler is).
    if (gAmbientBaseFreq > 0.0f && dlen > 1.0f) {
        float ux = -dx / dlen, uy = -dy / dlen; // unit vector: car -> listener
        CVector vv = v->m_vecMoveSpeed;
        CPlayerPed* pp = FindPlayerPed();
        float pvx = pp ? pp->m_vecMoveSpeed.x : 0.0f;
        float pvy = pp ? pp->m_vecMoveSpeed.y : 0.0f;
        const float C = 343.0f;                          // speed of sound, m/s
        float vSrc = (vv.x * ux + vv.y * uy) * 50.0f * AMBIENT_DOPPLER_FACTOR;
        float vLis = -(pvx * ux + pvy * uy) * 50.0f * AMBIENT_DOPPLER_FACTOR;
        if (vSrc > 300.0f) vSrc = 300.0f;                // keep denominator sane
        float ratio = (C + vLis) / (C - vSrc);
        if (ratio < 0.78f) ratio = 0.78f;                // sanity clamp
        if (ratio > 1.28f) ratio = 1.28f;
        gAmbientDoppler += (ratio - gAmbientDoppler) * 0.15f; // smooth ramp
        BASS_ChannelSetAttribute(gAmbientStream,
            gAmbientUseTempoFreq ? BASS_ATTRIB_TEMPO_FREQ : BASS_ATTRIB_FREQ,
            gAmbientBaseFreq * gAmbientDoppler);
    }
}

extern bool gAmbientRadioEnabled;

class AmbientCarPlugin
{
public:
    AmbientCarPlugin()
    {
        Events::gameProcessEvent.Add([]()
            {
                // One-shot config echo so the log shows the ambient settings in effect.
                static bool sLoggedCfg = false;
                if (!sLoggedCfg) {
                    sLoggedCfg = true;
                    gLog << "Ambient: config AmbientRadio=" << (gAmbientRadioEnabled ? 1 : 0)
                         << " AmbientRadio3D=" << (gAmbient3DEnabled ? 1 : 0) << std::endl;
                    gLog.flush();
                }

                if (!gAmbientRadioEnabled) return;
                // Only run when player is on foot
                if (gWasInVehicle) {
                    StopAmbientStream();
                    return;
                }

                CPlayerPed* pPlayer = FindPlayerPed();
                if (!pPlayer) {
                    StopAmbientStream();
                    return;
                }

                // Silence the ambient radio in the pause menu AND during cutscenes /
                // scripted scenes — same condition the main radio pauses on
                // (ms_running = theatrical cutscenes, widescreen bars = scripted
                // camera scenes). Returning here also stops new ambient broadcasts
                // from STARTING mid-scene; on resume the per-frame pool validation
                // cleans up if the car was despawned or moved by the script.
                bool inCutscene = CCutsceneMgr::ms_running || TheCamera.m_bWideScreenOn;
                if (FrontEndMenuManager.m_bMenuActive || inCutscene) {
                    if (gAmbientStream) BASS_ChannelPause(gAmbientStream);
                    return;
                }
                else if (gAmbientStream) {
                    BASS_ChannelPlay(gAmbientStream, FALSE);
                }

                CVector playerPos = pPlayer->GetPosition();

                // Scan vehicle pool at most every 100ms
                static CVehicle* sCachedVehicle = nullptr;
                static int sCachedVehicleIdx = -1;   // pool slot of sCachedVehicle
                static int sCachedStation = -1;
                static float sCachedDistSq = 9999.0f;
                static DWORD sScanTick = 0;

                DWORD nowScan = GetTickCount();
                if (nowScan - sScanTick >= 100) {
                    sScanTick = nowScan;

                    CVehicle* newVehicle = nullptr;
                    int newVehicleIdx = -1;
                    int newStation = -1;
                    float newDistSq = HEAR_DISTANCE * HEAR_DISTANCE;

                    for (int i = 0; i < CPools::ms_pVehiclePool->m_nSize; i++) {
                        if (CPools::ms_pVehiclePool->IsFreeSlotAtIndex(i)) continue;
                        CVehicle* pVeh = CPools::ms_pVehiclePool->GetAt(i);
                        if (!pVeh) continue;

                        int modelId = pVeh->m_nModelIndex;
                        if (IsNoRadioVehicle(modelId)) continue;  // [NORADIO]: no ambient either
                        auto it = gVehicleStationMap.find(modelId);
                        if (it == gVehicleStationMap.end()) continue;

                        // Only play if someone is in the car
                        if (!pVeh->m_pDriver) continue;

                        CVector vehPos = pVeh->GetPosition();
                        float distSq = DistanceSq(playerPos, vehPos);

                        if (distSq < newDistSq) {
                            newDistSq = distSq;
                            newVehicle = pVeh;
                            newVehicleIdx = i;
                            if (newVehicle != sCachedVehicle) {
                                const std::vector<int>& options = it->second;
                                newStation = options[rand() % options.size()];
                            }
                            else {
                                newStation = sCachedStation;
                            }
                        }
                    }

                    sCachedDistSq = newDistSq;
                    sCachedVehicleIdx = newVehicleIdx;
                    if (newVehicle != sCachedVehicle) {
                        sCachedVehicle = newVehicle;
                        sCachedStation = newStation;
                    }

                }

                CVehicle* closestVehicle = sCachedVehicle;
                int closestVehicleIdx = sCachedVehicleIdx;
                int closestStation = sCachedStation;
                float closestDistSq = sCachedDistSq;

                if (!closestVehicle) {
                    StopAmbientStream();
                    return;
                }

                float distance = sqrtf(closestDistSq);
                float volume = CalcVolume(distance);
                float gameVol = RadioVolume(*pMusicVolume);

                // Stream fully prepared in background — just play it
                if (gAmbientStreamReady && !gAmbientStream) {
                    gAmbientStreamReady = false;
                    {
                        std::lock_guard<std::mutex> lock(gAmbientMutex);
                        gAmbientStream = gAmbientReadyStream;
                        gAmbientReadyStream = 0;
                        gAmbientEqLow = gAmbientReadyEqLow;
                        gAmbientEqHigh = gAmbientReadyEqHigh;
                        gAmbientReadyEqLow = 0;
                        gAmbientReadyEqHigh = 0;
                    }
                    gAmbientStation = gAmbientLoadingStation;
                    float vol = volume * gameVol;
                    BASS_ChannelSetAttribute(gAmbientStream, BASS_ATTRIB_VOL, vol);
                    // 3D drive-by: capture the stream's natural sample rate as the
                    // doppler baseline. Ours is a bass_fx tempo stream, so prefer
                    // BASS_ATTRIB_TEMPO_FREQ (rate+pitch together); fall back to the
                    // plain sample-rate attribute for any non-tempo stream.
                    gAmbientBaseFreq = 0.0f;
                    gAmbientUseTempoFreq = true;
                    if (!BASS_ChannelGetAttribute(gAmbientStream, BASS_ATTRIB_TEMPO_FREQ, &gAmbientBaseFreq)
                        || gAmbientBaseFreq <= 0.0f) {
                        gAmbientUseTempoFreq = false;
                        if (!BASS_ChannelGetAttribute(gAmbientStream, BASS_ATTRIB_FREQ, &gAmbientBaseFreq))
                            gAmbientBaseFreq = 0.0f;
                    }
                    gAmbientDoppler = 1.0f;
                    gAmbientPan = 0.0f;
                    BASS_ChannelPlay(gAmbientStream, FALSE);
                    gAmbientLastVol = vol;
                    gLog << "Ambient: playing [" << stations[gAmbientStation].name << "]"
                         << (gAmbient3DEnabled ? " (3D pan+doppler)" : "") << std::endl;
                    gLog.flush();
                    return;
                }

                // Stream playing — per-frame spatial update (validated against the
                // pool), or reload if a different vehicle/station is now closest
                if (gAmbientStream) {
                    if (closestVehicle != gAmbientVehicle || closestStation != gAmbientStation) {
                        if (distance >= START_DISTANCE && !gAmbientLoadingInProgress)
                            StartAmbientLoad(closestStation, closestVehicle, closestVehicleIdx);
                    }
                    else {
                        UpdateAmbientSpatial(playerPos, gameVol);
                    }
                    return;
                }

                // Nothing playing, nothing loading — only start if player is far enough
                if (!gAmbientLoadingInProgress && distance >= START_DISTANCE)
                    StartAmbientLoad(closestStation, closestVehicle, closestVehicleIdx);
            });
    }
} ambientCarPlugin;
