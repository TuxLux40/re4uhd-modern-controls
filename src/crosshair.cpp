// Crosshair at the laser's end point, in addition to the laser.
//
// The game's laser update (re4.h kFnLaser) traces the beam from the gun and cuts it at the first hit, leaving the end
// point in kLaserEnd. While it runs with its draw flag set (the player aims a laser weapon), the mod projects that
// point through the rendered camera (CameraControl::Camera_60's eye/target/fovy) and, just before the frame is
// presented, draws a remake-style reticle (dot + four ticks) there with IDirect3DDevice9::Clear rectangles: no
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

static volatile DWORD g_laserTick = 0;  // last frame the laser was drawn
static volatile float g_aimX = 0.0f, g_aimY = 0.0f;  // laser end in NDC (-1..1, y up)
static volatile bool g_aimOnScreen = false;

// --------------------------------------------------------------------------------------------------- laser hook
typedef void(__fastcall* PFN_Laser)(uintptr_t self, void* edx, char draw, char b, char c);
static PFN_Laser g_origLaser = nullptr;

static void ProjectLaserEnd() {
    const uintptr_t cam = Va(kCamCtrl) + cc::Camera + cc::CameraPoint;  // CAMERA_POINT of the rendered view
    const float* eye = (const float*)cam;
    const float* tgt = eye + 3;
    const float fovy = ((const float*)cam)[7];
    const float* p = (const float*)Va(kLaserEnd);

    float f[3] = {tgt[0] - eye[0], tgt[1] - eye[1], tgt[2] - eye[2]};
    float fl = sqrtf(f[0] * f[0] + f[1] * f[1] + f[2] * f[2]);
    if (fl < 1e-3f || !(fovy > 1.0f && fovy < 170.0f)) {
        g_aimOnScreen = false;
        return;
    }
    for (float& v : f) v /= fl;
    // right = forward x worldUp, up = right x forward (the game's handedness: see camera.cpp's yaw convention)
    float r[3] = {-f[2], 0.0f, f[0]};
    const float rl = sqrtf(r[0] * r[0] + r[2] * r[2]);
    if (rl < 1e-4f) {
        g_aimOnScreen = false;
        return;
    }
    r[0] /= rl;
    r[2] /= rl;
    const float u[3] = {r[1] * f[2] - r[2] * f[1], r[2] * f[0] - r[0] * f[2], r[0] * f[1] - r[1] * f[0]};
    const float d[3] = {p[0] - eye[0], p[1] - eye[1], p[2] - eye[2]};
    const float z = d[0] * f[0] + d[1] * f[1] + d[2] * f[2];
    if (z < 10.0f) {
        g_aimOnScreen = false;
        return;
    }
    const float x = d[0] * r[0] + d[1] * r[1] + d[2] * r[2];
    const float y = d[0] * u[0] + d[1] * u[1] + d[2] * u[2];
    const float t = tanf(fovy * 0.5f * 0.01745329252f);
    g_aimX = x / (z * t);  // divided by the aspect ratio when drawn
    g_aimY = y / (z * t);
    g_aimOnScreen = true;
}

static void __fastcall HookLaser(uintptr_t self, void* edx, char draw, char b, char c) {
    g_origLaser(self, edx, draw, b, c);
    if (!draw || !Cfg().crosshair) return;
    ProjectLaserEnd();
    g_laserTick = GetTickCount();
}

// --------------------------------------------------------------------------------------------------- drawing
static void DrawCrosshair(IDirect3DDevice9* dev) {
    if (!g_aimOnScreen || GetTickCount() - g_laserTick > 100) return;
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
    const float aspect = w / h;
    const float ndcX = g_aimX / aspect, ndcY = g_aimY;
    if (fabsf(ndcX) < 1.0f && fabsf(ndcY) < 1.0f) {
        // Remake-style reticle (RE2/RE3/RE4 2019-2023): a small centre dot and four short ticks around it.
        const LONG cx = (LONG)((ndcX * 0.5f + 0.5f) * w);
        const LONG cy = (LONG)((0.5f - ndcY * 0.5f) * h);
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
