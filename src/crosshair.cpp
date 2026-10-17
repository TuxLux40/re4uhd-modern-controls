// Crosshair at the laser's end point, in addition to the laser.
//
// The game's laser update (re4.h kFnLaser) traces the beam from the gun and cuts it at the first hit, leaving the end
// point in kLaserEnd. While it runs with its draw flag set (the player aims a laser weapon), the mod projects that
// point the way the game does (camera matrix and FOV from CameraControl::Camera_60, the game's projection aspect)
// just before the frame is presented, and draws a remake-style reticle (dot + four ticks) there with IDirect3DDevice9::Clear rectangles: no
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

// The laser's end point through the game's own camera. CameraControl::Camera_60 (copied to the global render camera
// every frame) holds at +0 the camera-to-world matrix (GX 3x4: rotation columns, eye in the last column; the camera
// looks down -Z, roll included) and its field of view at +0xC0. The game projects with
// MTXPerspective(fovy, *kProjAspect) and stretches that to the full screen width, so the horizontal scale comes from
// that aspect (4:3 by default), not from the screen's. The height is cropped to the screen's shape instead: a 16:9
// screen shows the 4:3 width of view and 3/4 of its height (measured: off by exactly that factor otherwise).
// Returns NDC (-1..1, y up).
static bool ProjectLaserEnd(float screenAspect, float* ndcX, float* ndcY) {
    const uintptr_t cam = Va(kCamCtrl) + cc::Camera;
    const float* m = (const float*)cam;
    const float fovy = At<float>(cam, 0xC0);
    const float aspect = At<float>(Va(kProjAspect), 0);
    if (!(fovy > 1.0f && fovy < 170.0f) || !(aspect > 0.5f && aspect < 8.0f)) return false;
    const float* p = (const float*)Va(kLaserEnd);
    const float d[3] = {p[0] - m[3], p[1] - m[7], p[2] - m[11]};
    // camera space = R^T * d (columns of R are the camera axes in world space)
    const float x = m[0] * d[0] + m[4] * d[1] + m[8] * d[2];
    const float y = m[1] * d[0] + m[5] * d[1] + m[9] * d[2];
    const float z = m[2] * d[0] + m[6] * d[1] + m[10] * d[2];
    if (!(-z > 10.0f)) return false;
    const float t = tanf(fovy * 0.5f * 0.01745329252f);
    *ndcX = x / (-z * t * aspect);
    *ndcY = y / (-z * t * aspect / screenAspect);
    static bool logged = false;
    if (!logged) {
        logged = true;
        Log("crosshair: fovy %.1f aspect %.3f screen %.3f -> ndc %.3f %.3f", fovy, aspect, screenAspect, *ndcX, *ndcY);
    }
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

    static float screenAspect = 16.0f / 9.0f;  // from the back buffer, below
    float nx, ny;
    if (GetTickCount() - g_laserTick > 100 || !ProjectLaserEnd(screenAspect, &nx, &ny)) {
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
    if (w > 0.0f && h > 0.0f) screenAspect = w / h;
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
