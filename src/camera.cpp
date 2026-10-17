// Free camera and camera-relative movement.
//
// Camera: the game's gameplay camera (CameraQuasiFPS, camera mode 10) always sits behind Leon's yaw. Rather than draw
// our own rig, the mod keeps a world yaw and a pitch of its own (turned by the right stick, nothing springs back) and,
// after the game's QuasiFPS update, moves the game's eye and target as one rig: raised by Config::height, rotated
// around Leon's vertical axis by (g_yaw - the game's camera yaw), then tilted around a pivot at the game's target
// height by g_pitch. FOV and shoulder offset stay the game's.
//
// Yaw convention (same as Leon's ang.y): yaw = atan2(dx, dz). Turning the view right lowers the yaw. Pitch > 0 lifts
// the camera and looks down.
//
// Movement: the pad thread turns the left stick into a world yaw for Leon (camera yaw minus stick angle) and hands the
// game a plain "forward", so the game walks/runs him forward and never tank-turns or backpedals; the camera hook turns
// Leon toward that yaw every frame. After the game converts the pad to action bits (KeyConvert), the run bit is set
// or cleared according to Config::run.
//
// While aiming the game's own aim camera runs. Aiming starts by turning Leon by the current yaw offset, so he faces
// where the free camera looked; the free camera follows the game's yaw until the aim ends. The game aims with the
// right stick (UHD swaps the sticks while the aim action is held). With Config::aimWalk the pad thread keeps the left
// stick from the game and the camera hook moves Leon's position along it, relative to the aim camera, with no
// animation (he glides).
#include "common.h"
#include "re4.h"
#include "camera.h"
#include "config.h"
#include <math.h>
#include "MinHook.h"

using namespace re4;

static float WrapPi(float a) {
    while (a > 3.14159265f) a -= 6.2831853f;
    while (a < -3.14159265f) a += 6.2831853f;
    return a;
}
static float Clamp(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }

// Written by the pad thread.
static volatile float g_stickRx = 0.0f, g_stickRy = 0.0f;  // right stick, -1..1 after the dead zone
static volatile float g_moveYaw = 0.0f;                     // world yaw Leon should face
static volatile float g_moveMag = 0.0f;                     // left stick deflection 0..1
static volatile bool g_moving = false;
static volatile float g_aimMoveX = 0.0f, g_aimMoveY = 0.0f;  // left stick while aiming (aim walk)

// Camera state (game thread).
static volatile float g_yaw = 0.0f;  // world yaw the free camera looks along
static float g_pitch = 0.0f;         // radians on top of the game's own camera pitch
static bool g_haveYaw = false;
static volatile DWORD g_lastFrameTick = 0;
static bool g_wasAiming = false;
static bool g_wasMoving = false;
enum View { VIEW_NONE, VIEW_FREE, VIEW_AIM };
static volatile int g_view = VIEW_NONE;  // what the camera hook did on the last gameplay frame

// Gameplay only: the camera hook ran within the last 100 ms (menus, the inventory and the merchant freeze the world,
// so the pad there is left alone).
static bool Fresh() { return GetTickCount() - g_lastFrameTick < 100; }
bool FreeCameraActive() { return g_view == VIEW_FREE && Fresh(); }
static bool AimActive() { return g_view == VIEW_AIM && Fresh(); }

static float DeadZone(float v, float dz) {
    return fabsf(v) < dz ? 0.0f : (v - copysignf(dz, v)) / (1.0f - dz);
}

static bool Aiming() {
    const uint64_t keys = *(const uint64_t*)Va(kKeyBtnOn);
    return (keys & (KEY_KAMAE | KEY_KAMAE_KNIFE)) != 0;
}

void FreeCameraPad(XGamepad& pad) {
    ConfigTick();
    const Config& c = Cfg();
    g_stickRx = DeadZone(pad.sThumbRX / 32767.0f, 0.2f) * (c.invertX ? -1.0f : 1.0f);
    g_stickRy = DeadZone(pad.sThumbRY / 32767.0f, 0.2f) * (c.invertY ? -1.0f : 1.0f);
    g_aimMoveX = g_aimMoveY = 0.0f;
    if (AimActive() || (FreeCameraActive() && Aiming())) {
        g_moving = false;
        if (c.aimWalk) {
            const float ax = pad.sThumbLX / 32767.0f, ay = pad.sThumbLY / 32767.0f;
            if (ax * ax + ay * ay >= 0.25f * 0.25f) {
                g_aimMoveX = ax;
                g_aimMoveY = ay;
            }
            // The game already aims with the right stick: while the aim action is held, KeyConvert fills Joy[0]'s
            // left stick (which PlWepLockCtrl reads) from the pad's right stick. The left stick is only ours to walk.
            pad.sThumbLX = pad.sThumbLY = 0;
        }
        return;
    }
    if (!FreeCameraActive()) {
        g_moving = false;
        return;
    }
    // The game's own right-stick look-around springs back and would fight the free camera.
    pad.sThumbRX = pad.sThumbRY = 0;
    const float lx = pad.sThumbLX / 32767.0f, ly = pad.sThumbLY / 32767.0f;
    const float mag = sqrtf(lx * lx + ly * ly);
    if (mag < 0.25f) {
        g_moving = false;
        pad.sThumbLX = pad.sThumbLY = 0;
        return;
    }
    // Stick angle: 0 = up, + = right. Screen-right is the camera yaw minus 90 degrees.
    g_moveYaw = WrapPi(g_yaw - atan2f(lx, ly));
    g_moveMag = mag > 1.0f ? 1.0f : mag;
    g_moving = true;
    pad.sThumbLX = 0;
    pad.sThumbLY = 32767;
}

static uintptr_t Player() {
    const uintptr_t p = *(uintptr_t*)Va(kPlayerPtr);
    return (p >= 0x10000 && Readable((void*)p, 0x200)) ? p : 0;
}

// The game's line-vs-world trace (re4.h kFnTrace) on the world collision set. Returns true and the hit point.
typedef int(__fastcall* PFN_Trace)(uintptr_t self, void* edx, const float* from, const float* to, float* hit,
                                   int unused, uint32_t maskA, uint32_t maskB);
static PFN_Trace g_trace = nullptr;

static bool Trace(const float* from, const float* to, float* hit, uint32_t maskA, uint32_t maskB) {
    if (!g_trace) return false;
    float end[3] = {to[0], to[1], to[2]};
    return g_trace(Va(kCollWorld), nullptr, from, end, hit, 0, maskA, maskB) != 0;
}

// Aim walk: moves Leon by (dx, dz) unless a wall is in the way at knee height; slides along it when one axis is free.
static void WalkWithCollision(float* pos, float dx, float dz) {
    const float radius = 300.0f;  // keeps him off the wall
    auto clear = [&](float mx, float mz) {
        const float len = sqrtf(mx * mx + mz * mz);
        if (len < 1e-3f) return false;
        const float from[3] = {pos[0], pos[1] + 500.0f, pos[2]};
        const float to[3] = {from[0] + mx + mx / len * radius, from[1], from[2] + mz + mz / len * radius};
        float hit[3];
        return !Trace(from, to, hit, kTracePlA, kTracePlB);
    };
    if (clear(dx, dz)) {
        pos[0] += dx;
        pos[2] += dz;
    } else if (clear(dx, 0.0f)) {
        pos[0] += dx;
    } else if (clear(0.0f, dz)) {
        pos[2] += dz;
    }
}

// Keeps the camera in front of walls: traces from Leon's head to the eye and pulls the eye in to the first hit.
static void CameraCollision(const float* pos, float* eye) {
    const float head[3] = {pos[0], pos[1] + 1500.0f, pos[2]};
    float hit[3];
    if (!Trace(head, eye, hit, kTraceCamA, kTraceCamB)) return;
    const float dx = head[0] - hit[0], dy = head[1] - hit[1], dz = head[2] - hit[2];
    const float len = sqrtf(dx * dx + dy * dy + dz * dz);
    const float back = len > 150.0f ? 150.0f / len : 1.0f;  // stay 150 units off the wall
    eye[0] = hit[0] + dx * back;
    eye[1] = hit[1] + dy * back;
    eye[2] = hit[2] + dz * back;
}

// Seconds since the last frame (the game counts in 30 fps frames).
static float DeltaSec() {
    const uintptr_t g = *(uintptr_t*)Va(kGlobalPtr);
    float dt = 1.0f;
    if (g && Readable((void*)(g + g::DeltaTime), 4)) {
        dt = At<float>(g, g::DeltaTime);
        if (!(dt > 0.0f && dt < 10.0f)) dt = 1.0f;
    }
    return dt / 30.0f;
}

// Rotates p (x, z) around the pivot (x, z) so that its yaw grows by d.
static void RotateYaw(float* p, const float* pivot, float c, float s) {
    const float x = p[0] - pivot[0], z = p[2] - pivot[2];
    p[0] = pivot[0] + x * c + z * s;
    p[2] = pivot[2] - x * s + z * c;
}

// Tilts p around the horizontal axis through pivot that is perpendicular to the forward direction f (x, z unit):
// the camera's forward turns down by the angle whose cos/sin are c/s.
static void RotatePitch(float* p, const float* pivot, float fx, float fz, float c, float s) {
    const float x = p[0] - pivot[0], y = p[1] - pivot[1], z = p[2] - pivot[2];
    const float a = x * fx + z * fz;           // along forward
    const float lx = x - a * fx, lz = z - a * fz;  // sideways part, unchanged
    const float a2 = a * c + y * s;
    const float y2 = -a * s + y * c;
    p[0] = pivot[0] + lx + a2 * fx;
    p[1] = pivot[1] + y2;
    p[2] = pivot[2] + lz + a2 * fz;
}

typedef void(__fastcall* PFN_QfpsMove)(uintptr_t self, void* edx);
static PFN_QfpsMove g_origQfpsMove = nullptr;

static void __fastcall HookQfpsMove(uintptr_t self, void* edx) {
    g_origQfpsMove(self, edx);
    g_view = VIEW_NONE;
    const uintptr_t pl = Player();
    if (!pl) return;
    const Config& cfg = Cfg();

    float* eye = (float*)(self + cc::CameraPoint);
    float* tgt = eye + 3;
    const float gameYaw = atan2f(tgt[0] - eye[0], tgt[2] - eye[2]);

    // A cutscene, door or menu in between: start again from the game's heading and the resting pitch.
    const DWORD now = GetTickCount();
    if (!g_haveYaw || now - g_lastFrameTick > 500) {
        g_yaw = gameYaw;
        g_pitch = cfg.defaultPitch;
        g_haveYaw = true;
        g_wasAiming = false;
    }
    g_lastFrameTick = now;
    const float dt = DeltaSec();

    if (Aiming()) {
        if (!g_wasAiming) {
            // Turn Leon so the aim starts where the free camera looked.
            float* ang = (float*)(pl + pl::Ang);
            ang[1] = WrapPi(ang[1] + WrapPi(g_yaw - gameYaw));
            Log("cam: aim start, leon yaw -> %.3f", ang[1]);
        }
        g_wasAiming = true;
        g_yaw = gameYaw;
        g_view = VIEW_AIM;
        const float ax = g_aimMoveX, ay = g_aimMoveY;
        if (cfg.aimWalk && (ax != 0.0f || ay != 0.0f)) {
            // Relative to the aim camera: stick up walks where the camera looks.
            float mag = sqrtf(ax * ax + ay * ay);
            if (mag > 1.0f) mag = 1.0f;
            const float yaw = gameYaw - atan2f(ax, ay);
            float* pos = (float*)(pl + pl::Pos);
            const float stepLen = cfg.aimWalkSpeed * mag * dt;
            WalkWithCollision(pos, sinf(yaw) * stepLen, cosf(yaw) * stepLen);
        }
        return;
    }
    g_wasAiming = false;

    float* ang = (float*)(pl + pl::Ang);
    if (g_moving) {
        const float diff = WrapPi(g_moveYaw - ang[1]);
        const float step = cfg.turnSpeed * dt;
        ang[1] = WrapPi(ang[1] + (fabsf(diff) <= step ? diff : copysignf(step, diff)));
        if (!g_wasMoving) Log("move: start, target yaw %.3f leon %.3f cam %.3f", g_moveYaw, ang[1], g_yaw);
    } else if (g_wasMoving) {
        Log("move: stop");
    }
    g_wasMoving = g_moving;

    g_yaw = WrapPi(g_yaw - g_stickRx * cfg.yawSpeed * dt);
    // Stick up looks up: the camera comes down.
    g_pitch = Clamp(g_pitch - g_stickRy * cfg.pitchSpeed * dt, -0.6f, 1.0f);

    const float* pos = (const float*)(pl + pl::Pos);
    eye[1] += cfg.height;
    tgt[1] += cfg.height;

    const float d = WrapPi(g_yaw - gameYaw);
    const float cy = cosf(d), sy = sinf(d);
    RotateYaw(eye, pos, cy, sy);
    RotateYaw(tgt, pos, cy, sy);

    const float pivot[3] = {pos[0], tgt[1], pos[2]};
    RotatePitch(eye, pivot, sinf(g_yaw), cosf(g_yaw), cosf(g_pitch), sinf(g_pitch));
    RotatePitch(tgt, pivot, sinf(g_yaw), cosf(g_yaw), cosf(g_pitch), sinf(g_pitch));
    // Looking up must not put the camera into the floor.
    const float minEye = pos[1] + 250.0f;
    if (eye[1] < minEye) {
        tgt[1] += minEye - eye[1];
        eye[1] = minEye;
    }
    CameraCollision(pos, eye);
    g_view = VIEW_FREE;
}

// KeyConvert: the game's per-frame pad -> KEY_BTN conversion (writes Key_btn_on). Afterwards the run bit follows
// Config::run while the stick moves Leon.
typedef void(__cdecl* PFN_KeyConvert)();
static PFN_KeyConvert g_origKeyConvert = nullptr;

static void __cdecl HookKeyConvert() {
    g_origKeyConvert();
    if (!g_moving || !FreeCameraActive()) return;
    uint64_t& keys = *(uint64_t*)Va(kKeyBtnOn);
    const Config& c = Cfg();
    bool run;
    switch (c.run) {
        case RUN_ALWAYS: run = !(keys & KEY_RUN); break;  // the run button walks
        case RUN_OUTER_RING: run = g_moveMag >= c.outerRing || (keys & KEY_RUN); break;
        default: return;
    }
    keys = run ? (keys | KEY_RUN) : (keys & ~KEY_RUN);
}

static bool Hook(uint32_t rva, const uint8_t* sig, size_t n, void* detour, void** orig, const char* name) {
    void* target = (void*)Va(rva);
    if (!SafeEqual(target, sig, n)) {
        Log("hook %s: unexpected code at %p, left alone", name, target);
        return false;
    }
    if (MH_CreateHook(target, detour, orig) != MH_OK || MH_EnableHook(target) != MH_OK) {
        Log("hook %s: MinHook failed", name);
        return false;
    }
    Log("hook %s at %p", name, target);
    return true;
}

bool InitCameraHooks() {
    LoadConfig();
    if (SafeEqual((void*)Va(kFnTrace), kSigTrace, sizeof(kSigTrace))) {
        g_trace = (PFN_Trace)Va(kFnTrace);
        Log("trace at %p", (void*)g_trace);
    } else {
        Log("trace: unexpected code at %p, no wall checks", (void*)Va(kFnTrace));
    }
    const bool cam = Hook(kFnQfpsMove, kSigQfpsMove, sizeof(kSigQfpsMove), (void*)HookQfpsMove,
                          (void**)&g_origQfpsMove, "CameraQuasiFPS::move");
    Hook(kFnKeyConvert, kSigKeyConvert, sizeof(kSigKeyConvert), (void*)HookKeyConvert, (void**)&g_origKeyConvert,
         "KeyConvert");
    return cam;
}
