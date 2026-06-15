// Optional radio-icon HUD (experimental, [SETTINGS] RadioIconHud = 1).
//
// Draws a station ICON top-center when changing stations, instead of the text
// name. Icons are loose PNG files in a "RadioHud" folder next to the asi
// (i.e. <scripts>\RadioHud\). Each PNG's file name (without .png) is the texture
// name referenced by the mapping below — e.g. flash.png, wild.png, paradise.png.
//
// Resolution order per station: explicit [STATIONICONS] mapping -> built-in
// default for the 9 VC stations / MP3 player -> none (falls back to the text
// banner in Main.cpp). Any station can get an icon: drop <name>.png in RadioHud
// and (for custom stations) add a [STATIONICONS] line.
//
//   [STATIONICONS]
//   Station Name | texturename     (-> texturename.png)
//
//   [SETTINGS]
//   RadioIconScale = 0.10           (icon size, fraction of screen height)
//   RadioIconY     = 0.05           (icon top position, fraction of height)
//
// Disabled by default so stock behaviour (text name) is unchanged.

#include "plugin.h"
#include "SpriteLoader.h"   // plugin-sdk: loads loose PNGs (stb_image) into RW textures
#include "CSprite2d.h"
#include "CRect.h"
#include "CRGBA.h"
#include <string>
#include <vector>
#include <map>
#include <set>
#include <fstream>
#include <algorithm>
#include <cctype>
#include <cstdlib>

using namespace plugin;

// Mirror of the station record defined in Main.cpp.
struct RadioStation { std::string name; std::string file; };
extern std::vector<RadioStation> stations;
extern std::ofstream gLog;
extern std::string gScriptsFolder; // ends with a path separator; the asi lives here

// Default texture (PNG) names for the nine original VC stations, in [STATIONS]
// canonical order (index 0..8). Slot for the MP3 player is "player".
static const char* kIconName[9] = {
    "wild", "flash", "kchat", "fever", "vrock", "vcpr", "espan", "emoti", "wave"
};

static bool  gTried = false;     // load attempted (once)
static bool  gEnabled = false;   // [SETTINGS] RadioIconHud
static bool  gReady = false;     // RadioHud folder processed
static float gIconScale = 0.10f; // size as a fraction of screen height
static float gIconY = 0.05f;     // top position as a fraction of screen height

static SpriteLoader gIconLoader;                       // loose PNGs -> textures by name
static std::map<std::string, std::string> gIconOverride; // lower station name -> texture
static std::set<std::string> gLoggedMiss;              // textures logged as missing (once)

static std::string Lower(const std::string& s)
{
    std::string o = s;
    std::transform(o.begin(), o.end(), o.begin(), [](unsigned char c) { return (char)::tolower(c); });
    return o;
}

// Reads [SETTINGS] RadioIconHud / RadioIconScale / RadioIconY and the
// [STATIONICONS] mapping in one pass. Returns the RadioIconHud flag (default false).
static bool ReadIniConfig()
{
    std::ifstream ini(gScriptsFolder + "NorthstarRadio.ini", std::ios::binary);
    if (!ini.is_open())
        return false;

    unsigned char bom[3] = {};
    ini.read((char*)bom, 3);
    if (!(bom[0] == 0xEF && bom[1] == 0xBB && bom[2] == 0xBF))
        ini.seekg(0);

    auto trim = [](std::string& s) {
        size_t a = s.find_first_not_of(" \t\r\n");
        size_t b = s.find_last_not_of(" \t\r\n");
        s = (a == std::string::npos) ? "" : s.substr(a, b - a + 1);
        };

    bool flag = false;
    int section = 0; // 1 = [settings], 2 = [stationicons]
    std::string line;
    while (std::getline(ini, line)) {
        size_t c = line.find('#');
        if (c != std::string::npos) line = line.substr(0, c);
        trim(line);
        if (line.empty()) continue;

        if (line[0] == '[') {
            std::string h = Lower(line);
            section = (h == "[settings]") ? 1 : (h == "[stationicons]") ? 2 : 0;
            continue;
        }

        if (section == 1) {
            size_t eq = line.find('=');
            if (eq == std::string::npos) continue;
            std::string k = line.substr(0, eq), v = line.substr(eq + 1);
            trim(k); trim(v);
            std::string lk = Lower(k);
            if (lk == "radioiconhud" && !v.empty())
                flag = (atoi(v.c_str()) != 0);
            else if (lk == "radioiconscale" && !v.empty())
                gIconScale = (float)atof(v.c_str());
            else if (lk == "radioicony" && !v.empty())
                gIconY = (float)atof(v.c_str());
        }
        else if (section == 2) {
            size_t bar = line.find('|');
            if (bar == std::string::npos) continue;
            std::string name = line.substr(0, bar), tex = line.substr(bar + 1);
            trim(name); trim(tex);
            if (!name.empty() && !tex.empty())
                gIconOverride[Lower(name)] = tex;
        }
    }
    return flag;
}

// Loaded lazily on the first HUD draw (RenderWare is up by then). Reads config and
// loads every PNG in <scripts>\RadioHud as a texture named after its file.
static void EnsureLoaded()
{
    if (gTried) return;
    gTried = true;

    gEnabled = ReadIniConfig();
    if (gIconScale < 0.02f) gIconScale = 0.02f;
    if (gIconScale > 0.6f)  gIconScale = 0.6f;
    if (gIconY < 0.0f)  gIconY = 0.0f;
    if (gIconY > 0.85f) gIconY = 0.85f;

    if (!gEnabled) {
        gLog << "RadioIcons: disabled (RadioIconHud=0)" << std::endl;
        gLog.flush();
        return;
    }

    std::string folder = gScriptsFolder + "RadioHud";
    gIconLoader.LoadAllSpritesFromFolder(folder);
    gReady = true; // folder processed; any missing PNG just falls back to text
    gLog << "RadioIcons: loading PNGs from " << folder << "  ("
         << gIconOverride.size() << " custom mapping(s))" << std::endl;
    gLog.flush();
}

// Resolve a station (by display name) to a texture/PNG name, or "" if none.
static std::string ResolveTexture(const std::string& name)
{
    std::string low = Lower(name);

    // 1. Explicit [STATIONICONS] mapping wins (custom stations + overrides).
    auto ov = gIconOverride.find(low);
    if (ov != gIconOverride.end())
        return ov->second;

    // 2. The "Radio Off" banner uses radiooff.png automatically — no [STATIONICONS]
    //    mapping needed; just drop radiooff.png in RadioHud (falls back to the
    //    "Radio Off" text if the PNG is absent).
    if (low == "radio off")
        return "radiooff";

    // 3. Built-in defaults: the nine VC stations (by [STATIONS] index) + MP3 player.
    int idx = -1;
    for (int i = 0; i < (int)stations.size(); i++) {
        if (stations[i].name == name) { idx = i; break; }
    }
    if (low.find("mp3") != std::string::npos)
        return "player";
    if (idx >= 0 && idx < 9)
        return kIconName[idx];

    // 3. No icon -> caller falls back to the text banner.
    return "";
}

// Draws the station's icon centered near the top of the screen. Returns true if an
// icon was drawn (caller then skips the text banner); false to fall back.
bool DrawRadioStationIcon(const std::string& stationName, float resW, float resH, unsigned char alpha)
{
    EnsureLoaded();
    if (!gEnabled || !gReady)
        return false;

    std::string tex = ResolveTexture(stationName);
    if (tex.empty())
        return false;

    // GetTex returns the loader-OWNED RwTexture* (case-insensitive). We must never
    // let a CSprite2d destructor free it: ~CSprite2d -> Delete -> RwTextureDestroy,
    // so a local "CSprite2d spr = GetSprite(...)" frees the shared texture on scope
    // exit, and the next frame draws a freed texture (white box) and crashes. So we
    // keep one persistent sprite, borrow the texture only for the draw, then release.
    RwTexture* texture = gIconLoader.GetTex(tex);
    if (!texture) {
        if (gLoggedMiss.insert(tex).second) {
            gLog << "RadioIcons: no PNG '" << tex << ".png' in RadioHud (this station shows text)" << std::endl;
            gLog.flush();
        }
        return false;
    }

    float size = resH * gIconScale;   // size as a fraction of screen height
    float x = (resW - size) * 0.5f;   // centered horizontally
    float y = resH * gIconY;          // top of the icon, near the station-name band

    static CSprite2d sDraw;           // constructed once on first use; never freed per frame
    sDraw.m_pTexture = texture;       // borrow
    sDraw.SetRenderState();
    sDraw.Draw(CRect(x, y, x + size, y + size), CRGBA(255, 255, 255, alpha));
    sDraw.m_pTexture = nullptr;       // release borrow so ~CSprite2d never frees the loader's texture
    return true;
}
