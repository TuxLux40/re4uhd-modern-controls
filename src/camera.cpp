// Free camera, M1 (yaw only).
//
// The game's gameplay camera (CameraQuasiFPS, camera mode 10) always sits behind Leon's yaw. Rather than draw our own
// rig, the mod keeps a world yaw of its own (g_yaw, turned by the right stick) and, after the game's QuasiFPS update,
// rotates the game's eye and target around Leon's vertical axis by (g_yaw - the game's camera yaw). Framing, height,
// shoulder offset and FOV stay exactly as the game draws them; only the heading is ours.
//
// Yaw convention (same as Leon's ang.y): yaw = atan2(dx, dz). Turning the view right lowers the yaw.
//
// While aiming the game's own aim camera runs untouched. Aiming starts by turning Leon by the current offset, so he
// faces where the free camera looked, and the free camera then follows the game's yaw until the aim ends.
#include "common.h"
#include "re4.h"
#include "camera.h"
#include <math.h>
#include "MinHook.h"

using namespace re4;

static float WrapPi(float a) {
    while (a > 3.14159265f) a -= 6.2831853f;
    while (a < -3.14159265f) a += 6.2831853f;
    return a;
}

static volatile float g_stickRx = 0.0f;  // right stick X, -1..1 after the dead zone (written by the pad thread)
static volatile float g_yaw = 0.0f;      // world yaw the free camera looks along
static bool g_haveYaw = false;
static volatile DWORD g_lastFrameTick = 0;
static bool g_wasAiming = false;
static volatile bool g_active = false;  // the free camera drew the last gameplay frame

// Camera-relative movement (M2): the pad thread turns the left stick into a world yaw for Leon and hands the game a
// plain "forward", so the game walks/runs him forward and never tank-turns or backpedals. The camera hook turns Leon
// toward that yaw every frame.
static volatile float g_moveYaw = 0.0f;
static volatile bool g_moving = false;
static bool g_wasMoving = false;

// Gameplay only: the camera hook ran within the last 100 ms (menus, the inventory and the merchant freeze the world,
// so the pad there is left alone).
bool FreeCameraActive() { return g_active && GetTickCount() - g_lastFrameTick < 100; }

static float DeadZone(float v, float dz) {
    return fabsf(v) < dz ? 0.0f : (v - copysignf(dz, v)) / (1.0f - dz);
}

static bool Aiming();

void FreeCameraPad(XGamepad& pad) {
    g_stickRx = DeadZone(pad.sThumbRX / 32767.0f, 0.2f);
    if (!FreeCameraActive()) {
        g_moving = false;
        return;
    }
    // The game's own right-stick look-around springs back and would fight the free yaw.
    pad.sThumbRX = 0;
    if (Aiming()) {
        g_moving = false;
        return;
    }
    const float lx = pad.sThumbLX / 32767.0f, ly = pad.sThumbLY / 32767.0f;
    const float mag = sqrtf(lx * lx + ly * ly);
    if (mag < 0.25f) {
        g_moving = false;
        pad.sThumbLX = pad.sThumbLY = 0;
        return;
    }
    // Stick angle: 0 = up, + = right. Screen-right is the camera yaw minus 90 degrees.
    g_moveYaw = WrapPi(g_yaw - atan2f(lx, ly));
    g_moving = true;
    pad.sThumbLX = 0;
    pad.sThumbLY = 32767;
}

static uintptr_t Player() {
    const uintptr_t p = *(uintptr_t*)Va(kPlayerPtr);
    return (p >= 0x10000 && Readable((void*)p, 0x200)) ? p : 0;
}

static float DeltaFrames() {
    const uintptr_t g = *(uintptr_t*)Va(kGlobalPtr);
    if (!g || !Readable((void*)(g + g::DeltaTime), 4)) return 1.0f;
    const float dt = At<float>(g, g::DeltaTime);
    return (dt > 0.0f && dt < 10.0f) ? dt : 1.0f;
}

static bool Aiming() {
    const uint64_t keys = *(const uint64_t*)Va(kKeyBtnOn);
    return (keys & (KEY_KAMAE | KEY_KAMAE_KNIFE)) != 0;
}

// Rotates p (x, z) around the pivot (x, z) so that its yaw grows by d.
static void RotateAround(float* p, const float* pivot, float c, float s) {
    const float x = p[0] - pivot[0], z = p[2] - pivot[2];
    p[0] = pivot[0] + x * c + z * s;
    p[2] = pivot[2] - x * s + z * c;
}

typedef void(__fastcall* PFN_QfpsMove)(uintptr_t self, void* edx);
static PFN_QfpsMove g_origQfpsMove = nullptr;

static void __fastcall HookQfpsMove(uintptr_t self, void* edx) {
    g_origQfpsMove(self, edx);
    g_active = false;
    const uintptr_t pl = Player();
    if (!pl) return;

    float* eye = (float*)(self + cc::CameraPoint);
    float* tgt = eye + 3;
    const float gameYaw = atan2f(tgt[0] - eye[0], tgt[2] - eye[2]);

    // A cutscene, door or menu in between: start again from the game's heading.
    const DWORD now = GetTickCount();
    if (!g_haveYaw || now - g_lastFrameTick > 500) {
        g_yaw = gameYaw;
        g_haveYaw = true;
        g_wasAiming = false;
    }
    g_lastFrameTick = now;

    const bool aiming = Aiming();
    if (aiming) {
        if (!g_wasAiming) {
            // Turn Leon so the aim starts where the free camera looked.
            float* ang = (float*)(pl + pl::Ang);
            ang[1] = WrapPi(ang[1] + WrapPi(g_yaw - gameYaw));
            Log("cam: aim start, leon yaw -> %.3f", ang[1]);
        }
        g_wasAiming = true;
        g_yaw = gameYaw;
        return;
    }
    g_wasAiming = false;

    if (g_moving) {
        float* ang = (float*)(pl + pl::Ang);
        const float diff = WrapPi(g_moveYaw - ang[1]);
        const float step = 0.30f * DeltaFrames();  // about 180 degrees in 0.35 s
        ang[1] = WrapPi(ang[1] + (fabsf(diff) <= step ? diff : copysignf(step, diff)));
        if (!g_wasMoving) Log("move: start, target yaw %.3f leon %.3f cam %.3f", g_moveYaw, ang[1], g_yaw);
    } else if (g_wasMoving) {
        Log("move: stop");
    }
    g_wasMoving = g_moving;

    const float turnRate = 0.10f;  // radians per 30 fps frame at full deflection (3 rad/s)
    g_yaw = WrapPi(g_yaw - g_stickRx * turnRate * DeltaFrames());

    const float d = WrapPi(g_yaw - gameYaw);
    const float* pos = (const float*)(pl + pl::Pos);
    const float c = cosf(d), s = sinf(d);
    RotateAround(eye, pos, c, s);
    RotateAround(tgt, pos, c, s);
    g_active = true;
}

bool InitCameraHooks() {
    void* target = (void*)Va(kFnQfpsMove);
    if (!SafeEqual(target, kSigQfpsMove, sizeof(kSigQfpsMove))) {
        Log("hook CameraQuasiFPS::move: unexpected code at %p, free camera off", target);
        return false;
    }
    if (MH_CreateHook(target, (void*)HookQfpsMove, (void**)&g_origQfpsMove) != MH_OK ||
        MH_EnableHook(target) != MH_OK) {
        Log("hook CameraQuasiFPS::move: MinHook failed");
        return false;
    }
    Log("hook CameraQuasiFPS::move at %p", target);
    return true;
}
