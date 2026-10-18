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
static float g_laserYaw = 0.0f;         // Leon's yaw and position (x, z) at that moment
static float g_laserPivot[2] = {};

// --------------------------------------------------------------------------------------------------- laser hook
typedef void(__fastcall* PFN_Laser)(uintptr_t self, void* edx, char draw, char b, char c);
static PFN_Laser g_origLaser = nullptr;

static void __fastcall HookLaser(uintptr_t self, void* edx, char draw, char b, char c) {
    g_origLaser(self, edx, draw, b, c);
    if (draw) {
        g_laserTick = GetTickCount();
        // Leon's heading when the laser was computed: the camera may run a frame later, after he turned further.
        const uintptr_t pl = *(uintptr_t*)Va(kPlayerPtr);
        if (pl >= 0x10000 && Readable((void*)pl, 0x200)) {
            g_laserYaw = At<float>(pl, pl::Ang + 4);
            g_laserPivot[0] = At<float>(pl, pl::Pos);
            g_laserPivot[1] = At<float>(pl, pl::Pos + 8);
        }
    }
}

static volatile DWORD g_centeredTick = 0;  // last frame the camera was turned to look at the aim point
void SetAimCentered() { g_centeredTick = GetTickCount(); }
bool AimCentered() { return GetTickCount() - g_centeredTick < 100; }

// The laser's end point through the game's own camera. CameraControl::Camera_60 (copied to the global render camera
// every frame) holds at +0 the camera-to-world matrix (GX 3x4: rotation columns, eye in the last column; the camera
// looks down -Z, roll included) and its field of view at +0xC0. The game projects with
// MTXPerspective(fovy, *kProjAspect) and stretches that to the full screen width, so the horizontal scale comes from
// that aspect (4:3 by default), not from the screen's. The height is cropped to the screen's shape instead: a 16:9
// screen shows the 4:3 width of view and 3/4 of its height (measured: off by exactly that factor otherwise).
// Returns NDC (-1..1, y up).
// The point the reticle marks without the centered aim: the laser's first hit (the gun's own line).
bool GunLine(float* start, float* hit);
float SpreadRadius();
bool AimOnTarget();
bool CenteredView();
bool FreeCameraActive();
static void AimPoint(float* out) {
    float s[3], e[3];
    if (GunLine(s, e)) {
        out[0] = e[0], out[1] = e[1], out[2] = e[2];
        return;
    }
    const float* le = (const float*)Va(kLaserEnd);
    out[0] = le[0], out[1] = le[1], out[2] = le[2];
}

// For the camera: the point the crosshair marks while the laser is shown.
bool LaserAimPoint(float* out, float* yaw, float* pivotXZ) {
    if (GetTickCount() - g_laserTick > 100) return false;
    AimPoint(out);
    *yaw = g_laserYaw;
    pivotXZ[0] = g_laserPivot[0];
    pivotXZ[1] = g_laserPivot[1];
    return true;
}

static bool ProjectLaserEnd(float screenAspect, float* ndcX, float* ndcY) {
    const uintptr_t cam = Va(kCamCtrl) + cc::Camera;
    const float* m = (const float*)cam;
    const float fovy = At<float>(cam, 0xC0);
    const float aspect = At<float>(Va(kProjAspect), 0);
    if (!(fovy > 1.0f && fovy < 170.0f) || !(aspect > 0.5f && aspect < 8.0f)) return false;
    float p[3];
    AimPoint(p);
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
    static float screenAspect = 16.0f / 9.0f;  // from the back buffer, below
    // Centered aim: the reticle stays in the middle of the screen from the moment Leon raises the gun (the laser and the
    // shots go there). Otherwise it sits on the laser's end.
    float nx = 0.0f, ny = 0.0f;
    const bool centered = CenteredView();
    const bool aiming = GetTickCount() - g_laserTick <= 100;
    // Outside of aiming, with the free camera: just the dot in the middle, where the aim will start.
    const bool dotOnly = !aiming && Cfg().centeredAim && Cfg().centerDot && FreeCameraActive();
    if (!Cfg().crosshair || (!aiming && !dotOnly) || (aiming && !centered && !ProjectLaserEnd(screenAspect, &nx, &ny)))
        return;
    if (fabsf(nx) >= 1.0f || fabsf(ny) >= 1.0f) return;

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
        const LONG cx = (LONG)((nx * 0.5f + 0.5f) * w);
        const LONG cy = (LONG)((0.5f - ny * 0.5f) * h);
        const LONG th = h >= 1400 ? 3 : 2;              // line thickness and dot size
        const LONG len = (LONG)(h * 0.008f) + 1;        // tick length
        LONG gap = (LONG)(h * 0.011f) + 2;              // centre to the inner end of a tick
        // Centered aim: the ticks show the spread (remake style): they move out while Leon walks, turns or fires and
        // come back in while he holds still.
        if (centered) {
            const uintptr_t cam = Va(kCamCtrl) + cc::Camera;
            const float fovy = At<float>(cam, 0xC0), aspect = At<float>(Va(kProjAspect), 0);
            const float r = SpreadRadius();
            if (r > 0.0f && fovy > 1.0f && fovy < 170.0f && aspect > 0.5f && aspect < 8.0f) {
                const float px = tanf(r) / (tanf(fovy * 0.5f * 0.01745329252f) * aspect) * w * 0.5f;
                gap += (LONG)(px < h * 0.1f ? px : h * 0.1f);
            }
        }
        const LONG big = (LONG)(h * 0.006f) > th + 3 ? (LONG)(h * 0.006f) : th + 3;  // red dot, ~6 px at 1080p
        const LONG a = th / 2, b = th - a;              // splits the thickness around the centre line
        // The dot turns red and grows while the aim rests on something that takes damage.
        const bool hot = centered && AimOnTarget();
        const LONG ds = hot ? big : th, da = ds / 2, db = ds - da;
        const D3DRECT marks[5] = {
            {cx - gap - len, cy - a, cx - gap, cy + b},        // left
            {cx + gap, cy - a, cx + gap + len, cy + b},        // right
            {cx - a, cy - gap - len, cx + b, cy - gap},        // top
            {cx - a, cy + gap, cx + b, cy + gap + len},        // bottom
            {cx - da, cy - da, cx + db, cy + db},              // dot
        };
        D3DRECT edge[5];
        for (int i = 0; i < 5; ++i) edge[i] = {marks[i].x1 - 1, marks[i].y1 - 1, marks[i].x2 + 1, marks[i].y2 + 1};
        const DWORD first = dotOnly ? 4 : 0;  // the dot alone: skip the ticks
        dev->Clear(5 - first, edge + first, D3DCLEAR_TARGET, D3DCOLOR_ARGB(255, 30, 30, 30), 1.0f, 0);
        if (!dotOnly) dev->Clear(4, marks, D3DCLEAR_TARGET, D3DCOLOR_ARGB(255, 245, 245, 245), 1.0f, 0);
        dev->Clear(1, &marks[4], D3DCLEAR_TARGET,
                   hot ? D3DCOLOR_ARGB(255, 235, 35, 30) : D3DCOLOR_ARGB(255, 245, 245, 245), 1.0f, 0);
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
