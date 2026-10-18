#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>
#include <string.h>

#ifndef RE4MC_VERSION_STR
#define RE4MC_VERSION_STR "dev"  // release builds pass -DRE4MC_VERSION_STR (build.sh, VERSION=...)
#endif

// XInput types (declared here so we don't depend on an SDK xinput.h version).
struct XGamepad {
    WORD wButtons;
    BYTE bLeftTrigger;
    BYTE bRightTrigger;
    SHORT sThumbLX;
    SHORT sThumbLY;
    SHORT sThumbRX;
    SHORT sThumbRY;
};
struct XState {
    DWORD dwPacketNumber;
    XGamepad Gamepad;
};

enum : WORD {
    XB_DPAD_UP = 0x0001, XB_DPAD_DOWN = 0x0002, XB_DPAD_LEFT = 0x0004, XB_DPAD_RIGHT = 0x0008,
    XB_START = 0x0010, XB_BACK = 0x0020, XB_LTHUMB = 0x0040, XB_RTHUMB = 0x0080,
    XB_LB = 0x0100, XB_RB = 0x0200, XB_GUIDE = 0x0400,
    XB_A = 0x1000, XB_B = 0x2000, XB_X = 0x4000, XB_Y = 0x8000,
};

void Log(const char* fmt, ...);
const char* ModDir();

// bio4.exe is built with /DYNAMICBASE, so everything is addressed relative to the image base.
// re4.h stores RVAs; Va() turns one into a pointer in the running process.
uintptr_t ImageBase();
inline uintptr_t Va(uint32_t rva) { return ImageBase() + rva; }

// True if [p, p+n) is committed readable memory.
inline bool Readable(const void* p, size_t n) {
    MEMORY_BASIC_INFORMATION mbi;
    if (!p || !VirtualQuery(p, &mbi, sizeof(mbi))) return false;
    if (mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS))) return false;
    const DWORD readable = PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ |
                           PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
    if (!(mbi.Protect & readable)) return false;
    return (const BYTE*)p + n <= (const BYTE*)mbi.BaseAddress + mbi.RegionSize;
}
inline bool SafeEqual(const void* p, const void* sig, size_t n) {
    return Readable(p, n) && memcmp(p, sig, n) == 0;
}

template <typename T>
inline T& At(uintptr_t base, uint32_t off) { return *(T*)(base + off); }

// xinput_proxy.cpp
void RedirectGameXInputImports();
// input.cpp: rewrites pad 0 before the game sees it.
void TransformPad(DWORD user, XGamepad& pad);
bool DebugInputActive();
bool DebugInputPending();
