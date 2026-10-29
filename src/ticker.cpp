// Pickup ticker PNG icons only. Place icons/ next to xinput1_3.dll.
#include "common.h"
#include "config.h"
#include <d3d9.h>
#include <gdiplus.h>
#pragma comment(lib, "gdiplus.lib")
#include <stdio.h>
#include <string.h>
#include <math.h>

static const DWORD kShow = 4000, kFade = 600, kIn = 250;
static const int kLines = 5;
static const COLORREF kWhite = RGB(236, 234, 228);

enum AmmoBox : uint8_t { AB_HANDGUN, AB_SHOTGUN, AB_RIFLE, AB_TMP, AB_MAGNUM, AB_HANDCANNON };
enum Icon : uint8_t { IC_BOX, IC_AMMO, IC_CRATE, IC_ARROWS, IC_GRENADE, IC_HERB, IC_SPRAY, IC_EGG, IC_FISH, IC_GEM, IC_CROWN, IC_RING, IC_BAR, IC_TREASURE, IC_COINS };

struct Entry { char text[64]; Icon icon; COLORREF col; uint8_t kind; DWORD t; };
static Entry g_entries[kLines];
static int g_count = 0;
static bool g_dirty = false;
static CRITICAL_SECTION g_lock;
static bool g_lockInit = false;
static IDirect3DTexture9* g_tex = nullptr;
static IDirect3DDevice9* g_texDev = nullptr;
static UINT g_texW = 0, g_texH = 0, g_lineH = 0;
static LONG g_width[kLines] = {};

static void Lock() { if (!g_lockInit) InitializeCriticalSection(&g_lock), g_lockInit = true; EnterCriticalSection(&g_lock); }
static UINT Pow2(UINT v) { UINT p = 1; while (p < v) p <<= 1; return p; }

static COLORREF ColorFromName(const char* n, COLORREF dflt) {
    if (strstr(n, "Ruby") || strstr(n, "Red")) return RGB(214, 44, 52);
    if (strstr(n, "Emerald") || strstr(n, "Green")) return RGB(46, 176, 92);
    if (strstr(n, "Sapphire") || strstr(n, "Blue")) return RGB(58, 98, 214);
    if (strstr(n, "Yellow")) return RGB(232, 202, 46);
    if (strstr(n, "Purple") || strstr(n, "Spinel")) return RGB(168, 78, 196);
    if (strstr(n, "Pearl")) return RGB(236, 232, 222);
    return dflt;
}

void TickerItem(uint32_t id, uint32_t num, uint8_t type, const char* name) {
    if (!Cfg().pickupTicker) return;
    Entry e = {};
    const bool money = type == 8 && id != 0x73 && id != 0x75;
    if (money) snprintf(e.text, sizeof(e.text), "%u PTAS", num);
    else if (num > 1) snprintf(e.text, sizeof(e.text), "%s  x%u", name, num);
    else snprintf(e.text, sizeof(e.text), "%s", name);
    auto has = [&](const char* s) { return strstr(name, s) != nullptr; };
    e.col = kWhite;
    if (money || type == 8) e.icon = IC_COINS;
    else if (type == 2) {
        e.icon = IC_AMMO;
        if (has("Dart")) e.icon = IC_CRATE;
        else if (has("Arrow") || has("Bolt")) e.icon = IC_ARROWS;
        else if (has("Shotgun")) e.kind = AB_SHOTGUN;
        else if (has("Rifle")) e.kind = AB_RIFLE;
        else if (has("TMP") || has("Typewriter")) e.kind = AB_TMP;
        else if (has("Magnum")) e.kind = AB_MAGNUM;
        else if (has("Handcannon")) e.kind = AB_HANDCANNON;
        else e.kind = AB_HANDGUN;
    } else if (type == 3) {
        e.icon = IC_GRENADE;
        e.col = has("Incendiary") ? RGB(170, 52, 44) : has("Flash") ? RGB(52, 76, 150) : RGB(86, 110, 66);
    } else if (type == 5 || type == 12) {
        if (has("Crown") && !has("Jewel")) e.icon = IC_CROWN;
        else if (has("Ruby") || has("Emerald") || has("Sapphire") || has("Spinel") || has("Velvet") || has("Catseye") || has("Gem") || has("Jewel"))
            e.icon = IC_GEM, e.col = ColorFromName(name, RGB(222, 182, 62));
        else if (has("Pendant") || has("Bangle") || has("Ring")) e.icon = IC_RING;
        else if (has("Gold Bar")) e.icon = IC_BAR;
        else e.icon = IC_TREASURE;
    } else if (has("Herb")) {
        e.icon = IC_HERB;
        e.col = has("Red") ? RGB(204, 52, 52) : has("Yellow") ? RGB(232, 202, 52) : RGB(62, 172, 72);
    } else if (has("Spray")) e.icon = IC_SPRAY;
    else if (has("Egg")) {
        e.icon = IC_EGG;
        e.col = has("Gold") ? RGB(232, 192, 60) : has("Brown") ? RGB(170, 118, 76) : RGB(242, 236, 222);
    } else if (has("Bass") || has("Fish")) e.icon = IC_FISH;
    Lock();
    if (g_count == kLines) { memmove(g_entries, g_entries + 1, sizeof(Entry) * (kLines - 1)); --g_count; }
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

static void PutIcon(uint32_t* dst, UINT pitchPx, int x, int y, int size, const Entry& e) {
    const char* fn = IconFile(e);
    if (!fn) return;
    char path[MAX_PATH];
    GetModuleFileNameA(nullptr, path, MAX_PATH);
    char* slash = strrchr(path, '\\');
    if (slash) *slash = 0;
    strcat(path, "\\icons\\");
    strcat(path, fn);
    static ULONG_PTR gdiplusToken = 0;
    if (!gdiplusToken) {
        Gdiplus::GdiplusStartupInput gsi;
        Gdiplus::GdiplusStartup(&gdiplusToken, &gsi, nullptr);
    }
    Gdiplus::Bitmap* bmp = Gdiplus::Bitmap::FromFile(path);
    if (!bmp || bmp->GetLastStatus() != Gdiplus::Ok) { if (bmp) delete bmp; return; }
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
                if ((p >> 24) == 0) continue;
                uint32_t& d = dst[(y + oy) * pitchPx + x + ox];
                uint32_t ca = p >> 24;
                uint32_t ba = d >> 24;
                uint32_t oa = ca + ba * (255 - ca) / 255;
                if (!oa) continue;
                uint32_t r = (p >> 16) & 0xFF, gg = (p >> 8) & 0xFF, b = p & 0xFF;
                d = (oa << 24) | ((r * ca / oa) << 16) | ((gg * ca / oa) << 8) | (b * ca / oa);
            }
        SelectObject(dc, old);
        DeleteObject(dib);
    }
    if (dc) DeleteDC(dc);
    delete bmp;
}

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
    bi.bmiHeader.biWidth = (LONG)tw, bi.bmiHeader.biHeight = -(LONG)th;
    bi.bmiHeader.biPlanes = 1, bi.bmiHeader.biBitCount = 32, bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HDC dc = CreateCompatibleDC(nullptr);
    HBITMAP bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!dc || !bmp || !bits) { if (bmp) DeleteObject(bmp); if (dc) DeleteDC(dc); return false; }
    HGDIOBJ oldBmp = SelectObject(dc, bmp);
    memset(bits, 0, tw * th * 4);
    const int fontPx = (int)(lineH * 0.5f);
    HFONT font = CreateFontA(-fontPx, 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH, "Tahoma");
    HGDIOBJ oldFont = SelectObject(dc, font);
    SetBkMode(dc, TRANSPARENT);
    const LONG pad = (LONG)(lineH * 0.3f), fade = pad * 4;
    const int icon = (int)(lineH * 0.92f);
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
                uint32_t band = 0;
                if (line < g_count && (LONG)x < g_width[line])
                    band = (LONG)x >= fade ? 120 : (uint32_t)(120 * x / fade);
                uint32_t a = edgeLum * 220 / 255;
                if (band > a) a = band;
                if (lum > a) a = lum;
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

struct Vtx { float x, y, z, rhw; DWORD color; float u, v; };

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
    if (!g_count) { LeaveCriticalSection(&g_lock); return; }
    IDirect3DSurface9* bb = nullptr;
    if (FAILED(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) || !bb) { LeaveCriticalSection(&g_lock); return; }
    D3DSURFACE_DESC desc;
    bb->GetDesc(&desc);
    const UINT lineH = desc.Height / 27 > 18 ? desc.Height / 27 : 18;
    const float step = lineH * 1.2f;
    if ((g_dirty || !g_tex || g_lineH != lineH || g_texDev != dev) && !Render(dev, lineH)) {
        bb->Release();
        LeaveCriticalSection(&g_lock);
        return;
    }
    g_dirty = false;
    IDirect3DStateBlock9* saved = nullptr;
    if (FAILED(dev->CreateStateBlock(D3DSBT_ALL, &saved)) || !saved) { bb->Release(); LeaveCriticalSection(&g_lock); return; }
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
    dev->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
    dev->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
    dev->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
    dev->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
    dev->SetRenderState(D3DRS_ZENABLE, FALSE);
    dev->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
    dev->SetRenderState(D3DRS_LIGHTING, FALSE);
    dev->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
    dev->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
    dev->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
    dev->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
    dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    dev->SetRenderState(D3DRS_FOGENABLE, FALSE);
    dev->SetRenderState(D3DRS_STENCILENABLE, FALSE);
    dev->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
    dev->SetRenderState(D3DRS_CLIPPING, TRUE);
    dev->SetRenderState(D3DRS_COLORWRITEENABLE, 0xF);
    dev->SetRenderState(D3DRS_SRGBWRITEENABLE, FALSE);
    dev->SetRenderState(D3DRS_FILLMODE, D3DFILL_SOLID);
    if (SUCCEEDED(dev->BeginScene())) {
        const float right = desc.Width - desc.Height * 0.05f, bottom = desc.Height * 0.62f;
        for (int i = 0; i < g_count; ++i) {
            const DWORD age = now - g_entries[i].t, left_ms = kShow - age;
            DWORD a = left_ms >= kFade ? 255 : 255 * left_ms / kFade;
            float slide = 0.0f;
            if (age < kIn) {
                const float k = 1.0f - (float)age / kIn;
                a = a * age / kIn;
                slide = k * k * lineH * 1.5f;
            }
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
    if (oldRt) { dev->SetRenderTarget(0, oldRt); oldRt->Release(); }
    bb->Release();
    LeaveCriticalSection(&g_lock);
}
