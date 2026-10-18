// Game hooks and the controls layer.
// Milestone 0: the proxy loads, forwards XInput and logs the left stick, so the load path (Proton,
// WINEDLLOVERRIDES, re4_tweaks side by side) is proven before any game code is changed.
// M1: free camera yaw (camera.cpp). CameraControl::move stays hooked read-only as a probe: it logs the camera mode,
// eye/target and Leon's pose whenever the camera mode, Leon's routine bytes or the aim keys change.
#include "common.h"
#include <stdio.h>
#include "re4.h"
#include "camera.h"
#include <math.h>
#include "MinHook.h"

using namespace re4;

// Debug input: scripted pad states read from Bin32/RE4MC_input.txt (deleted once read), one step per line:
// "<ms> <lx> <ly> <rx> <ry> <buttons hex> <lt> <rt>" with sticks in -1..1 and triggers 0..255. While a script runs it
// replaces the physical pad, so a session can be driven without touching the controller. Only exists to test the mod.
struct DebugStep {
    DWORD ms;
    float lx, ly, rx, ry;
    unsigned buttons, lt, rt;
};
static DebugStep g_steps[64];
static int g_stepCount = 0, g_step = -1;
static DWORD g_stepStart = 0, g_lastScriptCheck = 0;

bool DebugInputActive() { return g_step >= 0; }

// A script is running or waiting to be read (checked at most every 200 ms).
bool DebugInputPending() {
    if (g_step >= 0) return true;
    static DWORD last = 0;
    static bool pending = false;
    const DWORD now = GetTickCount();
    if (now - last >= 200) {
        last = now;
        char path[MAX_PATH];
        wsprintfA(path, "%sRE4MC_input.txt", ModDir());
        pending = GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES;
    }
    return pending;
}

static void DebugInput(XGamepad& pad) {
    const DWORD now = GetTickCount();
    if (g_step < 0 && now - g_lastScriptCheck >= 200) {
        g_lastScriptCheck = now;
        char path[MAX_PATH];
        wsprintfA(path, "%sRE4MC_input.txt", ModDir());
        if (FILE* f = fopen(path, "r")) {
            g_stepCount = 0;
            DebugStep st;
            while (g_stepCount < 64 && fscanf(f, "%lu %f %f %f %f %x %u %u", &st.ms, &st.lx, &st.ly, &st.rx, &st.ry,
                                              &st.buttons, &st.lt, &st.rt) == 8)
                g_steps[g_stepCount++] = st;
            fclose(f);
            DeleteFileA(path);
            if (g_stepCount > 0) {
                g_step = 0;
                g_stepStart = now;
                Log("debug input: %d steps", g_stepCount);
            }
        }
    }
    if (g_step < 0) return;
    while (g_step < g_stepCount && now - g_stepStart >= g_steps[g_step].ms) {
        g_stepStart += g_steps[g_step].ms;
        ++g_step;
    }
    if (g_step >= g_stepCount) {
        g_step = -1;
        Log("debug input: done");
        return;
    }
    const DebugStep& st = g_steps[g_step];
    auto axis = [](float v) { return (SHORT)(v < -1.0f ? -32767 : v > 1.0f ? 32767 : v * 32767.0f); };
    pad.sThumbLX = axis(st.lx);
    pad.sThumbLY = axis(st.ly);
    pad.sThumbRX = axis(st.rx);
    pad.sThumbRY = axis(st.ry);
    pad.wButtons = (WORD)st.buttons;
    pad.bLeftTrigger = (BYTE)st.lt;
    pad.bRightTrigger = (BYTE)st.rt;
}

void RemapButtons(XGamepad& pad);
void PromptsTick();
void TransformPad(DWORD, XGamepad& pad) {
    DebugInput(pad);
    RemapButtons(pad);
    PromptsTick();
    FreeCameraPad(pad);
    static DWORD last = 0;
    const DWORD now = GetTickCount();
    if (now - last >= 2000 && (pad.sThumbLX || pad.sThumbLY)) {
        last = now;
        Log("pad0: L(%d,%d) R(%d,%d) buttons %04X triggers %u/%u", pad.sThumbLX, pad.sThumbLY, pad.sThumbRX,
            pad.sThumbRY, pad.wButtons, pad.bLeftTrigger, pad.bRightTrigger);
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
    static uint8_t lastMode = 0xFF;
    static uint32_t lastRno = 0xFFFFFFFF;
    static uint64_t lastKeys = ~0ull;
    const uint8_t mode = At<uint8_t>(self, cc::Mode);
    const uintptr_t plp = Player();
    const uint32_t rno = plp ? At<uint32_t>(plp, pl::RNo0) : 0;
    const uint64_t keys = *(const uint64_t*)Va(kKeyBtnOn) & (KEY_KAMAE | KEY_KAMAE_KNIFE);
    if (mode == lastMode && rno == lastRno && keys == lastKeys) return;
    lastMode = mode;
    lastRno = rno;
    lastKeys = keys;
    const float* eye = (const float*)(self + cc::Point);
    const float* tgt = eye + 3;
    const float camYaw = atan2f(tgt[0] - eye[0], tgt[2] - eye[2]);
    if (const uintptr_t pl = Player()) {
        const float* pos = (const float*)(pl + pl::Pos);
        const float* ang = (const float*)(pl + pl::Ang);
        Log("cam mode %u eye(%.0f,%.0f,%.0f) tgt(%.0f,%.0f,%.0f) yaw %.3f fov %.1f | leon pos(%.0f,%.0f,%.0f) yaw %.3f "
            "rno %02X%02X%02X%02X aimkeys %llX free %d",
            mode, eye[0], eye[1], eye[2], tgt[0], tgt[1], tgt[2], camYaw, eye[7], pos[0], pos[1], pos[2], ang[1],
            At<uint8_t>(pl, pl::RNo0), At<uint8_t>(pl, pl::RNo0 + 1), At<uint8_t>(pl, pl::RNo0 + 2),
            At<uint8_t>(pl, pl::RNo0 + 3), (unsigned long long)keys, FreeCameraActive() ? 1 : 0);
    } else {
        Log("cam mode %u (no player)", mode);
    }
}

bool HookAt(uint32_t rva, const uint8_t* sig, size_t n, void* detour, void** orig, const char* name) {
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

void InitButtonHooks();
void InitPromptHooks();
void InitQteHooks();
void InitGameHooks() {
    InitButtonHooks();
    InitPromptHooks();
    InitQteHooks();
    InitCameraHooks();
    HookAt(kFnCamCtrlMove, kSigCamCtrlMove, sizeof(kSigCamCtrlMove), (void*)HookCamMove, (void**)&g_origCamMove,
           "CameraControl::move");
}
