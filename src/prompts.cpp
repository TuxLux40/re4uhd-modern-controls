// The action prompt ("X Open", "X Save", kicks...). With the RE5 layout A does the action, so the prompt's X icon
// misleads; HideActionPrompt removes the prompt:
// - 0x0058A100 (stdcall(entry), "ret 4"), cActionButton's display: kills/sets the ID units (class IDC_ACT_BUTTON 0x20)
//   and puts up the text (message 0x16 + type). Skipped. (The game's DPF_ACTBTN flag only guards one of its callers.)
// - the icon unit tree (anchor mark 0x20 + children) is kept invisible as well, in case something else shows it.
// QTE prompts use the same button class, so this goes with re4_tweaks' DisableQTE.
#include "common.h"
#include "re4.h"
#include "config.h"

typedef void(__stdcall* PFN_ActBtnDisp)(uintptr_t entry);
static PFN_ActBtnDisp g_origActBtnDisp = nullptr;
static void __stdcall HookActBtnDisp(uintptr_t entry) {
    if (Cfg().hideActionPrompt) return;
    g_origActBtnDisp(entry);
}
static const uint8_t kSigActBtnDisp[] = {0x55, 0x8B, 0xEC, 0x51, 0x8B, 0x45, 0x08, 0x0F, 0xB6, 0x48, 0x0F, 0x53};
bool HookAt(uint32_t rva, const uint8_t* sig, size_t n, void* detour, void** orig, const char* name);
void InitPromptHooks() {
    HookAt(0x0018A100, kSigActBtnDisp, sizeof(kSigActBtnDisp), (void*)HookActBtnDisp, (void**)&g_origActBtnDisp,
           "cActionButton display");
}

static const uint32_t kIdSys = 0x00859788;  // IDSystem (re4_tweaks Game.cpp pattern "B9 ? ? ? ? E8 ? ? ? ? 8B ? ? ? ? ? 8B C8 D9")

void PromptsTick() {
    if (!Cfg().hideActionPrompt) return;
    const uintptr_t sys = Va(kIdSys);
    if (!Readable((void*)sys, 0x58)) return;
    const int count = *(int*)sys;
    const uintptr_t units = *(uintptr_t*)(sys + 0x50);
    if (count <= 0 || count > 2048 || units < 0x10000 || !Readable((void*)units, count * 0x144)) return;
    int anchor = -1;
    for (int i = 0; i < count && anchor < 0; ++i) {
        const uint8_t* u = (const uint8_t*)(units + i * 0x144);
        if (u[2] == 0x20 && u[3] == 0x20 && (u[0] & 0x8)) anchor = u[1];
    }
    if (anchor < 0) return;
    for (int i = 0; i < count; ++i) {
        uint8_t* u = (uint8_t*)(units + i * 0x144);
        if (u[2] == 0x20 && (u[1] == anchor || u[6] == anchor)) u[0] &= ~0x8;
    }
}
