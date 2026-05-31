#include "plugin.h"
#include "CPad.h"
#include <fstream>
#include <string>
#include <windows.h>
#include <Xinput.h>
#pragma comment(lib, "Xinput.lib")

using namespace plugin;

// Shared variables — read by Main.cpp
bool gSwitchNext = false;
bool gSwitchPrev = false;

// Raw mouse wheel state bytes from the game's own input system
static uint8_t* const pMouseWheelUp = (uint8_t*)0x93690B;
static uint8_t* const pMouseWheelDown = (uint8_t*)0x93690C;

// Configurable keys
static int gRadioSwitchNextKey = 82;  // keyboard: R by default
static int gRadioSwitchNextPad = 0;   // controller: 0 = disabled

static void LoadControlsFromINI()
{
    char path[MAX_PATH];
    HMODULE hModule = NULL;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
        GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        (LPCSTR)&LoadControlsFromINI, &hModule);
    GetModuleFileNameA(hModule, path, MAX_PATH);
    std::string scriptsFolder = std::string(path);
    scriptsFolder = scriptsFolder.substr(0, scriptsFolder.find_last_of("\\/") + 1);

    std::string iniPath = scriptsFolder + "NorthstarRadio.ini";
    std::ifstream ini(iniPath, std::ios::binary);
    if (!ini.is_open())
        return;

    // Handle BOM
    unsigned char bom[3] = {};
    ini.read((char*)bom, 3);
    if (!(bom[0] == 0xEF && bom[1] == 0xBB && bom[2] == 0xBF))
        ini.seekg(0);

    auto trim = [](std::string& s) {
        size_t start = s.find_first_not_of(" \t\r\n");
        size_t end = s.find_last_not_of(" \t\r\n");
        s = (start == std::string::npos) ? "" : s.substr(start, end - start + 1);
        };

    bool inControlsSection = false;
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
            inControlsSection = (header == "[controls]");
            continue;
        }

        if (!inControlsSection)
            continue;

        size_t sep = line.find('=');
        if (sep == std::string::npos)
            continue;

        std::string key = line.substr(0, sep);
        std::string val = line.substr(sep + 1);
        trim(key);
        trim(val);
        std::transform(key.begin(), key.end(), key.begin(), ::tolower);

        if (key == "radioswitchnext" && !val.empty())
            gRadioSwitchNextKey = atoi(val.c_str());
        else if (key == "radioswitchnextpad" && !val.empty())
            gRadioSwitchNextPad = atoi(val.c_str());
    }
}

// Block game's radio switch
static bool __fastcall Hook_ChangeStationJustDown(CPad* self, void* edx)
{
    return false;
}

class SwitchDetectorPlugin
{
public:
    SwitchDetectorPlugin()
    {
        injector::MakeJMP(0x4AA590, (void*)Hook_ChangeStationJustDown, true);

        Events::initGameEvent.Add([]()
            {
                LoadControlsFromINI();
            });

        Events::gameProcessEvent.Add([]()
            {
                if (*pMouseWheelUp)
                    gSwitchNext = true;

                if (*pMouseWheelDown)
                    gSwitchPrev = true;

                // Keyboard key — fires once per press
                static bool gKeyWasDown = false;
                bool keyDown = (GetAsyncKeyState(gRadioSwitchNextKey) & 0x8000) != 0;
                if (keyDown && !gKeyWasDown)
                    gSwitchNext = true;
                gKeyWasDown = keyDown;

                // Controller button via XInput — checks all 4 ports
                if (gRadioSwitchNextPad != 0) {
                    static bool gPadWasDown = false;
                    bool padDown = false;
                    for (DWORD i = 0; i < XUSER_MAX_COUNT; i++) {
                        XINPUT_STATE state = {};
                        if (XInputGetState(i, &state) == ERROR_SUCCESS) {
                            if (state.Gamepad.wButtons & gRadioSwitchNextPad) {
                                padDown = true;
                                break;
                            }
                        }
                    }
                    if (padDown && !gPadWasDown)
                        gSwitchNext = true;
                    gPadWasDown = padDown;
                }
            });
    }
} switchDetectorPlugin;
