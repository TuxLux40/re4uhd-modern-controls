// Game hooks and the controls layer.
// Milestone 0: the proxy loads, forwards XInput and logs the left stick, so the load path (Proton,
// WINEDLLOVERRIDES, re4_tweaks side by side) is proven before any game code is changed.
// Probe for M1: CameraControl::move is hooked read-only and logs the camera mode, eye/target and Leon's
// position/yaw once a second, to tell which camera mode is the gameplay camera.
#include "common.h"
#include "re4.h"
#include <math.h>
#include "MinHook.h"

using namespace re4;

void TransformPad(DWORD, XGamepad& pad) {
    static DWORD last = 0;
    const DWORD now = GetTickCount();
    if (now - last >= 2000 && (pad.sThumbLX || pad.sThumbLY)) {
        last = now;
        Log("pad0: L(%d,%d) R(%d,%d) buttons %04X", pad.sThumbLX, pad.sThumbLY, pad.sThumbRX, pad.sThumbRY,
            pad.wButtons);
    }
}

static uintptr_t Player() {
    const uintptr_t p = *(uintptr_t*)Va(kPlayerPtr);
    return (p >= 0x10000 && Readable((void*)p, 0x200)) ? p : 0;
}

typedef void(__fastcall* PFN_CamMove)(uintptr_t self, void* edx);
static PFN_CamMove g_origCamMove = nullptr;

static void __fastcall HookCamMove(uintptr_t self, void* edx) {
    g_origCamMove(self, edx);
    static DWORD last = 0;
    static uint8_t lastMode = 0xFF;
    const DWORD now = GetTickCount();
    const uint8_t mode = At<uint8_t>(self, cc::Mode);
    if (mode == lastMode && now - last < 1000) return;
    last = now;
    lastMode = mode;
    const float* eye = (const float*)(self + cc::Point);
    const float* tgt = eye + 3;
    const float camYaw = atan2f(tgt[0] - eye[0], tgt[2] - eye[2]);
    if (const uintptr_t pl = Player()) {
        const float* pos = (const float*)(pl + pl::Pos);
        const float* ang = (const float*)(pl + pl::Ang);
        Log("cam mode %u eye(%.0f,%.0f,%.0f) tgt(%.0f,%.0f,%.0f) yaw %.3f fov %.1f | leon pos(%.0f,%.0f,%.0f) yaw %.3f "
            "rno %02X%02X%02X%02X",
            mode, eye[0], eye[1], eye[2], tgt[0], tgt[1], tgt[2], camYaw, eye[7], pos[0], pos[1], pos[2], ang[1],
            At<uint8_t>(pl, pl::RNo0), At<uint8_t>(pl, pl::RNo0 + 1), At<uint8_t>(pl, pl::RNo0 + 2),
            At<uint8_t>(pl, pl::RNo0 + 3));
    } else {
        Log("cam mode %u (no player)", mode);
    }
}

static bool HookAt(uint32_t rva, const uint8_t* sig, size_t n, void* detour, void** orig, const char* name) {
    void* target = (void*)Va(rva);
    if (!SafeEqual(target, sig, n)) {
        Log("hook %s: unexpected code at %p, left alone (different exe build?)", name, target);
        return false;
    }
    if (MH_CreateHook(target, detour, orig) != MH_OK || MH_EnableHook(target) != MH_OK) {
        Log("hook %s: MinHook failed", name);
        return false;
    }
    Log("hook %s at %p", name, target);
    return true;
}

void InitGameHooks() {
    HookAt(kFnCamCtrlMove, kSigCamCtrlMove, sizeof(kSigCamCtrlMove), (void*)HookCamMove, (void**)&g_origCamMove,
           "CameraControl::move");
}
