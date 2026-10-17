// Shots and the laser go where the crosshair is. With the centered aim, the crosshair sits in the middle of the screen
// while the gun lags behind the camera when Leon turns. Two hooks fix that:
//  - Bullet (re4.h kFnBullet): each of Leon's shots is turned by the angle between the gun's line and the line from
//    the muzzle to the point under the crosshair. Turning instead of replacing keeps the gun's spread.
//  - Laser line (kFnLaserLine): the laser is aimed from the muzzle at that point, so its dot stays under the crosshair
//    instead of catching up. The gun's own line is kept for the camera (GunLine), which follows it.
// The point under the crosshair is the camera's centre line cut by the game's line hit test (world and enemies).
#include "common.h"
#include "re4.h"
#include "config.h"
#include <math.h>
#include "MinHook.h"

using namespace re4;

bool AimCentered();

typedef void(__cdecl* PFN_Bullet)(uintptr_t owner, float* from, float* to, uint32_t wep, uint32_t a5, uint32_t a6);
typedef void(__fastcall* PFN_LaserLine)(uintptr_t self, void* edx, float* start, float* end);
typedef int(__cdecl* PFN_LineHit)(float* from, float* to, int a3, int a4, int* hitEm, int* poly);
static PFN_Bullet g_origBullet = nullptr;
static PFN_LaserLine g_origLaserLine = nullptr;
static PFN_LineHit g_lineHit = nullptr;

static float g_viewEye[3], g_viewDir[3];
static volatile DWORD g_viewTick = 0;
static float g_gunStart[3], g_gunEnd[3], g_gunHit[3];  // the gun's line before the laser is turned, and its first hit
static volatile DWORD g_gunTick = 0;
static float g_target[3];  // the point under the crosshair
static volatile DWORD g_targetTick = 0;

// The aim camera as shown this frame (set by the camera hook while the centered aim runs).
void SetShotView(const float* eye, const float* tgt) {
    const float d[3] = {tgt[0] - eye[0], tgt[1] - eye[1], tgt[2] - eye[2]};
    const float l = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    if (l < 1.0f) return;
    for (int i = 0; i < 3; ++i) g_viewEye[i] = eye[i], g_viewDir[i] = d[i] / l;
    g_viewTick = GetTickCount();
}

// The gun's line, from the muzzle to its first hit, as the laser would show it without the mod turning it.
bool GunLine(float* start, float* hit) {
    if (GetTickCount() - g_gunTick > 100) return false;
    for (int i = 0; i < 3; ++i) start[i] = g_gunStart[i], hit[i] = g_gunHit[i];
    return true;
}

static float Dot(const float* a, const float* b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
static float Normalize(float* v) {
    const float l = sqrtf(Dot(v, v));
    if (l < 1e-4f) return 0.0f;
    v[0] /= l, v[1] /= l, v[2] /= l;
    return l;
}

// Cast the camera's centre line, starting level with the muzzle so nothing between the camera and Leon counts.
// The view as rendered (CameraControl::Camera_60: camera-to-world, eye in the last column, looking down -Z). The game
// eases it after the camera hook, so while turning it trails the camera the hook computed; the crosshair is drawn in
// this one.
static bool RenderView(float* eye, float* dir) {
    const float* m = (const float*)(Va(kCamCtrl) + cc::Camera);
    eye[0] = m[3], eye[1] = m[7], eye[2] = m[11];
    dir[0] = -m[2], dir[1] = -m[6], dir[2] = -m[10];
    return Normalize(dir) > 0.5f;
}

static float g_lastViewDiff = 0.0f;  // measurement: yaw of the rendered view minus the hook's view (degrees)

static bool CastTarget(const float* muzzle) {
    if (!g_lineHit || !Cfg().centeredAim || !AimCentered() || GetTickCount() - g_viewTick > 100) return false;
    float eye[3], dir[3];
    if (!RenderView(eye, dir)) return false;
    {
        float d = atan2f(dir[0], dir[2]) - atan2f(g_viewDir[0], g_viewDir[2]);
        while (d > 3.14159265f) d -= 6.2831853f;
        while (d < -3.14159265f) d += 6.2831853f;
        g_lastViewDiff = d * 57.3f;
    }
    const float m[3] = {muzzle[0] - eye[0], muzzle[1] - eye[1], muzzle[2] - eye[2]};
    const float t0 = fmaxf(0.0f, Dot(m, dir));
    float from[3], to[3];
    for (int i = 0; i < 3; ++i) from[i] = eye[i] + dir[i] * t0, to[i] = eye[i] + dir[i] * 40000.0f;
    int em = 0, poly = 0;
    g_lineHit(from, to, 0, 0, &em, &poly);
    for (int i = 0; i < 3; ++i) g_target[i] = to[i];
    g_targetTick = GetTickCount();
    return true;
}

// Direction from `from` that hits the target, turned the way the gun's line would have to turn to get there.
// Returns false (leave the shot alone) when there is no target or it is far off the gun's line.
static bool Steer(const float* from, const float* shotDir, float* out, float* angle) {
    if (GetTickCount() - g_targetTick > 100) return false;
    float want[3] = {g_target[0] - from[0], g_target[1] - from[1], g_target[2] - from[2]};
    if (!Normalize(want)) return false;
    float gun[3] = {g_gunEnd[0] - g_gunStart[0], g_gunEnd[1] - g_gunStart[1], g_gunEnd[2] - g_gunStart[2]};
    // No fresh gun line (the laser skips some frames while firing): straight at the target, without the spread.
    if (GetTickCount() - g_gunTick > 100 || !Normalize(gun))
        for (int i = 0; i < 3; ++i) gun[i] = shotDir[i];
    const float d = Dot(gun, want);
    if (d < 0.5f) return false;  // target behind or beside the gun
    for (int i = 0; i < 3; ++i) out[i] = shotDir[i] + want[i] - gun[i];
    if (!Normalize(out)) return false;
    *angle = acosf(fminf(1.0f, d)) * 57.29578f;
    return true;
}

static void __fastcall HookLaserLine(uintptr_t self, void* edx, float* start, float* end) {
    g_origLaserLine(self, edx, start, end);
    // Only the laser itself (LaserUpdate); the other callers use the line for their own checks.
    const uintptr_t ret = (uintptr_t)__builtin_return_address(0);
    const uintptr_t lu = Va(kFnLaser);
    if (ret < lu || ret > lu + 0x500 || !g_lineHit) return;
    for (int i = 0; i < 3; ++i) g_gunStart[i] = start[i], g_gunEnd[i] = end[i], g_gunHit[i] = end[i];
    int em = 0, poly = 0;
    float s[3] = {start[0], start[1], start[2]};
    g_lineHit(s, g_gunHit, 0, 0, &em, &poly);
    g_gunTick = GetTickCount();

    static int s_diag = 0;
    static DWORD s_diagTick = 0;
    const DWORD now = GetTickCount();
    const bool diag = s_diag < 150 && now - s_diagTick >= 100;
    if (diag) s_diagTick = now, ++s_diag;
    if (!CastTarget(start)) {
        if (diag)
            Log("laser: no target (centered %d, view age %lu)", AimCentered() ? 1 : 0, (unsigned long)(now - g_viewTick));
        return;
    }
    float dir[3] = {end[0] - start[0], end[1] - start[1], end[2] - start[2]};
    const float len = Normalize(dir);
    float nd[3], angle = -1.0f;
    const bool ok = len > 0.0f && Steer(start, dir, nd, &angle);
    if (diag) {
        float w[3] = {g_target[0] - start[0], g_target[1] - start[1], g_target[2] - start[2]};
        const float wl = Normalize(w);
        Log("laser: %s gun %.1f -> target %.1f (%.2f deg, target %.0f away) len %.0f, rendered view %+.2f deg off",
            ok ? "turned" : "LEFT",
            atan2f(dir[0], dir[2]) * 57.3f, atan2f(w[0], w[2]) * 57.3f, acosf(fminf(1.0f, Dot(dir, w))) * 57.3f, wl, len, g_lastViewDiff);
    }
    if (!ok) return;
    for (int i = 0; i < 3; ++i) end[i] = start[i] + nd[i] * len;
}

static void __cdecl HookBullet(uintptr_t owner, float* from, float* to, uint32_t wep, uint32_t a5, uint32_t a6) {
    static int logged = 0;
    float nt[3];
    if (owner && owner == *(uintptr_t*)Va(kPlayerPtr) && Readable(from, 12) && Readable(to, 12) && CastTarget(from)) {
        float dir[3] = {to[0] - from[0], to[1] - from[1], to[2] - from[2]};
        const float len = Normalize(dir);
        float nd[3], angle;
        if (len > 0.0f && Steer(from, dir, nd, &angle)) {
            for (int i = 0; i < 3; ++i) nt[i] = from[i] + nd[i] * len;
            if (logged < 5) {
                ++logged;
                Log("shot: weapon %u turned %.2f deg toward the crosshair", wep & 0xFFFF, angle);
            }
            g_origBullet(owner, from, nt, wep, a5, a6);  // a copy: the caller's buffer stays as it was
            return;
        }
        if (logged < 5) {
            ++logged;
            Log("shot: weapon %u left alone (target %s)", wep & 0xFFFF,
                GetTickCount() - g_targetTick > 100 ? "stale" : "ok");
        }
    }
    g_origBullet(owner, from, to, wep, a5, a6);
}

static bool Hook(uint32_t rva, const uint8_t* sig, size_t n, void* detour, void** orig, const char* name) {
    void* target = (void*)Va(rva);
    if (!SafeEqual(target, sig, n)) {
        Log("%s: unexpected code at %p", name, target);
        return false;
    }
    if (MH_CreateHook(target, detour, orig) != MH_OK || MH_EnableHook(target) != MH_OK) {
        Log("hook %s failed", name);
        return false;
    }
    Log("hook %s at %p", name, target);
    return true;
}

void InitShotHook() {
    void* lineHit = (void*)Va(kFnLineHit);
    if (!SafeEqual(lineHit, kSigLineHit, sizeof(kSigLineHit))) {
        Log("line hit: unexpected code at %p, shots and laser follow the gun", lineHit);
        return;
    }
    g_lineHit = (PFN_LineHit)lineHit;
    Hook(kFnLaserLine, kSigLaserLine, sizeof(kSigLaserLine), (void*)HookLaserLine, (void**)&g_origLaserLine,
         "laser line");
    Hook(kFnBullet, kSigBullet, sizeof(kSigBullet), (void*)HookBullet, (void**)&g_origBullet, "bullet");
}
