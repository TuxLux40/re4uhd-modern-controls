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
#include <gdiplus.h>
#pragma comment(lib, "gdiplus.lib")

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

static const char* IconFile(const Entry& e) {
    switch (e.icon) {
    case IC_AMMO:
        switch (e.kind) {
        case AB_SHOTGUN: return "shotgun-ammo.png";
        case AB_RIFLE: return "rifle-ammo.png";
        case AB_TMP: return "tmp-ammo.png";
        case AB_MAGNUM: return "magnum-ammo.png";
        case AB_HANDCANNON: return "handcannon-ammo.png";
        default: return "handgun-ammo.png";
        }
    case IC_GRENADE:
        if (e.col == RGB(170, 52, 44)) return "incendiary-grenade.png";
        if (e.col == RGB(52, 76, 150)) return "flash-grenade.png";
        return "hand-grenade.png";
    case IC_HERB:
        if (e.col == RGB(204, 52, 52)) return "red-herb.png";
        if (e.col == RGB(232, 202, 52)) return "yellow-herb.png";
        return "green-herb.png";
    case IC_SPRAY: return "first-aid-spray.png";
    case IC_EGG:
        if (e.col == RGB(232, 192, 60)) return "gold-egg.png";
        if (e.col == RGB(170, 118, 76)) return "brown-egg.png";
        return "white-egg.png";
    case IC_FISH: return "black-bass.png";
    case IC_COINS: return "pesetas.png";
    default: return nullptr;
    }
}

// The icon, drawn 4x and averaged down (GDI's shapes have no anti-aliasing), over the line's pixels at (x, y).
static void PutIcon(uint32_t* dst, UINT pitchPx, int x, int y, int size, const Entry& e) {
    // Try real PNG from icons/ folder next to the DLL first
    const char* fn = IconFile(e);
    if (fn) {
        char path[MAX_PATH];
        GetModuleFileNameA(nullptr, path, MAX_PATH);
        char* slash = strrchr(path, '\\');
        if (slash) *slash = 0;
        strcat(path, "\\icons\\");
        strcat(path, fn);
        // Use GDI+ to load and blit if file exists
        FILE* f = fopen(path, "rb");
        if (f) {
            fclose(f);
            // GDI+ init (once)
            static ULONG_PTR gdiplusToken = 0;
            if (!gdiplusToken) {
                Gdiplus::GdiplusStartupInput gsi;
                Gdiplus::GdiplusStartup(&gdiplusToken, &gsi, nullptr);
            }
            Gdiplus::Bitmap* bmp = Gdiplus::Bitmap::FromFile(path);
            if (bmp && bmp->GetLastStatus() == Gdiplus::Ok) {
                // Draw scaled into a temporary DIB and composite
                const int n = size;
                BITMAPINFO bi = {};
                bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
                bi.bmiHeader.biWidth = n, bi.bmiHeader.biHeight = -n;
                bi.bmiHeader.biPlanes = 1, bi.bmiHeader.biBitCount = 32, bi.bmiHeader.biCompression = BI_RGB;
                void* bits = nullptr;
                HDC dc = CreateCompatibleDC(nullptr);
                HBITMAP dib = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
                if (dib && bits) {
                    HGDIOBJ old = SelectObject(dc, dib);
                    Gdiplus::Graphics g(dc);
                    g.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
                    g.DrawImage(bmp, 0, 0, n, n);
                    GdiFlush();
                    uint32_t* src = (uint32_t*)bits;
                    for (int oy = 0; oy < n; ++oy)
                        for (int ox = 0; ox < n; ++ox) {
                            uint32_t p = src[oy * n + ox];
                            if ((p >> 24) == 0) continue; // fully transparent
                            uint32_t& d = dst[(y + oy) * pitchPx + x + ox];
                            uint32_t ca = p >> 24;
                            uint32_t ba = d >> 24;
                            uint32_t oa = ca + ba * (255 - ca) / 255;
                            if (!oa) continue;
                            uint32_t r = (p >> 16) & 0xFF, g = (p >> 8) & 0xFF, b = p & 0xFF;
                            d = (oa << 24) | ((r * ca / oa) << 16) | ((g * ca / oa) << 8) | (b * ca / oa);
                        }
                    SelectObject(dc, old);
                    DeleteObject(dib);
                }
                if (dc) DeleteDC(dc);
                delete bmp;
                return; // success
            }
            if (bmp) delete bmp;
        }
    }
    // Fallback to procedural drawing
    const int s = 4, n = size * s;
    const uint32_t key = 0x00FF00FF;
    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = n, bi.bmiHeader.biHeight = -n;
    bi.bmiHeader.biPlanes = 1, bi.bmiHeader.biBitCount = 32, bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HDC dc = CreateCompatibleDC(nullptr);
    HBITMAP bmp = dc ? CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0) : nullptr;
    if (!bmp || !bits) {
        if (bmp) DeleteObject(bmp);
        if (dc) DeleteDC(dc);
        return;
    }
    HGDIOBJ old = SelectObject(dc, bmp);
    uint32_t* src = (uint32_t*)bits;
    for (int i = 0; i < n * n; ++i) src[i] = key;
    SetGraphicsMode(dc, GM_ADVANCED);
    DrawIconShapes(dc, n, e);
    GdiFlush();
    for (int oy = 0; oy < size; ++oy)
        for (int ox = 0; ox < size; ++ox) {
            uint32_t r = 0, g = 0, b = 0, cov = 0;
            for (int sy = 0; sy < s; ++sy)
                for (int sx = 0; sx < s; ++sx) {
                    const uint32_t p = src[(oy * s + sy) * n + ox * s + sx] & 0xFFFFFF;
                    if (p == key) continue;
                    r += (p >> 16) & 0xFF, g += (p >> 8) & 0xFF, b += p & 0xFF, ++cov;
                }
            if (!cov) continue;
            r /= cov, g /= cov, b /= cov;
            uint32_t& d = dst[(y + oy) * pitchPx + x + ox];
            // Over the band (black, its own alpha): straight alpha "over".
            const uint32_t ca = cov * 255 / (s * s), ba = d >> 24;
            const uint32_t oa = ca + ba * (255 - ca) / 255;
            if (!oa) continue;
            d = (oa << 24) | ((r * ca / oa) << 16) | ((g * ca / oa) << 8) | (b * ca / oa);
        }
    SelectObject(dc, old);
    DeleteObject(bmp);
    DeleteDC(dc);
}
