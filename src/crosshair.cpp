// Crosshair at the laser's end point, in addition to the laser.
//
// The game's laser update (re4.h kFnLaser) traces the beam from the gun and cuts it at the first hit, leaving the end
// point in kLaserEnd. While it runs with its draw flag set (the player aims a laser weapon), the mod projects that
// point through the game's own view and projection matrices (CameraControl::Camera_60) just before the frame is
// presented, and draws a remake-style reticle (dot + four ticks) there with IDirect3DDevice9::Clear rectangles: no
// shaders, no render states touched. It sits on the laser by construction: same 3D point, same camera; no laser, no
// reticle.
//
// Device access: Direct3DCreate9 is hooked to catch IDirect3D9::CreateDevice, which hooks the device's Present.
#include "common.h"
#include "re4.h"
#include "config.h"
#include "camera.h"
#include <d3d9.h>
#include <math.h>
#include "MinHook.h"

using namespace re4;

static volatile DWORD g_laserTick = 0;  // last frame the game drew the laser

// --------------------------------------------------------------------------------------------------- laser hook
typedef void(__fastcall* PFN_Laser)(uintptr_t self, void* edx, char draw, char b, char c);
static PFN_Laser g_origLaser = nullptr;

static void __fastcall HookLaser(uintptr_t self, void* edx, char draw, char b, char c) {
    g_origLaser(self, edx, draw, b, c);
    if (draw && Cfg().crosshair) g_laserTick = GetTickCount();
}

// The laser's end point through the game's own camera, CameraControl::Camera_60 (move() copies it to the global
// render camera every frame): two 3x4 matrices at +0 and +0x30 and a 4x4 projection at +0x64. Which of the two is
// world->view and how the projection is laid out is settled at runtime: the camera's own look-at target must land on
// the screen centre, and the first combination that does that is used from then on.
static void Mul34(const float* m, const float* p, float* out) {
    for (int r = 0; r < 3; ++r) out[r] = m[r * 4] * p[0] + m[r * 4 + 1] * p[1] + m[r * 4 + 2] * p[2] + m[r * 4 + 3];
}
static bool Proj(const float* pm, bool transposed, const float* c, float* nx, float* ny) {
    float x, y, w;
    if (!transposed) {
        x = pm[0] * c[0] + pm[1] * c[1] + pm[2] * c[2] + pm[3];
        y = pm[4] * c[0] + pm[5] * c[1] + pm[6] * c[2] + pm[7];
        w = pm[12] * c[0] + pm[13] * c[1] + pm[14] * c[2] + pm[15];
    } else {
        x = pm[0] * c[0] + pm[4] * c[1] + pm[8] * c[2] + pm[12];
        y = pm[1] * c[0] + pm[5] * c[1] + pm[9] * c[2] + pm[13];
        w = pm[3] * c[0] + pm[7] * c[1] + pm[11] * c[2] + pm[15];
    }
    if (!(w > 1e-3f)) return false;
    *nx = x / w;
    *ny = y / w;
    return true;
}

static int g_layout = -1;  // bit 0: view matrix at +0x30 instead of +0; bit 1: projection transposed

static bool ProjectLaserEnd(float* ndcX, float* ndcY) {
    const uintptr_t cam = Va(kCamCtrl) + cc::Camera;
    const float* pm = (const float*)(cam + 0x64);
    const float* tgt = (const float*)(cam + cc::CameraPoint + 0xC);
    float c[3], nx, ny;
    if (g_layout < 0) {
        static int tries = 0;
        for (int l = 0; l < 4; ++l) {
            Mul34((const float*)(cam + ((l & 1) ? 0x30 : 0)), tgt, c);
            const bool ok = Proj(pm, (l & 2) != 0, c, &nx, &ny);
            if (tries < 3) Log("crosshair: layout %d target -> %s %.3f %.3f", l, ok ? "ok" : "behind", ok ? nx : 0.0f, ok ? ny : 0.0f);
            if (ok && fabsf(nx) < 0.05f && fabsf(ny) < 0.05f) {
                g_layout = l;
                break;
            }
        }
        if (tries++ < 3) {
            const float* m0 = (const float*)cam;
            const float* m1 = (const float*)(cam + 0x30);
            Log("crosshair: m0 %.2f %.2f %.2f %.0f | %.2f %.2f %.2f %.0f | %.2f %.2f %.2f %.0f", m0[0], m0[1], m0[2], m0[3],
                m0[4], m0[5], m0[6], m0[7], m0[8], m0[9], m0[10], m0[11]);
            Log("crosshair: m1 %.2f %.2f %.2f %.0f | %.2f %.2f %.2f %.0f | %.2f %.2f %.2f %.0f", m1[0], m1[1], m1[2], m1[3],
                m1[4], m1[5], m1[6], m1[7], m1[8], m1[9], m1[10], m1[11]);
            Log("crosshair: proj %.3f %.3f %.3f %.3f | %.3f %.3f %.3f %.3f | %.3f %.3f %.3f %.3f | %.3f %.3f %.3f %.3f",
                pm[0], pm[1], pm[2], pm[3], pm[4], pm[5], pm[6], pm[7], pm[8], pm[9], pm[10], pm[11], pm[12], pm[13],
                pm[14], pm[15]);
        }
        if (g_layout < 0) return false;
        Log("crosshair: using layout %d", g_layout);
    }
    Mul34((const float*)(cam + ((g_layout & 1) ? 0x30 : 0)), (const float*)Va(kLaserEnd), c);
    if (!Proj(pm, (g_layout & 2) != 0, c, ndcX, ndcY)) return false;
    return true;
}

// --------------------------------------------------------------------------------------------------- drawing
static void DrawCrosshair(IDirect3DDevice9* dev) {
    static bool visible = false;
    static float sx = 0.0f, sy = 0.0f;  // smoothed NDC
    static LARGE_INTEGER last = {}, freq = {};
    if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    const float dt = last.QuadPart ? (float)(now.QuadPart - last.QuadPart) / (float)freq.QuadPart : 0.0f;
    last = now;

    float nx, ny;
    if (GetTickCount() - g_laserTick > 100 || !ProjectLaserEnd(&nx, &ny)) {
        visible = false;
        return;
    }
    // RE4 sways the aim, so the laser end shakes. The reticle follows it smoothed (~60 ms): steady like the remakes',
    // still on the average aim point. It snaps when the aim starts.
    if (!visible) {
        sx = nx;
        sy = ny;
        visible = true;
    } else {
        const float k = 1.0f - expf(-dt / 0.06f);
        sx += (nx - sx) * k;
        sy += (ny - sy) * k;
    }
    if (fabsf(sx) >= 1.0f || fabsf(sy) >= 1.0f) return;

    IDirect3DSurface9* bb = nullptr;
    if (FAILED(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) || !bb) return;
    D3DSURFACE_DESC desc;
    bb->GetDesc(&desc);
    IDirect3DSurface9* oldRt = nullptr;
    dev->GetRenderTarget(0, &oldRt);
    D3DVIEWPORT9 oldVp;
    dev->GetViewport(&oldVp);
    dev->SetRenderTarget(0, bb);

    const float w = (float)desc.Width, h = (float)desc.Height;
    {
        // Remake-style reticle (RE2/RE3/RE4 2019-2023): a small centre dot and four short ticks around it.
        const LONG cx = (LONG)((sx * 0.5f + 0.5f) * w);
        const LONG cy = (LONG)((0.5f - sy * 0.5f) * h);
        const LONG th = h >= 1400 ? 3 : 2;              // line thickness and dot size
        const LONG len = (LONG)(h * 0.008f) + 1;        // tick length
        const LONG gap = (LONG)(h * 0.011f) + 2;        // centre to the inner end of a tick
        const LONG a = th / 2, b = th - a;              // splits the thickness around the centre line
        const D3DRECT marks[5] = {
            {cx - a, cy - a, cx + b, cy + b},                  // dot
            {cx - gap - len, cy - a, cx - gap, cy + b},        // left
            {cx + gap, cy - a, cx + gap + len, cy + b},        // right
            {cx - a, cy - gap - len, cx + b, cy - gap},        // top
            {cx - a, cy + gap, cx + b, cy + gap + len},        // bottom
        };
        D3DRECT edge[5];
        for (int i = 0; i < 5; ++i) edge[i] = {marks[i].x1 - 1, marks[i].y1 - 1, marks[i].x2 + 1, marks[i].y2 + 1};
        dev->Clear(5, edge, D3DCLEAR_TARGET, D3DCOLOR_ARGB(255, 30, 30, 30), 1.0f, 0);
        dev->Clear(5, marks, D3DCLEAR_TARGET, D3DCOLOR_ARGB(255, 245, 245, 245), 1.0f, 0);
    }

    if (oldRt) {
        dev->SetRenderTarget(0, oldRt);
        oldRt->Release();
    }
    dev->SetViewport(&oldVp);
    bb->Release();
}

// --------------------------------------------------------------------------------------------------- D3D9 hooks
typedef HRESULT(STDMETHODCALLTYPE* PFN_Present)(IDirect3DDevice9*, const RECT*, const RECT*, HWND, const RGNDATA*);
static PFN_Present g_origPresent = nullptr;

static HRESULT STDMETHODCALLTYPE HookPresent(IDirect3DDevice9* dev, const RECT* src, const RECT* dst, HWND wnd,
                                             const RGNDATA* dirty) {
    DrawCrosshair(dev);
    return g_origPresent(dev, src, dst, wnd, dirty);
}

typedef HRESULT(STDMETHODCALLTYPE* PFN_CreateDevice)(IDirect3D9*, UINT, D3DDEVTYPE, HWND, DWORD,
                                                     D3DPRESENT_PARAMETERS*, IDirect3DDevice9**);
static PFN_CreateDevice g_origCreateDevice = nullptr;

static HRESULT STDMETHODCALLTYPE HookCreateDevice(IDirect3D9* d3d, UINT adapter, D3DDEVTYPE type, HWND wnd,
                                                  DWORD flags, D3DPRESENT_PARAMETERS* pp, IDirect3DDevice9** out) {
    const HRESULT hr = g_origCreateDevice(d3d, adapter, type, wnd, flags, pp, out);
    if (SUCCEEDED(hr) && out && *out && !g_origPresent) {
        void* present = (*(void***)*out)[17];  // IDirect3DDevice9::Present
        if (MH_CreateHook(present, (void*)HookPresent, (void**)&g_origPresent) == MH_OK &&
            MH_EnableHook(present) == MH_OK)
            Log("crosshair: hooked Present at %p", present);
        else
            Log("crosshair: could not hook Present");
    }
    return hr;
}

typedef IDirect3D9*(WINAPI* PFN_Direct3DCreate9)(UINT);
static PFN_Direct3DCreate9 g_origCreate9 = nullptr;

static IDirect3D9* WINAPI HookDirect3DCreate9(UINT sdk) {
    IDirect3D9* d3d = g_origCreate9(sdk);
    if (d3d && !g_origCreateDevice) {
        void* create = (*(void***)d3d)[16];  // IDirect3D9::CreateDevice
        if (MH_CreateHook(create, (void*)HookCreateDevice, (void**)&g_origCreateDevice) == MH_OK)
            MH_EnableHook(create);
    }
    return d3d;
}

void InitCrosshairHooks() {
    void* laser = (void*)Va(kFnLaser);
    if (SafeEqual(laser, kSigLaser, sizeof(kSigLaser)) &&
        MH_CreateHook(laser, (void*)HookLaser, (void**)&g_origLaser) == MH_OK && MH_EnableHook(laser) == MH_OK) {
        Log("hook LaserUpdate at %p", laser);
    } else {
        Log("hook LaserUpdate: unexpected code at %p, no crosshair", laser);
        return;
    }
    HMODULE d3d9 = GetModuleHandleA("d3d9.dll");
    void* create = d3d9 ? (void*)GetProcAddress(d3d9, "Direct3DCreate9") : nullptr;
    if (!create || MH_CreateHook(create, (void*)HookDirect3DCreate9, (void**)&g_origCreate9) != MH_OK ||
        MH_EnableHook(create) != MH_OK) {
        Log("crosshair: could not hook Direct3DCreate9");
        return;
    }
    Log("crosshair: waiting for the game's D3D9 device");
}
