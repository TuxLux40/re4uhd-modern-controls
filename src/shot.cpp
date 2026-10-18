// Shots and the laser follow the gun, inside a remake-style spread. With the centered aim the camera follows Leon's
// steady aim (camera.cpp) and the crosshair marks the gun's line: it wobbles a little around the middle of the screen
// with his breathing and the recoil, instead of the whole view. Hooks:
//  - Aim sway (re4.h kFnSway): the game's sway (drift while the stick rests, a kick per shot) is dropped; the game gets
//    the steady aim back.
//  - Laser line (kFnLaserLine): records the gun's line for the crosshair. The laser's red dot is not spawned
//    (kFnLaserDot): the reticle's own dot marks the aim.
//  - Bullet (kFnBullet): each of Leon's shots is turned to a random point inside the current spread around its line
//    (on top of the gun's own spread, e.g. the shotgun's).
// The spread works like the remakes': it opens while Leon walks, turns or fires and closes again while he holds still;
// the reticle's ticks show it.
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

static float g_viewDir[3];
static volatile DWORD g_viewTick = 0;
static bool g_swayOn = false;  // the sway hook is holding the gun steady
static float g_spread = 0.0f;  // radians around the crosshair the next shot may go
static LARGE_INTEGER g_spreadLast = {};
static float g_lastViewYaw = 0.0f, g_lastViewPitch = 0.0f, g_lastPos[2] = {};
static bool g_spreadHave = false;
static DWORD g_lastKickTick = 0;
extern "C" {
volatile int g_hideLaserDot = 0;
void* g_origLaserDot = nullptr;
}
static float g_gunStart[3], g_gunEnd[3], g_gunHit[3];  // the laser (the gun's line) and its first hit
static volatile DWORD g_gunTick = 0;
static bool g_onTarget = false;  // the gun's line rests on something that takes damage

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
// The widest spread of the weapon in hand (radians): half of its sway limit (TMP 0.122 -> 3.5 deg).
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
        else
            g_spread += (target - g_spread) * (1.0f - expf(-dt / 0.3f));
    }
    g_lastViewYaw = yaw, g_lastViewPitch = pitch, g_lastPos[0] = px, g_lastPos[1] = pz;
    g_spreadHave = havePl;
}

// One shot: the spread opens (once per frame, so a shotgun blast counts once).
static void KickSpread() {
    const DWORD now = GetTickCount();
    if (now == g_lastKickTick) return;
    g_lastKickTick = now;
    const float max = SpreadMax();
    g_spread = fminf(max, g_spread + max * 0.3f);
}

// The aim camera as shown this frame (set by the camera hook while the centered aim runs): its turn opens the spread.
void SetShotView(const float* eye, const float* tgt) {
    const float d[3] = {tgt[0] - eye[0], tgt[1] - eye[1], tgt[2] - eye[2]};
    const float l = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    if (l < 1.0f) return;
    for (int i = 0; i < 3; ++i) g_viewDir[i] = d[i] / l;
    const DWORD now = GetTickCount();
    const bool fresh = now - g_viewTick > 200;  // a new aim
    g_viewTick = now;
    UpdateSpread(fresh);
}

// The current spread (radians), for the reticle; 0 without the centered aim.
float SpreadRadius() { return GetTickCount() - g_viewTick > 200 ? 0.0f : g_spread; }

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

// For the reticle: the aim rests on something that takes damage.
bool AimOnTarget() { return g_onTarget && GetTickCount() - g_gunTick <= 100; }

// The gun's line, from the muzzle to its first hit, as the laser would show it without the mod turning it.
bool GunLine(float* start, float* hit) {
    if (GetTickCount() - g_gunTick > 100) return false;
    for (int i = 0; i < 3; ++i) start[i] = g_gunStart[i], hit[i] = g_gunHit[i];
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
    const int hit = g_lineHit(s, g_gunHit, 0, 0, &em, &poly);
    g_gunTick = GetTickCount();
    // Something that takes damage: an enemy or object with health left (the line hit only counts those: 2, or 3 on
    // some body parts), or a shootable surface close enough (LaserUpdate's own test).
    bool target = hit >= 2 && em != 0;
    if (hit == 1) {
        const uint32_t f = (uint32_t)poly;
        const int i = ((f & 0x800000) ? 1 : 0) + ((f & 0x8000) ? 2 : 0) + ((f & 0x80) ? 4 : 0);
        const uintptr_t at = Va(kPolyAttr);
        float d2 = 0.0f;
        for (int k = 0; k < 3; ++k) d2 += (g_gunHit[k] - start[k]) * (g_gunHit[k] - start[k]);
        target = *(const uint8_t*)(at + 0x240 + i) && (*(const uint32_t*)(at + 0x20 + i * 0x44) & 2) &&
                 d2 < *(const float*)Va(kShootableDist2);
    }
    g_onTarget = target;
    static int s_log = 0;
    static int s_lastHit = -1;
    static uintptr_t s_lastEm = 0;
    if (s_log < 80 && (hit != s_lastHit || (uintptr_t)em != s_lastEm)) {
        ++s_log;
        Log("aim at: hit %d em %p type %02X hp %d poly %08X -> %s", hit, (void*)em,
            em && Readable((void*)em, 0x328) ? *(uint8_t*)(em + 0x100) : 0xFF,
            em && Readable((void*)em, 0x328) ? *(int16_t*)(em + 0x324) : 0, poly, target ? "TARGET" : "-");
    }
    s_lastHit = hit, s_lastEm = (uintptr_t)em;
    g_hideLaserDot = Cfg().centeredAim && AimCentered();
}

static void __cdecl HookBullet(uintptr_t owner, float* from, float* to, uint32_t wep, uint32_t a5, uint32_t a6) {
    static int logged = 0;
    if (owner && owner == *(uintptr_t*)Va(kPlayerPtr) && Cfg().centeredAim && AimCentered() &&
        GetTickCount() - g_viewTick <= 100 && Readable(from, 12) && Readable(to, 12)) {
        const float scatter = g_spread;
        KickSpread();
        float dir[3] = {to[0] - from[0], to[1] - from[1], to[2] - from[2]};
        const float len = Normalize(dir);
        if (len > 0.0f && scatter > 0.0f) {
            Scatter(dir, scatter);
            float nt[3];
            for (int i = 0; i < 3; ++i) nt[i] = from[i] + dir[i] * len;
            if (logged < 5) {
                ++logged;
                Log("shot: weapon %u, spread %.2f deg", wep & 0xFFFF, scatter * 57.3f);
            }
            g_origBullet(owner, from, nt, wep, a5, a6);  // a copy: the caller's buffer stays as it was
            return;
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
        g_origSway(pl, mode, v, side);
        return;
    }
    g_origSway(pl, mode, v, side);
    *v = At<float>(sw, 0x28) * 0.63661977f, *side = At<float>(sw, 0x2C);  // back to the anchor: no drift, no kick
    g_swayOn = true;
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
    Hook(kFnLaserDot, kSigLaserDot, sizeof(kSigLaserDot), (void*)LaserDotStub, &g_origLaserDot, "laser dot");
}
