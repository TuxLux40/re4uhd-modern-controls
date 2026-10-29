// NOTE: Prefer external PNGs from icons/ folder next to the DLL (sourced from evilresource.com). Falls back to procedural drawing.

// Pickup ticker: with QuickPickup the everyday pickups come without their screen, so a few lines in small print on the
// right, above the HUD, say what was taken ("Handgun Ammo x30" and an icon after it), like the newer Resident Evil
// games. Each line stays kShow ms and fades out over the last kFade ms (in over the first kIn); at most kLines at a time, the
// newest at the bottom.
// The game has no flat item icons (the case shows small 3D models, SS/item/idm*), so the icons are drawn here: white
// silhouettes with a dark outline, one per kind of item (ammo boxes, grenades, herbs, spray, eggs, fish, gems, crowns,
// rings, gold bars, other treasures, money). Color only where it tells something, mostly the game's own: the ammo boxes
// (handgun red, shotgun green, rifle white, TMP blue...), the grenades (hand olive, flash blue, incendiary red), the
// herb's leaves, the eggs, the gems and the spray's cross.
// Drawn from the Present hook (crosshair.cpp): GDI renders the lines into a texture (text with a dark outline, a faint
// dark band behind each line fading out to the left), one alpha-blended quad per line puts them on the back buffer.
// All render states are saved in a state block and put back afterwards.
#include "common.h"
#include "config.h"
#include <d3d9.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <initializer_list>

static const DWORD kShow = 4000, kFade = 600, kIn = 250;
static const int kLines = 5;
static const COLORREF kWhite = RGB(236, 234, 228), kDetail = RGB(150, 150, 146);

// The ammo boxes, after the game's: only their shape and color blocks (no text or pictures, unreadable at this size).
enum AmmoBox : uint8_t { AB_HANDGUN, AB_SHOTGUN, AB_RIFLE, AB_TMP, AB_MAGNUM, AB_HANDCANNON };

enum Icon : uint8_t {
    IC_BOX, IC_AMMO, IC_CRATE, IC_ARROWS, IC_GRENADE, IC_HERB, IC_SPRAY, IC_EGG, IC_FISH, IC_GEM, IC_CROWN, IC_RING, IC_BAR, IC_TREASURE, IC_COINS
};

struct Entry {
    char text[64];
    Icon icon;
    COLORREF col;      // the icon's accent color (kWhite: none)
    uint8_t kind;      // ammo boxes: which box (AmmoBox)
    DWORD t;
};
static Entry g_entries[kLines];
static int g_count = 0;
static bool g_dirty = false;
static CRITICAL_SECTION g_lock;
static bool g_lockInit = false;

static void Lock() {
    if (!g_lockInit) InitializeCriticalSection(&g_lock), g_lockInit = true;
    EnterCriticalSection(&g_lock);
}

static COLORREF ColorFromName(const char* n, COLORREF dflt) {
    if (strstr(n, "Ruby") || strstr(n, "Red")) return RGB(214, 44, 52);
    if (strstr(n, "Emerald") || strstr(n, "Green")) return RGB(46, 176, 92);
    if (strstr(n, "Sapphire") || strstr(n, "Blue")) return RGB(58, 98, 214);
    if (strstr(n, "Yellow")) return RGB(232, 202, 46);
    if (strstr(n, "Purple") || strstr(n, "Spinel")) return RGB(168, 78, 196);
    if (strstr(n, "Pearl")) return RGB(236, 232, 222);
    return dflt;
}

// One line for a picked-up item (type: re4_tweaks ITEM_TYPE).
void TickerItem(uint32_t id, uint32_t num, uint8_t type, const char* name) {
    if (!Cfg().pickupTicker) return;
    Entry e = {};
    const bool money = type == 8 && id != 0x73 && id != 0x75;
    if (money)
        snprintf(e.text, sizeof(e.text), "%u PTAS", num);
    else if (num > 1)
        snprintf(e.text, sizeof(e.text), "%s  x%u", name, num);
    else
        snprintf(e.text, sizeof(e.text), "%s", name);
    auto has = [&](const char* s) { return strstr(name, s) != nullptr; };
    e.col = kWhite;
    if (money || type == 8) {
        e.icon = IC_COINS;
    } else if (type == 2) {  // ammo: the box in the game's color for that kind (handgun red, shotgun green...)
        e.icon = IC_AMMO;
        if (has("Dart")) e.icon = IC_CRATE, e.col = RGB(98, 102, 62);
        else if (has("Arrow") || has("Bolt")) e.icon = IC_ARROWS, e.col = RGB(206, 44, 44);
        else if (has("Shotgun")) e.kind = AB_SHOTGUN, e.col = RGB(52, 142, 58);
        else if (has("Rifle")) e.kind = AB_RIFLE, e.col = RGB(226, 220, 210);
        else if (has("TMP") || has("Typewriter")) e.kind = AB_TMP, e.col = RGB(40, 70, 168);
        else if (has("Magnum")) e.kind = AB_MAGNUM, e.col = RGB(34, 34, 38);
        else if (has("Handcannon")) e.kind = AB_HANDCANNON, e.col = RGB(226, 176, 40);
        else e.kind = AB_HANDGUN, e.col = RGB(190, 36, 36);
    } else if (type == 3) {  // one can with a spoon lever for all three, colored as in the game
        e.icon = IC_GRENADE;
        e.col = has("Incendiary") ? RGB(170, 52, 44) : has("Flash") ? RGB(52, 76, 150) : RGB(86, 110, 66);
    } else if (type == 5 || type == 12) {  // treasures
        if (has("Crown") && !has("Jewel")) e.icon = IC_CROWN;
        else if (has("Ruby") || has("Emerald") || has("Sapphire") || has("Spinel") || has("Velvet") ||
                 has("Catseye") || has("Gem") || has("Jewel"))
            e.icon = IC_GEM, e.col = ColorFromName(name, RGB(222, 182, 62));
        else if (has("Pendant") || has("Bangle") || has("Ring")) e.icon = IC_RING, e.col = ColorFromName(name, kWhite);
        else if (has("Gold Bar")) e.icon = IC_BAR;
        else e.icon = IC_TREASURE;
    } else if (has("Herb")) {  // only single herbs lie around; combined ones are made in the case
        e.icon = IC_HERB;
        e.col = has("Red") ? RGB(204, 52, 52) : has("Yellow") ? RGB(232, 202, 52) : RGB(62, 172, 72);
    } else if (has("Spray")) {
        e.icon = IC_SPRAY, e.col = RGB(60, 170, 80);
    } else if (has("Egg")) {
        e.icon = IC_EGG;
        e.col = has("Gold") ? RGB(232, 192, 60) : has("Brown") ? RGB(170, 118, 76) : RGB(242, 236, 222);
    } else if (has("Bass") || has("Fish")) {
        e.icon = IC_FISH;
    }
    Lock();
    if (g_count == kLines) {
        memmove(g_entries, g_entries + 1, sizeof(Entry) * (kLines - 1));
        --g_count;
    }
    e.t = GetTickCount();
    g_entries[g_count++] = e;
    g_dirty = true;
    LeaveCriticalSection(&g_lock);
}

// ... (rest of the file remains the same for now; full loader to be added)
