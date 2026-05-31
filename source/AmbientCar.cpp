#include "plugin.h"
#include "CPlayerPed.h"
#include "CPed.h"
#include "CVehicle.h"
#include "CVector.h"
#include "CMenuManager.h"
#include "CPools.h"
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

// Ambient stream — only touched on main thread
static HSTREAM gAmbientStream = 0;
int gAmbientStation = -1;
CVehicle* gAmbientVehicle = nullptr;
static float gAmbientLastVol = -1.0f;
static DWORD gAmbientLastVolTick = 0;

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
        gAmbientBuf.clear();
    }
    gAmbientStation = -1;
    gAmbientVehicle = nullptr;
    gAmbientLastVol = -1.0f;
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
static void AmbientLoadThread(int stationIndex)
{
    if (stationIndex < 0 || stationIndex >= (int)stations.size()) {
        gAmbientLoadingInProgress = false;
        return;
    }

    std::string fullPath = gGameFolder + stations[stationIndex].file;
    HSTREAM stream = 0;

    if (IsADF(fullPath)) {
        std::ifstream f(fullPath, std::ios::binary);
        if (!f.is_open()) {
            gLog << "Ambient: cannot open " << fullPath << std::endl;
            gLog.flush();
            gAmbientLoadingInProgress = false;
            return;
        }

        std::vector<BYTE> buf;
        buf.assign(
            std::istreambuf_iterator<char>(f),
            std::istreambuf_iterator<char>()
        );
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
        double seekMs = fmod(gRadioTime, totalMs);
        QWORD seekBytes = BASS_ChannelSeconds2Bytes(stream, seekMs / 1000.0);
        BASS_ChannelSetPosition(stream, seekBytes, BASS_POS_BYTE);
    }

    // Wrap with BASS_FX for EQ support
    HSTREAM fxStream = BASS_FX_TempoCreate(stream, BASS_FX_FREESOURCE);

    // Apply low-pass EQ — muffled car sound
    BASS_BFX_PEAKEQ eq;
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

    // Hand the ready stream to the main thread
    {
        std::lock_guard<std::mutex> lock(gAmbientMutex);
        gAmbientReadyStream = fxStream;
    }

    gLog << "Ambient: ready [" << stations[stationIndex].name << "]" << std::endl;
    gLog.flush();

    gAmbientStreamReady = true;
    gAmbientLoadingInProgress = false;
}

static void StartAmbientLoad(int stationIndex, CVehicle* pVehicle)
{
    if (gAmbientLoadingInProgress) return;

    StopAmbientStream();
    gAmbientLoadingStation = stationIndex;
    gAmbientVehicle = pVehicle;
    gAmbientLoadingInProgress = true;
    gAmbientStreamReady = false;

    std::thread(AmbientLoadThread, stationIndex).detach();
}

extern bool gAmbientRadioEnabled;

class AmbientCarPlugin
{
public:
    AmbientCarPlugin()
    {
        Events::gameProcessEvent.Add([]()
            {
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

                if (FrontEndMenuManager.m_bMenuActive) {
                    if (gAmbientStream) BASS_ChannelPause(gAmbientStream);
                    return;
                }
                else if (gAmbientStream) {
                    BASS_ChannelPlay(gAmbientStream, FALSE);
                }

                CVector playerPos = pPlayer->GetPosition();

                // Scan vehicle pool at most every 100ms
                static CVehicle* sCachedVehicle = nullptr;
                static int sCachedStation = -1;
                static float sCachedDistSq = 9999.0f;
                static DWORD sScanTick = 0;

                DWORD nowScan = GetTickCount();
                if (nowScan - sScanTick >= 100) {
                    sScanTick = nowScan;

                    CVehicle* newVehicle = nullptr;
                    int newStation = -1;
                    float newDistSq = HEAR_DISTANCE * HEAR_DISTANCE;

                    for (int i = 0; i < CPools::ms_pVehiclePool->m_nSize; i++) {
                        if (CPools::ms_pVehiclePool->IsFreeSlotAtIndex(i)) continue;
                        CVehicle* pVeh = CPools::ms_pVehiclePool->GetAt(i);
                        if (!pVeh) continue;

                        int modelId = pVeh->m_nModelIndex;
                        auto it = gVehicleStationMap.find(modelId);
                        if (it == gVehicleStationMap.end()) continue;

                        // Only play if someone is in the car
                        if (!pVeh->m_pDriver) continue;

                        CVector vehPos = pVeh->GetPosition();
                        float distSq = DistanceSq(playerPos, vehPos);

                        if (distSq < newDistSq) {
                            newDistSq = distSq;
                            newVehicle = pVeh;
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
                    if (newVehicle != sCachedVehicle) {
                        sCachedVehicle = newVehicle;
                        sCachedStation = newStation;
                    }
                }

                CVehicle* closestVehicle = sCachedVehicle;
                int closestStation = sCachedStation;
                float closestDistSq = sCachedDistSq;

                if (!closestVehicle) {
                    StopAmbientStream();
                    return;
                }

                float distance = sqrtf(closestDistSq);
                float volume = CalcVolume(distance);
                float gameVol = (*pMusicVolume) / 64.0f;

                // Stream fully prepared in background — just play it
                if (gAmbientStreamReady && !gAmbientStream) {
                    gAmbientStreamReady = false;
                    {
                        std::lock_guard<std::mutex> lock(gAmbientMutex);
                        gAmbientStream = gAmbientReadyStream;
                        gAmbientReadyStream = 0;
                    }
                    gAmbientStation = gAmbientLoadingStation;
                    float vol = volume * gameVol;
                    BASS_ChannelSetAttribute(gAmbientStream, BASS_ATTRIB_VOL, vol);
                    BASS_ChannelPlay(gAmbientStream, FALSE);
                    gAmbientLastVol = vol;
                    gLog << "Ambient: playing [" << stations[gAmbientStation].name << "]" << std::endl;
                    gLog.flush();
                    return;
                }

                // Stream playing — update volume or reload if vehicle changed
                if (gAmbientStream) {
                    if (closestVehicle != gAmbientVehicle || closestStation != gAmbientStation) {
                        if (distance >= START_DISTANCE && !gAmbientLoadingInProgress)
                            StartAmbientLoad(closestStation, closestVehicle);
                    }
                    else {
                        DWORD now = GetTickCount();
                        if (now - gAmbientLastVolTick >= 100) {
                            gAmbientLastVolTick = now;
                            float newVol = volume * gameVol;
                            if (fabsf(newVol - gAmbientLastVol) > 0.005f) {
                                BASS_ChannelSetAttribute(gAmbientStream, BASS_ATTRIB_VOL, newVol);
                                gAmbientLastVol = newVol;
                            }
                        }
                    }
                    return;
                }

                // Nothing playing, nothing loading — only start if player is far enough
                if (!gAmbientLoadingInProgress && distance >= START_DISTANCE)
                    StartAmbientLoad(closestStation, closestVehicle);
            });
    }
} ambientCarPlugin;
