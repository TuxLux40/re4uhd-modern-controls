// Pickup ticker: with QuickPickup the everyday pickups come without their screen, so a few lines in small print on the
// right, above the HUD, say what was taken (an icon, "Handgun Ammo x30" with the count in color), like the newer
// Resident Evil games. Each line stays kShow ms and fades out over the last kFade ms (in over the first kIn); at most kLines at a time, the
// newest at the bottom.
// The game has no flat item icons (the case shows small 3D models, SS/item/idm*), so the icons are drawn here: a shape
// per kind of item (cartridges, leaves, grenade, spray, egg, fish, gem, coin), colored after the item's name.
// Drawn from the Present hook (crosshair.cpp): GDI renders the lines into a texture (text with a dark outline, a faint
// dark band behind each line fading out to the left), one alpha-blended quad per line puts them on the back buffer.
// All render states are saved in a state block and put back afterwards.
#include "common.h"
#include "config.h"
#include <d3d9.h>
#include <stdio.h>
#include <string.h>
#include <math.h>

static const DWORD kShow = 4000, kFade = 600, kIn = 250;
static const int kLines = 5;
static const COLORREF kCountColor = RGB(255, 84, 64);

enum Icon : uint8_t { IC_BOX, IC_AMMO, IC_HERB, IC_GRENADE, IC_SPRAY, IC_EGG, IC_FISH, IC_GEM, IC_COIN };

struct Entry {
    char text[2][64];  // drawn one after the other
    uint8_t colored;   // which of the two is the count (drawn in kCountColor)
    Icon icon;
    COLORREF col[3];   // the icon's colors (herbs: one per leaf)
    uint8_t ncol;
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
    if (money) {
        snprintf(e.text[0], sizeof(e.text[0]), "%u", num);
        snprintf(e.text[1], sizeof(e.text[1]), " PTAS");
        e.colored = 0;
    } else {
        snprintf(e.text[0], sizeof(e.text[0]), "%s", name);
        if (num > 1) snprintf(e.text[1], sizeof(e.text[1]), "  x%u", num);
        e.colored = 1;
    }
    e.ncol = 1;
    e.col[0] = RGB(170, 170, 170);
    if (money) {
        e.icon = IC_COIN, e.col[0] = RGB(226, 184, 64);
    } else if (type == 2) {
        e.icon = IC_AMMO, e.col[0] = RGB(206, 164, 64);
    } else if (type == 3) {
        e.icon = IC_GRENADE;
        e.col[0] = strstr(name, "Incendiary") ? RGB(184, 54, 40) : strstr(name, "Flash") ? RGB(206, 206, 206)
                                                                                        : RGB(84, 118, 64);
    } else if (type == 5 || type == 12) {
        e.icon = IC_GEM, e.col[0] = ColorFromName(name, RGB(222, 182, 62));
    } else if (strstr(name, "Herb")) {
        e.icon = IC_HERB, e.ncol = 0;
        const COLORREF g = RGB(62, 172, 72), r = RGB(204, 52, 52), y = RGB(232, 202, 52);
        const char* mix = strstr(name, "Mixed") ? strchr(name, '(') : nullptr;
        if (mix) {
            for (const char* c = mix; *c && *c != ')' && e.ncol < 3; ++c)
                if (*c == 'G' || *c == 'R' || *c == 'Y') e.col[e.ncol++] = *c == 'G' ? g : *c == 'R' ? r : y;
        } else {
            const COLORREF c = strstr(name, "Red") ? r : strstr(name, "Yellow") ? y : g;
            const int n = strstr(name, "X3") ? 3 : strstr(name, "X2") ? 2 : 1;
            for (int i = 0; i < n; ++i) e.col[e.ncol++] = c;
        }
        if (!e.ncol) e.col[e.ncol++] = g;
    } else if (strstr(name, "Spray")) {
        e.icon = IC_SPRAY, e.col[0] = RGB(60, 170, 80);
    } else if (strstr(name, "Egg")) {
        e.icon = IC_EGG;
        e.col[0] = strstr(name, "Gold") ? RGB(232, 192, 60) : strstr(name, "Brown") ? RGB(170, 118, 76)
                                                                                    : RGB(242, 236, 222);
    } else if (strstr(name, "Bass") || strstr(name, "Fish")) {
        e.icon = IC_FISH, e.col[0] = RGB(110, 134, 120);
    } else if (type == 8) {
        e.icon = IC_COIN, e.col[0] = RGB(226, 184, 64);
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
    const COLORREF c = e.col[0];
    switch (e.icon) {
    case IC_AMMO:  // two cartridges
        for (int k = 0; k < 2; ++k) {
            const float x0 = k ? 0.52f : 0.20f, x1 = x0 + 0.28f;
            fill(RGB(186, 108, 60));
            Ellipse(dc, P(x0), P(0.10f), P(x1), P(0.62f));
            fill(c);
            Rectangle(dc, P(x0), P(0.38f), P(x1), P(0.90f));
            fill(Shade(c, 70));
            Rectangle(dc, P(x0 - 0.02f), P(0.82f), P(x1 + 0.02f), P(0.92f));
        }
        break;
    case IC_HERB: {  // one leaf per herb, fanned out
        static const float fan[3][3] = {{0, 0, 0}, {-24, 24, 0}, {-36, 0, 36}};
        const int k = e.ncol;
        HPEN stem = CreatePen(PS_SOLID, w * 2, RGB(70, 96, 40));
        HGDIOBJ op = SelectObject(dc, stem);
        MoveToEx(dc, P(0.5f), P(0.94f), nullptr), LineTo(dc, P(0.5f), P(0.66f));
        SelectObject(dc, op);
        DeleteObject(stem);
        for (int i = 0; i < k; ++i) {
            rot(fan[k - 1][i], 0.5f, 0.70f);
            fill(e.col[i]);
            Ellipse(dc, P(0.36f), P(0.06f), P(0.64f), P(0.74f));
            MoveToEx(dc, P(0.5f), P(0.16f), nullptr), LineTo(dc, P(0.5f), P(0.68f));
            unrot();
        }
        break;
    }
    case IC_GRENADE:
        fill(Shade(c, 80));
        Rectangle(dc, P(0.40f), P(0.14f), P(0.60f), P(0.32f));
        Ellipse(dc, P(0.58f), P(0.10f), P(0.76f), P(0.26f));  // the pin's ring
        fill(c);
        Ellipse(dc, P(0.20f), P(0.26f), P(0.80f), P(0.92f));
        MoveToEx(dc, P(0.30f), P(0.59f), nullptr), LineTo(dc, P(0.70f), P(0.59f));
        break;
    case IC_SPRAY:
        fill(c);
        Rectangle(dc, P(0.40f), P(0.08f), P(0.60f), P(0.26f));
        fill(RGB(228, 228, 224));
        RoundRect(dc, P(0.28f), P(0.22f), P(0.72f), P(0.94f), P(0.12f), P(0.12f));
        fill(c);
        SelectObject(dc, GetStockObject(NULL_PEN));
        Rectangle(dc, P(0.45f), P(0.42f), P(0.55f), P(0.76f));
        Rectangle(dc, P(0.33f), P(0.54f), P(0.67f), P(0.64f));
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
    case IC_FISH: {
        fill(Shade(c, 85));
        const POINT tail[3] = {{P(0.66f), P(0.50f)}, {P(0.94f), P(0.28f)}, {P(0.94f), P(0.72f)}};
        Polygon(dc, tail, 3);
        fill(c);
        Ellipse(dc, P(0.06f), P(0.30f), P(0.74f), P(0.70f));
        fill(RGB(18, 18, 18));
        Ellipse(dc, P(0.18f), P(0.42f), P(0.25f), P(0.49f));
        break;
    }
    case IC_GEM: {
        fill(c);
        const POINT gem[5] = {{P(0.28f), P(0.16f)}, {P(0.72f), P(0.16f)}, {P(0.92f), P(0.40f)},
                              {P(0.50f), P(0.92f)}, {P(0.08f), P(0.40f)}};
        Polygon(dc, gem, 5);
        fill(Shade(c, 130));
        const POINT top[4] = {{P(0.28f), P(0.16f)}, {P(0.72f), P(0.16f)}, {P(0.62f), P(0.40f)}, {P(0.38f), P(0.40f)}};
        Polygon(dc, top, 4);
        MoveToEx(dc, P(0.08f), P(0.40f), nullptr), LineTo(dc, P(0.92f), P(0.40f));
        MoveToEx(dc, P(0.38f), P(0.40f), nullptr), LineTo(dc, P(0.50f), P(0.92f));
        MoveToEx(dc, P(0.62f), P(0.40f), nullptr), LineTo(dc, P(0.50f), P(0.92f));
        break;
    }
    case IC_COIN:
        fill(c);
        Ellipse(dc, P(0.10f), P(0.10f), P(0.90f), P(0.90f));
        fill(Shade(c, 80));
        Ellipse(dc, P(0.24f), P(0.24f), P(0.76f), P(0.76f));
        fill(c);
        SelectObject(dc, GetStockObject(NULL_PEN));
        Rectangle(dc, P(0.42f), P(0.33f), P(0.50f), P(0.68f));
        Ellipse(dc, P(0.42f), P(0.33f), P(0.62f), P(0.53f));
        SelectObject(dc, pen);
        break;
    default:
        fill(c);
        RoundRect(dc, P(0.16f), P(0.24f), P(0.84f), P(0.84f), P(0.14f), P(0.14f));
        MoveToEx(dc, P(0.16f), P(0.44f), nullptr), LineTo(dc, P(0.84f), P(0.44f));
        break;
    }
    DeleteObject(SelectObject(dc, oldBrush));
    SelectObject(dc, oldPen);
    DeleteObject(pen);
}

// The icon, drawn 4x and averaged down (GDI's shapes have no anti-aliasing), over the line's pixels at (x, y).
static void PutIcon(uint32_t* dst, UINT pitchPx, int x, int y, int size, const Entry& e) {
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
    const LONG textX = fade + icon + (LONG)(lineH * 0.25f);
    for (int i = 0; i < g_count; ++i) {
        LONG x = textX;
        for (int k = 0; k < 2; ++k) {
            const char* s = g_entries[i].text[k];
            const int len = (int)strlen(s);
            if (!len) continue;
            SIZE sz = {};
            GetTextExtentPoint32A(dc, s, len, &sz);
            SetTextColor(dc, k == g_entries[i].colored ? kCountColor : RGB(255, 255, 255));
            TextOutA(dc, x, (int)(i * lineH + (lineH - sz.cy) / 2), s, len);
            x += sz.cx;
        }
        g_width[i] = x + pad < (LONG)tw ? x + pad : (LONG)tw;
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
            PutIcon((uint32_t*)lr.pBits, lr.Pitch / 4, fade, (int)(i * lineH + (lineH - icon) / 2), icon, g_entries[i]);
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
        // On the right, above the HUD (life gauge and ammo in the lower right); the newest line at the bottom.
        const float right = desc.Width - desc.Height * 0.05f, bottom = desc.Height * 0.74f;
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
