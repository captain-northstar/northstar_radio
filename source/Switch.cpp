#include "plugin.h"
#include "CPad.h"
#include "CPlayerPed.h"
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

// ---- Executable verification -------------------------------------------------
//
// Every address this plugin patches is a GTA VC 1.0 US address (the build the
// whole VC modding scene targets; gta-vc.exe is 3,088,896 bytes). Some copies in
// circulation — repacks in particular — ship a DIFFERENT build of the game: same
// game, but only about half the code in common and every function at a different
// address. Blindly writing a JMP into such an executable corrupts whatever
// instruction happens to live there, which is far worse than the plugin simply
// not running. So each target's first bytes are checked before it is patched; on
// a mismatch that patch is skipped and the reason is logged.
//
// IMPORTANT: a single site mismatching does NOT mean the wrong executable. Other
// ASIs hook these functions too — CLEO patches the script dispatcher, so
// ProcessOneCommand very often does not match by the time we load — and
// safetyhook is perfectly able to hook on top of that. Refusing to patch on a
// single mismatch broke script integration (and with it [STARTOFFSET], which
// rides on the intro's 041E station change) on a normal, working install. So the
// check is an AGGREGATE fingerprint: if ANY site still matches, this is the 1.0
// US build and everything is patched as usual. Only when NOTHING matches do we
// conclude it is a different build of the game and skip patching entirely.
struct CodeSite { unsigned int addr; const char* name; BYTE expect[8]; size_t len; };

static const CodeSite kSiteChangeStation = { 0x4AA590, "CPad::ChangeStationJustDown",
                                             {0x53,0x89,0xCB,0x66,0x83,0xBB,0xF0,0x00}, 8 };
static const CodeSite kSiteSetRadioInCar = { 0x5F9730, "cDMAudio::SetRadioInCar",
                                             {0x83,0xEC,0x08,0x8B,0x44,0x24,0x0C,0x50}, 8 };
static const CodeSite kSiteRadioProcess  = { 0x5FB600, "vehicle radio audio process",
                                             {0x53,0x56,0x57,0x55,0x83,0xEC,0x08,0x89}, 8 };
static const CodeSite kSiteProcessCommand= { 0x44FBE0, "CRunningScript::ProcessOneCommand",
                                             {0x66,0xFF,0x05,0x66,0x0A,0xA1,0x00,0x8B}, 8 };
// The 057D PLAY_ANNOUNCEMENT dispatch call, used only by the fallback below:
//   6377E5  mov eax,[0x7D7438]   ; script parameter (0 or 1)
//   6377EA  mov ecx,0xA10B8A     ; DMAudio instance (thiscall)
//   6377EF  add eax,0x19         ; +25  -> 0 becomes 25, 1 becomes 26
//   6377F3  call 0x5F9940        ; cDMAudio::PlayRadioAnnouncement
static const CodeSite kSiteAnnounceCall  = { 0x6377F3, "057D PLAY_ANNOUNCEMENT call site",
                                             {0xE8,0x48,0x21,0xFC,0xFF}, 5 };
// Fingerprint-only (never patched, so mods do not disturb it):
static const CodeSite kSiteAnnounceFn    = { 0x5F9940, "cDMAudio::PlayRadioAnnouncement",
                                             {0x83,0xEC,0x08,0x8B,0x44,0x24,0x0C,0x50}, 8 };

static bool VerifySite(const CodeSite& site)
{
    BYTE actual[8] = {};
    injector::ReadMemoryRaw(site.addr, actual, site.len, true);
    for (size_t i = 0; i < site.len; i++) {
        if (actual[i] != site.expect[i])
            return false;
    }
    return true;
}

// Startup status, recorded here (the constructor runs before gLog is open) and
// written to the log from initGameEvent so a user's log always says what happened.
static bool gExeVerified = false;         // aggregate: at least one site matched
static int  gSitesMatched = 0;            // how many of the fingerprint sites matched
static bool gSiteOkProcessCommand = false;
static bool gSiteOkAnnounceCall = false;
static bool gScmHookInstalled = false;
static bool gAnnounceFallbackActive = false;
static bool gAnnounceFallbackTried = false;

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
    // While the frontend menu / cutscene is up (gRadioInputBlocked), DON'T read or
    // clear the mouse wheel. MenuMapVC and other menu plugins read the very same
    // bytes (CPad::NewMouseControllerState.wheelUp/.wheelDown, 0x94D78B/C) to zoom
    // the map, and this poll runs at the top of the frame — clearing them here every
    // frame starved the map zoom (keyboard PageUp/Down kept working because that is
    // separate key state). The radio is paused in the menu and doesn't need the
    // wheel. During gameplay we still consume it so a scroll never leaks to the
    // native radio (retune / stock banner flash).
    bool wheelUp = false, wheelDown = false;
    if (!gRadioInputBlocked) {
        wheelUp = *pMouseWheelUp != 0;
        wheelDown = *pMouseWheelDown != 0;
        *pMouseWheelUp = 0;
        *pMouseWheelDown = 0;
    }

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

// ---- Announcement fallback ---------------------------------------------------
//
// Used only when the SCM mid-hook could not be installed. safetyhook has been
// seen to FAIL SILENTLY on some custom/repacked executables in this project
// before (that is what let the stock radio banner flash through in 1.0.1), and
// when it fails at CRunningScript::ProcessOneCommand the plugin never sees opcode
// 057D — so the bridge-closed / bridge-open announcements never play, with
// nothing in the log to say why.
//
// This fallback replaces the game's own 057D dispatch call with a call to us,
// which is a plain byte patch and therefore does not depend on safetyhook at all.
// The native argument has already been translated by the game to 25 (bridge
// closed) / 26 (bridge open) at that point, so it is mapped back to the 0 / 1 the
// rest of the plugin expects. Any other value is handed to the original function
// untouched. Announcements only: mission radio changes (041E) and audio ducking
// (0394 / 03D1) still need the SCM hook, and their loss is reported in the log.
typedef void(__thiscall* PlayRadioAnnouncementFn)(void* self, unsigned int announcement);

static void __fastcall Hook_PlayRadioAnnouncement(void* self, void* edx, unsigned int announcement)
{
    if (announcement == 25) {        // bridge closed  -> BCLOSED.mp3
        gPendingAnnouncement = 0;
        return;
    }
    if (announcement == 26) {        // bridge open    -> BOPEN.mp3
        gPendingAnnouncement = 1;
        return;
    }
    // Not one of ours: let the game handle it exactly as before.
    ((PlayRadioAnnouncementFn)0x5F9940)(self, announcement);
}

class SwitchDetectorPlugin
{
public:
    SwitchDetectorPlugin()
    {
        // Fingerprint the executable. Individual sites may legitimately mismatch
        // because another ASI patched them first (CLEO hooks the dispatcher), so
        // only a TOTAL mismatch means a different build of the game.
        gSiteOkProcessCommand = VerifySite(kSiteProcessCommand);
        gSiteOkAnnounceCall = VerifySite(kSiteAnnounceCall);
        const bool okChangeStation = VerifySite(kSiteChangeStation);
        const bool okSetRadio = VerifySite(kSiteSetRadioInCar);
        const bool okRadioProcess = VerifySite(kSiteRadioProcess);
        const bool okAnnounceFn = VerifySite(kSiteAnnounceFn);

        gSitesMatched = (okChangeStation ? 1 : 0) + (okSetRadio ? 1 : 0)
                      + (okRadioProcess ? 1 : 0) + (gSiteOkProcessCommand ? 1 : 0)
                      + (gSiteOkAnnounceCall ? 1 : 0) + (okAnnounceFn ? 1 : 0);
        gExeVerified = (gSitesMatched > 0);

        if (gExeVerified) {
            injector::MakeJMP(kSiteChangeStation.addr, (void*)Hook_ChangeStationJustDown, true);
            // Save the original prologues, then suppress the native radio. The saved
            // bytes let gameProcessEvent lift the patches while the player is on foot
            // in an interior (so interior ambient music plays) and re-apply on exit.
            injector::ReadMemoryRaw(0x5F9730, gOrigSetRadioInCar, sizeof(gOrigSetRadioInCar), true);
            injector::ReadMemoryRaw(0x5FB600, gOrigRadioProcess, sizeof(gOrigRadioProcess), true);
            ApplyRadioSuppression(true);
        }

        // Watch the SCM dispatcher for opcodes 057D (announcements),
        // 041E (mission radio-station changes), 0394/03D1 (audio ducking).
        // Skipped entirely when ScriptIntegration is disabled (total conversions
        // with an incompatible custom main.scm) — the core radio still runs.
        //
        // Always ATTEMPT the hook: safetyhook can hook a function another mod has
        // already patched, and the byte check above is only a fingerprint. The
        // fallback is driven by whether the hook really installed, nothing else.
        gScriptIntegrationEnabled = ReadScriptIntegrationFlag();
        if (gScriptIntegrationEnabled && gExeVerified) {
            gScmHook = safetyhook::create_mid((void*)0x44FBE0, OnProcessOneCommand);
            gScmHookInstalled = static_cast<bool>(gScmHook);

            // Hook genuinely failed to install (this has happened silently on some
            // custom executables): keep the announcements alive with a plain byte
            // patch of the game's own 057D dispatch call instead.
            if (!gScmHookInstalled) {
                gAnnounceFallbackTried = true;
                if (gSiteOkAnnounceCall) {
                    injector::MakeCALL(kSiteAnnounceCall.addr, (void*)Hook_PlayRadioAnnouncement, true);
                    gAnnounceFallbackActive = true;
                }
            }
        }

        Events::initGameEvent.Add([]()
            {
                LoadControlsFromINI();
                if (gLog.is_open()) {
                    // Executable fingerprint. Fewer than 6 matches is normal — it
                    // just means another ASI hooked that function before us.
                    if (!gExeVerified) {
                        gLog << "RadioHooks: *** UNEXPECTED EXECUTABLE *** none of the 6 known code"
                                " sites matched — ALL patches skipped to avoid corrupting the game."
                             << std::endl;
                        gLog << "RadioHooks: this plugin targets GTA VC 1.0 US (gta-vc.exe 3,088,896"
                                " bytes). A repack shipping a different build will not work."
                             << std::endl;
                    }
                    else {
                        gLog << "RadioHooks: byte-patch suppression active (lifted on foot in interiors)"
                             << "  [exe fingerprint " << gSitesMatched << "/6"
                             << (gSitesMatched < 6 ? ", rest already hooked by other mods]" : "]")
                             << std::endl;
                    }

                    gLog << "ScriptIntegration (announcements / mission-radio / ducking): "
                         << (gScriptIntegrationEnabled ? "ENABLED"
                                                       : "DISABLED by INI (SCM hook not installed)")
                         << std::endl;

                    if (gScriptIntegrationEnabled) {
                        if (gScmHookInstalled) {
                            gLog << "ScriptIntegration: SCM hook installed OK" << std::endl;
                        }
                        else {
                            gLog << "ScriptIntegration: SCM hook FAILED to install "
                                    "(safetyhook could not patch this exe)" << std::endl;
                            if (gAnnounceFallbackActive)
                                gLog << "ScriptIntegration: announcement FALLBACK active (bridge bulletins will play; "
                                        "mission-radio changes and audio ducking are NOT available)" << std::endl;
                            else if (gAnnounceFallbackTried)
                                gLog << "ScriptIntegration: announcement fallback unavailable too "
                                        "(057D call site does not match) — no announcements on this exe" << std::endl;
                        }
                    }
                    gLog.flush();
                }
            });

        Events::gameProcessEvent.Add([]()
            {
                // Native radio pass-through toggle: lift the suppression patches for
                // the WHOLE time the player is inside an interior — on foot or driving.
                // Interiors play their own music/crowd audio through these functions,
                // and a patched (no-op) function cannot STOP a sound the game already
                // started. Re-patching when the player got into the event car inside the
                // stadium therefore froze that audio mid-play, and the stadium music
                // kept going after the event ended (until a pause re-synced audio).
                //
                // A settle window keeps them lifted briefly AFTER the interior is left:
                // the native code has to run at least once with the new area to notice
                // it should stop the interior music, so re-patching on the exact
                // transition frame would strand the music playing all over again.
                //
                // This was tried once before and crashed at event exit — that crash was
                // the DMAudio.SetRadioInCar(10) call in Main.cpp executing the real
                // native setter during the game's teardown on that frame. That call is
                // now gone, so lifting here is safe. The stock car radio still cannot
                // come back while driving: the vehicle's station byte is pinned to 10
                // (off) every frame in drawHudEvent, the radio key stays patched out,
                // and the wheel bytes are consumed before native code can see them.
                // Transitions are rare and happen on the main thread — the same thread
                // that runs those functions.
                // Suppression was never installed (unexpected executable): there is
                // nothing to toggle, and the saved original bytes are not valid.
                if (!gExeVerified)
                    return;

                // The player is in a vehicle, or has STARTED getting into one. Checked
                // from the ped's task state, not just m_bInVehicle: that only turns true
                // once the player is SEATED, about a second after the entry animation
                // begins, and the game assigns the car its station inside that gap —
                // long enough for the native side to LATCH its stock name banner (which
                // is latched at retune, so pinning the station byte back to 10 a frame
                // later cannot un-draw it). Read live from the ped rather than via
                // gPlayerInVehicle, which is a frame stale depending on handler order.
                CPlayerPed* pPed = FindPlayerPed();
                bool vehicleBusy = gPlayerInVehicle;
                if (pPed) {
                    DWORD st = *(DWORD*)((BYTE*)pPed + 0x244);
                    vehicleBusy = pPed->m_bInVehicle
                        || st == 24   // SEEK_CAR
                        || st == 50   // DRIVING
                        || st == 51   // PASSENGER
                        || st == 52   // TAXI_PASSNGR
                        || st == 53   // OPEN_DOOR
                        || st == 56   // CARJACK
                        || st == 58   // ENTER_CAR
                        || st == 59   // STEAL_CAR
                        || st == 60;  // EXIT_CAR
                }

                // Native audio is allowed ONLY while the player is on foot inside an
                // interior — never anywhere near a vehicle, so the native side can never
                // latch a station banner. No timing window is involved any more.
                //
                // The reason a timed window was needed before: a patched (no-op)
                // function cannot STOP a sound the game already started, so re-patching
                // while interior music was playing froze it and it played forever.
                // Fixed properly below by telling the game to stop it first.
                bool allowNative = (CGame::currArea != 0) && !vehicleBusy;

                if (allowNative != gNativeAudioAllowed) {
                    if (!allowNative) {
                        // About to re-apply the patches. VC plays interior/club/stadium
                        // music THROUGH the radio system (that is why these patches
                        // silence it), so ask the game to switch that radio off while
                        // its setter is still live. The sound is then already stopped
                        // when the no-op patch lands, instead of being frozen mid-play.
                        // Deterministic — no settle window, and it covers every exit
                        // (walking out, getting into the event car, or being placed
                        // outside still sitting in a vehicle).
                        //
                        // Safe to call here, unlike the per-frame call this replaces:
                        // that one also ran during the event-end teleport/teardown,
                        // which is what crashed inside gta-vc.exe. By the time an event
                        // ends the player is already in the event vehicle, so the
                        // patches are applied and this transition has long since passed.
                        const unsigned int RADIO_OFF = 10;
                        DMAudio.SetRadioInCar(RADIO_OFF);
                    }
                    ApplyRadioSuppression(!allowNative);
                    if (gLog.is_open()) {
                        gLog << "RadioHooks: native audio "
                             << (allowNative ? "ALLOWED" : "suppressed (native radio switched off first)")
                             << " (area " << CGame::currArea
                             << ", vehicle " << (vehicleBusy ? 1 : 0) << ")" << std::endl;
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
