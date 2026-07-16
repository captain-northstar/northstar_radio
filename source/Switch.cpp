#include "plugin.h"
#include "CPad.h"
#include "cDMAudio.h"
#include "CRunningScript.h"
#include "CTheScripts.h"
#include "CGame.h"
#include "safetyhook.hpp"
#include <fstream>
#include <string>
#include <atomic>
#include <windows.h>
#include <Xinput.h>
#pragma comment(lib, "Xinput.lib")

using namespace plugin;

// Defined in Main.cpp — shared debug log.
extern std::ofstream gLog;

// Shared variables — read by Main.cpp
bool gSwitchNext = false;
bool gSwitchPrev = false;

// Set true when the radio-switch button (keyboard or pad) is HELD ~2.5s — Main.cpp
// then turns the radio off (the same "Radio Off" state as scrolling past the last
// station). A quick tap of the same button still just changes station.
bool gRadioOff = false;

// Set by the SCM opcode hook below when the game's script fires opcode
// 057D PLAY_ANNOUNCEMENT. -1 = nothing pending; 0 = bclosed, 1 = bopen.
// Read (and reset) by Main.cpp on the main thread. The hook runs on the same
// (main) thread during script processing, so this is never contended, but
// std::atomic keeps the access well-defined.
std::atomic<int> gPendingAnnouncement{ -1 };

// Set by the same hook when main.scm fires opcode 041E SET_RADIO_CHANNEL
// (missions that force a specific car-radio station). Holds the requested
// station index; -1 = nothing pending. Applied to the BASS radio by Main.cpp.
std::atomic<int> gPendingScmStation{ -1 };

// The second parameter of 041E: the requested playback position in ms, or -1
// meaning "continue" (use our synced clock). Main.cpp honors this only for the
// original VC stations (indices 0..8); added stations always use the clock.
std::atomic<int> gPendingScmStationTime{ -1 };

// GetTickCount() deadlines until which Main.cpp ducks (dims) the radio, matching
// the original game's audio ducking. gMissionPassedDuckUntil is set by the
// "mission passed" tune (opcode 0394); gDialogueDuckUntil is refreshed by each
// mission dialogue line (opcode 03D1 PLAY_MISSION_AUDIO).
std::atomic<DWORD> gMissionPassedDuckUntil{ 0 };
std::atomic<DWORD> gDialogueDuckUntil{ 0 };

// CPad::NewMouseControllerState.wheelUp / .wheelDown  (current-frame mouse state).
// The vehicle audio code reads these directly to detect scroll-wheel radio changes.
// OldMouseControllerState (0x93690B/C) is the previous-frame copy — that is what
// was being cleared before, which had no effect on the current frame's input.
static uint8_t* const pMouseWheelUp   = (uint8_t*)0x94D78B;
static uint8_t* const pMouseWheelDown = (uint8_t*)0x94D78C;

// Configurable keys
static int gRadioSwitchNextKey = 82;  // keyboard: R by default
static int gRadioSwitchNextPad = 0;   // controller: 0 = disabled

// [SETTINGS] ScriptIntegration — when false, the SCM opcode mid-hook is NOT
// installed, so the plugin never touches the game's script engine: no story
// announcements, no mission radio-station changes, and no audio ducking. The
// core radio (stations, vehicle assignment, ambient, suppression, volume) is
// unaffected. This exists for total-conversion mods whose custom main.scm has
// different opcode / mission-audio behavior and crashes under our interception.
// Default true (no behavior change for stock/standard installs).
static bool gScriptIntegrationEnabled = true;

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

// Reads [SETTINGS] ScriptIntegration (default true). Called from the static
// plugin constructor BEFORE Main.cpp's gLog is guaranteed to be open, so it must
// not touch gLog. Self-contained INI scan, mirroring LoadControlsFromINI's
// folder lookup and parsing.
static bool ReadScriptIntegrationFlag()
{
    char path[MAX_PATH];
    HMODULE hModule = NULL;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
        GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        (LPCSTR)&ReadScriptIntegrationFlag, &hModule);
    GetModuleFileNameA(hModule, path, MAX_PATH);
    std::string scriptsFolder = std::string(path);
    scriptsFolder = scriptsFolder.substr(0, scriptsFolder.find_last_of("\\/") + 1);

    std::ifstream ini(scriptsFolder + "NorthstarRadio.ini", std::ios::binary);
    if (!ini.is_open())
        return true; // default: enabled

    unsigned char bom[3] = {};
    ini.read((char*)bom, 3);
    if (!(bom[0] == 0xEF && bom[1] == 0xBB && bom[2] == 0xBF))
        ini.seekg(0);

    auto trim = [](std::string& s) {
        size_t start = s.find_first_not_of(" \t\r\n");
        size_t end = s.find_last_not_of(" \t\r\n");
        s = (start == std::string::npos) ? "" : s.substr(start, end - start + 1);
        };

    bool inSettings = false;
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
            inSettings = (header == "[settings]");
            continue;
        }
        if (!inSettings)
            continue;

        size_t sep = line.find('=');
        if (sep == std::string::npos)
            continue;
        std::string key = line.substr(0, sep);
        std::string val = line.substr(sep + 1);
        trim(key);
        trim(val);
        std::transform(key.begin(), key.end(), key.begin(), ::tolower);

        if (key == "scriptintegration" && !val.empty())
            return atoi(val.c_str()) != 0;
    }
    return true; // default: enabled
}

// Block game's radio switch button (keyboard / controller)
static bool __fastcall Hook_ChangeStationJustDown(CPad* self, void* edx)
{
    return false;
}

// Suppression of the native radio setter cDMAudio::SetRadioInCar (0x5F9730) and
// the radio audio-process function (0x5FB600) — the function that reads the
// scroll wheel / radio key, retunes, and draws the stock HUD radio-name banner.
//
// Done with plain injector::MakeJMP byte patches (JMP to a no-op), the technique
// that has been reliable since 0.9. safetyhook inline trampolines were tried here
// for the interior-audio fix but FAILED to install on some game EXEs, silently
// leaving the native radio alive — wheel scrolls then retuned it and flashed the
// stock banner behind ours. Instead, the original prologue bytes are saved at
// startup so the patches can be LIFTED while the player is on foot inside an
// interior (interior ambient music plays through these functions) and re-applied
// on exit. The toggle lives in gameProcessEvent; both the patching and every call
// to these functions happen on the game's main thread, so flipping the bytes at
// a frame boundary is safe.
static BYTE gOrigSetRadioInCar[5];
static BYTE gOrigRadioProcess[5];
static bool gNativeAudioAllowed = false; // false = patches applied (suppressed)

extern bool gPlayerInVehicle;    // defined in Main.cpp; updated every frame
extern bool gRadioInputBlocked;  // defined in Main.cpp; true while paused (menu/cutscene/load)

// Patch targets — never called while the patches are lifted.
static void __fastcall Hook_SetRadioInCar(cDMAudio* self, void* edx, unsigned int radio)
{
    // Intentionally empty.
}

static void __fastcall Hook_VehicleRadioProcess(void* self, void* edx)
{
    // Intentionally empty.
}

static void ApplyRadioSuppression(bool suppress)
{
    if (suppress) {
        injector::MakeJMP(0x5F9730, (void*)Hook_SetRadioInCar, true);
        injector::MakeJMP(0x5FB600, (void*)Hook_VehicleRadioProcess, true);
    }
    else {
        injector::WriteMemoryRaw(0x5F9730, gOrigSetRadioInCar, sizeof(gOrigSetRadioInCar), true);
        injector::WriteMemoryRaw(0x5FB600, gOrigRadioProcess, sizeof(gOrigRadioProcess), true);
    }
    gNativeAudioAllowed = !suppress;
}

// ---- SCM opcode interception: radio announcements + mission station changes ----
//
// The game's script (main.scm) fires:
//   * 057D PLAY_ANNOUNCEMENT  at fixed story moments (game start -> "bridges
//     closed / storm"; after Phnom Penh '86 -> "bridges open / hurricane gone").
//   * 041E SET_RADIO_CHANNEL  during some missions, to force the car radio to a
//     specific station. (Param 1 = station index 0..8; 9/10 = off.)
// Both store their relevant value as the FIRST opcode parameter, so one decoder
// handles both. We hand the value to Main.cpp, which plays the matching audio
// (announcement MP3, or the requested station) over the new BASS radio.
//
// We use a safetyhook *mid* hook at CRunningScript::ProcessOneCommand (the one
// function that dispatches every opcode). create_mid relocates the original
// instructions and resumes them after our callback, so the script engine runs
// completely unmodified — we only peek. At the function's entry the thiscall
// 'this' pointer is still in ECX.
static SafetyHookMid gScmHook;

// Reads one SCM int parameter at ss[*off] and advances *off past it. Resolves
// immediates AND global/local script variables (so a mission passing the radio
// timecode in a variable is read correctly). Returns false only for parameter
// types we can't interpret. *typeOut receives the raw parameter type byte.
static bool ReadScmInt(CRunningScript* self, const unsigned char* ss, int* off, int* out, unsigned char* typeOut)
{
    unsigned char type = ss[*off];
    *off += 1;
    if (typeOut) *typeOut = type;
    switch (type) {
    case SCRIPTPARAM_STATIC_INT_32BITS:  *out = *(const int*)(ss + *off);          *off += 4; return true;
    case SCRIPTPARAM_STATIC_INT_16BITS:  *out = *(const short*)(ss + *off);        *off += 2; return true;
    case SCRIPTPARAM_STATIC_INT_8BITS:   *out = *(const signed char*)(ss + *off);  *off += 1; return true;
    case SCRIPTPARAM_STATIC_FLOAT:       *out = (int)*(const float*)(ss + *off);   *off += 4; return true;
    case SCRIPTPARAM_GLOBAL_NUMBER_VARIABLE: {
        unsigned short goff = *(const unsigned short*)(ss + *off); *off += 2;
        *out = *(const int*)(ss + goff); // globals live in the script space
        return true;
    }
    case SCRIPTPARAM_LOCAL_NUMBER_VARIABLE: {
        unsigned short idx = *(const unsigned short*)(ss + *off); *off += 2;
        if (self && idx < 16) { *out = self->m_aLocalVars[idx].iParam; return true; }
        return false;
    }
    default: return false;
    }
}

static void OnProcessOneCommand(SafetyHookContext& ctx)
{
    CRunningScript* self = (CRunningScript*)ctx.ecx;
    if (!self)
        return;

    int ip = self->m_nIp;
    if (ip < 0)
        return;

    const unsigned char* ss = CTheScripts::ScriptSpace;
    // Opcode is the low 15 bits; high bit is the "not" flag.
    unsigned short op = (*(const unsigned short*)(ss + ip)) & 0x7FFF;
    if (op != 0x057D && op != 0x041E && op != 0x0394 && op != 0x03D1)
        return;

    if (op == 0x0394) {          // PLAY_MISSION_PASSED_TUNE -> duck the radio for the jingle
        gMissionPassedDuckUntil = GetTickCount() + 5000;
        return;
    }

    int off = ip + 2;            // first parameter (skip the 2-byte opcode)
    int first;
    if (!ReadScmInt(self, ss, &off, &first, nullptr))
        return;                  // unsupported param — leave it to the game

    if (op == 0x03D1) {          // PLAY_MISSION_AUDIO (mission dialogue) -> duck; refreshed per line
        gDialogueDuckUntil = GetTickCount() + 5000;
        return;
    }

    if (op == 0x057D) {          // PLAY_ANNOUNCEMENT: param 1 = announcement id
        gPendingAnnouncement = first;
        return;
    }

    // 041E SET_RADIO_CHANNEL: param 1 = station index, param 2 = play timecode
    // in ms (-1 = continue). Decode param 2 (immediate OR variable); it stays -1
    // if the parameter type is unsupported (we then fall back to our synced clock).
    int timecode = -1;
    ReadScmInt(self, ss, &off, &timecode, nullptr);
    gPendingScmStationTime = timecode; // set time first, station last (the signal)
    gPendingScmStation = first;
}

// Polls the radio-switch inputs (mouse wheel + configured key / pad, plus the
// hold-to-off timer) and sets the shared gSwitchNext / gSwitchPrev / gRadioOff
// flags. Main.cpp calls this at the TOP of its own gameProcessEvent, immediately
// before it consumes those flags, so a scroll is detected and acted on in the SAME
// frame — this is what keeps the on-screen station name from trailing the wheel by
// a frame during a fast spin. The VC mouse-wheel bytes are consumed every frame so
// the native radio code path never sees them, even while paused.
void PollRadioSwitchInput()
{
    bool wheelUp = *pMouseWheelUp != 0;
    bool wheelDown = *pMouseWheelDown != 0;
    *pMouseWheelUp = 0;
    *pMouseWheelDown = 0;

    // Radio-switch button (keyboard key OR controller button): a quick TAP changes
    // station; a HOLD of ~2.5s turns the radio OFF (same as scrolling past the last
    // station). The switch fires on RELEASE so a tap and a hold can be told apart.
    // (Mouse scroll above stays tap-only.)
    bool keyDown = (GetAsyncKeyState(gRadioSwitchNextKey) & 0x8000) != 0;
    bool padDown = false;
    if (gRadioSwitchNextPad != 0) {
        for (DWORD i = 0; i < XUSER_MAX_COUNT; i++) {
            XINPUT_STATE state = {};
            if (XInputGetState(i, &state) == ERROR_SUCCESS &&
                (state.Gamepad.wButtons & gRadioSwitchNextPad)) {
                padDown = true;
                break;
            }
        }
    }
    bool btnDown = keyDown || padDown;

    static bool sBtnWasDown = false;
    static DWORD sBtnDownTick = 0;
    static bool sHoldFired = false;
    const DWORD RADIO_OFF_HOLD_MS = 2500;

    // While the pause menu / cutscene is up the radio is paused and must not change.
    // GetAsyncKeyState / XInput read the physical device even then, so swallow all
    // switch input here (the wheel bytes are already consumed above) and reset the
    // hold tracker, so an in-menu press or a hold spanning the pause is not
    // registered and does not fire a switch the moment the menu closes.
    if (gRadioInputBlocked) {
        sBtnWasDown = false;
        sHoldFired = false;
        return;
    }

    if (wheelUp)
        gSwitchNext = true;
    if (wheelDown)
        gSwitchPrev = true;

    if (btnDown && !sBtnWasDown) {          // press started
        sBtnDownTick = GetTickCount();
        sHoldFired = false;
    }
    else if (btnDown && sBtnWasDown) {      // still held
        if (!sHoldFired && GetTickCount() - sBtnDownTick >= RADIO_OFF_HOLD_MS) {
            gRadioOff = true;               // held long enough -> off (once)
            sHoldFired = true;
        }
    }
    else if (!btnDown && sBtnWasDown) {     // released
        if (!sHoldFired)
            gSwitchNext = true;             // it was a tap -> change station
    }
    sBtnWasDown = btnDown;
}

class SwitchDetectorPlugin
{
public:
    SwitchDetectorPlugin()
    {
        injector::MakeJMP(0x4AA590, (void*)Hook_ChangeStationJustDown, true);
        // Save the original prologues, then suppress the native radio. The saved
        // bytes let gameProcessEvent lift the patches while the player is on foot
        // in an interior (so interior ambient music plays) and re-apply on exit.
        injector::ReadMemoryRaw(0x5F9730, gOrigSetRadioInCar, sizeof(gOrigSetRadioInCar), true);
        injector::ReadMemoryRaw(0x5FB600, gOrigRadioProcess, sizeof(gOrigRadioProcess), true);
        ApplyRadioSuppression(true);

        // Watch the SCM dispatcher for opcodes 057D (announcements),
        // 041E (mission radio-station changes), 0394/03D1 (audio ducking).
        // Skipped entirely when ScriptIntegration is disabled (total conversions
        // with an incompatible custom main.scm) — the core radio still runs.
        gScriptIntegrationEnabled = ReadScriptIntegrationFlag();
        if (gScriptIntegrationEnabled)
            gScmHook = safetyhook::create_mid((void*)0x44FBE0, OnProcessOneCommand);

        Events::initGameEvent.Add([]()
            {
                LoadControlsFromINI();
                if (gLog.is_open()) {
                    gLog << "ScriptIntegration (announcements / mission-radio / ducking): "
                         << (gScriptIntegrationEnabled ? "ENABLED"
                                                       : "DISABLED (SCM hook not installed)")
                         << std::endl;
                    gLog << "RadioHooks: byte-patch suppression active (lifted on foot in interiors)" << std::endl;
                    gLog.flush();
                }
            });

        Events::gameProcessEvent.Add([]()
            {
                // Native radio pass-through toggle: lift the suppression patches only
                // while the player is on foot inside an interior (clubs and shops play
                // their ambient music through the patched functions); re-apply them
                // everywhere else.
                //
                // We deliberately do NOT lift them while DRIVING inside an interior.
                // Tried that to fix the stadium challenges (hotring / bloodring / dirt
                // ring) but it re-applied the byte patches at the exact frame the event
                // ends and the player teleports out (area -> 0), colliding with the
                // game's audio teardown and crashing inside gta-vc.exe. Silencing our
                // radio during those events is handled separately and cleanly in
                // Main.cpp (a vehicle inside an interior is treated as no-radio), which
                // needs no native patching. Transitions are rare and happen on the main
                // thread — the same thread that runs those functions.
                bool allowNative = (CGame::currArea != 0) && !gPlayerInVehicle;
                if (allowNative != gNativeAudioAllowed) {
                    ApplyRadioSuppression(!allowNative);
                    if (gLog.is_open()) {
                        gLog << "RadioHooks: native audio "
                             << (allowNative ? "ALLOWED" : "suppressed")
                             << " (area " << CGame::currArea
                             << ", inVehicle " << (gPlayerInVehicle ? 1 : 0) << ")" << std::endl;
                        gLog.flush();
                    }
                }

                // NOTE: radio-switch input polling (mouse wheel + key / pad, and the
                // hold-to-off detection) now happens in PollRadioSwitchInput(), which
                // Main.cpp calls at the TOP of its gameProcessEvent — the same frame it
                // consumes the flags. That guarantees a scroll is detected and acted on
                // in one frame regardless of handler order, so the on-screen station
                // name no longer trails the wheel by a frame during a fast spin.
            });
    }
} switchDetectorPlugin;
