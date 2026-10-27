// Legs while aiming (Aim/Legs): RE4 has no strafe animations, so walking while aiming used to slide Leon along with
// his legs still. This puts a walk cycle on his lower body while he steps: thighs swing opposite, each knee bends
// through its swing, the ankle keeps the foot level and the lower body turns toward the step; his upper body, arms and
// gun stay the game's aim pose.
//
// Ported from boshjerns' RE4 SDK (src/move.cpp, "legs v5", https://github.com/boshjerns/re4-boshjerns-sdk), altered:
// the step comes from our aim walk (camera.cpp) instead of the SDK's own, and the jump tuck, the enemy speed and bullet
// time parts are left out. The reload glide instead plays Leon's own walk animation on his legs (ours).
//   MIT License. Copyright (c) 2026 boshjerns. Permission is hereby granted, free of charge, to any person obtaining a
//   copy of this software and associated documentation files (the "Software"), to deal in the Software without
//   restriction, including without limitation the rights to use, copy, modify, merge, publish, distribute, sublicense,
//   and/or sell copies of the Software, and to permit persons to whom the Software is furnished to do so, subject to
//   the following conditions: The above copyright notice and this permission notice shall be included in all copies
//   or substantial portions of the Software. THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
//   IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
//   NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
//   LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
//   SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
//
// Where the pose goes (the SDK's docs/game/animation-pipeline.md): cModel_MotionUpdate (0x00662B90, cdecl
// (cModel*, int) -> int, non-zero when the motion has finished: the callers step their state machines on it, so the
// hook must hand it back) poses every bone from the body slot (+0x1D8) through cModel_PoseFromMotion (0x00661D60, cdecl
// (cModel*, MOTION_INFO*, int)), lerps the aim layer (*(+0x2A8)) over it, then builds the local and world matrices the
// renderer draws. The gait is planned once per frame before the update and added right after the game poses the body
// slot (and a full aim layer), so the game builds every matrix, its arms and the gun from our legs.
#include "common.h"
#include "config.h"
#include "re4.h"
#include <math.h>
#include <string.h>
#include "MinHook.h"

using namespace re4;

bool AimStep(float* side, float* fwd, bool* reload);  // camera.cpp: Leon steps while aiming, relative to his facing

namespace {
constexpr uint32_t kFnMotionUpdate = 0x00262B90;
constexpr uint8_t kSigMotionUpdate[] = {0x55, 0x8B, 0xEC, 0x81, 0xEC, 0x38, 0x01, 0x00, 0x00};  // sub esp,0x138
constexpr uint32_t kFnPoseFromMotion = 0x00261D60;
constexpr uint8_t kSigPoseFromMotion[] = {0x55, 0x8B, 0xEC, 0x81, 0xEC, 0xAC, 0x00, 0x00, 0x00};  // sub esp,0xAC
// Bones: a chain from cModel +0xF4 through Bone +0xF4; local position +0x94, euler rotation +0xA0, scale +0xAC.
constexpr uint32_t kFirstBone = 0xF4, kNextBone = 0xF4, kBonePos = 0x94, kBoneRot = 0xA0;
constexpr uint32_t kBodySlot = 0x1D8, kAimLayer = 0x2A8, kMotionFlags = 0x44;
constexpr int kLowerFirst = 17, kLowerLast = 25;  // Leon: lower root 17, left leg 18-21, right leg 22-25
constexpr int kMaxBones = 160;
constexpr float kPi = 3.14159265f;

typedef int(__cdecl* PFN_MotionUpdate)(uintptr_t model, int flag);
typedef void(__cdecl* PFN_PoseFromMotion)(uintptr_t model, uintptr_t mi, int arg);
PFN_MotionUpdate g_origMotionUpdate = nullptr;
PFN_PoseFromMotion g_origPose = nullptr;

uintptr_t Player() {
    const uintptr_t p = *(uintptr_t*)Va(kPlayerPtr);
    return (p >= 0x10000 && Readable((void*)p, 0x300)) ? p : 0;
}

float FrameDt() {  // 30 fps frames this frame
    const uintptr_t g = *(uintptr_t*)Va(kGlobalPtr);
    const float dt = (g && Readable((void*)(g + g::DeltaTime), 4)) ? At<float>(g, g::DeltaTime) : 1.0f;
    return dt > 0.0f && dt < 4.0f ? dt : 1.0f;
}

// Leon's bones 0..kLowerLast, or 0 when this isn't his skeleton (26+ bones, knees well below the thighs).
int LowerBones(uintptr_t pl, uintptr_t* bones) {
    int n = 0;
    for (uintptr_t b = At<uintptr_t>(pl, kFirstBone); b >= 0x10000 && n <= kLowerLast && Readable((void*)b, 0x100);
         b = At<uintptr_t>(b, kNextBone))
        bones[n++] = b;
    if (n <= kLowerLast || !(At<float>(bones[19], kBonePos + 4) < -300.0f)) return 0;
    return n;
}

// Bone writes go through BoneAdd: each bone remembers the game's own rotation (its base) and gets base + delta. A bone
// the game re-poses every frame gets a fresh base; one no motion keys keeps its base, so a delta never piles up on
// last frame's.
struct BoneBase { float base[3], wrote[3]; bool have; };
BoneBase g_base[kLowerLast + 1];
uintptr_t g_baseModel = 0;

void BoneAdd(uintptr_t pl, uintptr_t bone, int i, float dx, float dy, float dz) {
    if (g_baseModel != pl) g_baseModel = pl, memset(g_base, 0, sizeof g_base);
    float* r = (float*)(bone + kBoneRot);
    BoneBase& b = g_base[i];
    if (!b.have || r[0] != b.wrote[0] || r[1] != b.wrote[1] || r[2] != b.wrote[2])
        b.base[0] = r[0], b.base[1] = r[1], b.base[2] = r[2], b.have = true;
    r[0] = b.base[0] + dx, r[1] = b.base[1] + dy, r[2] = b.base[2] + dz;
    b.wrote[0] = r[0], b.wrote[1] = r[1], b.wrote[2] = r[2];
}

void BoneRestore(uintptr_t pl, uintptr_t bone, int i) {
    if (g_baseModel != pl || !g_base[i].have) return;
    float* r = (float*)(bone + kBoneRot);
    const BoneBase& b = g_base[i];
    if (r[0] == b.wrote[0] && r[1] == b.wrote[1] && r[2] == b.wrote[2])
        r[0] = b.base[0], r[1] = b.base[1], r[2] = b.base[2];
    g_base[i].have = false;
}

struct Gait {
    bool on;
    float thigh[2], knee[2], ankle[2], turn;
} g_gait{};
float g_phase = 0.0f, g_amp = 0.0f, g_turn = 0.0f;

// The gait for this frame, from the step direction relative to his aim: a phase driven by the distance he moves.
void GaitPlan(uintptr_t pl) {
    g_gait.on = false;
    uintptr_t bones[kLowerLast + 1];
    if (!LowerBones(pl, bones)) return;
    const Config& c = Cfg();
    const float ff = FrameDt();
    float side = 0.0f, fwd = 0.0f;
    bool reload = false;
    const bool stepping = AimStep(&side, &fwd, &reload) && !reload;  // the reload glide walks (below)
    const float mag = fminf(1.0f, sqrtf(side * side + fwd * fwd));
    const bool moving = stepping && isfinite(mag) && mag > 0.0f;
    float ang = moving ? atan2f(side, fwd) : 0.0f;  // 0 ahead, +pi/2 right
    float dir = 1.0f;                               // backward-ish steps walk back, still facing the aim
    if (fabsf(ang) > 1.75f) dir = -1.0f, ang = ang > 0.0f ? ang - kPi : ang + kPi;
    const float k = fminf(1.0f, 0.3f * ff);  // settles in ~0.1 s
    g_amp += ((moving ? mag : 0.0f) - g_amp) * k;
    g_turn += ((moving ? fmaxf(-1.4f, fminf(1.4f, -ang)) : 0.0f) - g_turn) * k;  // turning right lowers the angle
    // Our aim walk speed is in units a second; a full cycle (two steps) covers LegsStride.
    if (moving) g_phase += dir * c.aimWalkSpeed / 30.0f * ff * mag / fmaxf(100.0f, c.legsStride) * 2.0f * kPi;
    g_phase = isfinite(g_phase) ? fmodf(g_phase, 2.0f * kPi) : 0.0f;
    if (!isfinite(g_amp) || !isfinite(g_turn)) g_amp = g_turn = 0.0f;
    if (g_amp < 0.01f && fabsf(g_turn) < 0.01f) return;
    const float swing = c.legsSwing * g_amp, knee = c.legsKnee * g_amp;
    for (int i = 0; i < 2; ++i) {
        const float ph = g_phase + (i ? kPi : 0.0f);
        g_gait.thigh[i] = -swing * sinf(ph);                                      // negative = forward
        g_gait.knee[i] = knee * fmaxf(0.0f, dir * cosf(ph)) + 0.08f * g_amp;      // bends while that leg swings
        g_gait.ankle[i] = -0.6f * (g_gait.thigh[i] + g_gait.knee[i]);             // the foot stays near level
    }
    g_gait.turn = g_turn;
    g_gait.on = true;
}

void ApplyGait(uintptr_t pl) {
    uintptr_t bones[kLowerLast + 1];
    if (!g_gait.on || !LowerBones(pl, bones)) return;
    static DWORD s_log = 0;
    if (GetTickCount() - s_log > 2000)
        s_log = GetTickCount(), Log("legs: gait on, amp %.2f thigh %.2f %.2f turn %.2f", g_amp, g_gait.thigh[0],
                                    g_gait.thigh[1], g_gait.turn);
    const int thighs[2] = {18, 22}, knees[2] = {19, 23}, ankles[2] = {20, 24};
    for (int i = 0; i < 2; ++i) {
        BoneAdd(pl, bones[thighs[i]], thighs[i], g_gait.thigh[i], 0.0f, 0.0f);
        BoneAdd(pl, bones[knees[i]], knees[i], g_gait.knee[i], 0.0f, 0.0f);
        BoneAdd(pl, bones[ankles[i]], ankles[i], g_gait.ankle[i], 0.0f, 0.0f);
    }
    BoneAdd(pl, bones[kLowerFirst], kLowerFirst, 0.0f, g_gait.turn, 0.0f);
}

void EndGait(uintptr_t pl) {
    uintptr_t bones[kLowerLast + 1];
    if (!LowerBones(pl, bones)) return;
    for (int i = kLowerFirst; i <= kLowerLast; ++i) BoneRestore(pl, bones[i], i);
}

// Last step of every player pose: a bone whose local transform holds NaN/Inf takes back its last finite one. The game
// blends each new pose from the current bones, so one bad value would otherwise spread and stay (the SDK lost Leon's
// legs that way once).
float g_good[kMaxBones][9];
bool g_haveGood[kMaxBones];
uintptr_t g_goodModel = 0;

// The blend source a new motion starts from (cModel_SavePose / cModel_BlendSaved 0x0065B890): position, rotation,
// scale at Bone +0x174. The SDK saw NaN there too after putting other motions on the legs.
constexpr uint32_t kBoneSaved = 0x174;

void KeepBonesFinite(uintptr_t pl) {
    if (g_goodModel != pl) g_goodModel = pl, memset(g_haveGood, 0, sizeof g_haveGood);
    int n = 0, fixed = -1, savedFixed = -1;
    unsigned badMask = 0;
    for (uintptr_t b = At<uintptr_t>(pl, kFirstBone); b >= 0x10000 && n < kMaxBones && Readable((void*)b, 0x1D8);
         b = At<uintptr_t>(b, kNextBone), ++n) {
        float* v = (float*)(b + kBonePos);  // position, rotation, scale: 9 floats in a row
        bool ok = true;
        for (int k = 0; k < 9; ++k) ok = ok && isfinite(v[k]);
        float* s = (float*)(b + kBoneSaved);
        bool sok = true;
        for (int k = 0; k < 9; ++k) sok = sok && isfinite(s[k]);
        if (!sok) {
            if (savedFixed < 0) savedFixed = n;
            for (int k = 0; k < 9; ++k)
                if (!isfinite(s[k])) s[k] = ok ? v[k] : (k >= 6 ? 1.0f : 0.0f);
        }
        if (ok) {
            memcpy(g_good[n], v, sizeof g_good[n]), g_haveGood[n] = true;
            continue;
        }
        if (fixed < 0)
            for (int k = 0; k < 9; ++k) badMask |= isfinite(v[k]) ? 0 : 1u << k;
        if (g_haveGood[n]) memcpy(v, g_good[n], sizeof g_good[n]);
        else
            for (int k = 0; k < 9; ++k)
                if (!isfinite(v[k])) v[k] = k >= 6 ? 1.0f : 0.0f;
        if (fixed < 0) fixed = n;
    }
    static DWORD s_log = 0;
    if ((fixed >= 0 || savedFixed >= 0) && GetTickCount() - s_log > 5000)
        s_log = GetTickCount(), Log("legs: bone %d had NaN (pos/rot/scale mask %03X), saved pose of bone %d had NaN; restored",
                                    fixed, badMask, savedFixed);
}

// The reload glide (a reload while walking, without the aim button): Leon turns the way he goes, so his own walk
// animation fits. Starting a motion (0x006611A0) resets blend state on every bone and re-links attachments (it
// distorted the gun once), so we never start one: whenever the game's walk routine (r_no_1 1) has just set his walk
// on the body slot (cPlayer +0x1D8, MOTION_INFO up to the aim layer pointer at +0x2A8), we keep a copy of that slot,
// and the glide plays the copy (frame advanced by 0x0065FD80) and takes only its legs (bones 18-25), after the
// game poses the body slot and again after a full aim layer. Hurt, the copy is his limp, as the game picked it.
typedef uint16_t(__cdecl* PFN_AdvanceMotion)(uintptr_t mi);
constexpr uint32_t kFnAdvanceMotion = 0x0025FD80;
constexpr uint32_t kMiSize = kAimLayer - kBodySlot;  // 0xD0
constexpr uint32_t kMiFrame = 0xB8, kMiSpeed = 0xC0, kMiAttach = 0xCC;
constexpr float kWalkAnimSpeed = 1700.0f;  // game units a second his walk carries him (measured: 2036 in 1.2 s)
alignas(16) uint8_t g_walkMi[0x100];
uintptr_t g_walkModel = 0;
bool g_walkOn = false;
float g_walkW = 0.0f;  // how much of the walk the legs show: eases in and out over kWalkBlend, no jump at either end
constexpr float kWalkBlend = 0.15f;
float g_walkLower[kLowerLast + 1][9];  // this frame's walk pose of the lower body (position, rotation, scale)
uint32_t g_walkFrame = 0, g_walkPosed = ~0u;

// The copy points into the game's motion data (MOTION_INFO +0x00 fcv data, +0x04 hermite data, +0x34 joint numbers,
// +0x38 joint kinds, +0xA8 sequence). The game reuses those buffers: on 2026-10-27, after aiming another weapon, the
// walk's data held garbage, Leon's root came out at -2.2e38 and the game's angle wrap (0x00658360) looped forever. So
// the copy keeps a fingerprint of what it points at and is only played while that is unchanged.
constexpr uint32_t kMiPtrs[] = {0x00, 0x04, 0x34, 0x38, 0xA8};
uint32_t g_walkSig = 0;

uint32_t WalkDataSig() {  // 0: something the copy points at can't be read
    uint32_t h = 2166136261u;  // FNV-1a over the first 64 bytes behind each pointer
    for (uint32_t off : kMiPtrs) {
        const uintptr_t p = At<uintptr_t>((uintptr_t)g_walkMi, off);
        if (!p) continue;
        if (p < 0x10000 || !Readable((void*)p, 64)) return 0;
        for (int i = 0; i < 64; ++i) h = (h ^ ((const uint8_t*)p)[i]) * 16777619u;
    }
    return h ? h : 1;
}

// Called before every player motion update: keep the walk the game itself set up.
void WalkSnapshot(uintptr_t pl) {
    if (At<uint8_t>(pl, pl::RNo0) != 0 || At<uint8_t>(pl, pl::RNo0 + 1) != 1 || At<uint8_t>(pl, pl::RNo0 + 2) == 0) return;
    memcpy(g_walkMi, (void*)(pl + kBodySlot), kMiSize);
    At<uint32_t>((uintptr_t)g_walkMi, kMiAttach) = 0;  // no attachment of ours
    // No blend in the copy (+0xC4/+0xC5, frames left of a blend from the bones' saved matrices): the walk routine's
    // start blends the body slot from the previous motion; ours would blend from whatever those matrices hold now.
    At<uint16_t>((uintptr_t)g_walkMi, 0xC4) = 0;
    g_walkSig = WalkDataSig();
    if (!g_walkSig) {
        g_walkModel = 0;
        return;
    }
    if (g_walkModel != pl) Log("legs: walk copied (frame %.1f speed %.2f)", At<float>((uintptr_t)g_walkMi, kMiFrame),
                               At<float>((uintptr_t)g_walkMi, kMiSpeed));
    g_walkModel = pl;
}

bool WalkStart(uintptr_t pl) {
    if (g_walkModel != pl) return false;
    if (WalkDataSig() == g_walkSig) return true;
    Log("legs: walk copy dropped, the game reused its motion data (walks again once he walks normally)");
    g_walkModel = 0;
    return false;
}

// Once per frame: the walk cycle moves on as far as Leon glides (stick tilt x ReloadSpeed / his walk's speed).
void WalkPlan(uintptr_t pl) {
    float side = 0.0f, fwd = 0.0f;
    bool reload = false;
    g_walkOn = AimStep(&side, &fwd, &reload) && reload && WalkStart(pl);
    if (!g_walkOn) return;
    const float mag = fminf(1.0f, sqrtf(side * side + fwd * fwd));
    const uintptr_t g = *(uintptr_t*)Va(kGlobalPtr);
    float* factor = (g && Readable((void*)(g + g::DeltaTime), 4)) ? (float*)(g + g::DeltaTime) : nullptr;
    if (!factor) return;
    const float saved = *factor;
    *factor = saved * mag * Cfg().reloadSpeed / kWalkAnimSpeed;
    ((PFN_AdvanceMotion)Va(kFnAdvanceMotion))((uintptr_t)g_walkMi);
    *factor = saved;
    ++g_walkFrame;
    static DWORD s_log = 0;
    if (GetTickCount() - s_log > 2000)
        s_log = GetTickCount(), Log("legs: walk frame %.1f speed %.2f mag %.2f", At<float>((uintptr_t)g_walkMi, kMiFrame),
                                    At<float>((uintptr_t)g_walkMi, kMiSpeed), mag);
}

// After the game posed the reload: pose the walk (once a frame), keep its lower body, put the rest back.
void ApplyWalk(uintptr_t pl) {
    uintptr_t bones[kMaxBones];
    int n = 0;
    for (uintptr_t b = At<uintptr_t>(pl, kFirstBone); b >= 0x10000 && n < kMaxBones && Readable((void*)b, 0x100);
         b = At<uintptr_t>(b, kNextBone))
        bones[n++] = b;
    if (n <= kLowerLast || !(At<float>(bones[19], kBonePos + 4) < -300.0f)) return;
    if (g_walkPosed != g_walkFrame) {
        static float saved[kMaxBones][9];
        for (int i = 0; i < n; ++i) memcpy(saved[i], (void*)(bones[i] + kBonePos), sizeof saved[i]);
        g_origPose(pl, (uintptr_t)g_walkMi, 0);
        for (int i = kLowerFirst; i <= kLowerLast; ++i) memcpy(g_walkLower[i], (void*)(bones[i] + kBonePos), sizeof g_walkLower[i]);
        for (int i = 0; i < n; ++i) memcpy((void*)(bones[i] + kBonePos), saved[i], sizeof saved[i]);
        g_walkPosed = g_walkFrame;
        static DWORD s_log = 0;
        if (GetTickCount() - s_log > 2000)
            s_log = GetTickCount(), Log("legs: walk posed, left thigh rot %.2f %.2f %.2f (reload pose %.2f %.2f %.2f)",
                                        g_walkLower[18][3], g_walkLower[18][4], g_walkLower[18][5], saved[18][3],
                                        saved[18][4], saved[18][5]);
    }
    // The legs only (18-25): the lower root's turn in the walk is relative to his walking upper body, and under the
    // reload's upper body it twisted the whole lower half. The hips stay the reload's.
    const float w = g_walkW;
    for (int i = kLowerFirst + 1; i <= kLowerLast; ++i) {
        bool ok = true;
        for (int k = 0; k < 9; ++k) ok = ok && isfinite(g_walkLower[i][k]);
        if (!ok) continue;
        float* v = (float*)(bones[i] + kBonePos);  // position, rotation (euler, wraps), scale
        for (int k = 0; k < 9; ++k) {
            const float d = g_walkLower[i][k] - v[k];
            v[k] += (k >= 3 && k < 6 ? remainderf(d, 2.0f * kPi) : d) * w;
        }
    }
}

// The glide ends while Leon keeps moving: hand over the way the game goes from walk to run when the walk button is
// let go (pl_R1_Walk: r_no 0x04020300, +0x544 the step's phase 0..255, +0x549 = 5 blend frames). With r_no_3 bit 4
// the walk and run routines start their motion at that phase (+0x544 x the motion's length / 256) and blend into it
// from the current pose over +0x549 frames, so the step goes on instead of the legs jumping.
constexpr uint32_t kPhase = 0x544, kBlendFrames = 0x549, kMiCount = 0xBC;
// The routine dispatcher (0x007632E0, cdecl (cPlayer*)): the handover sets the start right before the walk or run
// routine starts, since the end of the weapon routine rewrites r_no_3 later in the frame the glide ends.
typedef void(__cdecl* PFN_Dispatch)(uintptr_t pl);
PFN_Dispatch g_origDispatch = nullptr;
constexpr uint32_t kFnDispatch = 0x003632E0;
constexpr uint8_t kSigDispatch[] = {0x55, 0x8B, 0xEC, 0x8B, 0x45, 0x08, 0x80, 0xB8, 0x70, 0x05, 0x00, 0x00, 0x00};
int g_handPhase = -1;    // a handover waiting for the next walk/run start: the step's phase
uint8_t g_handR1 = 0;    // 1 walk, 3 run

void __cdecl HookDispatch(uintptr_t pl) {
    if (g_handPhase >= 0 && pl == Player()) {
        if (At<uint8_t>(pl, pl::RNo0) == 0 && At<uint8_t>(pl, pl::RNo0 + 1) <= 3) {  // free movement (wait/walk/run)
            At<uint8_t>(pl, pl::RNo0 + 1) = g_handR1;
            At<uint8_t>(pl, pl::RNo0 + 2) = 0;  // start the routine
            At<uint8_t>(pl, pl::RNo0 + 3) |= 4;  // at the step's phase
            At<float>(pl, kPhase) = (float)g_handPhase;
            At<uint8_t>(pl, kBlendFrames) = 5;
        }
        g_handPhase = -1;
    }
    g_origDispatch(pl);
}

bool Handover(uintptr_t pl) {
    // Still pushing the stick: the glide's last step (the free movement only steers him again from the next frame).
    float side = 0.0f, fwd = 0.0f;
    bool reload = false;
    if (!AimStep(&side, &fwd, &reload) || side * side + fwd * fwd < 0.25f * 0.25f) return false;
    const uint64_t keys = *(uint64_t*)Va(kKeyBtnOn);
    const float count = (float)At<uint16_t>((uintptr_t)g_walkMi, kMiCount), frame = At<float>((uintptr_t)g_walkMi, kMiFrame);
    if (!(count > 1.0f) || !isfinite(frame)) return false;
    const int phase = (int)(frame / count * 255.0f + 0.5f) & 0xFF;  // as pl_R1_Walk: frame x 255 / length
    // The run button as the free movement reads it (Movement/Run): Always runs unless it is held.
    const bool held = (keys & KEY_RUN) != 0;
    const bool run = Cfg().run == RUN_ALWAYS ? !held : Cfg().run == RUN_OUTER_RING ? (held || side * side + fwd * fwd >= Cfg().outerRing * Cfg().outerRing) : held;
    At<float>(pl, kPhase) = (float)phase;
    At<uint8_t>(pl, kBlendFrames) = 5;
    At<uint32_t>(pl, pl::RNo0) = run ? 0x04000300 : 0x04000100;  // r_no_0 0, r_no_1 run/walk, start, keep the phase
    g_handPhase = phase, g_handR1 = run ? 3 : 1;  // and again right before the routine runs (HookDispatch)
    Log("legs: glide ends, %s from step phase %d/256", run ? "run" : "walk", phase);
    return true;
}

int __cdecl HookMotionUpdate(uintptr_t model, int flag) {
    if (model != Player()) return g_origMotionUpdate(model, flag);  // the callers' "motion done": always hand it back
    const bool was = g_gait.on;
    WalkSnapshot(model);
    // Out of the weapon routine with the walk still on the legs: let the game's walk/run pick the step up (the next
    // frame starts it); this frame still shows the walk in full.
    static uint8_t s_prevR1 = 0;
    const uint8_t r0 = At<uint8_t>(model, pl::RNo0), r1 = At<uint8_t>(model, pl::RNo0 + 1);
    bool handed = false;
    if (s_prevR1 == 6 && r0 == 0 && r1 != 6 && r1 <= 3 && g_walkW > 0.5f && Handover(model)) handed = true;
    s_prevR1 = r0 == 0 ? r1 : 0xFF;
    // In the weapon routine (r_no_0 0, r_no_1 6): aiming or the reload glide.
    if (Cfg().aimLegs && At<uint8_t>(model, pl::RNo0) == 0 && At<uint8_t>(model, pl::RNo0 + 1) == 6) {
        GaitPlan(model);
        WalkPlan(model);
    } else {
        g_gait.on = g_walkOn = false, g_amp = g_turn = 0.0f;
    }
    const float step = FrameDt() / 30.0f / kWalkBlend;
    g_walkW = g_walkOn ? fminf(1.0f, g_walkW + step) : fmaxf(0.0f, g_walkW - step);
    static bool s_handedLast = false;
    if (handed) g_walkW = 1.0f;                // this frame: the walk in full, the game blends from it next frame
    else if (s_handedLast) g_walkW = 0.0f;     // then the game's own blend takes over
    s_handedLast = handed;
    if (was && !g_gait.on) EndGait(model);
    const int finished = g_origMotionUpdate(model, flag);  // ApplyGait runs inside, from the pose hook
    if (Cfg().aimLegs) KeepBonesFinite(model);
    return finished;
}

// After the game poses the body slot, or a full aim layer (+0x44 bit 31 clear: it overwrites every bone), the gait
// goes on top, before any matrix is built.
void __cdecl HookPoseFromMotion(uintptr_t model, uintptr_t mi, int arg) {
    g_origPose(model, mi, arg);
    if ((!g_gait.on && g_walkW <= 0.0f) || mi == (uintptr_t)g_walkMi || model != Player()) return;
    const uintptr_t layer = At<uintptr_t>(model, kAimLayer);
    if (mi == model + kBodySlot || (mi == layer && layer && !(At<uint32_t>(mi, kMotionFlags) & 0x80000000u))) {
        if (g_walkW > 0.0f) ApplyWalk(model);
        else ApplyGait(model);
    }
}

// Backstop: the game's euler wrap (0x00658360, cdecl (float[3])) steps by 2 pi in a loop, so a huge or infinite angle
// hangs the game for good. Sane angles go through unchanged; past +-1e4 they are wrapped in one step (inf becomes 0).
constexpr uint32_t kFnWrapAngles = 0x00258360;
constexpr uint8_t kSigWrapAngles[] = {0x55, 0x8B, 0xEC, 0xD9, 0x05};  // fld dword [2 pi]
typedef void(__cdecl* PFN_WrapAngles)(float* v);
PFN_WrapAngles g_origWrap = nullptr;

void __cdecl HookWrapAngles(float* v) {
    for (int i = 0; i < 3; ++i)
        if (fabsf(v[i]) > 1.0e4f) {
            static DWORD s_log = 0;
            if (GetTickCount() - s_log > 5000) s_log = GetTickCount(), Log("legs: angle %g wrapped in one step (would hang)", v[i]);
            v[i] = isfinite(v[i]) ? remainderf(v[i], 2.0f * kPi) : 0.0f;
        }
    g_origWrap(v);
}

bool Hook(uint32_t rva, const uint8_t* sig, size_t n, void* detour, void** orig, const char* name) {
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
}  // namespace

void InitLegsHooks() {
    Hook(kFnMotionUpdate, kSigMotionUpdate, sizeof(kSigMotionUpdate), (void*)HookMotionUpdate,
         (void**)&g_origMotionUpdate, "motion update");
    Hook(kFnPoseFromMotion, kSigPoseFromMotion, sizeof(kSigPoseFromMotion), (void*)HookPoseFromMotion,
         (void**)&g_origPose, "pose from motion");
    Hook(kFnDispatch, kSigDispatch, sizeof(kSigDispatch), (void*)HookDispatch, (void**)&g_origDispatch,
         "player routine");
    Hook(kFnWrapAngles, kSigWrapAngles, sizeof(kSigWrapAngles), (void*)HookWrapAngles, (void**)&g_origWrap,
         "angle wrap");
}
