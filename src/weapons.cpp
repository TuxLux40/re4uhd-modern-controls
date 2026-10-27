// Remake-style weapon quick select on the d-pad ([Weapons] DpadSelect): during gameplay each direction equips the
// next weapon Leon carries from its list (default: up handguns and magnums, right shotguns, down rifles, machine guns
// and launchers, left grenades); pressing it again moves on through the list. The d-pad's tank controls are off then;
// menus and the inventory keep the d-pad.
// Rearm ([Weapons] Rearm): when the last grenade or egg leaves Leon's hands he'd stand there unarmed; the weapon he
// held before it comes back as soon as the game lets him switch (the same switch as the d-pad).
//
// The switch follows re4_tweaks' weapon hotkeys (dllmain/Trainer.cpp HotkeySlotPressed and the cycle lists in
// dllmain/Settings.h, https://github.com/nipkownix/re4_tweaks, zlib license, Copyright (C) 2021 nipkow):
// cItemMgr::arm makes the item the equipped one and WeaponChange swaps the model in Leon's hands. The item has to lie
// in the case (cItem onboard_D): cItemMgr::search, which re4_tweaks uses, also finds items the character doesn't
// have yet (Ada's shotgun early in Separate Ways), whose model isn't loaded; equipping one crashed the game. Not while Leon fires or reloads, and only where the game would let the inventory open (cPlayer::subScrCheck).
// The pad is read on the input side; the switch itself runs where re4_tweaks runs it, at the start of the game's frame
// (cSceSys::scheduler, before any task updates or queues a model for drawing): swapped mid-frame (from KeyConvert),
// the old weapon model was still queued and the draw crashed in a vertex upload (memcpy to a failed Lock, 2026-10-26).
#include "common.h"
#include "config.h"
#include "re4.h"

bool GameplayView();  // camera.cpp
int AimStance();      // buttons.cpp

namespace {
// cItemMgr ItemMgr: +0x0C cItem* m_pWep (equipped), +0x13 u8 m_char (whose items), +0x14 cItem* items, +0x1C count.
// cItem (0x0E bytes, re4_tweaks SDK/item.h): +0 u16 id, +2 u16 num, +4 u8 be_flag (bit 0 in use), +5 u8 chr,
// +0xD u8 onboard (in the case).
constexpr uint32_t kItemMgr = 0x00870FD4;
// thiscall bool cItemMgr::arm(cItem*): equips it (null unequips)
constexpr uint32_t kFnItemArm = 0x003080A0;
const uint8_t kSigItemArm[] = {0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x08, 0x56, 0x8B, 0x75, 0x08, 0x57, 0x8B, 0xF9};
// cdecl void WeaponChange(): puts the equipped weapon's model in Leon's hands
constexpr uint32_t kFnWeaponChange = 0x00368A00;
const uint8_t kSigWeaponChange[] = {0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x08, 0x0F, 0xB7, 0x05};
// thiscall bool cPlayer::subScrCheck(): the inventory may open now
constexpr uint32_t kFnSubScrCheck = 0x0027BEA0;
const uint8_t kSigSubScrCheck[] = {0x80, 0xB9, 0xFC, 0x00, 0x00, 0x00, 0x00, 0x75};

typedef bool(__thiscall* PFN_Arm)(uintptr_t mgr, uintptr_t item);
typedef void(__cdecl* PFN_WeaponChange)();
typedef bool(__thiscall* PFN_SubScrCheck)(uintptr_t player);

PFN_Arm g_arm = nullptr;
PFN_WeaponChange g_change = nullptr;
PFN_SubScrCheck g_subScr = nullptr;

// thiscall void cSceSys::scheduler(), ECX = SceSys (0x00C67BA0): runs every task of the frame (main loop 0x006C52B0)
constexpr uint32_t kFnScheduler = 0x002C4440;
const uint8_t kSigScheduler[] = {0x55, 0x8B, 0xEC, 0x51, 0x80, 0x3D};
typedef void(__fastcall* PFN_Scheduler)(uintptr_t self, void* edx);
PFN_Scheduler g_origScheduler = nullptr;

// cdecl (id, ITEM_INFO*): the item's type at +2 (1 = weapon; re4_tweaks bio4::itemInfo)
constexpr uint32_t kFnItemInfo = 0x003057D0;

volatile LONG g_request = -1;  // the d-pad direction waiting for the main thread (0 up, 1 right, 2 down, 3 left)

template <typename F>
F Resolve(uint32_t rva, const uint8_t* sig, size_t n, const char* name) {
    void* p = (void*)Va(rva);
    if (!SafeEqual(p, sig, n)) {
        Log("weapons: unexpected code for %s at %p, quick select off (different exe build?)", name, p);
        return nullptr;
    }
    return (F)p;
}

// The item with that id in the current character's case, or 0 (cItemMgr::search plus the onboard test).
uintptr_t Carried(uintptr_t mgr, uint16_t id) {
    const uintptr_t items = At<uintptr_t>(mgr, 0x14);
    const int n = At<int>(mgr, 0x1C);
    if (items < 0x10000 || n <= 0 || n > 1024 || !Readable((void*)items, n * 0x0E)) return 0;
    const uint8_t chr = At<uint8_t>(mgr, 0x13);
    for (int i = 0; i < n; ++i) {
        const uintptr_t it = items + i * 0x0E;
        if ((At<uint8_t>(it, 4) & 1) && At<uint8_t>(it, 5) == chr && At<uint16_t>(it, 0) == id && At<uint8_t>(it, 0xD))
            return it;
    }
    return 0;
}

void Switch(int dir) {
    const uintptr_t pl = *(uintptr_t*)Va(re4::kPlayerPtr);
    if (pl < 0x10000 || !Readable((void*)(pl + re4::pl::RNo0), 4)) return;
    const uint8_t r2 = At<uint8_t>(pl, re4::pl::RNo0 + 2);
    if (r2 == 2 || r2 == 4) return;  // firing or reloading
    if (!g_subScr(pl)) return;       // jumping, climbing, a ladder, an event ...
    const Config& c = Cfg();
    const int n = c.wepCount[dir];
    if (n <= 0) return;
    const uintptr_t mgr = Va(kItemMgr);
    const uintptr_t cur = At<uintptr_t>(mgr, 0x0C);
    const uint16_t curId = cur >= 0x10000 && Readable((void*)cur, 2) ? *(uint16_t*)cur : 0;
    // After the equipped weapon if it is in this list, else where this direction left off.
    static int next[4] = {};
    int start = next[dir] % n;
    for (int i = 0; i < n; ++i)
        if (c.wepGroup[dir][i] == curId) start = (i + 1) % n;
    for (int i = 0; i < n; ++i) {
        const int k = (start + i) % n;
        const uint16_t id = c.wepGroup[dir][k];
        const uintptr_t item = Carried(mgr, id);
        if (!item) continue;
        next[dir] = k;
        if (id == curId) return;  // the only one of this list Leon carries, already in his hands
        const bool ok = g_arm(mgr, item);
        if (ok) g_change();
        Log("weapons: d-pad %d -> item %u (was %u)%s", dir, id, curId, ok ? "" : ", arm refused");
        return;
    }
}
// The id of the item in Leon's hands, 0 for none (the pointer can outlive the item: a used-up one is no longer in use).
uint16_t ArmedId(uintptr_t mgr) {
    const uintptr_t cur = At<uintptr_t>(mgr, 0x0C);
    if (cur < 0x10000 || !Readable((void*)cur, 0x0E) || !(At<uint8_t>(cur, 4) & 1)) return 0;
    return At<uint16_t>(cur, 0);
}

bool IsWeapon(uint16_t id) {
    uint8_t info[32] = {};
    ((void(__cdecl*)(uint32_t, void*))Va(kFnItemInfo))(id, info);
    return info[2] == 1;
}

// Once per frame: notes the last real weapon, and brings it back after the last throwable is gone.
void Rearm() {
    static uint16_t s_armed = 0, s_weapon = 0, s_want = 0;
    static DWORD s_since = 0;
    const uintptr_t mgr = Va(kItemMgr);
    const uint16_t id = ArmedId(mgr);
    if (id != s_armed) {
        if (id && IsWeapon(id)) s_weapon = id;
        // A throwable gone from his hands (not swapped for another item): the last one was thrown.
        if (!id && s_armed && !IsWeapon(s_armed) && s_weapon) s_want = s_weapon, s_since = GetTickCount();
        if (id) s_want = 0;  // he took something himself
        s_armed = id;
    }
    if (!s_want) return;
    if (GetTickCount() - s_since > 5000) {  // never got a moment to switch (a cutscene, the case opened ...)
        s_want = 0;
        return;
    }
    const uintptr_t pl = *(uintptr_t*)Va(re4::kPlayerPtr);
    if (pl < 0x10000 || !Readable((void*)(pl + re4::pl::RNo0), 4)) return;
    const uint8_t r2 = At<uint8_t>(pl, re4::pl::RNo0 + 2);
    if (r2 == 2 || r2 == 4 || !g_subScr(pl)) return;  // still throwing: wait
    const uintptr_t item = Carried(mgr, s_want);
    const bool ok = item && g_arm(mgr, item);
    if (ok) g_change();
    Log("weapons: last throwable gone -> item %u%s", s_want, !item ? " not carried any more" : ok ? "" : ", arm refused");
    s_want = 0;
}
}  // namespace

void WeaponsTick();
static void __fastcall HookScheduler(uintptr_t self, void* edx) {
    WeaponsTick();
    g_origScheduler(self, edx);
}

bool HookAt(uint32_t rva, const uint8_t* sig, size_t n, void* detour, void** orig, const char* name);  // game.cpp
void InitWeapons() {
    g_arm = Resolve<PFN_Arm>(kFnItemArm, kSigItemArm, sizeof(kSigItemArm), "cItemMgr::arm");
    g_change = Resolve<PFN_WeaponChange>(kFnWeaponChange, kSigWeaponChange, sizeof(kSigWeaponChange), "WeaponChange");
    g_subScr = Resolve<PFN_SubScrCheck>(kFnSubScrCheck, kSigSubScrCheck, sizeof(kSigSubScrCheck), "cPlayer::subScrCheck");
    if (!HookAt(kFnScheduler, kSigScheduler, sizeof(kSigScheduler), (void*)HookScheduler, (void**)&g_origScheduler,
                "cSceSys::scheduler"))
        g_arm = nullptr;  // no safe moment to switch: quick select off
}

static bool Ready() { return g_arm && g_change && g_subScr && Cfg().dpadWeapons; }

// Input side (TransformPad): d-pad presses that start in gameplay pick a weapon and don't reach the game.
void WeaponsPad(XGamepad& p) {
    static const WORD kDir[4] = {XB_DPAD_UP, XB_DPAD_RIGHT, XB_DPAD_DOWN, XB_DPAD_LEFT};
    static const WORD kDpad = XB_DPAD_UP | XB_DPAD_DOWN | XB_DPAD_LEFT | XB_DPAD_RIGHT;
    static WORD prev = 0, latched = 0;
    const WORD in = p.wButtons & kDpad;
    if (!Ready()) {
        prev = in, latched = 0;
        return;
    }
    const WORD pressed = in & ~prev;
    prev = in;
    if (pressed && (GameplayView() || AimStance() >= 0)) {
        latched |= pressed;
        for (int d = 0; d < 4; ++d)
            if (pressed & kDir[d]) InterlockedExchange(&g_request, d);
    }
    latched &= in;
    p.wButtons &= ~latched;
}

// Main thread, at the start of each frame (HookScheduler).
void WeaponsTick() {
    const LONG d = InterlockedExchange(&g_request, -1);
    if (d >= 0 && Ready()) Switch((int)d);
    if (g_arm && g_change && g_subScr && Cfg().rearm) Rearm();
}
