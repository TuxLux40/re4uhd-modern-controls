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
static float g_appliedDefaultPitch = 0.0f;  // Config::defaultPitch that g_pitch currently includes
static float g_stickIdle = 0.0f;             // seconds since the right stick last turned the camera
static float g_modBlend = 1.0f;              // how much of the free camera's tilt and height applies (0..1)
static float g_room = -1.0f;                 // smoothed share of the camera distance free of walls (-1 = start over)
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

static uintptr_t Player();

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

typedef void(__fastcall* PFN_QfpsMove)(uintptr_t self, void* edx);
static PFN_QfpsMove g_origQfpsMove = nullptr;

static void __fastcall HookQfpsMove(uintptr_t self, void* edx) {
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
    const float gameYaw = atan2f(tgt[0] - eye[0], tgt[2] - eye[2]);

    // A cutscene, door or menu in between: start again from the game's heading and the resting pitch.
    const DWORD now = GetTickCount();
    if (!g_haveYaw || now - g_lastFrameTick > 500) {
        g_yaw = gameYaw;
        g_pitch = cfg.defaultPitch;
        g_haveYaw = true;
        g_wasAiming = false;
        g_room = -1.0f;
    }
    g_lastFrameTick = now;
    const float dt = DeltaSec();

    if (Aiming()) {
        const bool wasAimingBefore = g_wasAiming;
        if (!g_wasAiming) {
            // Turn Leon so the aim starts where the free camera looked.
            float* ang = (float*)(pl + pl::Ang);
            ang[1] = WrapPi(ang[1] + WrapPi(g_yaw - gameYaw));
            Log("cam: aim start, leon yaw -> %.3f", ang[1]);
        }
        g_wasAiming = true;
        // Fade the free camera's tilt out over 0.25 s instead of dropping it in one frame.
        g_modBlend = g_modBlend > dt / 0.25f ? g_modBlend - dt / 0.25f : 0.0f;
        if (g_modBlend > 0.0f) {
            const float pitch = g_pitch * g_modBlend;
            const float* lpos = (const float*)(pl + pl::Pos);
            const float pivot[3] = {lpos[0], tgt[1], lpos[2]};
            RotatePitch(eye, pivot, sinf(gameYaw), cosf(gameYaw), cosf(pitch), sinf(pitch));
            RotatePitch(tgt, pivot, sinf(gameYaw), cosf(gameYaw), cosf(pitch), sinf(pitch));
        }
        g_yaw = gameYaw;
        g_view = VIEW_AIM;
        if (cfg.centeredAim) CenterAimCamera(eye, tgt, dt, !wasAimingBefore);
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
    if (g_wasAiming) g_pitch = cfg.defaultPitch;
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
    g_view = VIEW_FREE;
}

bool LaserAimPoint(float* out, float* yaw, float* pivotXZ);
void SetAimCentered();

// Centered aim: turns the aim camera (eye kept) toward the laser's aim point, so the crosshair sits in the middle of
// the screen and the laser and bullets go where it shows (the game's shots follow the gun's line, like the laser).
// Blended in over 0.2 s when the aim starts.
static float g_aimDir[3];
static float g_aimFollow = 0.0f;

static void CenterAimCamera(float* eye, float* tgt, float dt, bool start) {
    float p[3], laserYaw, pivotXZ[2];
    if (start) g_aimFollow = 0.0f;
    if (!LaserAimPoint(p, &laserYaw, pivotXZ)) {
        g_aimFollow = 0.0f;
        return;
    }
    // Turn the point with Leon by however far he turned since the laser was computed.
    if (const uintptr_t pl = Player()) {
        const float d = WrapPi(At<float>(pl, pl::Ang + 4) - laserYaw);
        const float pivot[3] = {pivotXZ[0], 0.0f, pivotXZ[1]};
        RotateYaw(p, pivot, cosf(d), sinf(d));
    }
    float cur[3] = {tgt[0] - eye[0], tgt[1] - eye[1], tgt[2] - eye[2]};
    const float dist = sqrtf(cur[0] * cur[0] + cur[1] * cur[1] + cur[2] * cur[2]);
    float want[3] = {p[0] - eye[0], p[1] - eye[1], p[2] - eye[2]};
    const float wl = sqrtf(want[0] * want[0] + want[1] * want[1] + want[2] * want[2]);
    if (dist < 1.0f || wl < 50.0f) return;
    for (int i = 0; i < 3; ++i) cur[i] /= dist, want[i] /= wl;
    if (g_aimFollow == 0.0f) for (int i = 0; i < 3; ++i) g_aimDir[i] = want[i];
    // Locked exactly onto the gun's line: the shots land in the middle of the crosshair; the aim sway moves the view.
    for (int i = 0; i < 3; ++i) g_aimDir[i] = want[i];
    g_aimFollow = g_aimFollow + dt / 0.2f < 1.0f ? g_aimFollow + dt / 0.2f : 1.0f;
    const float b = g_aimFollow * g_aimFollow * (3.0f - 2.0f * g_aimFollow);
    float d[3], dl = 0.0f;
    for (int i = 0; i < 3; ++i) d[i] = cur[i] + (g_aimDir[i] - cur[i]) * b, dl += d[i] * d[i];
    dl = sqrtf(dl);
    for (int i = 0; i < 3; ++i) tgt[i] = eye[i] + d[i] / dl * dist;
    if (g_aimFollow >= 1.0f) SetAimCentered();
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
    Hook(kFnPartnerHit, kSigPartnerHit, sizeof(kSigPartnerHit), (void*)HookPartnerHit, (void**)&g_origPartnerHit,
         "partner hit");
    return cam;
}
