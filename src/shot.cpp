// Shots land under the crosshair. Every gunshot goes through the game's shot raycast (re4.h kFnShotRay), a line test
// from the muzzle against the world and enemies. With the centered aim on, the line is turned so it points at what is
// in the middle of the screen: the camera's centre line is cast with the same raycast (so enemies count, not just
// walls), and each shot is rotated by the angle between the gun's line (the laser) and the line from the muzzle to
// that point. Rotating instead of replacing keeps shotgun spread and the like.
#include "common.h"
#include "re4.h"
#include "config.h"
#include <math.h>
#include "MinHook.h"

using namespace re4;

bool LaserLine(float* start, float* end);

// The raycast takes ECX = int* hit enemy (out), EDX = shot type, EDI = float[3] hit point (out), and on the stack
// from, to, a mode value (caller cleans up). C can't call that directly, so the call goes through this block.
extern "C" {
struct ShotCall {
    uint32_t fn, ecx, edx, from, to, mode, hit;
};
ShotCall g_shotCall;
void* g_origShotRay = nullptr;
void __cdecl AdjustShotRay(uint32_t* regs, float** args);
}

static void CallShotRay() {
    asm volatile(
        "pushl _g_shotCall+20\n\t"
        "pushl _g_shotCall+16\n\t"
        "pushl _g_shotCall+12\n\t"
        "movl _g_shotCall+4, %%ecx\n\t"
        "movl _g_shotCall+8, %%edx\n\t"
        "movl _g_shotCall+24, %%edi\n\t"
        "call *_g_shotCall\n\t"
        "addl $12, %%esp\n\t" ::
            : "eax", "ecx", "edx", "edi", "xmm0", "xmm1", "xmm2", "xmm3", "xmm4", "xmm5", "xmm6", "xmm7", "memory",
              "cc");
}

// Detour: save every register, let AdjustShotRay edit from/to on the caller's stack, then run the original as if
// nothing happened. After pushal the return address is at esp+32 and the stack arguments start at esp+36.
extern "C" __attribute__((naked)) void ShotRayStub() {
    asm("pushal\n\t"
        "leal 36(%esp), %eax\n\t"
        "pushl %eax\n\t"
        "leal 4(%esp), %eax\n\t"
        "pushl %eax\n\t"
        "call _AdjustShotRay\n\t"
        "addl $8, %esp\n\t"
        "popal\n\t"
        "jmp *_g_origShotRay\n\t");
}

static float g_viewEye[3], g_viewDir[3];
static volatile DWORD g_viewTick = 0;
static bool g_inAdjust = false;

// The aim camera as shown this frame (set by the camera hook while the centered aim runs).
void SetShotView(const float* eye, const float* tgt) {
    const float d[3] = {tgt[0] - eye[0], tgt[1] - eye[1], tgt[2] - eye[2]};
    const float l = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    if (l < 1.0f) return;
    for (int i = 0; i < 3; ++i) g_viewEye[i] = eye[i], g_viewDir[i] = d[i] / l;
    g_viewTick = GetTickCount();
}

static bool Normalize(float* v) {
    const float l = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    if (l < 1e-4f) return false;
    v[0] /= l, v[1] /= l, v[2] /= l;
    return true;
}

extern "C" void __cdecl AdjustShotRay(uint32_t* regs, float** args) {
    // pushal frame: edi esi ebp esp ebx edx ecx eax
    if (g_inAdjust || !Cfg().centeredAim || GetTickCount() - g_viewTick > 100) return;
    float* from = args[0];
    float* to = args[1];
    const uint32_t mode = (uint32_t)(uintptr_t)args[2];
    float ls[3], le[3];
    if (!Readable(from, 12) || !Readable(to, 12) || !LaserLine(ls, le)) return;
    // Only Leon's own shots: they start at his gun, where the laser starts (enemy weapons may use this raycast too).
    const float ox = from[0] - ls[0], oy = from[1] - ls[1], oz = from[2] - ls[2];
    if (ox * ox + oy * oy + oz * oz > 400.0f * 400.0f) return;

    // What is in the middle of the screen.
    static float camFrom[3], camTo[3], hit[3];
    static int hitEm;
    for (int i = 0; i < 3; ++i) camFrom[i] = g_viewEye[i], camTo[i] = g_viewEye[i] + g_viewDir[i] * 40000.0f;
    for (int i = 0; i < 3; ++i) hit[i] = camTo[i];
    hitEm = 0;
    g_shotCall = {(uint32_t)(uintptr_t)g_origShotRay, (uint32_t)(uintptr_t)&hitEm, regs[5],
                  (uint32_t)(uintptr_t)camFrom, (uint32_t)(uintptr_t)camTo, mode, (uint32_t)(uintptr_t)hit};
    g_inAdjust = true;
    CallShotRay();
    g_inAdjust = false;

    float gun[3] = {le[0] - ls[0], le[1] - ls[1], le[2] - ls[2]};
    float want[3] = {hit[0] - from[0], hit[1] - from[1], hit[2] - from[2]};
    float shot[3] = {to[0] - from[0], to[1] - from[1], to[2] - from[2]};
    const float len = sqrtf(shot[0] * shot[0] + shot[1] * shot[1] + shot[2] * shot[2]);
    if (!Normalize(gun) || !Normalize(want) || !Normalize(shot)) return;
    if (gun[0] * want[0] + gun[1] * want[1] + gun[2] * want[2] < 0.5f) return;  // centre point behind the gun
    float nd[3] = {shot[0] + want[0] - gun[0], shot[1] + want[1] - gun[1], shot[2] + want[2] - gun[2]};
    if (!Normalize(nd)) return;
    for (int i = 0; i < 3; ++i) to[i] = from[i] + nd[i] * len;

    static int logged = 0;
    if (logged < 5) {
        ++logged;
        Log("shot: turned %.2f deg toward the crosshair (hit %s at %.0f,%.0f,%.0f)",
            acosf(fminf(1.0f, gun[0] * want[0] + gun[1] * want[1] + gun[2] * want[2])) * 57.29578f,
            hitEm ? "enemy" : "world", hit[0], hit[1], hit[2]);
    }
}

void InitShotHook() {
    void* target = (void*)Va(kFnShotRay);
    if (!SafeEqual(target, kSigShotRay, sizeof(kSigShotRay))) {
        Log("shot ray: unexpected code at %p, shots follow the gun", target);
        return;
    }
    if (MH_CreateHook(target, (void*)ShotRayStub, &g_origShotRay) == MH_OK && MH_EnableHook(target) == MH_OK)
        Log("hook shot ray at %p", target);
    else
        Log("hook shot ray failed");
}
