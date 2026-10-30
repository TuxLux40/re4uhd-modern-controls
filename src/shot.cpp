// Remake-style aim (CenteredAim): the reticle sits in the middle of the screen, and the laser and Leon's shots go there,
// the shots to a random point inside the current spread. The camera follows Leon's aim, never the gun itself
// (camera.cpp). Hooks:
//  - Aim sway (re4.h kFnSway): the game's sway (drift while the stick rests, a kick per shot) is dropped; the game gets
//    the steady aim back. The game's own changes to the aim (the stick) go to the camera.
//  - Laser line (kFnLaserLine): the laser runs from the muzzle to the point under the reticle. Its red dot is not
//    spawned (kFnLaserDot): the reticle's own dot marks the aim, red while it rests on something that takes damage.
//  - Bullet (kFnBullet): each of Leon's shots is turned from the gun's line to the point under the reticle, plus the
//    spread (on top of the gun's own spread, e.g. the shotgun's pellets).
// The spread works like the remakes': it opens while Leon walks, turns or fires and closes again while he holds still;
// the reticle's ticks show it. The point under the reticle is the view on screen (CameraControl::Camera_60) cut by the
// game's line hit test (world and enemies).
#include "common.h"
#include "re4.h"
#include "config.h"
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "MinHook.h"

using namespace re4;

typedef void(__cdecl* PFN_Bullet)(uintptr_t owner, float* from, float* to, uint32_t wep, uint32_t a5, uint32_t a6);
typedef void(__fastcall* PFN_LaserLine)(uintptr_t self, void* edx, float* start, float* end);
typedef int(__cdecl* PFN_LineHit)(float* from, float* to, int a3, int a4, int* hitEm, int* poly);
static PFN_Bullet g_origBullet = nullptr;
static PFN_LaserLine g_origLaserLine = nullptr;
static PFN_LineHit g_lineHit = nullptr;

static float g_viewDir[3];
static float g_viewLeonYaw = 0.0f;  // Leon's heading when the camera hook built the view
static volatile DWORD g_viewTick = 0;
static float g_spread = 0.0f;  // radians around the reticle the next shot may go
static LARGE_INTEGER g_spreadLast = {};
static float g_lastViewYaw = 0.0f, g_lastViewPitch = 0.0f, g_lastPos[2] = {};
static bool g_spreadHave = false;
static DWORD g_lastKickTick = 0;
extern "C" {
volatile int g_hideLaserDot = 0;
void* g_origLaserDot = nullptr;
}
static float g_gunStart[3], g_gunEnd[3], g_gunHit[3];  // the gun's own line (before the laser is turned) and its first hit
static volatile DWORD g_gunTick = 0;
static float g_target[3];  // the point under the reticle
static float g_gunPoint[3];  // what the gun aims at: on the reticle's line, at its point's distance eased
static float g_gunInvDist = 0.0f;
static LARGE_INTEGER g_gunDistLast = {};
static volatile DWORD g_targetTick = 0;
static bool g_onTarget = false;  // it is on something that takes damage

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
static uintptr_t PlayerPtr() {
    const uintptr_t pl = *(uintptr_t*)Va(kPlayerPtr);
    return pl >= 0x10000 && Readable((void*)pl, 0x200) ? pl : 0;
}

// The widest spread of the weapon in hand (radians): half of its sway limit (TMP 0.122 -> 3.5 deg).
// Firing opens it up to the weapon's whole sway limit (TMP 0.122 -> 7 deg): long bursts scatter like the original,
// short ones stay tight.
static float FireMax() {
    const float r = *(const float*)Va(kSwayMax);
    return r > 0.0f && r < 0.5f ? r : 0.12f;
}
static volatile DWORD g_lastShotTick = 0;
static float SpreadMax() {
    const float r = *(const float*)Va(kSwayMax);
    return r > 0.0f && r < 0.5f ? r * 0.5f : 0.06f;
}

// Where the spread heads this frame and how it gets there: open right away, close over about half a second.
static void UpdateSpread(bool start) {
    LARGE_INTEGER now, freq;
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&freq);
    float dt = g_spreadLast.QuadPart ? (float)(now.QuadPart - g_spreadLast.QuadPart) / (float)freq.QuadPart : 0.0f;
    g_spreadLast = now;
    if (dt > 0.1f) dt = 0.1f;
    float yaw, pitch;
    YawPitch(g_viewDir, &yaw, &pitch);
    const uintptr_t pl = *(uintptr_t*)Va(kPlayerPtr);
    const bool havePl = pl >= 0x10000 && Readable((void*)pl, 0x200);
    const float px = havePl ? At<float>(pl, pl::Pos) : 0.0f, pz = havePl ? At<float>(pl, pl::Pos + 8) : 0.0f;
    const float max = SpreadMax();
    if (start || !g_spreadHave) {
        g_spread = max;  // raising the gun: unfocused, then it settles
    } else if (dt > 0.0f) {
        const float turn = (fabsf(WrapAngle(yaw - g_lastViewYaw)) + fabsf(pitch - g_lastViewPitch)) / dt;  // rad/s
        const float move = sqrtf((px - g_lastPos[0]) * (px - g_lastPos[0]) + (pz - g_lastPos[1]) * (pz - g_lastPos[1])) / dt;
        float target = 0.0f;
        target = fmaxf(target, max * fminf(1.0f, turn / 0.8f));        // full at Leon's top aim turn (~46 deg/s)
        target = fmaxf(target, max * 0.7f * fminf(1.0f, move / 1500.0f));  // walking while aiming
        if (target > g_spread)
            g_spread += (target - g_spread) * (1.0f - expf(-dt / 0.08f));
        else if (GetTickCount() - g_lastShotTick > 200)  // closes once the trigger rests a moment
            g_spread += (target - g_spread) * (1.0f - expf(-dt / 0.3f));
    }
    g_lastViewYaw = yaw, g_lastViewPitch = pitch, g_lastPos[0] = px, g_lastPos[1] = pz;
    g_spreadHave = havePl;
}

// One shot: the spread opens by a tenth of the weapon's whole sway limit (once per frame, so a shotgun blast counts
// once); about ten shots in a row reach it.
static void KickSpread() {
    const DWORD now = GetTickCount();
    g_lastShotTick = now;
    if (now == g_lastKickTick) return;
    g_lastKickTick = now;
    const float max = FireMax();
    g_spread = fminf(max, g_spread + max * 0.1f);
}

// The aim camera handed on this frame (set by the camera hook while aiming with the centered aim): its turn opens the
// spread, and Leon's heading then turns the view on screen to this frame's (CastTarget).
float AimGameYaw(uintptr_t pl);
void SetShotView(const float* eye, const float* tgt) {
    const float d[3] = {tgt[0] - eye[0], tgt[1] - eye[1], tgt[2] - eye[2]};
    const float l = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    if (l < 1.0f) return;
    for (int i = 0; i < 3; ++i) g_viewDir[i] = d[i] / l;
    if (const uintptr_t pl = PlayerPtr()) g_viewLeonYaw = AimGameYaw(pl);
    const DWORD now = GetTickCount();
    const bool fresh = now - g_viewTick > 200;  // a new aim
    g_viewTick = now;
    UpdateSpread(fresh);
}

// The centered aim is running: the reticle sits in the middle of the screen.
bool CenteredView() { return Cfg().centeredAim && GetTickCount() - g_viewTick <= 100; }

// The current spread (radians), for the reticle; 0 without the centered aim.
float SpreadRadius() { return CenteredView() ? g_spread : 0.0f; }

// For the reticle: the point under it takes damage.
bool AimOnTarget() { return g_onTarget && GetTickCount() - g_targetTick <= 100; }

// Uniform random point in a disk of radius r (radians) around a direction.
static uint32_t g_rng = 0x9E3779B9u;
static float Rand01() {
    g_rng ^= g_rng << 13, g_rng ^= g_rng >> 17, g_rng ^= g_rng << 5;
    return (g_rng >> 8) * (1.0f / 16777216.0f);
}
static void Scatter(float* dir, float r) {
    if (r <= 0.0f) return;
    float yaw, pitch;
    YawPitch(dir, &yaw, &pitch);
    const float a = Rand01() * 6.2831853f, d = r * sqrtf(Rand01());
    const float c = cosf(pitch);
    FromYawPitch(yaw + d * cosf(a) / (c > 0.2f ? c : 0.2f), pitch + d * sinf(a), dir);
}

// The gun's own line, from the muzzle to its first hit (for the camera's log).
bool GunLine(float* start, float* hit) {
    if (GetTickCount() - g_gunTick > 100) return false;
    for (int i = 0; i < 3; ++i) start[i] = g_gunStart[i], hit[i] = g_gunHit[i];
    return true;
}

// The point under the reticle: the view on screen (Camera_60, last frame's), turned about Leon by however far he turned
// since (the laser runs before the camera in a frame), cast from level with the muzzle so nothing between the camera
// and Leon counts. Also whether it takes damage: an enemy or object with health left (the line hit counts only those:
// 2, or 3 on some body parts), or a shootable surface close enough (LaserUpdate's own test).
static bool CastTarget(const float* muzzle) {
    if (!g_lineHit || !CenteredView()) return false;
    const uintptr_t pl = PlayerPtr();
    if (!pl) return false;
    const float* m = (const float*)(Va(kCamCtrl) + cc::Camera);
    float eye[3] = {m[3], m[7], m[11]}, dir[3] = {-m[2], -m[6], -m[10]};
    if (!Normalize(dir)) return false;
    const float turn = WrapAngle(AimGameYaw(pl) - g_viewLeonYaw);
    if (turn != 0.0f) {
        const float c = cosf(turn), s = sinf(turn);
        const float px = At<float>(pl, pl::Pos), pz = At<float>(pl, pl::Pos + 8);
        const float ex = eye[0] - px, ez = eye[2] - pz;
        eye[0] = px + ex * c + ez * s, eye[2] = pz - ex * s + ez * c;
        const float dx = dir[0], dz = dir[2];
        dir[0] = dx * c + dz * s, dir[2] = -dx * s + dz * c;
    }
    const float mv[3] = {muzzle[0] - eye[0], muzzle[1] - eye[1], muzzle[2] - eye[2]};
    const float t0 = fmaxf(0.0f, Dot(mv, dir));
    float from[3], to[3];
    for (int i = 0; i < 3; ++i) from[i] = eye[i] + dir[i] * t0, to[i] = eye[i] + dir[i] * 40000.0f;
    int em = 0, poly = 0;
    const int hit = g_lineHit(from, to, 0, 0, &em, &poly);
    bool target = hit >= 2 && em != 0;
    if (hit == 1) {
        const uint32_t f = (uint32_t)poly;
        const int i = ((f & 0x800000) ? 1 : 0) + ((f & 0x8000) ? 2 : 0) + ((f & 0x80) ? 4 : 0);
        const uintptr_t at = Va(kPolyAttr);
        float d2 = 0.0f;
        for (int k = 0; k < 3; ++k) d2 += (to[k] - muzzle[k]) * (to[k] - muzzle[k]);
        target = *(const uint8_t*)(at + 0x240 + i) && (*(const uint32_t*)(at + 0x20 + i * 0x44) & 2) &&
                 d2 < *(const float*)Va(kShootableDist2);
    }
    for (int i = 0; i < 3; ++i) g_target[i] = to[i];
    // The gun's point eases its distance (by its inverse, about the angle the gun turns by): the point jumps whenever the
    // reticle crosses an edge onto something nearer or farther, and with the gun beside the view that swung the gun a few
    // degrees each time, so it jittered while the view turned slowly (2026-10-29). Its direction stays the reticle's.
    LARGE_INTEGER now, freq;
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&freq);
    const float dt = g_gunDistLast.QuadPart ? (float)(now.QuadPart - g_gunDistLast.QuadPart) / (float)freq.QuadPart : 1.0f;
    g_gunDistLast = now;
    const float mt[3] = {to[0] - eye[0], to[1] - eye[1], to[2] - eye[2]};
    // Not nearer than 1000: for the floor right in front Leon would turn far off the view.
    const float inv = 1.0f / fmaxf(1000.0f, Dot(mt, dir));
    if (dt > 0.3f || g_gunInvDist <= 0.0f) g_gunInvDist = inv;
    else g_gunInvDist += (inv - g_gunInvDist) * (1.0f - expf(-dt / 0.12f));
    for (int i = 0; i < 3; ++i) g_gunPoint[i] = eye[i] + dir[i] / g_gunInvDist;
    g_onTarget = target;
    g_targetTick = GetTickCount();
    return true;
}

// Direction from `from` that hits the target, turned the way the gun's line would have to turn to get there (so a
// shotgun's pellets keep their own spread). Returns false (leave the shot alone) when there is no target or it is far
// off the gun's line.
static bool Steer(const float* from, const float* shotDir, float* out, float scatter) {
    if (GetTickCount() - g_targetTick > 100) return false;
    float want[3] = {g_target[0] - from[0], g_target[1] - from[1], g_target[2] - from[2]};
    if (!Normalize(want)) return false;
    Scatter(want, scatter);
    float gun[3] = {g_gunEnd[0] - g_gunStart[0], g_gunEnd[1] - g_gunStart[1], g_gunEnd[2] - g_gunStart[2]};
    if (GetTickCount() - g_gunTick > 100 || !Normalize(gun))
        for (int i = 0; i < 3; ++i) gun[i] = shotDir[i];
    if (Dot(gun, want) < 0.5f) return false;  // target behind or beside the gun
    for (int i = 0; i < 3; ++i) out[i] = shotDir[i] + want[i] - gun[i];
    return Normalize(out) > 0.0f;
}

// Diagnostics: the angle between the gun's own line from `from` and the line to the point under the reticle (-1 when
// there is none this frame).
static float DiagGunOff(const float* from) {
    if (GetTickCount() - g_targetTick > 100 || GetTickCount() - g_gunTick > 100) return -1.0f;
    float gun[3] = {g_gunEnd[0] - g_gunStart[0], g_gunEnd[1] - g_gunStart[1], g_gunEnd[2] - g_gunStart[2]};
    float want[3] = {g_target[0] - from[0], g_target[1] - from[1], g_target[2] - from[2]};
    if (!Normalize(gun) || !Normalize(want)) return -1.0f;
    const float c = Dot(gun, want);
    return acosf(c > 1.0f ? 1.0f : c < -1.0f ? -1.0f : c) * 57.3f;
}

// Diagnostics: the laser's state (0 to the reticle, 1 no centred view so it follows the gun, 2 refused: the reticle's
// point is behind or beside the gun), logged when it changes, plus every 0.5 s the largest gun-to-reticle angle.
static void DiagLaser(int state, float off) {
    static int s_state = -1;
    static DWORD s_since = 0, s_sum = 0;
    static float s_max = 0.0f;
    static int s_frames = 0, s_steered = 0;
    const DWORD now = GetTickCount();
    if (now - s_since > 300 && s_state >= 0 && now - s_sum > 300) s_state = -1;  // a new aim after a pause
    static const char* names[] = {"to the reticle", "FOLLOWS THE GUN (no centred view)", "FOLLOWS THE GUN (reticle point behind/beside it)"};
    if (state != s_state) Log("diag laser: %s, gun %.1f deg off the reticle", names[state], off);
    if (state != s_state) s_state = state;
    s_since = now;
    if (off > s_max) s_max = off;
    ++s_frames, s_steered += state == 0;
    if (now - s_sum >= 500) {
        if (s_frames) Log("diag laser 0.5s: gun up to %.1f deg off the reticle, laser steered %d/%d frames", s_max, s_steered, s_frames);
        s_sum = now, s_max = 0.0f, s_frames = s_steered = 0;
    }
}

static float g_convYaw = 0.0f;  // added to Leon's heading (radians) during this aim, so the gun is on the reticle's point
static float g_convV = 0.0f;    // added to the arm's vertical value
// Diag: the gun's line against the line from its laser sight to the reticle's point, split into sideways and up/down
// (degrees, + = the gun points left of / above the point), every 100 ms for 1.5 s after the aim start, then every 1 s.
static DWORD g_poseDiagFrom = 0;
static void DiagGunSplit(const float* sight, const float* gun) {
    static DWORD s_last = 0;
    const DWORD now = GetTickCount(), since = now - g_poseDiagFrom;
    if (now - s_last < (since < 1500 ? 100u : 1000u) || GetTickCount() - g_targetTick > 100) return;
    s_last = now;
    float want[3] = {g_target[0] - sight[0], g_target[1] - sight[1], g_target[2] - sight[2]};
    const float dist = Normalize(want);
    if (dist <= 0.0f) return;
    const float gy = atan2f(gun[0], gun[2]), gp = asinf(fmaxf(-1.0f, fminf(1.0f, gun[1])));
    const float wy = atan2f(want[0], want[2]), wp = asinf(fmaxf(-1.0f, fminf(1.0f, want[1])));
    Log("diag gun %4lu ms: gun yaw %.1f pitch %.1f, to the point yaw %.1f pitch %.1f -> off sideways %+.1f, up %+.1f, "
        "point %.0f away, arm %.2f side %.2f, added heading %+.1f arm %+.2f",
        (unsigned long)since, gy * 57.3f, gp * 57.3f, wy * 57.3f, wp * 57.3f, WrapAngle(gy - wy) * 57.3f,
        (gp - wp) * 57.3f, dist, *(float*)Va(kAimV), PlayerPtr() ? At<float>(PlayerPtr(), 0x448) : 0.0f, g_convYaw * 57.3f, g_convV);
}

// The gun on the reticle's point (the remakes' look). The view sits over the shoulder, beside the gun, so a gun parallel
// to the view hits beside the reticle's point. The gun is turned onto it with the game's own controls: sideways Leon's
// heading (the game turns his whole body to aim sideways), up and down the arm's vertical value. Each frame the gun's
// measured line (the laser's) against the line from its sight to the point gives the error, and half of it is added:
// no model of the arm. The heading's share is kept apart (AimGameYaw), so the view, which follows Leon's heading, doesn't
// turn with it. (Before: a learnt shoulder model twisting the upper body through the aim's sideways value cPlayer+0x448,
// up to 30 deg. That value is only the sway's, a few degrees: the shotgun hung high at the edge of the view, 2026-10-29.)
// The arm's vertical value turns the gun about 60 deg per unit (log 2026-10-29: -0.459 -> -27.5 deg, -0.407 -> -24.3),
// not the 90 deg of the game's own radians-to-value factor (2/pi): the starting value, the rest is the error's.
static const float kGunRadPerV = 1.047f;

static bool AimLive(uintptr_t pl) {  // aiming with the gun up (r_no 6/1/0 or 6/1/1), not raising it
    return At<uint8_t>(pl, pl::RNo0 + 1) == 6 && At<uint8_t>(pl, pl::RNo0 + 2) == 1 && At<uint8_t>(pl, pl::RNo0 + 3) <= 1;
}
// Leon's heading without the gun's share: what the stick and the view turned him to.
float AimGameYaw(uintptr_t pl) { return WrapAngle(At<float>(pl, pl::Ang + 4) - g_convYaw); }
float AimConvYaw() { return g_convYaw; }
// The aim ended or starts: his heading keeps the share (a few degrees), it just is his own from now on.
void AimConvEnd() { g_convYaw = g_convV = 0.0f; }
static float ArmV(float pitch) { return fmaxf(-1.0f, fminf(1.0f, pitch / kGunRadPerV + g_convV)); }

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
    g_hideLaserDot = 0;
    if (!CastTarget(start)) {
        DiagLaser(1, -1.0f);
        return;
    }
    g_hideLaserDot = 1;
    float dir[3] = {end[0] - start[0], end[1] - start[1], end[2] - start[2]};
    if (Normalize(dir) <= 0.0f) return;
    // The beam stays the gun's own (the gun is turned onto the reticle's point instead). LaserUpdate moves its start
    // kLaserPush along it: the laser sight, where the gun's error is measured from.
    constexpr float kLaserPush = 500.0f;  // 0x00B26CB4
    float sight[3];
    for (int i = 0; i < 3; ++i) sight[i] = start[i] + dir[i] * kLaserPush;
    DiagGunSplit(sight, dir);
    DiagLaser(0, DiagGunOff(sight));
}

static void __cdecl HookBullet(uintptr_t owner, float* from, float* to, uint32_t wep, uint32_t a5, uint32_t a6) {
    static int logged = 0;
    if (owner && owner == *(uintptr_t*)Va(kPlayerPtr) && Readable(from, 12) && Readable(to, 12) && !CenteredView())
        Log("diag shot: weapon %u NOT steered (no centred view)", wep & 0xFFFF);
    if (owner && owner == *(uintptr_t*)Va(kPlayerPtr) && Readable(from, 12) && Readable(to, 12) && CastTarget(from)) {
        float dir[3] = {to[0] - from[0], to[1] - from[1], to[2] - from[2]};
        const float len = Normalize(dir);
        const float scatter = g_spread;
        KickSpread();
        float nd[3];
        if (len > 0.0f && Steer(from, dir, nd, scatter)) {
            float nt[3];
            for (int i = 0; i < 3; ++i) nt[i] = from[i] + nd[i] * len;
            if (logged < 5) {
                ++logged;
                Log("shot: weapon %u to the reticle, spread %.2f deg", wep & 0xFFFF, scatter * 57.3f);
            }
            static DWORD s_last = 0;
            if (GetTickCount() - s_last > 100)
                s_last = GetTickCount(), Log("diag shot: weapon %u steered, gun %.1f deg off the reticle, spread %.2f deg",
                                             wep & 0xFFFF, DiagGunOff(from), scatter * 57.3f);
            g_origBullet(owner, from, nt, wep, a5, a6);  // a copy: the caller's buffer stays as it was
            return;
        }
        Log("diag shot: weapon %u NOT steered (reticle point behind/beside the gun), gun %.1f deg off", wep & 0xFFFF,
            DiagGunOff(from));
    }
    g_origBullet(owner, from, to, wep, a5, a6);
}

typedef void(__cdecl* PFN_Sway)(uintptr_t pl, uint32_t mode, float* v, float* side);
static PFN_Sway g_origSway = nullptr;
static float g_pitchMove = 0.0f;    // the game's changes to the vertical aim (the stick), for the camera
static float g_lastAnchor = 0.0f;
static bool g_haveAnchor = false;
static float g_setPitch = 0.0f;      // the vertical aim to swing to when the aim starts (radians)
static volatile DWORD g_setPitchUntil = 0;
typedef void(__thiscall* PFN_ArmPose)(uintptr_t ctrl, float v);
static PFN_ArmPose g_armPose = nullptr;  // null: unexpected code there, left alone
// The sway's last outputs (what the gun got) and the game's own values behind them. The game adds the stick to the
// outputs and hands them back on the next call: they go back to its own values first, so the gun's offsets never reach
// its aim or the anchors the view follows.
static float g_gameV = 0.0f, g_outV = 0.0f;
static bool g_outValid = false;
static DWORD g_swayTick = 0;
static float g_viewPitch = 0.0f;  // the view's pitch the camera last handed over (radians)
// Aim start: Leon swings his vertical aim to the view's (applied on the sway calls of the next 0.3 s, after the game's own
// reset of the aim at its start). The pose is set at once as well; while the gun comes up, RaisePose keeps it.
void SetAimPitch(float rad) {
    g_setPitch = g_viewPitch = fmaxf(-1.4f, fminf(1.4f, rad));
    g_setPitchUntil = GetTickCount() + 300;
    g_convV = 0.0f;  // the gun comes up afresh
    const float v = ArmV(g_setPitch);
    *(float*)Va(kAimV) = *(float*)Va(kAimVStart) = v;
    g_gameV = g_setPitch * 0.63661977f, g_outV = v;
    const uintptr_t ctrl = Va(kArmPoseCtrl);
    const uintptr_t model = Readable((void*)ctrl, 8) ? *(uintptr_t*)ctrl : 0;
    if (g_armPose && model >= 0x10000 && Readable((void*)(model + 0x2A8), 4) && *(uintptr_t*)(model + 0x2A8))
        g_armPose(ctrl, v);
    g_poseDiagFrom = GetTickCount();
}
// The gun coming up (aim start, r_no 6/0) sets the arm's layer up with the raise motion for all three of its
// motions (level, up, down: 0x00753660), so the arm's value did nothing until the aim motions came in at r_no 6/1/0,
// about 0.5 s in: the gun came up level and then tilted to the aim (log 2026-10-29). The aim's up and down motions go in
// instead, so the gun comes up at its height. Each weapon kind raises from its own function and tables (0x00753660,
// 0x008FCAA0, 0x008FECB0, 0x009152C0, ...), so the aim's motions are learnt from the layer once the aim is live, per
// raise motion and weapon; the first raise of each still comes up level.
static uintptr_t MotionAt(uintptr_t table, int off) { return table + *(int*)(table + off); }
struct RaiseAim { uintptr_t raise, weapon, up, down; };
static RaiseAim g_raiseAims[16];
static int g_raiseAimCount = 0;
static uintptr_t g_raiseSeen = 0, g_raiseWeapon = 0;  // this aim's raise motion, until the aim is live
static void RaisePose() {
    const uintptr_t ctrl = Va(kArmPoseCtrl), pl = PlayerPtr();
    uintptr_t* c = (uintptr_t*)ctrl;
    if (!g_armPose || !pl || !CenteredView() || At<uint8_t>(pl, pl::RNo0 + 1) != 6 || c[0] < 0x10000 ||
        !Readable((void*)(c[0] + 0x2A8), 4) || *(uintptr_t*)(c[0] + 0x2A8) != ctrl + 0x18)
        return;
    const uintptr_t weapon = Readable((void*)(pl + 0x46C), 4) ? *(uintptr_t*)(pl + 0x46C) : 0;
    static bool s_switch = false, s_swapped = false;
    if (c[2] == c[3] && c[3] == c[4]) {  // the raise's layer, just set up
        g_raiseSeen = c[2], g_raiseWeapon = weapon, s_swapped = false;
        for (int i = 0; i < g_raiseAimCount; ++i)
            if (g_raiseAims[i].raise == c[2] && g_raiseAims[i].weapon == weapon) {
                c[3] = g_raiseAims[i].up, c[4] = g_raiseAims[i].down, s_switch = s_swapped = true;
                Log("gun: raise with the aim's up/down motions, arm %.2f", ArmV(g_viewPitch));
                break;
            }
        // The handgun family (0x008FCAA0) raises with the weapon motion table's +0x88; the game's own modern-mouse-aim
        // path (0x012224B0 == 1) raises with that table's +0x9C/+0xA0 as up and down, the live aim's (0x008FD390).
        // Checked live 2026-10-30. So the first raise of these weapons is right too, without learning.
        const uintptr_t g = *(uintptr_t*)Va(kGlobalPtr);
        const uintptr_t table = g && Readable((void*)(g + 0x44), 4) ? *(uintptr_t*)(g + 0x44) : 0;
        if (!s_swapped && table >= 0x10000 && Readable((void*)(table + 0x88), 0x1C) && MotionAt(table, 0x88) == c[2]) {
            c[3] = MotionAt(table, 0x9C), c[4] = MotionAt(table, 0xA0), s_switch = s_swapped = true;
            Log("gun: raise with the table's aim up/down motions, arm %.2f", ArmV(g_viewPitch));
        }
    } else if (g_raiseSeen && c[2] != g_raiseSeen && AimLive(pl)) {  // the aim's layer: remember its motions
        int i = 0;
        while (i < g_raiseAimCount && !(g_raiseAims[i].raise == g_raiseSeen && g_raiseAims[i].weapon == g_raiseWeapon)) ++i;
        if (i == g_raiseAimCount && i < 16) ++g_raiseAimCount, Log("gun: learnt the aim's motions for raise %X", (unsigned)g_raiseSeen);
        if (i < 16) g_raiseAims[i] = {g_raiseSeen, g_raiseWeapon, c[3], c[4]};
        g_raiseSeen = 0, s_swapped = false;
    }
    if (!s_swapped || c[2] != g_raiseSeen) return;
    const float v = ArmV(g_viewPitch);
    // The pose switches its motion only when the value changes sign; from 0 it doesn't for an upward value.
    if (s_switch && v != 0.0f) ((float*)c)[1] = v > 0.0f ? -1e-6f : 1e-6f, s_switch = false;
    *(float*)Va(kAimV) = *(float*)Va(kAimVStart) = v;
    g_outV = v;
    g_armPose(ctrl, v);
}
// Once a frame (camera.cpp's input tick): the pose while the gun comes up, and the diag: the arm pose controller and its
// motion layer for 0.7 s after the aim start.
void AimPoseTick() {
    RaisePose();
    const DWORD t = GetTickCount() - g_poseDiagFrom;
    if (!g_poseDiagFrom || t > 700) return;
    const uintptr_t ctrl = Va(kArmPoseCtrl);
    const uintptr_t model = *(uintptr_t*)ctrl;
    uintptr_t layer = 0;
    if (model >= 0x10000 && Readable((void*)(model + 0x2A8), 4)) layer = *(uintptr_t*)(model + 0x2A8);
    char b[600];
    uintptr_t table = 0;
    if (const uintptr_t g = *(uintptr_t*)Va(kGlobalPtr)) table = *(uintptr_t*)(g + 0x50);
    const uintptr_t* c = (const uintptr_t*)ctrl;
    int k = snprintf(b, sizeof b, "diag pose %3lu ms: ctrl %.2f start %.2f aimV %.2f model %X layer %X | motions %X %X %X, "
                     "table raise %X up %X down %X | centred %d rno %02X%02X%02X |", (unsigned long)t,
                     *(float*)(ctrl + 4), *(float*)Va(kAimVStart), *(float*)Va(kAimV), (unsigned)model, (unsigned)layer,
                     (unsigned)c[2], (unsigned)c[3], (unsigned)c[4], table ? (unsigned)MotionAt(table, 0x8C) : 0u,
                     table ? (unsigned)MotionAt(table, 0x20C) : 0u, table ? (unsigned)MotionAt(table, 0x214) : 0u,
                     CenteredView() ? 1 : 0, PlayerPtr() ? At<uint8_t>(PlayerPtr(), pl::RNo0 + 1) : 0,
                     PlayerPtr() ? At<uint8_t>(PlayerPtr(), pl::RNo0 + 2) : 0, PlayerPtr() ? At<uint8_t>(PlayerPtr(), pl::RNo0 + 3) : 0);
    if (layer >= 0x10000 && Readable((void*)(layer + 0xA0), 0x40))
        for (int i = 0; i < 16; ++i) k += snprintf(b + k, sizeof b - k, " %.2f", ((float*)(layer + 0xA0))[i]);
    Log("%s", b);
}
// The sway anchor came back after a gap (a weapon switch drops it for a few frames): its value then is the game's
// reset, not a stick move, so the camera can't see it as a jump.
static bool g_anchorBack = false;
bool TakeAimReset() {
    const bool r = g_anchorBack;
    g_anchorBack = false;
    return r;
}
float TakeAimPitchMove() {
    const float m = g_pitchMove;
    g_pitchMove = 0.0f;
    return m;
}

// Sway: mode bit 0 (the stick moves) sets the anchor to the current aim; 0 drifts around the anchor; bit 1 is the kick of
// a shot. v is the vertical aim value, side Leon's sideways aim (pl+0x448); the anchor sits at (pl+0x7D8)->+0x28 (v in
// radians) and +0x2C (side).
static void __cdecl HookSway(uintptr_t pl, uint32_t mode, float* v, float* side) {
    uintptr_t sw = 0;
    const bool on = CenteredView() && pl && pl == *(uintptr_t*)Va(kPlayerPtr) && Readable((void*)(pl + 0x7D8), 4) &&
                    (sw = *(uintptr_t*)(pl + 0x7D8)) >= 0x10000 && Readable((void*)sw, 0x30);
    if (GetTickCount() - g_swayTick > 100) g_outValid = false;  // a new aim: the game set its values afresh
    g_swayTick = GetTickCount();
    if (on && g_outValid) *v = g_gameV + (*v - g_outV);
    g_origSway(pl, mode, v, side);
    if (!on) {
        g_haveAnchor = false, g_pitchMove = 0.0f, g_outValid = false;
        return;
    }
    if (!g_haveAnchor) g_anchorBack = true;
    const bool window = (int)(g_setPitchUntil - GetTickCount()) > 0;
    if (window) {
        *(float*)(sw + 0x28) = g_setPitch;
        g_lastAnchor = g_setPitch, g_haveAnchor = true;
    }
    const float a = At<float>(sw, 0x28);
    if (g_haveAnchor) g_pitchMove += a - g_lastAnchor;
    g_lastAnchor = a, g_haveAnchor = true;
    g_viewPitch = a;
    // The game's own aim: back to the anchor (no drift, no kick). The gun's: on the point under the reticle.
    g_gameV = a * 0.63661977f;
    const float outV = ArmV(a);
    *v = g_outV = outV;
    g_outValid = true;
    // Sideways back to the anchor too: the sway's sideways part shook the gun and the view, which follows this value
    // (dropped by mistake in the 10-29 redesign).
    *side = At<float>(sw, 0x2C);
    // The arm's pose follows kAimVStart, eased toward the aim by kAimVBlend: at the aim start it goes there at once.
    if (window) *(float*)Va(kAimVStart) = outV;
}
// The aim's sideways value (cPlayer+0x448), for the view.
float AimViewSide(uintptr_t pl) {
    return At<float>(pl, 0x448);
}

// The laser's red dot (kFnLaserDot, position in ESI): skipped while the centered aim runs.
extern "C" __attribute__((naked)) void LaserDotStub() {
    asm("cmpl $0, _g_hideLaserDot\n\t"
        "jne 1f\n\t"
        "jmp *_g_origLaserDot\n\t"
        "1:\n\t"
        "ret\n\t");
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
    if (SafeEqual((void*)Va(kFnArmPose), kSigArmPose, sizeof(kSigArmPose)))
        g_armPose = (PFN_ArmPose)Va(kFnArmPose);
    else
        Log("arm pose: unexpected code at %p, the gun comes up level", (void*)Va(kFnArmPose));
    Hook(kFnLaserDot, kSigLaserDot, sizeof(kSigLaserDot), (void*)LaserDotStub, &g_origLaserDot, "laser dot");
}
