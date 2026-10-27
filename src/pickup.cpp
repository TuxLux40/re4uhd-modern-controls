// QuickPickup: A takes an item straight away, like the remakes. The original stops the game for every item: the item
// on a dark full screen, a message ("Will you take it?") and a Yes/No choice.
// The game picks items up in one of two task routines (started by 0x006BCDB0): 0x006BB720 (an item with a model on
// the floor) and 0x006BC3E0 (without one). Both put up their message through MesSet (0x00718DE0, cdecl, ECX = the
// message control 0xC17830), then the puzzle items (weapons, ammo, herbs...) wait for the answer in 0xC17931
// (1 = Yes, 2 = No) and on Yes call PutInCase (0x0068C4D0, cdecl (id, num, case size) -> 1 if it fit). When it
// doesn't fit they open the case (SubScreenOpen 4) to make room, as always. Treasures, keys and money are taken
// before the message and only wait for it to close (0xC17848 bit 0).
// The mod rewrites the MesSet calls of those two routines. For the everyday pickups (ammo, grenades, herbs and the
// other consumables like eggs and fish, treasures, money) the message is skipped and a Yes/No answers Yes; weapons,
// key items and the other important finds keep their screen. The rest of the game's own routine runs as before: the
// item goes into the case, or the case opens when it's full. The item comes from the pickup start 0x006BCDB0 (cdecl,
// the item's entry; id at +0x78).
// 0x006BB720 waits one frame between its message and the answer check, so the answer is set again right before the
// check (its item-view call 0x0062B340 at 0x006BBC1C).
#include "common.h"
#include "config.h"
#include "item_names.h"

static const uint32_t kThunkMesSet = 0x0000E7A5;    // jmp MesSet 0x00718DE0
static const uint32_t kThunkItemView = 0x0000941C;  // jmp 0x0062B340 (thiscall, the item on the pickup screen)
static const uint32_t kThunkPutInCase = 0x00005FD8; // jmp PutInCase 0x0068C4D0
static const uint32_t kMesCalls[] = {
    // 0x006BB720 (item with a model)
    0x002BB810, 0x002BB8B0, 0x002BB900, 0x002BB965, 0x002BB9AA, 0x002BB9D8, 0x002BBA82, 0x002BBB1D,
    // 0x006BC3E0 (item without a model)
    0x002BC4D1, 0x002BC571, 0x002BC5C5, 0x002BC61A, 0x002BC673, 0x002BC6A1, 0x002BC74B, 0x002BC7E6,
};
static const uint32_t kItemViewCall = 0x002BBC1C;
static const uint32_t kPutInCaseCalls[] = {0x002BBCEF, 0x002BC8BE};
static const uint32_t kAnswer = 0x00817931;  // the message's Yes/No answer
static const uint32_t kFnItemInfo = 0x003057D0;  // cdecl (id, ITEM_INFO*), type at +2 (re4_tweaks bio4::itemInfo)
static const uint32_t kFnSndCall = 0x0056E230;   // cdecl SndCall(blk, no, pos, id, flag, model)

static uint16_t g_pickItem = 0, g_pickNum = 0;  // the item being picked up and its count (+0x7C)

void TickerItem(uint32_t id, uint32_t num, uint8_t type, const char* name);
static const char* ItemName(uint32_t id) {
    return id < sizeof(kItemNames) / sizeof(kItemNames[0]) ? kItemNames[id] : "Item";
}
// One ticker line: "Handgun Ammo x30", "Green Herb", "5000 PTAS".
static void Announce(uint32_t id, uint32_t num, uint8_t type) { TickerItem(id, num, type, ItemName(id)); }

// Read by the asm stubs.
extern "C" {
uintptr_t g_origMesSet = 0, g_origItemView = 0;
volatile uint8_t* g_pickupAnswer = nullptr;
volatile uint8_t g_pickupYes = 0;  // a skipped Yes/No waits for its PutInCase
}

// Called by MesSetStub with the message's flags (arg 4). Returns 1 to show the message.
extern "C" int __cdecl PickupMessage(uint32_t flags) {
    if (!Cfg().quickPickup) return 1;
    uint8_t info[32] = {};
    ((void(__cdecl*)(uint32_t, void*))Va(kFnItemInfo))(g_pickItem, info);
    // Item types (re4_tweaks ITEM_TYPE): 2 ammo, 3 grenade, 5 treasure, 6 consumable (herbs, sprays, eggs, fish),
    // 8 money and the Mercenaries bonuses, 12 gem. Everything else (weapons, key items, files...) keeps its screen.
    const uint8_t t = info[2];
    if (t != 2 && t != 3 && t != 5 && t != 6 && t != 8 && t != 12) return 1;
    if (flags & 0x2700) {  // the Yes/No ones (0x111, 0x211 ammo, 0x411 consumables, 0x2011); the others took it already
        g_pickupYes = 1;
        *g_pickupAnswer = 1;
    } else {
        Announce(g_pickItem, g_pickNum, t);
    }
    return 0;
}

extern "C" __attribute__((naked)) void MesSetStub() {
    asm("pushl %ecx\n\t"
        "pushl 0x14(%esp)\n\t"  // flags: arg 4 (ret + 3 args + the pushed ecx)
        "call _PickupMessage\n\t"
        "addl $4, %esp\n\t"
        "popl %ecx\n\t"
        "testl %eax, %eax\n\t"
        "je 1f\n\t"
        "jmp *_g_origMesSet\n\t"
        "1:\n\t"
        "ret\n\t");
}

extern "C" __attribute__((naked)) void ItemViewStub() {
    asm("cmpb $0, _g_pickupYes\n\t"
        "je 1f\n\t"
        "movl _g_pickupAnswer, %eax\n\t"
        "movb $1, (%eax)\n\t"
        "1:\n\t"
        "jmp *_g_origItemView\n\t");
}

typedef char(__cdecl* PFN_PutInCase)(uint32_t id, uint32_t num, int size);
static PFN_PutInCase g_origPutInCase = nullptr;
static char __cdecl HookPutInCase(uint32_t id, uint32_t num, int size) {
    const char fit = g_origPutInCase(id, num, size);
    if (!g_pickupYes) return fit;
    g_pickupYes = 0;
    *g_pickupAnswer = 0;
    uint8_t info[32] = {};
    ((void(__cdecl*)(uint32_t, void*))Va(kFnItemInfo))(id & 0xFFFF, info);
    if (fit == 1) {
        // re4_tweaks' item-added sounds: weapon / grenade, ammo, anything else
        const uint32_t se = (info[2] == 1 || info[2] == 3) ? 0x0F : info[2] == 2 ? 0x11 : 0x13;
        ((uint32_t(__cdecl*)(uint32_t, uint32_t, void*, uint32_t, uint32_t, void*))Va(kFnSndCall))(0, se, 0, 0, 0, 0);
        Announce(id & 0xFFFF, (num & 0xFFFF) ? (num & 0xFFFF) : info[3], info[2]);
    }
    Log("pickup: item %u x%u (type %u) %s", id & 0xFFFF, num & 0xFFFF, info[2],
        fit == 1 ? "taken" : "doesn't fit, the case opens");
    return fit;
}

typedef uint32_t(__cdecl* PFN_PickupStart)(uintptr_t entry);
static PFN_PickupStart g_origPickupStart = nullptr;
static uint32_t __cdecl HookPickupStart(uintptr_t entry) {
    const bool ok = Readable((void*)(entry + 0x78), 6);
    g_pickItem = ok ? At<uint16_t>(entry, 0x78) : 0;
    g_pickNum = ok ? At<uint16_t>(entry, 0x7C) : 0;
    return g_origPickupStart(entry);
}
static const uint8_t kSigPickupStart[] = {0x55, 0x8B, 0xEC, 0x56, 0x8B, 0x75, 0x08};
bool HookAt(uint32_t rva, const uint8_t* sig, size_t n, void* detour, void** orig, const char* name);

static bool CallsThunk(uint32_t rva, uint32_t thunk) {
    const uint8_t* p = (const uint8_t*)Va(rva);
    return SafeEqual(p, "\xE8", 1) && p + 5 + *(const int32_t*)(p + 1) == (const uint8_t*)Va(thunk);
}
static void Redirect(uint32_t rva, void* to) {
    uint8_t* p = (uint8_t*)Va(rva);
    const int32_t rel = (int32_t)((uint8_t*)to - (p + 5));
    DWORD old;
    VirtualProtect(p + 1, 4, PAGE_EXECUTE_READWRITE, &old);
    memcpy(p + 1, &rel, 4);
    VirtualProtect(p + 1, 4, old, &old);
    FlushInstructionCache(GetCurrentProcess(), p, 5);
}

void InitPickupHooks() {
    bool ok = CallsThunk(kItemViewCall, kThunkItemView);
    for (uint32_t r : kMesCalls) ok = ok && CallsThunk(r, kThunkMesSet);
    for (uint32_t r : kPutInCaseCalls) ok = ok && CallsThunk(r, kThunkPutInCase);
    if (!ok) {
        Log("quick pickup: unexpected code in the pickup routines, the game's pickup screen stays");
        return;
    }
    if (!HookAt(0x002BCDB0, kSigPickupStart, sizeof(kSigPickupStart), (void*)HookPickupStart,
                (void**)&g_origPickupStart, "pickup start")) {
        Log("quick pickup: the game's pickup screen stays");
        return;
    }
    g_pickupAnswer = (volatile uint8_t*)Va(kAnswer);
    g_origMesSet = Va(kThunkMesSet);
    g_origItemView = Va(kThunkItemView);
    g_origPutInCase = (PFN_PutInCase)Va(kThunkPutInCase);
    for (uint32_t r : kMesCalls) Redirect(r, (void*)MesSetStub);
    Redirect(kItemViewCall, (void*)ItemViewStub);
    for (uint32_t r : kPutInCaseCalls) Redirect(r, (void*)HookPutInCase);
    Log("quick pickup: %zu message calls, the item view and %zu case calls redirected", sizeof(kMesCalls) / 4,
        sizeof(kPutInCaseCalls) / 4);
}
