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
// Vertical aim, float -1..1 (PlWepLockCtrl adds the stick to it, clamps); kAimVStart is its copy at the aim's start.
constexpr uint32_t kAimV = 0x0087A7D8;
constexpr uint32_t kAimVStart = 0x0087A7D4;
// Aim start, cdecl(cPlayer*) (0x00686880, called once from the aim routine's init). Sets kAimV from a camera look
// angle (radians, doubled when > 0, times 2/pi) through 0x006F2A50, which the PC port stubbed to return 0; with
// option bit 0x20000000 (*kOptionsPtr) it aims at the locked-on enemy instead. Stores the angle in (+0x7D8)->+0x28.
constexpr uint32_t kFnAimInit = 0x00286880;
constexpr uint8_t kSigAimInit[] = {0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x14, 0xA1};  // sub esp,0x14; mov eax,[cookie]
// Partner (cSubChar: Ashley, Luis...) hit handling, thiscall, once per frame. cEm-style cDmgInfo at +0x328 (re4_tweaks
// SDK/em.h): +0x32C pending flag, +0x32E attack type. With the armour costume (pG+0x4FCB == 2) it drops the attack
// types of Leon's guns and knife and only lets explosions and the like through.
constexpr uint32_t kFnPartnerHit = 0x0035EDF0;
constexpr uint8_t kSigPartnerHit[] = {0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x20, 0xA1};
constexpr uint32_t kOptionsPtr = 0x00806F40;  // -> u32 option bits

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
