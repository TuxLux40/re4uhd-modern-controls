// Addresses and layouts for RE4 UHD Steam (bio4.exe, SHA-256 19aed4af...ee4a). Values are RVAs (VA - 0x400000);
// use Va(). Struct offsets follow re4_tweaks' SDK names (field_OFFSET). See docs/RE-NOTES.md.
#pragma once
#include <stdint.h>

namespace re4 {

// Globals
constexpr uint32_t kPlayerPtr = 0x00857054;  // cPlayer** (pPL)
constexpr uint32_t kAshleyPtr = 0x00857060;  // cPlayer** (pAS), same layout as Leon (pos at pl::Pos)
constexpr uint32_t kGlobalPtr = 0x00806F3C;  // GLOBAL_WK** (pG)
constexpr uint32_t kCamCtrl = 0x008702D0;    // CameraControl (the object itself, not a pointer)
// Camera smoothing (thiscall 0x006F2DA0, this = 0x00C70180): every frame the shown camera moves part of the way to the
// computed one (factor pow(+0xFC, dt)). Bit 0 of +0xF8 makes the next update jump straight there (a cut).
constexpr uint32_t kCamSmooth = 0x00870180;
constexpr uint32_t kCamSmoothCut = 0xF8;
constexpr uint32_t kKeyBtnOn = 0x00862F08;   // u64 KEY_BTN bits held this frame (after the game's mapping)

// Functions
constexpr uint32_t kFnCamCtrlMove = 0x002F6EF0;  // thiscall CameraControl::move(), ECX = kCamCtrl
constexpr uint8_t kSigCamCtrlMove[] = {0x55, 0x8B, 0xEC, 0xA1};  // push ebp; mov ebp,esp; mov eax,[pG]
// thiscall CameraQuasiFPS::move(), ECX = kCamCtrl + cc::QuasiFPS. Camera mode 10 (the gameplay camera) runs it and
// then copies its CamPoint (QuasiFPS + 0xA4) into CameraControl::m_camera_38.
constexpr uint32_t kFnQfpsMove = 0x0019E130;
constexpr uint8_t kSigQfpsMove[] = {0x55, 0x8B, 0xEC, 0x81, 0xEC, 0x9C, 0x01, 0x00, 0x00};  // sub esp,0x19C
// cdecl void KeyConvert(): turns the pad into KEY_BTN bits each frame (writes kKeyBtnOn).
constexpr uint32_t kFnKeyConvert = 0x00567360;
constexpr uint8_t kSigKeyConvert[] = {0x55, 0x8B, 0xEC, 0x81, 0xEC, 0x7C, 0x05, 0x00, 0x00};  // sub esp,0x57C

// Line-vs-world trace: thiscall bool Trace(const Vec* from, const Vec* to, Vec* hit, int, uint maskA, uint maskB),
// ret 0x18; returns nonzero and writes the first hit point. ECX = a collision set: kCollWorld is the one Leon's
// movement (masks 0, 0x800) and the game camera (masks 0x8000, 0) use; the laser uses 0x00BF9AA0 with 0, 0x400000.
constexpr uint32_t kFnTrace = 0x00194160;
constexpr uint8_t kSigTrace[] = {0x55, 0x8B, 0xEC, 0x8B, 0x45, 0x10, 0x56, 0x8B, 0x75, 0x14};
constexpr uint32_t kCollWorld = 0x007F9A7C;
constexpr uint32_t kTraceCamA = 0x8000, kTraceCamB = 0;   // the game camera's masks (0x006F69A9)
constexpr uint32_t kTracePlA = 0, kTracePlB = 0x800;      // Leon's movement (0x00761FE0)

// Laser sight: thiscall LaserUpdate(char draw, char, char) (0x0073EA60) computes the beam into kLaserStart/kLaserEnd
// (the end is cut at the first hit) and draws it when `draw` is set.
constexpr uint32_t kFnLaser = 0x0033EA60;
constexpr uint8_t kSigLaser[] = {0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x1C, 0xA1};
constexpr uint32_t kLaserStart = 0x00879818;  // Vec
constexpr uint32_t kLaserEnd = 0x00879828;    // Vec
// Shootable surfaces (LaserUpdate's own check): the line hit's poly code picks one of 8 attribute entries
// (0x0058EE70: bit 23 -> 1, bit 15 -> 2, bit 7 -> 4); entry i lives at kPolyAttr + 0x20 + i*0x44 if the byte at
// kPolyAttr + 0x240 + i is set, bit 1 of its first word = shootable, within sqrt(*kShootableDist2) of the muzzle.
constexpr uint32_t kPolyAttr = 0x007F9AA0;
constexpr uint32_t kShootableDist2 = 0x0070FB70;  // float, squared distance
// The game's projection: MTXPerspective(CAMERA fovy (+0xC0, degrees), *kProjAspect, ...) at 0x006F0053. The aspect is
// 4:3 (1.333) even at 16:9; the image is stretched to the full width. re4_tweaks rewrites it for ultrawide.
constexpr uint32_t kProjAspect = 0x0071377C;  // float

// JOY Joy[4] (re4_tweaks SDK/pad.h); the weapon aim (PlWepLockCtrl, 0x00688090) reads Joy[0]'s left stick and on bits.
// KeyConvert fills it from the pad device (0x00964670(0): floats +0xC/+0x10 left stick, +0x14/+0x18 right stick,
// scaled to +-127, dead zone 31, then +-63). While a bound action such as aim is held it swaps the sticks: Joy left
// <- right stick, Joy right <- left stick (0x009679BD..).
constexpr uint32_t kJoy0 = 0x00863008;
namespace joy {
constexpr uint32_t LX = 0x0, LY = 0x4;  // int
constexpr uint32_t RX = 0x8, RY = 0x9;  // int8
constexpr uint32_t On = 0x14;           // JOY_BTN, low nibble = digital directions from the stick
}  // namespace joy
// Read by PlWepLockCtrl: 1 selects a separate aim path (mouse aiming, presumably).
constexpr uint32_t kAimDeviceFlag = 0x00E224B0;
constexpr uint32_t kAimRamp = 0x00866C1C;  // float, grows while the aim stick is held
// Vertical aim, float -1..1, + = up: kAimV is the target (PlWepLockCtrl adds the stick to it, clamps), kAimVStart the
// value the pose uses, eased toward it by kAimVBlend (0 = follow at once). Both rewritten every frame by 0x006860C0.
constexpr uint32_t kAimV = 0x0087A7D8;
constexpr uint32_t kAimVStart = 0x0087A7D4;
constexpr uint32_t kAimVBlend = 0x0087A7DC;
// Bullet (0x00686C40), cdecl (owner, float* from, float* to, weapon id, int, float): the shot itself, used by
// every attacker. Leon's gun routines (0x008FD410 and friends) pass the muzzle and muzzle + gun direction (plus
// random spread); with mouse aim they first bend that direction toward the cursor (0x00683520).
constexpr uint32_t kFnBullet = 0x00286C40;
constexpr uint8_t kSigBullet[] = {0x55, 0x8B, 0xEC, 0x81, 0xEC, 0xD8, 0x00, 0x00, 0x00, 0xA1};
// Laser line (0x0073DC60), thiscall (float* start, float* end), ret 8: the gun's line from the muzzle, before
// LaserUpdate cuts it at the first hit and draws it.
constexpr uint32_t kFnLaserLine = 0x0033DC60;
constexpr uint8_t kSigLaserLine[] = {0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x38, 0xA1};
// Aim sway (0x00684330), cdecl (cPlayer*, u8 mode, float* v, float* side). kSwayMax: the current weapon's sway limit
// (radians, written by it).
constexpr uint32_t kFnSway = 0x00284330;
constexpr uint8_t kSigSway[] = {0x55, 0x8B, 0xEC, 0x8B, 0x45, 0x08, 0x56, 0x8B, 0x75, 0x10, 0xD9, 0x06};
constexpr uint32_t kSwayMax = 0x00866C0C;
// Laser dot (0x0073DA50): spawns the red dot particle at ESI, cdecl, no stack args.
constexpr uint32_t kFnLaserDot = 0x0033DA50;
constexpr uint8_t kSigLaserDot[] = {0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x18, 0xA1, 0x10, 0x5E, 0xC3, 0x00};
// Line hit test (0x005BC050), cdecl (float* from, float* to, int, int, int* hit enemy out, int* poly out): world and
// enemies; cuts `to` at the nearest hit. LaserUpdate uses it on the laser.
constexpr uint32_t kFnLineHit = 0x001BC050;
constexpr uint8_t kSigLineHit[] = {0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x6C, 0xA1};
// Partner (cSubChar: Ashley, Luis...) hit handling, thiscall, once per frame. cEm-style cDmgInfo at +0x328 (re4_tweaks
// SDK/em.h): +0x32C pending flag, +0x32E attack type. With the armour costume (pG+0x4FCB == 2) it drops the attack
// types of Leon's guns and knife and only lets explosions and the like through.
constexpr uint32_t kFnPartnerHit = 0x0035EDF0;
constexpr uint8_t kSigPartnerHit[] = {0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x20, 0xA1};

// cModel draw, cdecl(cModel*): returns early when +0x158 * +0x154 is 0, else the alpha of every part is scaled by it.
// cdecl void LookAt(CAMERA*) (0x0059E660): builds a camera's view matrix from its finished eye (+0xA4), target (+0xB0)
// and fovy (+0xC0), after every smoothing and camera-switch blend. Called with kCamCtrl + cc::Camera for the shown view.
constexpr uint32_t kFnLookAt = 0x0019E660;
constexpr uint8_t kSigLookAt[] = {0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x58, 0xA1};
constexpr uint32_t kFnModelDraw = 0x002EB450;
constexpr uint8_t kSigModelDraw[] = {0x55, 0x8B, 0xEC, 0x56, 0x8B, 0x75, 0x08, 0xD9, 0x86, 0x58, 0x01, 0x00, 0x00};

// KEY_BTN bits (re4_tweaks SDK/pad.h)
constexpr uint64_t KEY_KAMAE = 0x10;         // aim (weapon ready)
constexpr uint64_t KEY_KAMAE_KNIFE = 0x800;  // knife ready
constexpr uint64_t KEY_RUN = 0x40;

// cModel / cCoord (player)
namespace pl {
constexpr uint32_t Pos = 0x94;      // Vec
constexpr uint32_t Ang = 0xA0;      // Vec, .y = yaw (radians)
constexpr uint32_t RNo0 = 0xFC;     // u8 routine numbers r_no_0..3
constexpr uint32_t Motion = 0x1D8;  // MOTION_INFO; +0x40 Mot_attr (4 = no root motion)
}  // namespace pl

// CameraControl
namespace cc {
constexpr uint32_t Mode = 0x34;        // u8 r0: which camera runs this frame (switch in move())
constexpr uint32_t Point = 0x38;       // CAMERA_POINT m_camera_38: Campos(0) Target(0xC) Roll(0x18) Fovy(0x1C)
constexpr uint32_t Camera = 0x60;      // CAMERA Camera_60 (final view): Mtx view at +0 (3x4 rows), Mtx44 proj at +0x64
constexpr uint32_t QuasiFPS = 0x278;   // CameraQuasiFPS
constexpr uint32_t CameraPoint = 0xA4;  // CAMERA::CamPoint_A4 inside any CAMERA (QuasiFPS included)
}  // namespace cc

// GLOBAL_WK
namespace g {
constexpr uint32_t DeltaTime = 0x70;  // float, 1.0 = one 30 fps frame
}  // namespace g

}  // namespace re4
