#include "plugin.h"
#include "CPlayerPed.h"
#include "CVehicle.h"
#include <fstream>
#include <string>
#include <vector>
#include <map>
#include <algorithm>
#include <windows.h>

using namespace plugin;

// Declared in Main.cpp
struct RadioStation {
    std::string name;
    std::string file;
};
extern std::vector<RadioStation> stations;
extern int gCurrentStation;
extern int gPendingStation;
extern std::string gScriptsFolder;
extern std::ofstream gLog;

// Timer: after leaving a car, keep same station for 5 seconds in any new car
static DWORD gLastExitTick = 0;
static int gLastStation = -1;
static const DWORD KEEP_STATION_MS = 5000;

// Per-vehicle saved station — keyed by vehicle pointer (unique per instance)
static std::map<CVehicle*, int> gVehicleSavedStation;

// Vehicle model ID -> list of station indices (pick randomly if multiple)
std::map<int, std::vector<int>> gVehicleStationMap;

static std::string ToLower(const std::string& s)
{
    std::string out = s;
    std::transform(out.begin(), out.end(), out.begin(), ::tolower);
    return out;
}

static void LoadVehicleSection()
{
    std::string iniPath = gScriptsFolder + "NorthstarRadio.ini";
    std::ifstream ini(iniPath);
    if (!ini.is_open()) {
        gLog << "RadioVehicles: cannot open NorthstarRadio.ini" << std::endl;
        gLog.flush();
        return;
    }

    auto trim = [](std::string& s) {
        size_t start = s.find_first_not_of(" \t\r\n");
        size_t end = s.find_last_not_of(" \t\r\n");
        s = (start == std::string::npos) ? "" : s.substr(start, end - start + 1);
        };

    bool inVehicleSection = false;
    int mapped = 0;
    std::string line;

    while (std::getline(ini, line))
    {
        // Strip inline comments
        size_t comment = line.find('#');
        if (comment != std::string::npos)
            line = line.substr(0, comment);

        trim(line);
        if (line.empty())
            continue;

        if (!line.empty() && line[0] == '[') {
            std::string header = line;
            std::transform(header.begin(), header.end(), header.begin(), ::tolower);
            inVehicleSection = (header == "[vehicles]");
            continue;
        }

        if (!inVehicleSection)
            continue;

        size_t sep = line.find('|');
        if (sep == std::string::npos)
            continue;

        std::string modelStr = line.substr(0, sep);
        std::string stationName = line.substr(sep + 1);
        trim(modelStr);
        trim(stationName);

        if (modelStr.empty() || stationName.empty())
            continue;

        int modelId = atoi(modelStr.c_str());
        if (modelId <= 0)
            continue;

        std::string nameLower = ToLower(stationName);
        int stationIndex = -1;
        for (int i = 0; i < (int)stations.size(); i++) {
            if (ToLower(stations[i].name) == nameLower) {
                stationIndex = i;
                break;
            }
        }

        if (stationIndex == -1) {
            gLog << "RadioVehicles: station [" << stationName << "] not found for model " << modelId << std::endl;
        }
        else {
            gVehicleStationMap[modelId].push_back(stationIndex);
            gLog << "RadioVehicles: model " << modelId << " -> [" << stations[stationIndex].name << "]" << std::endl;
            mapped++;
        }
    }

    gLog << "RadioVehicles: " << mapped << " entries mapped" << std::endl;
    gLog.flush();
}

int GetStationForVehicle(CVehicle* pVehicle)
{
    // If this specific car has a saved station, resume it
    auto saved = gVehicleSavedStation.find(pVehicle);
    if (saved != gVehicleSavedStation.end()) {
        int station = saved->second;
        gLog << "RadioVehicles: resuming saved station [" << stations[station].name << "] for this vehicle" << std::endl;
        gLog.flush();
        return station;
    }

    // If within 5 seconds of leaving any car, keep the same station
    if (gLastStation != -1 && gLastExitTick != 0) {
        if (GetTickCount() - gLastExitTick < KEEP_STATION_MS) {
            gLog << "RadioVehicles: keeping last station [" << stations[gLastStation].name << "] (within 5s)" << std::endl;
            gLog.flush();
            return gLastStation;
        }
    }

    // Look up vehicle model
    int modelId = pVehicle->m_nModelIndex;
    auto it = gVehicleStationMap.find(modelId);
    if (it != gVehicleStationMap.end()) {
        const std::vector<int>& options = it->second;

        // If ambient was already playing a station for this vehicle, use that
        // to stay in sync with what the player heard outside
        extern CVehicle* gAmbientVehicle;
        extern int gAmbientStation;
        if (gAmbientVehicle == pVehicle && gAmbientStation >= 0) {
            gLog << "RadioVehicles: syncing to ambient station [" << stations[gAmbientStation].name << "]" << std::endl;
            gLog.flush();
            return gAmbientStation;
        }

        int pick = options[rand() % options.size()];
        gLog << "RadioVehicles: model " << modelId << " -> [" << stations[pick].name << "]" << std::endl;
        gLog.flush();
        return pick;
    }

    // Not in map — pick a random station from INI only (excludes MP3 PLAYER)
    if (!stations.empty()) {
        int count = (int)stations.size();
        if (count > 0 && stations[count - 1].name == "MP3 PLAYER") count--;
        int pick = rand() % count;
        gLog << "RadioVehicles: model " << modelId << " not mapped, random -> [" << stations[pick].name << "]" << std::endl;
        gLog.flush();
        return pick;
    }

    return 0;
}

void OnPlayerExitVehicle(CVehicle* pVehicle)
{
    // Save the current station to this specific vehicle instance
    int station = gCurrentStation != -1 ? gCurrentStation : gPendingStation;
    if (station >= 0 && station < (int)stations.size()) {
        gVehicleSavedStation[pVehicle] = station;
        gLog << "RadioVehicles: saved [" << stations[station].name << "] to vehicle instance" << std::endl;
    }

    // Also update the short-term timer for entering new cars
    gLastStation = station;
    gLastExitTick = GetTickCount();
    if (station >= 0 && station < (int)stations.size())
        gLog << "RadioVehicles: player exited, saving [" << stations[station].name << "] for 5s" << std::endl;
    gLog.flush();
}

class RadioVehiclesPlugin
{
public:
    RadioVehiclesPlugin()
    {
        Events::initGameEvent.Add([]()
            {
                LoadVehicleSection();
            });
    }
} radioVehiclesPlugin;
