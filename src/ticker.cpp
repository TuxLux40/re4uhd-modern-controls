// Pickup ticker: with QuickPickup the everyday pickups come without their screen, so a few lines in small print on the
// right, above the HUD, say what was taken ("Handgun Ammo x30" and an icon after it), like the newer Resident Evil
// games. Each line stays kShow ms and fades out over the last kFade ms (in over the first kIn); at most kLines at a time, the
// newest at the bottom.
// The game has no flat item icons (the case shows small 3D models, SS/item/idm*), so the icons are drawn here: white
// silhouettes with a dark outline, one per kind of item (ammo boxes, grenades, herbs, spray, eggs, fish, gems, crowns,
// rings, gold bars, other treasures, money). Color only where it tells something, mostly the game's own: the ammo boxes
// (handgun red, shotgun green, rifle white, TMP blue...), the grenades (hand olive, flash blue, incendiary red), the
// herb's leaves, the eggs, the gems and the spray's cross.
// Pictures: where assets/icons has a render of the item (built into the DLL, build/icons_data.h, named after the item:
// "Handgun Ammo" -> handgun-ammo), it takes the drawn icon's place.
// Drawn from the Present hook (crosshair.cpp): GDI renders the lines into a texture (text with a dark outline, a faint
// dark band behind each line fading out to the left), one alpha-blended quad per line puts them on the back buffer.
// All render states are saved in a state block and put back afterwards.
#include "common.h"
#include "config.h"
#include <d3d9.h>
#include <stdio.h>
#include <stdlib.h>
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
    char image[48];    // picture in assets/icons (the item's name, lower case, '-' between words)
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
    {
        const char* src = money ? "pesetas" : name;
        size_t k = 0;
        for (const char* c = src; *c && k + 1 < sizeof(e.image); ++c) {
            const char ch = *c >= 'A' && *c <= 'Z' ? (char)(*c - 'A' + 'a') : *c;
            if ((ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9')) e.image[k++] = ch;
            else if (k && e.image[k - 1] != '-') e.image[k++] = '-';
        }
        while (k && e.image[k - 1] == '-') --k;
        e.image[k] = 0;
    }
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

static IDirect3DTexture9* g_tex = nullptr;
static IDirect3DDevice9* g_texDev = nullptr;
static UINT g_texW = 0, g_texH = 0, g_lineH = 0;
static LONG g_width[kLines] = {};  // drawn width of each line in the texture, pixels

static UINT Pow2(UINT v) {
    UINT p = 64;
    while (p < v) p <<= 1;
    return p;
}

static COLORREF Shade(COLORREF c, int pct) {
    auto f = [&](int v) { v = v * pct / 100; return v > 255 ? 255 : v; };
    return RGB(f(GetRValue(c)), f(GetGValue(c)), f(GetBValue(c)));
}

// The icon's shapes into dc (n x n pixels), on the key color.
static void DrawIconShapes(HDC dc, int n, const Entry& e) {
    const int w = n / 18 > 1 ? n / 18 : 1;  // outline width
    HPEN pen = CreatePen(PS_SOLID, w, RGB(18, 18, 18));
    HGDIOBJ oldPen = SelectObject(dc, pen);
    auto P = [&](float v) { return (int)(v * n); };
    auto fill = [&](COLORREF c) {
        HBRUSH b = CreateSolidBrush(c);
        DeleteObject(SelectObject(dc, b));
    };
    HGDIOBJ oldBrush = SelectObject(dc, GetStockObject(NULL_BRUSH));
    auto rot = [&](float deg, float cx, float cy) {
        const float r = deg * 3.14159265f / 180.0f;
        XFORM x = {cosf(r), sinf(r), -sinf(r), cosf(r), 0, 0};
        x.eDx = cx * n - x.eM11 * cx * n - x.eM21 * cy * n;
        x.eDy = cy * n - x.eM12 * cx * n - x.eM22 * cy * n;
        SetWorldTransform(dc, &x);
    };
    auto unrot = [&] { ModifyWorldTransform(dc, nullptr, MWT_IDENTITY); };
    const COLORREF c = e.col;
    auto poly = [&](std::initializer_list<float> xy) {
        POINT pt[16];
        int k = 0;
        for (auto it = xy.begin(); it != xy.end() && k < 16; it += 2) pt[k++] = {P(it[0]), P(it[1])};
        Polygon(dc, pt, k);
    };
    auto line = [&](float x0, float y0, float x1, float y1) { MoveToEx(dc, P(x0), P(y0), nullptr), LineTo(dc, P(x1), P(y1)); };
    switch (e.icon) {
    case IC_AMMO: {  // a flat cardboard box from above its front corner, with the color blocks of the game's box
        const COLORREF red = RGB(196, 36, 36), white = RGB(240, 236, 228), yellow = RGB(232, 196, 52);
        auto box = [&](COLORREF front, COLORREF top, COLORREF side) {
            fill(top);
            poly({0.04f, 0.40f, 0.20f, 0.18f, 0.96f, 0.18f, 0.80f, 0.40f});
            fill(side);
            poly({0.80f, 0.40f, 0.96f, 0.18f, 0.96f, 0.64f, 0.80f, 0.86f});
            fill(front);
            Rectangle(dc, P(0.04f), P(0.40f), P(0.80f), P(0.86f));
        };
        auto block = [&](COLORREF col, float x0, float y0, float x1, float y1) {  // flat, no outline
            fill(col);
            SelectObject(dc, GetStockObject(NULL_PEN));
            Rectangle(dc, P(x0), P(y0), P(x1), P(y1));
            SelectObject(dc, pen);
        };
        switch (e.kind) {
        case AB_HANDGUN:  // red, a white title strip along the top of the front and the lid
            box(c, Shade(c, 125), Shade(c, 70));
            block(white, 0.07f, 0.45f, 0.77f, 0.53f);
            block(white, 0.07f, 0.76f, 0.77f, 0.80f);
            break;
        case AB_SHOTGUN:  // green with a yellow frame, a dark round emblem on the left
            box(yellow, Shade(c, 125), Shade(c, 70));
            block(c, 0.08f, 0.44f, 0.76f, 0.82f);
            fill(RGB(24, 30, 24));
            Ellipse(dc, P(0.12f), P(0.46f), P(0.38f), P(0.80f));
            fill(yellow);
            Ellipse(dc, P(0.19f), P(0.55f), P(0.31f), P(0.71f));
            block(yellow, 0.44f, 0.52f, 0.72f, 0.58f);
            break;
        case AB_RIFLE:  // off-white, red lettering across, a red edge on the left
            box(c, Shade(c, 110), Shade(c, 80));
            block(red, 0.07f, 0.43f, 0.13f, 0.83f);
            block(red, 0.18f, 0.50f, 0.74f, 0.56f);
            block(red, 0.18f, 0.64f, 0.62f, 0.68f);
            break;
        case AB_TMP:  // blue, a light picture panel on the right half
            box(c, Shade(c, 135), Shade(c, 70));
            block(RGB(210, 214, 222), 0.46f, 0.44f, 0.77f, 0.82f);
            block(white, 0.08f, 0.50f, 0.40f, 0.58f);
            break;
        case AB_MAGNUM:  // black, a red title block top left and a red stripe along the bottom
            box(c, RGB(60, 60, 64), red);
            block(red, 0.08f, 0.44f, 0.52f, 0.58f);
            block(red, 0.36f, 0.74f, 0.77f, 0.81f);
            break;
        case AB_HANDCANNON:  // yellow, a red mark on the right
            box(c, Shade(c, 120), Shade(c, 75));
            block(red, 0.50f, 0.48f, 0.72f, 0.68f);
            block(RGB(120, 70, 20), 0.08f, 0.48f, 0.42f, 0.54f);
            break;
        }
        break;
    }
    case IC_CRATE:  // mine darts: an olive crate with two latches
        fill(Shade(c, 125));
        poly({0.06f, 0.42f, 0.22f, 0.22f, 0.94f, 0.22f, 0.78f, 0.42f});
        fill(Shade(c, 70));
        poly({0.78f, 0.42f, 0.94f, 0.22f, 0.94f, 0.68f, 0.78f, 0.90f});
        fill(c);
        Rectangle(dc, P(0.06f), P(0.42f), P(0.78f), P(0.90f));
        line(0.06f, 0.54f, 0.78f, 0.54f);
        fill(kDetail);
        Rectangle(dc, P(0.18f), P(0.48f), P(0.28f), P(0.62f));
        Rectangle(dc, P(0.56f), P(0.48f), P(0.66f), P(0.62f));
        break;
    case IC_ARROWS:  // a bundle of three arrows with red fletching, tied in the middle (grey shafts: black ones
                     // vanished on the dark band, and narrow fletching was all outline)
        rot(40, 0.5f, 0.5f);
        for (float o : {-0.15f, 0.0f, 0.15f}) {
            fill(c);
            poly({0.50f + o, 0.58f, 0.36f + o, 0.98f, 0.50f + o, 0.86f, 0.64f + o, 0.98f});
            fill(kDetail);
            Rectangle(dc, P(0.46f + o), P(0.12f), P(0.54f + o), P(0.80f));
            fill(kWhite);
            poly({0.50f + o, 0.00f, 0.59f + o, 0.18f, 0.41f + o, 0.18f});
        }
        fill(RGB(60, 60, 64));
        Rectangle(dc, P(0.28f), P(0.42f), P(0.72f), P(0.50f));
        unrot();
        break;
    case IC_GRENADE:  // a can with a spoon lever and the pin's ring
        fill(kDetail);
        Rectangle(dc, P(0.36f), P(0.10f), P(0.60f), P(0.24f));     // the fuze
        Ellipse(dc, P(0.22f), P(0.04f), P(0.38f), P(0.18f));       // the ring
        poly({0.58f, 0.12f, 0.74f, 0.18f, 0.70f, 0.70f, 0.64f, 0.70f, 0.66f, 0.22f, 0.58f, 0.20f});  // the spoon
        fill(c);
        RoundRect(dc, P(0.24f), P(0.22f), P(0.66f), P(0.94f), P(0.14f), P(0.14f));
        fill(Shade(c, 180));  // the white print on the can
        SelectObject(dc, GetStockObject(NULL_PEN));
        for (int k = 0; k < 3; ++k) Rectangle(dc, P(0.31f), P(0.40f + k * 0.08f), P(0.59f - k * 0.04f), P(0.44f + k * 0.08f));
        SelectObject(dc, pen);
        break;
    case IC_HERB: {  // a sprig: stem and three leaves in the herb's color, kept inside the square (the side leaves
                     // were cut off at its edge)
        HPEN stem = CreatePen(PS_SOLID, w * 2, RGB(70, 96, 40));
        HGDIOBJ op = SelectObject(dc, stem);
        line(0.5f, 0.98f, 0.5f, 0.70f);
        SelectObject(dc, op);
        DeleteObject(stem);
        for (float deg : {-38.0f, 38.0f, 0.0f}) {
            rot(deg, 0.5f, 0.74f);
            fill(c);
            Ellipse(dc, P(0.39f), P(0.18f), P(0.61f), P(0.76f));
            line(0.5f, 0.28f, 0.5f, 0.70f);
            unrot();
        }
        break;
    }
    case IC_SPRAY:  // a spray can: domed top, cap with its nozzle, white body, colored cross
        fill(kDetail);
        Rectangle(dc, P(0.42f), P(0.04f), P(0.58f), P(0.18f));
        Rectangle(dc, P(0.56f), P(0.07f), P(0.66f), P(0.11f));
        fill(kWhite);
        Ellipse(dc, P(0.28f), P(0.13f), P(0.72f), P(0.33f));
        Rectangle(dc, P(0.28f), P(0.23f), P(0.72f), P(0.94f));
        SelectObject(dc, GetStockObject(NULL_PEN));
        Rectangle(dc, P(0.29f), P(0.20f), P(0.71f), P(0.26f));  // hides the dome's lower edge
        fill(c);
        Rectangle(dc, P(0.45f), P(0.44f), P(0.55f), P(0.78f));
        Rectangle(dc, P(0.33f), P(0.56f), P(0.67f), P(0.66f));
        SelectObject(dc, pen);
        break;
    case IC_EGG:
        fill(c);
        Ellipse(dc, P(0.24f), P(0.08f), P(0.76f), P(0.94f));
        fill(Shade(c, 125));
        SelectObject(dc, GetStockObject(NULL_PEN));
        Ellipse(dc, P(0.34f), P(0.24f), P(0.46f), P(0.42f));
        SelectObject(dc, pen);
        break;
    case IC_FISH:  // a bass from the side, filling the square
        fill(kWhite);
        poly({0.70f, 0.50f, 0.98f, 0.20f, 0.98f, 0.80f});
        poly({0.30f, 0.26f, 0.44f, 0.10f, 0.58f, 0.26f});  // the dorsal fin
        Ellipse(dc, P(0.02f), P(0.22f), P(0.80f), P(0.78f));
        line(0.26f, 0.28f, 0.26f, 0.72f);
        fill(RGB(18, 18, 18));
        Ellipse(dc, P(0.11f), P(0.40f), P(0.19f), P(0.48f));
        break;
    case IC_GEM:
        fill(c);
        poly({0.28f, 0.16f, 0.72f, 0.16f, 0.92f, 0.40f, 0.50f, 0.92f, 0.08f, 0.40f});
        fill(Shade(c, 130));
        poly({0.28f, 0.16f, 0.72f, 0.16f, 0.62f, 0.40f, 0.38f, 0.40f});
        line(0.08f, 0.40f, 0.92f, 0.40f);
        line(0.38f, 0.40f, 0.50f, 0.92f);
        line(0.62f, 0.40f, 0.50f, 0.92f);
        break;
    case IC_CROWN:
        fill(kWhite);
        poly({0.10f, 0.78f, 0.08f, 0.28f, 0.30f, 0.52f, 0.50f, 0.18f, 0.70f, 0.52f, 0.92f, 0.28f, 0.90f, 0.78f});
        Rectangle(dc, P(0.10f), P(0.72f), P(0.90f), P(0.88f));
        fill(kDetail);
        for (float x : {0.08f, 0.50f, 0.92f}) Ellipse(dc, P(x - 0.06f), P(x == 0.50f ? 0.10f : 0.20f), P(x + 0.06f), P(x == 0.50f ? 0.22f : 0.32f));
        break;
    case IC_RING:  // ring with a stone on top
        fill(kWhite);
        Ellipse(dc, P(0.16f), P(0.30f), P(0.84f), P(0.94f));
        fill(RGB(255, 0, 255));  // the key color: punches the hole
        Ellipse(dc, P(0.30f), P(0.44f), P(0.70f), P(0.80f));
        fill(c == kWhite ? kDetail : c);
        poly({0.38f, 0.12f, 0.62f, 0.12f, 0.70f, 0.24f, 0.50f, 0.40f, 0.30f, 0.24f});
        break;
    case IC_BAR:  // gold bar: top face and front
        fill(kWhite);
        poly({0.20f, 0.36f, 0.80f, 0.36f, 0.94f, 0.80f, 0.06f, 0.80f});
        fill(kDetail);
        poly({0.26f, 0.42f, 0.74f, 0.42f, 0.80f, 0.58f, 0.20f, 0.58f});
        break;
    case IC_TREASURE:  // goblet
        fill(kWhite);
        poly({0.20f, 0.08f, 0.80f, 0.08f, 0.70f, 0.40f, 0.30f, 0.40f});
        Ellipse(dc, P(0.30f), P(0.30f), P(0.70f), P(0.52f));
        Rectangle(dc, P(0.46f), P(0.48f), P(0.54f), P(0.78f));
        poly({0.38f, 0.76f, 0.62f, 0.76f, 0.74f, 0.92f, 0.26f, 0.92f});
        fill(kDetail);
        Rectangle(dc, P(0.24f), P(0.16f), P(0.76f), P(0.22f));
        break;
    case IC_COINS:  // a stack of coins
        fill(kWhite);
        for (int k = 0; k < 3; ++k) {
            const float y = 0.62f - k * 0.20f;
            RoundRect(dc, P(0.14f), P(y), P(0.86f), P(y + 0.30f), P(0.72f), P(0.22f));
        }
        fill(kDetail);
        Ellipse(dc, P(0.30f), P(0.24f), P(0.70f), P(0.34f));
        break;
    default:  // a box
        fill(kWhite);
        RoundRect(dc, P(0.16f), P(0.24f), P(0.84f), P(0.84f), P(0.14f), P(0.14f));
        line(0.16f, 0.44f, 0.84f, 0.44f);
        break;
    }
    DeleteObject(SelectObject(dc, oldBrush));
    SelectObject(dc, oldPen);
    DeleteObject(pen);
}

struct EmbeddedIcon {
    const char* name;
    int w, h;
    const uint32_t* px;  // ARGB, straight alpha, cropped to the item
};
#include "icons_data.h"

static const EmbeddedIcon* OwnIcon(const char* name) {
    for (const EmbeddedIcon& e : kEmbeddedIcons)
        if (!strcmp(e.name, name)) return &e;
    return nullptr;
}

// The item's picture into the line (size x size at x, y), scaled to fit, box-filtered, over the band.
static void PutImage(uint32_t* dst, UINT pitchPx, int x, int y, int size, const EmbeddedIcon& im) {
    const float sc = (float)size / (float)(im.w > im.h ? im.w : im.h);
    const int ow = (int)(im.w * sc + 0.5f), oh = (int)(im.h * sc + 0.5f);
    const int ox0 = x + (size - ow) / 2, oy0 = y + (size - oh) / 2;
    for (int oy = 0; oy < oh; ++oy)
        for (int ox = 0; ox < ow; ++ox) {
            const int sx0 = (int)(ox / sc), sx1 = (int)((ox + 1) / sc), sy0 = (int)(oy / sc), sy1 = (int)((oy + 1) / sc);
            uint32_t r = 0, g = 0, b = 0, a = 0, cnt = 0;
            for (int sy = sy0; sy < (sy1 > sy0 ? sy1 : sy0 + 1) && sy < im.h; ++sy)
                for (int sx = sx0; sx < (sx1 > sx0 ? sx1 : sx0 + 1) && sx < im.w; ++sx) {
                    const uint32_t p = im.px[sy * im.w + sx], pa = p >> 24;
                    r += ((p >> 16) & 0xFF) * pa, g += ((p >> 8) & 0xFF) * pa, b += (p & 0xFF) * pa, a += pa, ++cnt;
                }
            if (!cnt || !a) continue;
            r /= a, g /= a, b /= a;
            const uint32_t ca = a / cnt;
            uint32_t& d = dst[(oy0 + oy) * pitchPx + ox0 + ox];
            const uint32_t ba = d >> 24, oa = ca + ba * (255 - ca) / 255;
            if (!oa) continue;
            d = (oa << 24) | ((r * ca / oa) << 16) | ((g * ca / oa) << 8) | (b * ca / oa);
        }
}

// The icon, drawn 4x and averaged down (GDI's shapes have no anti-aliasing), over the line's pixels at (x, y).
static void PutIcon(uint32_t* dst, UINT pitchPx, int x, int y, int size, const Entry& e) {
    if (const EmbeddedIcon* im = OwnIcon(e.image)) return PutImage(dst, pitchPx, x, y, size, *im);
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

// Renders the current lines into g_tex (lineH pixels per line).
static bool Render(IDirect3DDevice9* dev, UINT lineH) {
    const UINT tw = Pow2(lineH * 24), th = Pow2(lineH * kLines);
    if (g_tex && (g_texDev != dev || g_texW != tw || g_texH != th)) g_tex->Release(), g_tex = nullptr;
    if (!g_tex) {
        if (FAILED(dev->CreateTexture(tw, th, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &g_tex, nullptr))) {
            g_tex = nullptr;
            return false;
        }
        g_texDev = dev, g_texW = tw, g_texH = th;
    }
    g_lineH = lineH;

    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = (LONG)tw, bi.bmiHeader.biHeight = -(LONG)th;  // top-down
    bi.bmiHeader.biPlanes = 1, bi.bmiHeader.biBitCount = 32, bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HDC dc = CreateCompatibleDC(nullptr);
    HBITMAP bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!dc || !bmp || !bits) {
        if (bmp) DeleteObject(bmp);
        if (dc) DeleteDC(dc);
        return false;
    }
    HGDIOBJ oldBmp = SelectObject(dc, bmp);
    memset(bits, 0, tw * th * 4);
    const int fontPx = (int)(lineH * 0.5f);
    HFONT font = CreateFontA(-fontPx, 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                             CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH, "Tahoma");
    HGDIOBJ oldFont = SelectObject(dc, font);
    SetBkMode(dc, TRANSPARENT);
    const LONG pad = (LONG)(lineH * 0.3f), fade = pad * 4;
    const int icon = (int)(lineH * 0.92f);
    // Text first, the icon after it on the right (as in RE Requiem), all in white.
    LONG iconX[kLines] = {};
    SetTextColor(dc, RGB(255, 255, 255));
    for (int i = 0; i < g_count; ++i) {
        const char* s = g_entries[i].text;
        const int len = (int)strlen(s);
        SIZE sz = {};
        GetTextExtentPoint32A(dc, s, len, &sz);
        TextOutA(dc, fade, (int)(i * lineH + (lineH - sz.cy) / 2), s, len);
        iconX[i] = fade + sz.cx + (LONG)(lineH * 0.25f);
        if (iconX[i] + icon > (LONG)tw) iconX[i] = (LONG)tw - icon;
        const LONG x = iconX[i] + icon + pad;
        g_width[i] = x < (LONG)tw ? x : (LONG)tw;
    }
    GdiFlush();

    D3DLOCKED_RECT lr;
    if (SUCCEEDED(g_tex->LockRect(0, &lr, nullptr, 0))) {
        const uint32_t* src = (const uint32_t*)bits;
        auto lumOf = [](uint32_t p) {
            const uint32_t r = (p >> 16) & 0xFF, g = (p >> 8) & 0xFF, b = p & 0xFF;
            return r > g ? (r > b ? r : b) : (g > b ? g : b);
        };
        for (UINT y = 0; y < th; ++y) {
            uint32_t* dst = (uint32_t*)((uint8_t*)lr.pBits + y * lr.Pitch);
            const int line = (int)(y / lineH);
            for (UINT x = 0; x < tw; ++x) {
                const uint32_t p = src[y * tw + x], lum = lumOf(p);
                // Outline: the brightest text pixel within 2 px, dark.
                uint32_t edgeLum = 0;
                for (int dy = -2; dy <= 2 && edgeLum < 255; ++dy) {
                    const int yy = (int)y + dy;
                    if (yy < 0 || yy >= (int)th) continue;
                    for (int dx = -2; dx <= 2; ++dx) {
                        const int xx = (int)x + dx;
                        if (xx < 0 || xx >= (int)tw) continue;
                        const uint32_t v = lumOf(src[yy * tw + xx]);
                        if (v > edgeLum) edgeLum = v;
                    }
                }
                // The band: dark and see-through behind the line, fading out to the left (the line sits on the right).
                uint32_t band = 0;
                if (line < g_count && (LONG)x < g_width[line])
                    band = (LONG)x >= fade ? 120 : (uint32_t)(120 * x / fade);
                uint32_t a = edgeLum * 220 / 255;
                if (band > a) a = band;
                if (lum > a) a = lum;
                // The text's color over black (outline and band).
                const uint32_t r = a ? ((p >> 16) & 0xFF) * 255 / a : 0, g = a ? ((p >> 8) & 0xFF) * 255 / a : 0,
                               b = a ? (p & 0xFF) * 255 / a : 0;
                dst[x] = (a << 24) | (r << 16) | (g << 8) | b;
            }
        }
        for (int i = 0; i < g_count; ++i)
            PutIcon((uint32_t*)lr.pBits, lr.Pitch / 4, iconX[i], (int)(i * lineH + (lineH - icon) / 2), icon, g_entries[i]);
        g_tex->UnlockRect(0);
    }
    SelectObject(dc, oldFont);
    DeleteObject(font);
    SelectObject(dc, oldBmp);
    DeleteObject(bmp);
    DeleteDC(dc);
    return true;
}

struct Vtx {
    float x, y, z, rhw;
    DWORD color;
    float u, v;
};

void DrawTicker(IDirect3DDevice9* dev) {
    if (!g_lockInit || !g_count) return;
    Lock();
    const DWORD now = GetTickCount();
    int drop = 0;
    while (drop < g_count && now - g_entries[drop].t >= kShow) ++drop;
    if (drop) {
        memmove(g_entries, g_entries + drop, sizeof(Entry) * (g_count - drop));
        g_count -= drop;
        g_dirty = true;
    }
    if (!g_count) {
        LeaveCriticalSection(&g_lock);
        return;
    }

    IDirect3DSurface9* bb = nullptr;
    if (FAILED(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) || !bb) {
        LeaveCriticalSection(&g_lock);
        return;
    }
    D3DSURFACE_DESC desc;
    bb->GetDesc(&desc);
    const UINT lineH = desc.Height / 27 > 18 ? desc.Height / 27 : 18;  // 40 px at 1080p
    const float step = lineH * 1.2f;  // a gap between the lines
    if ((g_dirty || !g_tex || g_lineH != lineH || g_texDev != dev) && !Render(dev, lineH)) {
        bb->Release();
        LeaveCriticalSection(&g_lock);
        return;
    }
    g_dirty = false;

    IDirect3DStateBlock9* saved = nullptr;
    if (FAILED(dev->CreateStateBlock(D3DSBT_ALL, &saved)) || !saved) {
        bb->Release();
        LeaveCriticalSection(&g_lock);
        return;
    }
    IDirect3DSurface9* oldRt = nullptr;
    dev->GetRenderTarget(0, &oldRt);
    dev->SetRenderTarget(0, bb);
    const D3DVIEWPORT9 vp = {0, 0, desc.Width, desc.Height, 0.0f, 1.0f};
    dev->SetViewport(&vp);
    dev->SetVertexShader(nullptr);
    dev->SetPixelShader(nullptr);
    dev->SetFVF(D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1);
    dev->SetTexture(0, g_tex);
    dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_MODULATE);
    dev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    dev->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
    dev->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_MODULATE);
    dev->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
    dev->SetTextureStageState(0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);
    dev->SetTextureStageState(0, D3DTSS_TEXCOORDINDEX, 0);
    dev->SetTextureStageState(0, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE);
    dev->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
    dev->SetTextureStageState(1, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
    dev->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_POINT);
    dev->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
    dev->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
    dev->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
    dev->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
    dev->SetSamplerState(0, D3DSAMP_SRGBTEXTURE, FALSE);
    dev->SetRenderState(D3DRS_ZENABLE, D3DZB_FALSE);
    dev->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
    dev->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
    dev->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
    dev->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
    dev->SetRenderState(D3DRS_BLENDOP, D3DBLENDOP_ADD);
    dev->SetRenderState(D3DRS_SEPARATEALPHABLENDENABLE, FALSE);
    dev->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
    dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    dev->SetRenderState(D3DRS_LIGHTING, FALSE);
    dev->SetRenderState(D3DRS_FOGENABLE, FALSE);
    dev->SetRenderState(D3DRS_STENCILENABLE, FALSE);
    dev->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
    dev->SetRenderState(D3DRS_CLIPPING, TRUE);
    dev->SetRenderState(D3DRS_COLORWRITEENABLE, 0xF);
    dev->SetRenderState(D3DRS_SRGBWRITEENABLE, FALSE);
    dev->SetRenderState(D3DRS_FILLMODE, D3DFILL_SOLID);

    if (SUCCEEDED(dev->BeginScene())) {
        // On the right, above the HUD (life gauge and ammo in the lower right); the newest line at the bottom. High enough
        // to clear Ashley's gauge too, which sits above Leon's while she is with him (0.74 covered it).
        const float right = desc.Width - desc.Height * 0.05f, bottom = desc.Height * 0.62f;
        for (int i = 0; i < g_count; ++i) {
            const DWORD age = now - g_entries[i].t, left_ms = kShow - age;
            DWORD a = left_ms >= kFade ? 255 : 255 * left_ms / kFade;
            // A new line fades in and slides in from the right (ease out).
            float slide = 0.0f;
            if (age < kIn) {
                const float k = 1.0f - (float)age / kIn;
                a = a * age / kIn;
                slide = k * k * lineH * 1.5f;
            }
            // The older lines move up to make room for it, just as softly.
            const DWORD newest = now - g_entries[g_count - 1].t;
            const float k = newest < kIn && i < g_count - 1 ? 1.0f - (float)newest / kIn : 0.0f;
            const float lift = k * k * step;
            const float x0 = right + slide - g_width[i] - 0.5f, x1 = right + slide - 0.5f;
            const float y0 = bottom - (float)(g_count - i) * step + lift - 0.5f, y1 = y0 + lineH;
            const float u1 = (float)g_width[i] / g_texW;
            const float v0 = (float)(i * lineH) / g_texH, v1 = (float)((i + 1) * lineH) / g_texH;
            const DWORD col = (a << 24) | 0xFFFFFF;
            const Vtx q[4] = {{x0, y0, 0, 1, col, 0, v0}, {x1, y0, 0, 1, col, u1, v0},
                              {x0, y1, 0, 1, col, 0, v1}, {x1, y1, 0, 1, col, u1, v1}};
            dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, q, sizeof(Vtx));
        }
        dev->EndScene();
    }

    saved->Apply();
    saved->Release();
    if (oldRt) {
        dev->SetRenderTarget(0, oldRt);
        oldRt->Release();
    }
    bb->Release();
    LeaveCriticalSection(&g_lock);
}

// Before a device Reset nothing of ours lives in D3DPOOL_DEFAULT (the texture is managed), so nothing to release.
