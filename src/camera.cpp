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
#include <stdio.h>
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
// The step Leon takes this frame while aiming or in the reload glide, relative to his facing (legs.cpp walks his legs).
static float g_stepSide = 0.0f, g_stepFwd = 0.0f;
static bool g_stepReload = false;
static DWORD g_stepTick = 0;
static void SetStep(float moveYaw, float leonYaw, float mag, bool reload) {
    const float rel = WrapPi(moveYaw - leonYaw);  // turning right lowers the yaw
    g_stepSide = -sinf(rel) * mag, g_stepFwd = cosf(rel) * mag, g_stepReload = reload, g_stepTick = GetTickCount();
}
// reload: the reload glide (Leon turns the way he goes), not the aim walk.
bool AimStep(float* side, float* fwd, bool* reload) {
    if (GetTickCount() - g_stepTick > 100) return false;
    *side = g_stepSide, *fwd = g_stepFwd, *reload = g_stepReload;
    return true;
}
static volatile bool g_reloadHold = false;  // a reload keeps Leon moving (camera.cpp, the hook below)
static volatile float g_aimMoveX = 0.0f, g_aimMoveY = 0.0f;  // left stick while aiming (aim walk)

// Camera state (game thread).
static volatile float g_yaw = 0.0f;  // world yaw the free camera looks along
static float g_pitch = 0.0f;         // radians on top of the game's own camera pitch
static float g_appliedDefaultPitch = 0.0f;  // Config::defaultPitch that g_pitch currently includes
static float g_stickIdle = 0.0f;             // seconds since the right stick last turned the camera
static float g_modBlend = 1.0f;              // how much of the free camera's tilt and height applies (0..1)
static float g_room = -1.0f;                 // smoothed share of the camera distance free of walls (-1 = start over)
static float g_outPoint[8];  // our camera this frame: eye, target, roll, fovy (CamPoint layout)
static float g_back = -1.0f;  // smoothed free camera distance behind Leon (-1 = start over, -2 = from the shown view)
static bool g_haveYaw = false;
static volatile DWORD g_lastFrameTick = 0;
static bool g_wasAiming = false;
// The centered aim hands its view back to the free camera when it ends: the free camera's heading and tilt when the
// aim started, the aim view's offset from that heading then, and the aim view's heading and tilt now.
static float g_aimFreeYaw = 0.0f, g_aimFreePitch = 0.0f, g_aimPitch0 = 0.0f;
static float g_aimViewYaw = 0.0f, g_aimViewPitch = 0.0f;
static bool g_aimHandBack = false;
static float g_zoomS = 0.0f;    // the aim camera's zoom, eased (0 = the free camera's distance and field of view)
static float g_freeFov = 0.0f;  // the free camera's field of view, last free frame
static bool g_wasMoving = false;
enum View { VIEW_NONE, VIEW_FREE, VIEW_AIM };
static volatile int g_view = VIEW_NONE;  // what the camera hook did on the last gameplay frame

// Gameplay only: the camera hook ran within the last 100 ms (menus, the inventory and the merchant freeze the world,
// so the pad there is left alone).
static bool Fresh() { return GetTickCount() - g_lastFrameTick < 100; }
bool FreeCameraActive() { return g_view == VIEW_FREE && Fresh(); }
float DiagStick() { return fabsf(g_stickRx) + fabsf(g_stickRy); }  // diagnostics: right stick in use
DWORD GameCameraTick();  // game.cpp
uint8_t PrevCamMode();
bool GameplayView() { return g_view != VIEW_NONE && Fresh(); }  // the gameplay camera ran just now (no menu, no event)
static bool AimActive() { return g_view == VIEW_AIM && Fresh(); }

static float DeadZone(float v, float dz) {
    return fabsf(v) < dz ? 0.0f : (v - copysignf(dz, v)) / (1.0f - dz);
}

static bool Aiming() {
    const uint64_t keys = *(const uint64_t*)Va(kKeyBtnOn);
    return (keys & (KEY_KAMAE | KEY_KAMAE_KNIFE)) != 0;
}

static uintptr_t Player();
bool QuickReload();

// Free movement only while Leon stands, walks or runs (routine r_no_0 0, r_no_1 wait 0 / walk 1 / run 3). Grabbed,
// hit, ladders and other actions keep the game's own stick handling, so Leon isn't turned while an enemy holds him.
static bool FreeToMove() {
    const uintptr_t pl = Player();
    if (!pl) return false;
    const uint8_t r0 = At<uint8_t>(pl, pl::RNo0), r1 = At<uint8_t>(pl, pl::RNo0 + 1);
    return r0 == 0 && (r1 == 0 || r1 == 1 || r1 == 3);
}

void FreeCameraPad(XGamepad& pad) {
    const Config& c = Cfg();
    g_stickRx = DeadZone(pad.sThumbRX / 32767.0f, 0.2f) * (c.invertX ? -1.0f : 1.0f);
    g_stickRy = DeadZone(pad.sThumbRY / 32767.0f, 0.2f) * (c.invertY ? -1.0f : 1.0f);
    g_aimMoveX = g_aimMoveY = 0.0f;
    if (AimActive() || (FreeCameraActive() && Aiming()) || g_reloadHold) {
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
    if (!FreeToMove()) {
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
    g_moveMag = mag > 1.0f ? 1.0f : mag;
    g_moving = true;
    pad.sThumbLX = 0;
    pad.sThumbLY = 32767;
}

static uintptr_t Character(uint32_t ptrRva) {
    const uintptr_t p = *(uintptr_t*)Va(ptrRva);
    return (p >= 0x10000 && Readable((void*)p, 0x200) && At<uint32_t>(p, 0) >= 0x10000) ? p : 0;
}
static uintptr_t Player() { return Character(kPlayerPtr); }

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

// How much of the line from the pivot to the eye is free of walls (1 = nothing in the way), keeping the eye 150 units
// off a wall. The pivot is beside Leon's shoulder, at the original camera's height and sideways offset, so the line
// runs along the view direction: a camera pressed in by walls dollies in and keeps Leon on the left of the frame.
static float FreeRoom(const float* pivot, const float* eye) {
    float hit[3];
    if (!Trace(pivot, eye, hit, kTraceCamA, kTraceCamB)) return 1.0f;
    const float ex = eye[0] - pivot[0], ey = eye[1] - pivot[1], ez = eye[2] - pivot[2];
    const float full = sqrtf(ex * ex + ey * ey + ez * ez);
    const float dx = hit[0] - pivot[0], dy = hit[1] - pivot[1], dz = hit[2] - pivot[2];
    const float len = sqrtf(dx * dx + dy * dy + dz * dz);
    return full > 1.0f ? Clamp((len - 150.0f) / full, 0.0f, 1.0f) : 1.0f;
}

// Diagnostics: the wall pull-in (share of the camera distance kept), logged when it crosses 0.95 / 0.6 / 0.3 or the
// wall's share jumps by more than 0.3 from one frame to the next.
static void DiagRoom(const char* which, float room, float want) {
    static int s_band[2] = {3, 3};
    static float s_want[2] = {1.0f, 1.0f};
    const int k = which[0] == 'a' ? 1 : 0;
    const int band = room >= 0.95f ? 3 : room >= 0.6f ? 2 : room >= 0.3f ? 1 : 0;
    const bool jump = fabsf(want - s_want[k]) > 0.3f;
    if (band != s_band[k] || jump)
        Log("diag wall %s: room %.2f (wall allows %.2f, was %.2f)%s", which, room, want, s_want[k], jump ? " JUMP" : "");
    s_band[k] = band, s_want[k] = want;
}

// Ashley ducks behind Leon while he aims; vanilla never moved Leon then, so the game doesn't keep her on the floor.
// While aim-walking, her height follows the ground under her.
static void KeepAshleyOnGround() {
    const uintptr_t as = Character(kAshleyPtr);
    const uintptr_t pl = Player();
    if (!as || !pl) return;
    float* apos = (float*)(as + pl::Pos);
    const float* lpos = (const float*)(pl + pl::Pos);
    const float dx = apos[0] - lpos[0], dz = apos[2] - lpos[2];
    if (dx * dx + dz * dz > 3000.0f * 3000.0f) return;  // not the one behind Leon
    // Leon stands on the ground (the game keeps him there) and she is right behind him, so her floor is within a
    // flight of stairs of his height: trace down from a bit above it, never from her own (possibly sunk) height and
    // not so high that a balcony above her catches the trace.
    const float from[3] = {apos[0], lpos[1] + 700.0f, apos[2]};
    const float to[3] = {apos[0], lpos[1] - 1500.0f, apos[2]};
    float hit[3];
    if (Trace(from, to, hit, kTracePlA, kTracePlB) || Trace(from, to, hit, 0, 0)) apos[1] = hit[1];
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

static void CenterAimCamera(float* eye, float* tgt, float dt, bool start);
extern bool g_aimLocked;
void SetShotView(const float* eye, const float* tgt);
bool GunLine(float* start, float* hit);

typedef void(__fastcall* PFN_QfpsMove)(uintptr_t self, void* edx);
static PFN_QfpsMove g_origQfpsMove = nullptr;

static void __fastcall HookQfpsMoveBody(uintptr_t self, void* edx) {
    g_origQfpsMove(self, edx);
    g_view = VIEW_NONE;
    const uintptr_t pl = Player();
    if (!pl) return;
    ConfigTick();  // here, not in the pad poll: the game stops polling the pad while its window is unfocused
    const Config& cfg = Cfg();
    if (g_haveYaw && cfg.defaultPitch != g_appliedDefaultPitch)  // a new resting angle from the ini applies right away
        g_pitch += cfg.defaultPitch - g_appliedDefaultPitch;
    g_appliedDefaultPitch = cfg.defaultPitch;

    float* eye = (float*)(self + cc::CameraPoint);
    float* tgt = eye + 3;
    // Back from the game's action camera (a jump down, a hop, a hit), the game's own gameplay camera starts from a point
    // near the world's origin (about Leon's position / 10, fov 2-8), eases in from there for about 0.6 s and ends on one
    // frame at fov 100, tens of thousands of units away. Until it is back, use its last sane spot relative to Leon.
    {
        static float s_rel[6], s_fov = 50.0f;
        static bool s_ok = false;
        static DWORD s_log = 0;
        const float* lp = (const float*)(pl + pl::Pos);
        const float hx = eye[0] - lp[0], hz = eye[2] - lp[2];
        const bool sane = hx * hx + hz * hz < 6000.0f * 6000.0f && fabsf(eye[1] - lp[1]) < 6000.0f && eye[7] > 20.0f &&
                          eye[7] < 90.0f;
        if (sane) {
            for (int i = 0; i < 3; ++i) s_rel[i] = eye[i] - lp[i], s_rel[3 + i] = tgt[i] - lp[i];
            s_fov = eye[7];
            s_ok = true;
        } else if (s_ok) {
            if (GetTickCount() - s_log > 1000)
                s_log = GetTickCount(), Log("diag cam: game camera off (%.0f from leon, fov %.1f), last sane one used",
                                            sqrtf(hx * hx + hz * hz), eye[7]);
            for (int i = 0; i < 3; ++i) eye[i] = lp[i] + s_rel[i], tgt[i] = lp[i] + s_rel[3 + i];
            eye[7] = s_fov;
        }
    }
    // A reload with X alone: Leon is in his aim routine, so the game has switched to its own aim camera (closer, fov
    // 45). Keep the camera from before instead, following Leon as he glides.
    static float s_eye[3], s_tgt[3], s_pos[3], s_fov = 0.0f;
    static bool s_have = false, s_hold = false;
    {
        const float* lp = (const float*)(pl + pl::Pos);
        // The reload outlasts our fake aim by a few frames (Leon is still in his aim routine, r_no_1 6): the game's aim
        // camera must not show through then either.
        const bool stance = At<uint8_t>(pl, pl::RNo0) == 0 && At<uint8_t>(pl, pl::RNo0 + 1) == 6;
        const bool reloading = stance && At<uint8_t>(pl, pl::RNo0 + 2) == 4;  // r_no_2 4: the reload animation
        s_hold = QuickReload() || (s_hold && stance && !Aiming()) || (reloading && !Aiming());
        g_reloadHold = s_hold;
        if (!s_hold || !s_have) {
            if (!Aiming()) {
                for (int i = 0; i < 3; ++i) s_eye[i] = eye[i], s_tgt[i] = tgt[i], s_pos[i] = lp[i];
                s_fov = eye[7];
                s_have = true;
            }
        } else {
            for (int i = 0; i < 3; ++i) {
                const float d = lp[i] - s_pos[i];
                eye[i] = s_eye[i] + d, tgt[i] = s_tgt[i] + d;
            }
            if (s_fov > 1.0f) eye[7] = s_fov;
        }
    }
    const float gameYaw = atan2f(tgt[0] - eye[0], tgt[2] - eye[2]);

    // A cutscene or door in between (one of the game's own cameras ran) or a new area: start again from the game's
    // heading and the resting pitch. A menu, the inventory or the game's action camera (a jump, a hit) keeps the view.
    const DWORD now = GetTickCount();
    const float* lpNow = (const float*)(pl + pl::Pos);
    static float s_lastPos[3] = {}, s_lastLeonYaw = 0.0f;
    const float mx = lpNow[0] - s_lastPos[0], mz = lpNow[2] - s_lastPos[2];
    const bool gap = now - g_lastFrameTick > 500;
    const bool gameCam = (int)(GameCameraTick() - g_lastFrameTick) > 0, moved = mx * mx + mz * mz > 3000.0f * 3000.0f;
    // A scripted moment in the game's action camera can turn Leon round (the view would then face him).
    const bool turned = fabsf(WrapPi(At<float>(pl, pl::Ang + 4) - s_lastLeonYaw)) > 1.57f;
    if (g_haveYaw && gap && !gameCam && !moved && !turned)
        Log("diag cam kept: %u ms without a gameplay frame, no game camera, yaw %.3f (game %.3f)", now - g_lastFrameTick,
            (float)g_yaw, gameYaw);
    if (!g_haveYaw || (gap && (gameCam || moved || turned))) {
        if (g_haveYaw) {
            const float* lp = lpNow;
            Log("diag cam reset: %u ms without a gameplay frame (%s), yaw %.3f -> game %.3f, game eye %.0f from leon, fov %.1f",
                now - g_lastFrameTick, gameCam ? "game camera" : moved ? "new area" : "leon turned", (float)g_yaw, gameYaw,
                sqrtf((eye[0] - lp[0]) * (eye[0] - lp[0]) + (eye[2] - lp[2]) * (eye[2] - lp[2])), eye[7]);
        }
        g_yaw = gameYaw;
        g_pitch = cfg.defaultPitch;
        g_haveYaw = true;
        g_wasAiming = false;
        g_room = -1.0f;
        g_back = -1.0f;
    }
    g_lastFrameTick = now;
    for (int i = 0; i < 3; ++i) s_lastPos[i] = lpNow[i];
    s_lastLeonYaw = At<float>(pl, pl::Ang + 4);
    // Back from one of the game's cameras (mostly its action camera: a jump down, a hop, a hit): its first and last
    // frames leave a point near the world's origin in the game's camera smoothing, which then sweeps in from there
    // (thousands of units away, a frame at fov 100 at the end). Show this camera as is instead.
    if (PrevCamMode() != 10 && At<uint8_t>(Va(kCamCtrl), cc::Mode) == 10) {
        At<uint32_t>(Va(kCamSmooth), kCamSmoothCut) |= 1;
        Log("diag cam: back from game camera mode %u, smoothing cut", PrevCamMode());
    }
    const float dt = DeltaSec();

    if (Aiming() && !QuickReload()) {  // a reload with X alone keeps the free camera
        const bool wasAimingBefore = g_wasAiming;
        if (!g_wasAiming) {
            g_aimFreeYaw = g_yaw, g_aimFreePitch = g_pitch, g_aimHandBack = false;
            // Turn Leon so the aim starts where the free camera looked.
            float* ang = (float*)(pl + pl::Ang);
            ang[1] = WrapPi(ang[1] + WrapPi(g_yaw - gameYaw));
            Log("cam: aim start, leon yaw -> %.3f", ang[1]);
        }
        g_wasAiming = true;
        // Fade the free camera's tilt out over 0.25 s instead of dropping it in one frame.
        g_modBlend = g_modBlend > dt / 0.25f ? g_modBlend - dt / 0.25f : 0.0f;
        if (g_modBlend > 0.0f && !cfg.centeredAim) {  // the centered aim keeps the view's tilt itself
            const float pitch = g_pitch * g_modBlend;
            const float* lpos = (const float*)(pl + pl::Pos);
            const float pivot[3] = {lpos[0], tgt[1], lpos[2]};
            RotatePitch(eye, pivot, sinf(gameYaw), cosf(gameYaw), cosf(pitch), sinf(pitch));
            RotatePitch(tgt, pivot, sinf(gameYaw), cosf(gameYaw), cosf(pitch), sinf(pitch));
        }
        g_yaw = gameYaw;
        g_view = VIEW_AIM;
        if (cfg.centeredAim) {
            CenterAimCamera(eye, tgt, dt, !wasAimingBefore);
            // The field of view zooms with the camera (the game's aim narrows it, 50 -> 45 deg); a quick aim keeps it.
            if (g_freeFov > 1.0f && eye[7] > 1.0f) eye[7] = g_freeFov + (eye[7] - g_freeFov) * g_zoomS;
            SetShotView(eye, tgt);
            // Once the aim has settled, show this camera as is: the game's smoothing would let the view trail Leon's
            // turn by about 0.15 s.
            if (g_aimLocked) At<uint32_t>(Va(kCamSmooth), kCamSmoothCut) |= 1;
        }
        const float ax = g_aimMoveX, ay = g_aimMoveY;
        // Only while Leon is actually in his aim or knife stance (r_no_0 0, r_no_1 6 / 0xB): a hit or stagger with the
        // aim button still held must not slide him around.
        const uint8_t r0 = At<uint8_t>(pl, pl::RNo0), r1 = At<uint8_t>(pl, pl::RNo0 + 1);
        const bool stance = r0 == 0 && (r1 == 6 || r1 == 0xB);
        if (cfg.aimWalk && stance && (ax != 0.0f || ay != 0.0f)) {
            // Relative to the aim camera: stick up walks where the camera looks.
            float mag = sqrtf(ax * ax + ay * ay);
            if (mag > 1.0f) mag = 1.0f;
            const float yaw = gameYaw - atan2f(ax, ay);
            float* pos = (float*)(pl + pl::Pos);
            SetStep(yaw, At<float>(pl, pl::Ang + 4), mag, false);
            const float stepLen = cfg.aimWalkSpeed * mag * dt;
            WalkWithCollision(pos, sinf(yaw) * stepLen, cosf(yaw) * stepLen);
        }
        // Turning while the game ignores the aim stick (reloading): while aiming, the game turns Leon itself, through
        // his yaw and the aim's sideways offset (cPlayer+0x448). If neither moved at all although the stick is pushed
        // (the game turns him in small steps otherwise), turn him here, like the remakes allow while reloading.
        {
            static float lastYaw = 0.0f, lastSide = 0.0f;
            static bool haveLast = false;
            float* yaw = (float*)(pl + pl::Ang) + 1;
            float* side = (float*)(pl + 0x448);
            const float want = g_stickRx * 2.0f * dt;  // rad this frame at about 115 degrees/s at full deflection
            if (stance && haveLast && fabsf(want) > 1e-4f &&
                fabsf(WrapPi(*yaw - lastYaw)) + fabsf(*side - lastSide) < 1e-5f)
                *yaw = WrapPi(*yaw - want);
            lastYaw = *yaw;
            lastSide = *side;
            haveLast = stance;
        }
        if (cfg.aimWalk) KeepAshleyOnGround();
        return;
    }
    // Coming out of an aim, the camera comes back at its resting tilt (the aim was level), not at the steep look it
    // had before the aim; the tilt itself fades in over 0.3 s below.
    if (eye[7] > 20.0f && eye[7] < 90.0f) g_freeFov = eye[7];  // not the fov 5-10 sweep back from an action camera
    // The centered aim instead hands its view back: the free camera continues looking exactly where the aim looked
    // (the free camera looks along g_yaw itself).
    if (g_wasAiming && g_aimHandBack) {
        g_yaw = WrapPi(g_aimViewYaw);
        g_pitch = Clamp(g_aimFreePitch - (g_aimViewPitch - g_aimPitch0), -0.6f, 1.0f);
        g_modBlend = 1.0f;
        Log("cam: aim end, free camera yaw %.3f pitch %.1f", g_yaw, g_pitch * 57.3f);
    } else if (g_wasAiming) {
        g_pitch = cfg.defaultPitch;
    }
    if (g_wasAiming) g_back = -2.0f;  // glide out from the aim's distance
    g_wasAiming = false;

    float* ang = (float*)(pl + pl::Ang);
    // Reloading without aiming (like the remakes): Leon keeps moving with the left stick, relative to the free camera,
    // and turns the way he goes. The game ignores the stick in its aim routine, so he glides (r_no_0 0, r_no_1 6).
    if (s_hold && cfg.aimWalk && At<uint8_t>(pl, pl::RNo0) == 0 && At<uint8_t>(pl, pl::RNo0 + 1) == 6 &&
        (g_aimMoveX != 0.0f || g_aimMoveY != 0.0f)) {
        float mag = sqrtf(g_aimMoveX * g_aimMoveX + g_aimMoveY * g_aimMoveY);
        if (mag > 1.0f) mag = 1.0f;
        const float yaw = WrapPi(g_yaw - atan2f(g_aimMoveX, g_aimMoveY));
        const float diff = WrapPi(yaw - ang[1]), step = cfg.turnSpeed * dt;
        ang[1] = WrapPi(ang[1] + (fabsf(diff) <= step ? diff : copysignf(step, diff)));
        SetStep(yaw, ang[1], mag, true);
        const float stepLen = cfg.reloadSpeed * mag * dt;
        WalkWithCollision((float*)(pl + pl::Pos), sinf(yaw) * stepLen, cosf(yaw) * stepLen);
    }
    if (g_moving) {
        const float diff = WrapPi(g_moveYaw - ang[1]);
        const float step = cfg.turnSpeed * dt;
        ang[1] = WrapPi(ang[1] + (fabsf(diff) <= step ? diff : copysignf(step, diff)));
        if (!g_wasMoving) Log("move: start, target yaw %.3f leon %.3f cam %.3f", g_moveYaw, ang[1], g_yaw);
    } else if (g_wasMoving) {
        Log("move: stop");
    }
    g_wasMoving = g_moving;

    const float stickX = g_stickRx, stickY = g_stickRy;
    g_yaw = WrapPi(g_yaw - stickX * cfg.yawSpeed * dt);
    // Auto-follow: while Leon moves and the right stick rests, ease the camera round to where the original camera
    // would be. Slower for small differences, so it settles
    // instead of snapping; any right-stick input hands the camera straight back to the player.
    g_stickIdle = (stickX != 0.0f || stickY != 0.0f) ? 0.0f : g_stickIdle + dt;
    if (cfg.autoFollow > 0.0f && g_moving && g_stickIdle >= cfg.autoFollowDelay) {
        // The game's own camera (gameYaw) already sits behind Leon with its natural lag: follow that.
        const float diff = WrapPi(gameYaw - g_yaw);
        const float ease = fabsf(diff) < 0.6f ? fabsf(diff) / 0.6f : 1.0f;
        const float step = cfg.autoFollow * ease * dt;
        g_yaw = WrapPi(g_yaw + (fabsf(diff) <= step ? diff : copysignf(step, diff)));
    }
    // Auto-center: on the same condition the view tilts back to its resting pitch, slowly and at a steady rate (eased
    // only over the last few degrees so it lands without a jolt).
    if (cfg.autoCenter > 0.0f && g_moving && g_stickIdle >= cfg.autoFollowDelay) {
        const float diff = cfg.defaultPitch - g_pitch;
        const float ease = fabsf(diff) < 0.05f ? 0.3f + 0.7f * fabsf(diff) / 0.05f : 1.0f;
        const float step = cfg.autoCenter * ease * dt;
        g_pitch += fabsf(diff) <= step ? diff : copysignf(step, diff);
    }
    // Stick up looks up: the camera comes down.
    g_pitch = Clamp(g_pitch - stickY * cfg.pitchSpeed * dt, -0.6f, 1.0f);

    const float* pos = (const float*)(pl + pl::Pos);
    const float gameEye[3] = {eye[0], eye[1], eye[2]}, gameTgt[3] = {tgt[0], tgt[1], tgt[2]};
    // The original camera's height above Leon's feet (about 1080, roughly his shoulders).
    const float pivotHeight = Clamp(gameEye[1] - pos[1], 400.0f, 1800.0f);
    // Builds the rig from the game's camera. `raise` scales the extra height and the default tilt: 1 outdoors, less
    // when walls pull the camera in (corridors), so tight spaces get a camera closer to the original.
    // After an aim (or a reset) the tilt and height come back over 0.3 s instead of in one frame.
    g_modBlend = g_modBlend + dt / 0.3f < 1.0f ? g_modBlend + dt / 0.3f : 1.0f;
    float shoulder[3];
    auto build = [&](float raise) {
        for (int i = 0; i < 3; ++i) eye[i] = gameEye[i], tgt[i] = gameTgt[i];
        eye[1] += cfg.height * raise * g_modBlend;
        tgt[1] += cfg.height * raise * g_modBlend;
        const float d = WrapPi(g_yaw - gameYaw);
        const float cy = cosf(d), sy = sinf(d);
        RotateYaw(eye, pos, cy, sy);
        RotateYaw(tgt, pos, cy, sy);
        const float pitch = (g_pitch - cfg.defaultPitch * (1.0f - raise)) * g_modBlend;
        const float pivot[3] = {pos[0], tgt[1], pos[2]};
        RotatePitch(eye, pivot, sinf(g_yaw), cosf(g_yaw), cosf(pitch), sinf(pitch));
        RotatePitch(tgt, pivot, sinf(g_yaw), cosf(g_yaw), cosf(pitch), sinf(pitch));
        // Over the right shoulder (RE5-like): eye and target move sideways together, the direction stays.
        {
            const float rx = -cosf(g_yaw), rz = sinf(g_yaw);
            eye[0] += rx * cfg.shoulder, eye[2] += rz * cfg.shoulder;
            tgt[0] += rx * cfg.shoulder, tgt[2] += rz * cfg.shoulder;
            // Closer (Distance = share of the original distance to Leon): forward along the view. The game's own
            // distance changes per area (some bring it in to about 1400), so MinDistance keeps the camera from ending
            // up on his back there; it may go further back than the game's camera, the wall check below covers that.
            float f[3] = {tgt[0] - eye[0], tgt[1] - eye[1], tgt[2] - eye[2]};
            const float fl = sqrtf(f[0] * f[0] + f[1] * f[1] + f[2] * f[2]);
            if (fl > 1.0f) {
                for (int i = 0; i < 3; ++i) f[i] /= fl;
                const float toLeon = (pos[0] - eye[0]) * f[0] + (pos[2] - eye[2]) * f[2];
                const float want = toLeon * cfg.distance > cfg.minDistance ? toLeon * cfg.distance : cfg.minDistance;
                if (toLeon > 0.0f) {
                    // Eased: on stairs and slopes the game's own camera jumps back and forth by several hundred
                    // units from one frame to the next, and coming out of an aim the view starts at the aim's
                    // distance.
                    if (g_back == -2.0f) {
                        const float* o = g_outPoint;
                        float of[2] = {o[3] - o[0], o[5] - o[2]};
                        const float ol = sqrtf(of[0] * of[0] + of[1] * of[1]);
                        g_back = ol > 1.0f ? ((pos[0] - o[0]) * of[0] + (pos[2] - o[2]) * of[1]) / ol : want;
                        if (g_back < 100.0f) g_back = want;
                    } else if (g_back < 0.0f) {
                        g_back = want;
                    } else {
                        g_back += (want - g_back) * (1.0f - expf(-dt / 0.5f));
                    }
                    for (int i = 0; i < 3; ++i) eye[i] += f[i] * (toLeon - g_back);
                }
            }
        }
        // Looking up must not put the camera into the floor.
        const float minEye = pos[1] + 250.0f;
        if (eye[1] < minEye) {
            tgt[1] += minEye - eye[1];
            eye[1] = minEye;
        }
        // Sideways offset of the eye from Leon in the camera's frame (right = (-cos yaw, 0, sin yaw)).
        const float rx = -cosf(g_yaw), rz = sinf(g_yaw);
        const float side = (eye[0] - pos[0]) * rx + (eye[2] - pos[2]) * rz;
        shoulder[0] = pos[0] + rx * side;
        shoulder[1] = pos[1] + pivotHeight;
        shoulder[2] = pos[2] + rz * side;
    };
    build(g_room < 0.0f ? 1.0f : Clamp(g_room / 0.9f, 0.0f, 1.0f));
    // Walls: ease the camera in and out instead of jumping to the wall. In fast enough to barely show the wall's
    // far side, out slowly so walking past a pillar or out of a corridor doesn't throw the view around.
    const float want = FreeRoom(shoulder, eye);
    if (g_room < 0.0f) {
        g_room = want;
    } else {
        const float tau = want < g_room ? 0.1f : 0.6f;
        g_room += (want - g_room) * (1.0f - expf(-dt / tau));
        if (g_room > want + 0.3f) g_room = want + 0.3f;  // never far behind a wall
    }
    if (g_room < 1.0f)
        for (int i = 0; i < 3; ++i) eye[i] = shoulder[i] + (eye[i] - shoulder[i]) * g_room;
    DiagRoom("free", g_room, want);
    g_view = VIEW_FREE;
}

bool LaserAimPoint(float* out, float* yaw, float* pivotXZ);
void SetShotView(const float* eye, const float* tgt);
void InitShotHook();
void SetAimCentered();
float TakeAimPitchMove();
bool TakeAimReset();
void SetAimPitch(float rad);

// Centered aim, like RE5: the aim camera sits over Leon's right shoulder at a fixed spot relative to the view
// (AimShoulderRight/Back/Height), circling the shoulder as the view turns, so Leon always keeps his place on screen.
// Raising the gun keeps the view's direction (KeepViewOnAim=1; 0 = swing to where Leon looks, level) and the camera
// glides there from the free camera over kSwing. The view follows Leon's aim (heading, the stick's sideways and
// vertical aim), never the gun itself: recoil and breathing don't move it. The laser and the shots go to the middle of
// it (shot.cpp). The game's own aim camera is not used.
bool g_aimLocked = false;
static const float kSwing = 0.3f;
static float g_aimTime = 0.0f;
static float g_relYaw = 0.0f, g_relPitch = 0.0f;  // view direction: yaw relative to Leon's, pitch
static float g_lastSide = 0.0f;                     // the aim's sideways offset, last frame
static float g_freeEye[3] = {}, g_freeDir[3] = {};  // the free camera when the aim started
static float g_aimRoom = -1.0f;                     // share of the shoulder distance free of walls
static float g_freeOff[3] = {};                     // the free camera's eye in the shoulder's frame: right, up, back
static float g_zoom = 0.0f;                         // 0 = the free camera's distance, 1 = the aim camera's
static float g_aimP[3] = {};                        // the world point under the middle of the screen when the aim started
static bool g_aimLock = false;                      // the aim keeps looking at g_aimP while the camera glides to the shoulder
bool QuickAim();  // buttons.cpp: a reload or shot without the aim button; the camera stays where it is

static void CenterAimCamera(float* eye, float* tgt, float dt, bool start) {
    const uintptr_t pl = Player();
    if (!pl) return;
    const Config& cfg = Cfg();
    float leonYaw = At<float>(pl, pl::Ang + 4);
    bool stickMoved = false;  // the player moves the aim: the point lock hands over
    const float side = At<float>(pl, 0x448);
    const float* pos = (const float*)(pl + pl::Pos);
    if (start || !g_aimLocked) {
        // The view on screen (Camera_60's finished eye and target, last frame's: the free camera, after the game's
        // smoothing) is where the glide starts. Its view matrix gave an eye as far back as the game's own camera
        // (Distance and the shoulder left out) and a heading up to 20 deg off: the view jumped there for a frame.
        const float* sp = (const float*)(Va(kCamCtrl) + cc::Camera + cc::CameraPoint);
        float shown[3] = {sp[3] - sp[0], sp[4] - sp[1], sp[5] - sp[2]};
        const float sl = sqrtf(shown[0] * shown[0] + shown[1] * shown[1] + shown[2] * shown[2]);
        if (sl < 0.5f) return;
        for (int i = 0; i < 3; ++i) g_freeDir[i] = shown[i] / sl, g_freeEye[i] = sp[i];
        {
            const float* m = (const float*)(Va(kCamCtrl) + cc::Camera);
            auto hd = [&](float x, float z) { return sqrtf((x - pos[0]) * (x - pos[0]) + (z - pos[2]) * (z - pos[2])); };
            Log("diag aim start eye: shown %.0f from leon, matrix %.0f (%+.1f deg), ours %.0f", hd(sp[0], sp[2]),
                hd(m[3], m[11]), WrapPi(atan2f(-m[2], -m[10]) - atan2f(shown[0], shown[2])) * 57.3f,
                hd(g_outPoint[0], g_outPoint[2]));
        }
        // An aim started during the sweep back from an action camera (a kick) would keep that far-away eye as the spot
        // a hip-fire shot pulls back to: the eye of this frame instead.
        const float fx = g_freeEye[0] - pos[0], fz = g_freeEye[2] - pos[2];
        if (!(fx * fx + fz * fz < 6000.0f * 6000.0f && fabsf(g_freeEye[1] - pos[1]) < 6000.0f)) {
            Log("diag cam: aim start with the shown eye %.0f from leon, this frame's used", sqrtf(fx * fx + fz * fz));
            for (int i = 0; i < 3; ++i) g_freeEye[i] = eye[i];
        }
        if (cfg.keepViewOnAim) {
            // Leon turns to face exactly where the view looks (the hook turned him to the free camera's heading, which
            // the shoulder offset leaves 10-20 deg off), so his gun points at the reticle.
            float* ang = (float*)(pl + pl::Ang);
            const float to = atan2f(g_freeDir[0], g_freeDir[2]);
            Log("cam: aim start, leon turned %+.1f deg to the view", WrapPi(to - leonYaw) * 57.3f);
            ang[1] = leonYaw = to;
            g_relYaw = 0.0f;
            g_relPitch = Clamp(atan2f(g_freeDir[1], sqrtf(g_freeDir[0] * g_freeDir[0] + g_freeDir[2] * g_freeDir[2])),
                               -1.0f, 1.0f);
            g_aimPitch0 = g_relPitch, g_aimHandBack = true;
        } else {
            g_relYaw = 0.0f, g_relPitch = 0.0f;
        }
        g_aimTime = 0.0f, g_aimRoom = -1.0f, g_zoom = 0.0f;
        // The free camera's eye in the frame the aim camera turns with (right, up, back along the view), so a quick
        // aim keeps it there and the zoom starts from it.
        {
            const float yaw0 = leonYaw + g_relYaw, p0 = g_relPitch;
            const float d0[3] = {cosf(p0) * sinf(yaw0), sinf(p0), cosf(p0) * cosf(yaw0)};
            const float rx0 = -cosf(yaw0), rz0 = sinf(yaw0);
            const float sh[3] = {pos[0] + rx0 * cfg.aimShoulderRight, pos[1] + cfg.aimShoulderHeight,
                                 pos[2] + rz0 * cfg.aimShoulderRight};
            const float v[3] = {g_freeEye[0] - sh[0], g_freeEye[1] - sh[1], g_freeEye[2] - sh[2]};
            const float hh = d0[0] * d0[0] + d0[2] * d0[2];
            const float back = hh > 1e-4f ? -(v[0] * d0[0] + v[2] * d0[2]) / hh : 0.0f;
            g_freeOff[0] = v[0] * rx0 + v[2] * rz0, g_freeOff[1] = v[1] + back * d0[1], g_freeOff[2] = back;
        }
        g_aimLocked = true;
        g_aimLock = false;
        if (cfg.keepViewOnAim) {
            // The point the reticle marks at rest: where the free camera's line of sight meets the world.
            const float farEnd[3] = {g_freeEye[0] + g_freeDir[0] * 20000.0f, g_freeEye[1] + g_freeDir[1] * 20000.0f,
                                  g_freeEye[2] + g_freeDir[2] * 20000.0f};
            if (!Trace(g_freeEye, farEnd, g_aimP, kTraceCamA, kTraceCamB))
                for (int i = 0; i < 3; ++i) g_aimP[i] = g_freeEye[i] + g_freeDir[i] * 8000.0f;
            // Not on the boat: it moves away from the point, so the lock kept turning the view (by hundreds of degrees).
            g_aimLock = At<uint8_t>(pl, pl::RNo0 + 1) != 0x0F;
        }
        SetAimPitch(g_relPitch);
        Log("cam: aim start, view %+.1f from leon, pitch %.1f", g_relYaw * 57.3f, g_relPitch * 57.3f);
    } else {
        // The aim moved: sideways the aim's offset from Leon's heading (cPlayer+0x448, the gun turns about 0.85 rad per
        // unit), up and down the game's own changes to the aim (the sway anchor, radians). Leon's heading itself is in
        // the view's yaw already.
        // The game resets both when the aim starts: a jump no stick makes in one frame is ignored (it turned the view
        // by up to 140 deg).
        const float ds = side - g_lastSide, dp = TakeAimPitchMove();
        if (fabsf(ds) < 0.1f) g_relYaw = WrapPi(g_relYaw + ds * 0.85f);
        if (fabsf(dp) < 0.1f) g_relPitch = Clamp(g_relPitch + dp, -1.2f, 1.2f);
        // The game reset its vertical aim (a weapon switched with the d-pad while aiming brings the new gun up level):
        // the gun goes back to the view's pitch, else it stays off the reticle until the next aim.
        if (fabsf(dp) >= 0.1f || TakeAimReset()) {
            SetAimPitch(g_relPitch);
            Log("cam: the game reset the vertical aim (jump %.1f deg), gun back to the view's pitch %.1f", dp * 57.3f,
                g_relPitch * 57.3f);
        }
        stickMoved = (fabsf(ds) > 1e-4f && fabsf(ds) < 0.1f) || (fabsf(dp) > 1e-4f && fabsf(dp) < 0.1f);
    }
    g_lastSide = side;
    g_aimTime += dt;
    const float t = g_aimTime / kSwing < 1.0f ? g_aimTime / kSwing : 1.0f, b = t * t * (3.0f - 2.0f * t);

    float yaw = leonYaw + g_relYaw, pitch = g_relPitch;
    float d[3] = {cosf(pitch) * sinf(yaw), sinf(pitch), cosf(pitch) * cosf(yaw)};
    if (!cfg.keepViewOnAim && b < 1.0f) {  // from the free camera's direction to Leon's look
        float l = 0.0f;
        for (int i = 0; i < 3; ++i) d[i] = g_freeDir[i] + (d[i] - g_freeDir[i]) * b, l += d[i] * d[i];
        l = sqrtf(l);
        for (int i = 0; i < 3; ++i) d[i] /= l;
        yaw = atan2f(d[0], d[2]), pitch = atan2f(d[1], sqrtf(d[0] * d[0] + d[2] * d[2]));
    }
    // Zoom from the free camera's spot to the aim camera's over kSwing; a quick aim (reload, shot without the aim
    // button) stays at the free camera's spot, and zooms in once the aim button takes over.
    const float zt = QuickAim() ? 0.0f : 1.0f;
    g_zoom = g_zoom < zt ? (g_zoom + dt / kSwing < zt ? g_zoom + dt / kSwing : zt)
                         : (g_zoom - dt / kSwing > zt ? g_zoom - dt / kSwing : zt);
    const float z = g_zoomS = g_zoom * g_zoom * (3.0f - 2.0f * g_zoom);
    // The shoulder spot: right of Leon's head in the view's frame (right = (-cos yaw, 0, sin yaw)); the camera sits
    // behind it along the view, so looking up or down circles the shoulder. `place` puts the eye for a view direction.
    float shoulder[3], aimEye[3];
    auto place = [&](float yw, float pt) {
        const float dd[3] = {cosf(pt) * sinf(yw), sinf(pt), cosf(pt) * cosf(yw)};
        const float rx = -cosf(yw), rz = sinf(yw);
        shoulder[0] = pos[0] + rx * cfg.aimShoulderRight, shoulder[1] = pos[1] + cfg.aimShoulderHeight,
        shoulder[2] = pos[2] + rz * cfg.aimShoulderRight;
        for (int i = 0; i < 3; ++i) {
            const float up = i == 1 ? 1.0f : 0.0f, r = i == 0 ? rx : i == 2 ? rz : 0.0f;
            const float closeIn = shoulder[i] - dd[i] * cfg.aimShoulderBack;
            const float stayOut = shoulder[i] + r * g_freeOff[0] + up * g_freeOff[1] - dd[i] * g_freeOff[2];
            aimEye[i] = stayOut + (closeIn - stayOut) * z;
        }
    };
    // The glide to the shoulder moves the eye (higher, to the side), and the same direction from there would look at
    // another spot. While it lasts the view turns to keep looking at the point the reticle marked at rest, and Leon's
    // gun with it, so rest view and aim agree. The stick takes over as soon as it moves.
    static float s_lockYaw = 0.0f, s_lockPitch = 0.0f;
    if (start) s_lockYaw = s_lockPitch = 0.0f;
    if (g_aimLock && cfg.keepViewOnAim && !QuickAim()) {
        if (stickMoved) {
            g_aimLock = false;
        } else {
            float ny = yaw, np = pitch;
            for (int it = 0; it < 3; ++it) {
                place(ny, np);
                const float v[3] = {g_aimP[0] - aimEye[0], g_aimP[1] - aimEye[1], g_aimP[2] - aimEye[2]};
                const float hl = sqrtf(v[0] * v[0] + v[2] * v[2]);
                if (hl < 50.0f) break;  // the point is right at the eye: leave the view alone
                ny = atan2f(v[0], v[2]), np = Clamp(atan2f(v[1], hl), -1.2f, 1.2f);
            }
            const float dy = WrapPi(ny - yaw), dpitch = np - pitch;
            float* ang = (float*)(pl + pl::Ang);
            ang[1] = leonYaw = ny, g_relYaw = 0.0f;
            g_relPitch = np;
            SetAimPitch(np);
            s_lockYaw += dy, s_lockPitch += dpitch;
            yaw = ny, pitch = np;
            d[0] = cosf(pitch) * sinf(yaw), d[1] = sinf(pitch), d[2] = cosf(pitch) * cosf(yaw);
            // A point close by (the floor right in front of Leon) would turn the view by up to 90 deg: stop at 20.
            if (g_zoom >= 1.0f || fabsf(s_lockYaw) > 0.35f || fabsf(s_lockPitch) > 0.35f) {
                g_aimLock = false;
                Log("cam: aim point lock done, view turned %+.1f deg sideways, %+.1f deg up/down", s_lockYaw * 57.3f, s_lockPitch * 57.3f);
            }
        }
    }
    place(yaw, pitch);
    if (aimEye[1] < pos[1] + 250.0f) aimEye[1] = pos[1] + 250.0f;  // not into the floor
    // Walls: pull the camera toward the shoulder, in quickly, out slowly.
    const float want = FreeRoom(shoulder, aimEye);
    if (g_aimRoom < 0.0f) g_aimRoom = want;
    else g_aimRoom += (want - g_aimRoom) * (1.0f - expf(-dt / (want < g_aimRoom ? 0.05f : 0.4f)));
    if (g_aimRoom > want) g_aimRoom = want;
    for (int i = 0; i < 3; ++i) aimEye[i] = shoulder[i] + (aimEye[i] - shoulder[i]) * g_aimRoom;
    DiagRoom("aim", g_aimRoom, want);
    for (int i = 0; i < 3; ++i) eye[i] = aimEye[i], tgt[i] = eye[i] + d[i] * 1000.0f;
    g_aimViewYaw = yaw, g_aimViewPitch = pitch;
    SetAimCentered();
}

// Fade: Leon and Ashley turn see-through as the camera closes in (the remakes' behaviour). The game's model draw
// multiplies the alpha by cModel +0x154 * +0x158; +0x154 is the game's own fade, +0x158 is 1.0 unless set.
static float g_fadeEye[3];
static float g_fade[2] = {1.0f, 1.0f};  // current factor: Leon, Ashley
static int g_level[2] = {2, 2};  // 0 most see-through .. 2 visible
static DWORD g_fadeTick = 0;

static DWORD g_outTick = 0;

static void __fastcall HookQfpsMove(uintptr_t self, void* edx) {
    HookQfpsMoveBody(self, edx);
    if (g_view != VIEW_NONE) {
        memcpy(g_fadeEye, (float*)(self + cc::CameraPoint), sizeof(g_fadeEye));
        memcpy(g_outPoint, (float*)(self + cc::CameraPoint), sizeof(g_outPoint));
        g_outTick = GetTickCount();
    }
}

// The shown view, last stage. Back from the game's action camera (a jump down, a hop, a hit) the game blends from a
// point near the world's origin (about Leon's position / 10, fov 2-8) to our camera over about 0.6 s and ends on one
// frame at fov 100 tens of thousands of units away; the smoothing cut and the camera input are fine then, so the blend
// sits after both. When the view about to be shown is that far from our camera this frame, show ours.
typedef void(__cdecl* PFN_LookAt)(float* cam);
static PFN_LookAt g_origLookAt = nullptr;

static void __cdecl HookLookAt(float* cam) {
    if ((uintptr_t)cam == Va(kCamCtrl) + cc::Camera && g_view != VIEW_NONE && GetTickCount() - g_outTick < 100 &&
        At<uint8_t>(Va(kCamCtrl), cc::Mode) == 10) {
        float* pt = (float*)((uintptr_t)cam + (cc::CameraPoint));  // eye, target, roll, fovy
        const float dx = pt[0] - g_outPoint[0], dy = pt[1] - g_outPoint[1], dz = pt[2] - g_outPoint[2];
        const float d = sqrtf(dx * dx + dy * dy + dz * dz);
        if (d > 2500.0f || fabsf(pt[7] - g_outPoint[7]) > 15.0f || !(d == d)) {
            static DWORD s_log = 0;
            if (GetTickCount() - s_log > 1000)
                s_log = GetTickCount(), Log("diag cam: shown view %.0f from ours, fov %.1f (ours %.1f): ours shown", d, pt[7],
                                            g_outPoint[7]);
            memcpy(pt, g_outPoint, sizeof(g_outPoint));
        }
    }
    g_origLookAt(cam);
}

// The same blend, one stage earlier: CameraControl::move() blends m_camera_38 (normally a copy of our QuasiFPS
// output) from the action camera's point. On 2026-10-27 every kick after a headshot swept in from ~30 000 away at fov
// 6.7 there and ended on a fov-100 frame, and the look-at check above never fired. Called right after move(): when
// m_camera_38 (and the shown CamPoint) are that far from our camera this frame, ours goes in.
void KeepShownCamera(uintptr_t ccObj) {
    if (g_view == VIEW_NONE || GetTickCount() - g_outTick >= 100 || At<uint8_t>(ccObj, cc::Mode) != 10) return;
    float* pts[2] = {(float*)(ccObj + cc::Point), (float*)(ccObj + cc::Camera + cc::CameraPoint)};
    for (int i = 0; i < 2; ++i) {
        float* pt = pts[i];
        const float dx = pt[0] - g_outPoint[0], dy = pt[1] - g_outPoint[1], dz = pt[2] - g_outPoint[2];
        const float d = sqrtf(dx * dx + dy * dy + dz * dz);
        if (d > 2500.0f || fabsf(pt[7] - g_outPoint[7]) > 15.0f || !(d == d)) {
            static DWORD s_log = 0;
            if (GetTickCount() - s_log > 1000)
                s_log = GetTickCount(), Log("diag cam: %s %.0f from ours, fov %.1f (ours %.1f): ours put in",
                                            i ? "shown point" : "camera_38", d, pt[7], g_outPoint[7]);
            memcpy(pt, g_outPoint, sizeof(g_outPoint));
        }
    }
}

static float BodyDistance(uintptr_t ch) {
    const float* p = (const float*)(ch + pl::Pos);
    // Closest point of the body (a vertical line from the feet to the head) to the eye.
    const float y = Clamp(g_fadeEye[1] - p[1], 0.0f, 1700.0f);
    const float dx = g_fadeEye[0] - p[0], dy = g_fadeEye[1] - p[1] - y, dz = g_fadeEye[2] - p[2];
    return sqrtf(dx * dx + dy * dy + dz * dz);
}

// cModel draw (0x006EB450, cdecl(cModel*)). The factor is only in place for the draw of Leon and Ashley: several enemy
// update routines copy +0x154/+0x158 from the model they are linked to, so a value left in the model would fade those
// enemies too.
typedef void(__cdecl* PFN_ModelDraw)(uintptr_t model);
static PFN_ModelDraw g_origModelDraw = nullptr;

static void __cdecl HookModelDraw(uintptr_t model) {
    int who = -1;
    if (g_fade[0] != 1.0f || g_fade[1] != 1.0f) {
        if (model == Player()) who = 0;
        else if (model == Character(kAshleyPtr)) who = 1;
    }
    if (who < 0) return g_origModelDraw(model);
    float& f = At<float>(model, 0x158);
    const float saved = f;
    f = saved * g_fade[who];
    g_origModelDraw(model);
    f = saved;
}

static void FadeTick() {
    const DWORD now = GetTickCount();
    const float dt = Clamp((now - g_fadeTick) / 1000.0f, 0.0f, 0.1f);
    g_fadeTick = now;
    const bool on = Cfg().fade && GameplayView();
    uintptr_t ch[2] = {Player(), Character(kAshleyPtr)};
    float dist[2] = {-1.0f, -1.0f};
    for (int i = 0; i < 2; ++i) {
        if (!ch[i]) continue;
        // Steps instead of a continuous ramp: a half see-through model shows its own overlapping parts, so it rests on
        // a few fixed levels. Going up needs a margin past the edge so it doesn't flicker. Never fully hidden.
        const Config& c = Cfg();
        const float span = c.fadeFar - c.fadeNear, d = dist[i] = BodyDistance(ch[i]);
        const auto level = [&](float dist) {
            const float t = (dist - c.fadeNear) / span;
            return t < 0.5f ? 0 : t < 1.0f ? 1 : 2;
        };
        if (!on) g_level[i] = 2;
        else {
            const int down = level(d), up = level(d - 0.08f * span);
            if (down < g_level[i]) g_level[i] = down;
            else if (up > g_level[i]) g_level[i] = up;
        }
        const float lv[3] = {c.fadeMin, 0.5f * (1.0f + c.fadeMin), 1.0f};
        const float want = lv[g_level[i]];
        g_fade[i] += (want - g_fade[i]) * (1.0f - expf(-dt / 0.12f));  // ~0.35 s from one step to the next
        if (fabsf(want - g_fade[i]) < 0.01f) g_fade[i] = want;
    }
    const bool dirty = g_fade[0] != 1.0f || g_fade[1] != 1.0f;
    static DWORD s_log = 0;
    if (dirty && now - s_log > 500)
        s_log = now, Log("fade leon %.2f ashley %.2f | diag fade eye to leon %.0f ashley %.0f, view %d", g_fade[0], g_fade[1],
                         dist[0], dist[1], (int)g_view);
}

// Ashley: drop a hit from Leon's guns or knife before the game applies it. The attack types are exactly the ones her
// armour costume shrugs off (the game's own list, in the partner hit routine).
typedef void(__fastcall* PFN_PartnerHit)(uintptr_t self, void* edx);
static PFN_PartnerHit g_origPartnerHit = nullptr;

static bool PlayerWeaponHit(uint8_t t) {
    switch (t) {
        case 0x00: case 0x01: case 0x02: case 0x03: case 0x04: case 0x05: case 0x06: case 0x07: case 0x08:
        case 0x09: case 0x0A: case 0x0B: case 0x0E: case 0x10: case 0x11: case 0x14: case 0x15: case 0x16:
        case 0x17: case 0x18: case 0x19: case 0x1A: case 0x1B: case 0x1C: case 0x1D: case 0x1E: case 0x1F:
        case 0x20: case 0x21: case 0x2D: case 0x2E: case 0x2F:
            return true;
        default:
            return false;
    }
}

static void __fastcall HookPartnerHit(uintptr_t self, void* edx) {
    if (Cfg().protectAshley && At<uint8_t>(self, 0x32C) != 0 && PlayerWeaponHit(At<uint8_t>(self, 0x32E))) {
        Log("ashley: ignored a hit of type %02X", At<uint8_t>(self, 0x32E));
        *(uint8_t*)(self + 0x32C) = 0;
    }
    g_origPartnerHit(self, edx);
}

// KeyConvert: the game's per-frame pad -> KEY_BTN conversion (writes Key_btn_on). Afterwards the run bit follows
// Config::run while the stick moves Leon.
typedef void(__cdecl* PFN_KeyConvert)();
static PFN_KeyConvert g_origKeyConvert = nullptr;

void TestkitTick();  // testkit.cpp

// The camera's own state, for the test kit's status line.
void CameraStatus(char* out, size_t n) {
    static const char* views[] = {"none", "free", "aim"};
    snprintf(out, n, "view %s%s yaw %.3f pitch %.1f room %.2f aimroom %.2f fov %.1f zoom %.2f | moving %d | "
             "fade leon %.2f ashley %.2f",
             views[g_view >= 0 && g_view <= 2 ? g_view : 0], Fresh() ? "" : " (stale)", (float)g_yaw, g_pitch * 57.3f,
             g_room, g_aimRoom, g_freeFov, g_zoom, g_moving ? 1 : 0, g_fade[0], g_fade[1]);
}

static void __cdecl HookKeyConvert() {
    g_origKeyConvert();
    TestkitTick();
    FadeTick();
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
    Hook(kFnModelDraw, kSigModelDraw, sizeof(kSigModelDraw), (void*)HookModelDraw, (void**)&g_origModelDraw,
         "cModel draw");
    Hook(kFnLookAt, kSigLookAt, sizeof(kSigLookAt), (void*)HookLookAt, (void**)&g_origLookAt, "look-at");
    InitShotHook();
    Hook(kFnPartnerHit, kSigPartnerHit, sizeof(kSigPartnerHit), (void*)HookPartnerHit, (void**)&g_origPartnerHit,
         "partner hit");
    return cam;
}
