// Shots and the laser go where the crosshair is, and the aim sway moves them, not the screen. With the centered aim the
// camera follows the gun (camera.cpp), the crosshair sits in the middle of the screen. Three hooks:
//  - Aim sway (re4.h kFnSway): the game's sway (drift while the stick rests, a kick per shot) would swing the gun and
//    with it the whole view. The game gets the steady aim back; the sway is kept here and applied below instead.
//  - Laser line (kFnLaserLine): the laser runs from the muzzle to the point under the crosshair, moved by the sway, so
//    its red dot wanders inside the reticle. The gun's own line is kept for the camera (GunLine), which follows it.
//  - Bullet (kFnBullet): each of Leon's shots is turned by the angle between the gun's line and the line from the
//    muzzle to that point. Turning instead of replacing keeps the gun's spread.
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
static float g_viewGunYaw = 0.0f, g_viewGunPitch = 0.0f;  // the gun's line the camera followed for that view
static bool g_viewHasGun = false;
static volatile DWORD g_viewTick = 0;
static float g_swayV = 0.0f, g_swayH = 0.0f;  // sway taken out of the gun: vertical aim units, sideways radians
static bool g_swayOn = false;
// Vertical aim value to gun pitch (radians per unit), measured: -0.40 -> -22.6 deg, +0.24 -> +15.6 deg.
constexpr float kSwayPitchPerUnit = 1.1f;
static float g_gunStart[3], g_gunEnd[3], g_gunHit[3];  // the gun's line before the laser is turned, and its first hit
static volatile DWORD g_gunTick = 0;
static float g_target[3];  // the point under the crosshair
static volatile DWORD g_targetTick = 0;

static float Dot(const float* a, const float* b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
static float Normalize(float* v) {
    const float l = sqrtf(Dot(v, v));
    if (l < 1e-4f) return 0.0f;
    v[0] /= l, v[1] /= l, v[2] /= l;
    return l;
}
static void YawPitch(const float* d, float* yaw, float* pitch) {
    *yaw = atan2f(d[0], d[2]);
    *pitch = atan2f(d[1], sqrtf(d[0] * d[0] + d[2] * d[2]));
}
static void FromYawPitch(float yaw, float pitch, float* d) {
    d[0] = cosf(pitch) * sinf(yaw), d[1] = sinf(pitch), d[2] = cosf(pitch) * cosf(yaw);
}
static float WrapAngle(float a) {
    while (a > 3.14159265f) a -= 6.2831853f;
    while (a < -3.14159265f) a += 6.2831853f;
    return a;
}
static bool GunFresh() { return GetTickCount() - g_gunTick <= 100; }
static void GunYawPitch(float* yaw, float* pitch) {
    float d[3] = {g_gunEnd[0] - g_gunStart[0], g_gunEnd[1] - g_gunStart[1], g_gunEnd[2] - g_gunStart[2]};
    YawPitch(d, yaw, pitch);
}

// The aim camera as shown this frame (set by the camera hook while the centered aim runs; the game's camera smoothing
// is cut then, so this is the rendered view).
void SetShotView(const float* eye, const float* tgt) {
    const float d[3] = {tgt[0] - eye[0], tgt[1] - eye[1], tgt[2] - eye[2]};
    const float l = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    if (l < 1.0f) return;
    for (int i = 0; i < 3; ++i) g_viewEye[i] = eye[i], g_viewDir[i] = d[i] / l;
    g_viewHasGun = GunFresh();
    if (g_viewHasGun) GunYawPitch(&g_viewGunYaw, &g_viewGunPitch);
    g_viewTick = GetTickCount();
}

// How far the sway may take the aim from the centre (radians), for the reticle's size; 0 without the centered aim.
float SwayRadius() {
    if (!g_swayOn) return 0.0f;
    const float r = *(const float*)Va(kSwayMax);
    return r > 0.0f && r < 0.5f ? r : 0.0f;
}

// The gun's line, from the muzzle to its first hit, as the laser would show it without the mod turning it.
bool GunLine(float* start, float* hit) {
    if (GetTickCount() - g_gunTick > 100) return false;
    for (int i = 0; i < 3; ++i) start[i] = g_gunStart[i], hit[i] = g_gunHit[i];
    return true;
}

// Cast the camera's centre line, starting level with the muzzle so nothing between the camera and Leon counts. The
// laser runs before the camera in a frame, so the view is last frame's: it is turned by however far the gun's line moved
// since, which is where this frame's camera (following the gun) will look.
static bool CastTarget(const float* muzzle) {
    if (!g_lineHit || !Cfg().centeredAim || !AimCentered() || GetTickCount() - g_viewTick > 100) return false;
    float eye[3] = {g_viewEye[0], g_viewEye[1], g_viewEye[2]}, dir[3] = {g_viewDir[0], g_viewDir[1], g_viewDir[2]};
    if (g_viewHasGun && GunFresh()) {
        float gy, gp, vy, vp;
        GunYawPitch(&gy, &gp);
        YawPitch(dir, &vy, &vp);
        FromYawPitch(vy + WrapAngle(gy - g_viewGunYaw), vp + (gp - g_viewGunPitch), dir);
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
    if (g_swayOn) {  // the sway the gun no longer shows
        float y, p;
        YawPitch(want, &y, &p);
        FromYawPitch(y + g_swayH, p + g_swayV * kSwayPitchPerUnit, want);
    }
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

    if (!CastTarget(start)) return;
    float dir[3] = {end[0] - start[0], end[1] - start[1], end[2] - start[2]};
    const float len = Normalize(dir);
    float nd[3], angle = -1.0f;
    const bool ok = len > 0.0f && Steer(start, dir, nd, &angle);
    static int s_diag = 0;
    static DWORD s_diagTick = 0;
    const DWORD now = GetTickCount();
    if (s_diag < 60 && now - s_diagTick >= 250) {
        s_diagTick = now, ++s_diag;
        Log("laser: %s %.2f deg, sway v %+.3f h %+.3f (max %.3f)", ok ? "turned" : "LEFT", angle, g_swayV, g_swayH,
            SwayRadius());
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

typedef void(__cdecl* PFN_Sway)(uintptr_t pl, uint32_t mode, float* v, float* side);
static PFN_Sway g_origSway = nullptr;

// Sway: mode bit 0 (the stick moves) sets the anchor to the current aim; 0 drifts around the anchor; bit 1 is the kick of
// a shot. v is the vertical aim value, side Leon's sideways aim (pl+0x448); the anchor sits at (pl+0x7D8)->+0x28 (v in
// radians) and +0x2C (side).
static void __cdecl HookSway(uintptr_t pl, uint32_t mode, float* v, float* side) {
    uintptr_t sw = 0;
    const bool on = Cfg().centeredAim && AimCentered() && pl && pl == *(uintptr_t*)Va(kPlayerPtr) &&
                    Readable((void*)(pl + 0x7D8), 4) && (sw = *(uintptr_t*)(pl + 0x7D8)) >= 0x10000 &&
                    Readable((void*)sw, 0x30);
    if (!on) {
        g_swayOn = false;
        g_swayV = g_swayH = 0.0f;
        g_origSway(pl, mode, v, side);
        return;
    }
    const bool setAnchor = (mode & 1) != 0;
    if (g_swayOn && !setAnchor) *v += g_swayV, *side += g_swayH;  // the game carries on from the swayed aim
    g_origSway(pl, mode, v, side);
    const float av = At<float>(sw, 0x28) * 0.63661977f, ah = At<float>(sw, 0x2C);
    if (setAnchor) {
        g_swayV = g_swayH = 0.0f;
    } else {
        g_swayV = *v - av;
        g_swayH = *side - ah;
    }
    *v = av, *side = ah;
    g_swayOn = true;
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
    Hook(kFnSway, kSigSway, sizeof(kSigSway), (void*)HookSway, (void**)&g_origSway, "aim sway");
}
