// Addresses and layouts for RE4 UHD Steam (bio4.exe, SHA-256 19aed4af...ee4a). Values are RVAs (VA - 0x400000);
// use Va(). Struct offsets follow re4_tweaks' SDK names (field_OFFSET). See docs/RE-NOTES.md.
#pragma once
#include <stdint.h>

namespace re4 {

// Globals
constexpr uint32_t kPlayerPtr = 0x00857054;  // cPlayer** (pPL)
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
constexpr uint32_t Camera = 0x60;      // CAMERA Camera_60 (final view); CamPoint at +0xA4
constexpr uint32_t QuasiFPS = 0x278;   // CameraQuasiFPS
constexpr uint32_t CameraPoint = 0xA4;  // CAMERA::CamPoint_A4 inside any CAMERA (QuasiFPS included)
}  // namespace cc

// GLOBAL_WK
namespace g {
constexpr uint32_t DeltaTime = 0x70;  // float, 1.0 = one 30 fps frame
}  // namespace g

}  // namespace re4
