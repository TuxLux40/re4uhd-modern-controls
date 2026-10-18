// Automatic quick-time events (Buttons/AutoQTE). The action prompts are hidden (prompts.cpp), so the QTEs that show
// them must pass on their own.
//
// Ported from re4_tweaks' DisableQTE (dllmain/QTEFixes.cpp, https://github.com/nipkownix/re4_tweaks), altered:
// the same code locations, found by the same byte patterns, patched directly instead of through its hooking library.
//   Copyright (C) 2021 nipkow. This software is provided 'as-is', without any express or implied warranty. In no
//   event will the authors be held liable for any damages arising from the use of this software. Permission is
//   granted to anyone to use this software for any purpose, including commercial applications, and to alter it and
//   redistribute it freely, subject to the following restrictions: 1. The origin of this software must not be
//   misrepresented; you must not claim that you wrote the original software. If you use this software in a product,
//   an acknowledgment in the product documentation would be appreciated but is not required. 2. Altered source
//   versions must be plainly marked as such, and must not be misrepresented as being the original software. 3. This
//   notice may not be removed or altered from any source distribution.
//
// - Reaction QTEs (dodges, Krauser): cActionButton::checkButton reports a hit while a QTE is active.
// - Mashing QTEs (boulders, Del Lago, El Gigante, minecart, statue, Krauser's last): their KeyTrgCheck calls report
//   the key as pressed.
// - Both-trigger checks (statue run, lasers): the compare of the trigger state becomes "pressed".
// - Saddler's bridges: their opening timer is never advanced.
#include "common.h"
#include "config.h"
#include <string.h>
#include "MinHook.h"

// The code section as shipped, read from bio4.exe on disk: re4_tweaks (dinput8.dll) may already have rewritten some
// of these spots in memory, so the patterns are matched against the file and patched at the same place in memory.
static uint8_t* g_text = nullptr;  // file copy of the code section
static uint32_t g_textRva = 0, g_textSize = 0;

static bool LoadText() {
    char path[MAX_PATH];
    GetModuleFileNameA(nullptr, path, MAX_PATH);
    HANDLE f = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    const uintptr_t base = ImageBase();
    const IMAGE_NT_HEADERS* nt = (const IMAGE_NT_HEADERS*)(base + ((const IMAGE_DOS_HEADER*)base)->e_lfanew);
    const IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
    for (int i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++sec) {
        if (!(sec->Characteristics & IMAGE_SCN_CNT_CODE)) continue;
        g_textRva = sec->VirtualAddress, g_textSize = sec->SizeOfRawData;
        g_text = (uint8_t*)VirtualAlloc(nullptr, g_textSize, MEM_COMMIT, PAGE_READWRITE);
        DWORD got = 0;
        SetFilePointer(f, sec->PointerToRawData, nullptr, FILE_BEGIN);
        if (!g_text || !ReadFile(f, g_text, g_textSize, &got, nullptr) || got != g_textSize) g_text = nullptr;
        break;
    }
    CloseHandle(f);
    return g_text != nullptr;
}

// Finds a pattern ("E8 ? ? ? ? 83 C4") in the shipped code; returns the address in memory of the nth match (0-based),
// or 0 if missing.
static uint8_t* Find(const char* pat, int nth = 0) {
    uint8_t bytes[96];
    bool any[96];
    int n = 0;
    for (const char* p = pat; *p && n < 96;) {
        if (*p == ' ') { ++p; continue; }
        if (*p == '?') { any[n] = true, bytes[n++] = 0, ++p; continue; }
        unsigned v = 0;
        for (int k = 0; k < 2; ++k, ++p) v = v * 16 + (*p <= '9' ? *p - '0' : (*p | 0x20) - 'a' + 10);
        any[n] = false, bytes[n++] = (uint8_t)v;
    }
    if (!g_text || g_textSize < (uint32_t)n) return nullptr;
    for (uint32_t q = 0; q + n <= g_textSize; ++q) {
        int i = 0;
        while (i < n && (any[i] || g_text[q + i] == bytes[i])) ++i;
        if (i == n && nth-- == 0) return (uint8_t*)Va(g_textRva + q);
    }
    return nullptr;
}

static void Patch(uint8_t* at, const void* bytes, size_t n) {
    DWORD old;
    VirtualProtect(at, n, PAGE_EXECUTE_READWRITE, &old);
    memcpy(at, bytes, n);
    VirtualProtect(at, n, old, &old);
    FlushInstructionCache(GetCurrentProcess(), at, n);
}

static uint8_t* CallTarget(uint8_t* call) { return call + 5 + *(int32_t*)(call + 1); }
static void RedirectCall(uint8_t* call, void* to) {
    if (*call != 0xE8) return;
    const int32_t rel = (int32_t)((uint8_t*)to - (call + 5));
    Patch(call + 1, &rel, 4);
}

static volatile uint8_t* g_qteActive = nullptr;  // set by checkButton while a reaction QTE runs
static bool On() { return Cfg().autoQte; }

typedef bool(__fastcall* PFN_CheckButton)(void* self, void* edx, void* work);
static PFN_CheckButton g_origCheckButton = nullptr;
static bool __fastcall HookCheckButton(void* self, void* edx, void* work) {
    const bool r = g_origCheckButton(self, edx, work);  // sets the QTE-active flag
    return (On() && g_qteActive && *g_qteActive) ? true : r;
}

typedef unsigned(__cdecl* PFN_KeyTrg)(uint32_t, uint32_t, uint32_t, uint32_t);
static PFN_KeyTrg g_origKeyTrg = nullptr;
static unsigned __cdecl HookKeyTrg(uint32_t a, uint32_t b, uint32_t c, uint32_t d) {
    return On() ? 1 : g_origKeyTrg(a, b, c, d);
}

typedef int(__cdecl* PFN_Both)();
static PFN_Both g_origStatueBoth = nullptr, g_origLaserLast = nullptr;
static int __cdecl HookStatueBoth() { return On() ? 1 : g_origStatueBoth(); }
static int __cdecl HookLaserLast() { return On() ? 1 : g_origLaserLast(); }

void InitQteHooks() {
    if (!Cfg().autoQte) return;  // patched at start only; the ini switch takes effect after a restart
    if (!LoadText()) {
        Log("qte: could not read bio4.exe, QTEs left as they are");
        return;
    }
    int ok = 0, total = 0;
    auto note = [&](bool found) {
        if (!found) Log("qte: code location %d not found", total);
        ++total, ok += found ? 1 : 0;
    };

    uint8_t* p = Find("C6 05 ? ? ? ? ? E8 ? ? ? ? 83 C4 ? 84 C0 0F 84");
    note(p != nullptr);
    if (p) g_qteActive = *(uint8_t**)(p + 2);

    p = Find("E8 ? ? ? ? 3C ? 0F 85 ? ? ? ? 8B 4E");
    note(p != nullptr);
    if (p) {
        void* fn = CallTarget(p);
        if (MH_CreateHook(fn, (void*)HookCheckButton, (void**)&g_origCheckButton) == MH_OK) MH_EnableHook(fn);
    }

    // KeyTrgCheck call sites (mashing): pattern, match index, offset of the call.
    static const struct { const char* pat; int nth, off; } kTrg[] = {
        {"E8 ? ? ? ? 83 C4 10 85 C0 74 4F", 0, 0},                                         // boulders
        {"E8 ? ? ? ? 83 C4 ? 85 C0 74 ? A1 ? ? ? ? 66 83 B8 ? ? ? ? ? 7F ? D9 86", 0, 0},  // Del Lago: swim back
        {"E8 ? ? ? ? 83 C4 10 85 C0 74 06 FF 86 ? ? ? ? 5E", 0, 0},                        // Del Lago: rope
        {"E8 ? ? ? ? 83 C4 10 85 C0 74 06 FF 86 ? ? ? ? 0F B6 96 ? ? ? ?", 0, 0},          // El Gigante 1
        {"E8 ? ? ? ? 83 C4 10 85 C0 74 06 FF 86 ? ? ? ? 57", 0, 0},                        // El Gigante 2
        {"E8 ? ? ? ? 83 C4 ? 85 C0 74 ? 8B 15 ? ? ? ? 8A 82", 0, 0},                       // El Gigante 3
        {"E8 ? ? ? ? 83 C4 10 85 C0 74 24 D9 EE", 0, 0},                                   // minecart climb
        {"E8 ? ? ? ? 83 C4 10 85 C0 74 1D 8B 7D 14", 0, 0},                                // statue run
        {"E8 ? ? ? ? 83 C4 ? 85 C0 74 ? A1 ? ? ? ? ? ? ? ? ? ? 6A ? 6A ? 6A ? 6A ? 6A ? 6A ? 6A ? 6A ? 6A ? B9", 2, 0},
        {"E8 ? ? ? ? 83 C4 ? 85 C0 74 ? A1 ? ? ? ? ? ? ? ? ? ? 6A ? 6A ? 6A ? 6A ? 6A ? 6A ? 6A ? 6A ? 6A ? B9", 3, 0},
        {"83 FF ? 75 ? 68 ? ? ? ? E8 ? ? ? ? 83 C4 ? 85 C0", 0, 10},                       // Krauser's last
    };
    for (const auto& t : kTrg) {
        p = Find(t.pat, t.nth);
        note(p != nullptr);
        if (!p) continue;
        p += t.off;
        if (!g_origKeyTrg) g_origKeyTrg = (PFN_KeyTrg)CallTarget(p);
        RedirectCall(p, (void*)HookKeyTrg);
    }

    // Both QTE keys at once: statue run (two calls), the lasers' last QTE.
    p = Find("E8 ? ? ? ? 85 C0 EB ? 6A ? 6A ? 6A ? 6A ? 6A");
    note(p != nullptr);
    if (p) g_origStatueBoth = (PFN_Both)CallTarget(p), RedirectCall(p, (void*)HookStatueBoth);
    p = Find("E8 ? ? ? ? 85 C0 0F 84 ? ? ? ? 6A ? 6A ? 6A ? 8D 8E");
    note(p != nullptr);
    if (p) RedirectCall(p, (void*)HookStatueBoth);
    p = Find("E8 ? ? ? ? 85 C0 74 ? FE 86 ? ? ? ? 5F 5E");
    note(p != nullptr);
    if (p) g_origLaserLast = (PFN_Both)CallTarget(p), RedirectCall(p, (void*)HookLaserLast);

    // Trigger compares (cmp byte [Key.trigger], imm8: 7 bytes) become add esp,0 + nops: ZF = 0, CF = 0, "pressed".
    static const uint8_t kPressed[7] = {0x83, 0xC4, 0x00, 0x90, 0x90, 0x90, 0x90};
    static const char* const kCmp[] = {
        "80 3D ? ? ? ? ? 0F 84 ? ? ? ? A1 ? ? ? ? 81 A0",                                       // statue A
        "80 3D ? ? ? ? 00 EB ? 6A 0E 6A ? 6A ? 6A ? 6A ? 6A",                                   // statue B
        "80 3D ? ? ? ? ? 74 ? 66 C7 05 ? ? ? ? ? ? A1 ? ? ? ? 81 88 ? ? ? ? ? ? ? ? 68",        // lasers A
        "80 3D ? ? ? ? ? 74 ? 66 C7 05 ? ? ? ? ? ? A1 ? ? ? ? 81 88 ? ? ? ? ? ? ? ? 8B C7",     // lasers A
        "80 3D ? ? ? ? ? 74 ? ? ? ? ? ? ? ? 74 ? 66 C7 05 ? ? ? ? ? ? A1 ? ? ? ? 81 88 ? ? ? ? ? ? ? ? 68",
        "80 3D ? ? ? ? ? 74 ? ? ? ? ? ? ? ? 74 ? 66 C7 05 ? ? ? ? ? ? A1 ? ? ? ? 81 88 ? ? ? ? ? ? ? ? 8B C7",
    };
    for (const char* c : kCmp) {
        p = Find(c);
        note(p != nullptr);
        if (p) Patch(p, kPressed, sizeof(kPressed));
    }

    // Saddler's bridges: "fstp [esi+eax*8+disp32]" (7 bytes) of the opening timer becomes "fstp st(0)": popped, not
    // stored, so the timer stays where it is and the bridges never open.
    p = Find("D9 9C C6 ? ? ? ? E9 ? ? ? ? 68 ? ? ? ? 6A ? E8");
    note(p != nullptr);
    if (p) {
        static const uint8_t kPop[7] = {0xDD, 0xD8, 0x90, 0x90, 0x90, 0x90, 0x90};
        Patch(p, kPop, sizeof(kPop));
    }
    Log("qte: automatic QTEs, %d of %d code locations found", ok, total);
    VirtualFree(g_text, 0, MEM_RELEASE);
    g_text = nullptr;
}
