// xinput1_3.dll proxy: forwards to the system XInput and lets the mod rewrite pad 0 before the game sees it.
// Adapted from RE3Controls (MIT, (c) 2026 palecvzhope).
#include "common.h"

typedef DWORD(WINAPI* PFN_GetState)(DWORD, XState*);
typedef DWORD(WINAPI* PFN_SetState)(DWORD, void*);
typedef DWORD(WINAPI* PFN_GetCaps)(DWORD, DWORD, void*);
typedef void(WINAPI* PFN_Enable)(BOOL);
typedef DWORD(WINAPI* PFN_GetBattery)(DWORD, BYTE, void*);
typedef DWORD(WINAPI* PFN_GetKeystroke)(DWORD, DWORD, void*);
typedef DWORD(WINAPI* PFN_WaitGuide)(DWORD, DWORD, void*);
typedef DWORD(WINAPI* PFN_DwordOnly)(DWORD);

static struct {
    HMODULE mod;
    PFN_GetState getState, getStateEx;
    PFN_SetState setState;
    PFN_GetCaps getCaps;
    PFN_Enable enable;
    PFN_GetBattery getBattery;
    PFN_GetKeystroke getKeystroke;
    PFN_WaitGuide waitGuide;
    PFN_DwordOnly cancelGuide, powerOff;
} R;

static INIT_ONCE g_once = INIT_ONCE_STATIC_INIT;

static BOOL CALLBACK LoadReal(PINIT_ONCE, PVOID, PVOID*) {
    const char* candidates[] = {"XInput1_4.dll", "XInput9_1_0.dll"};
    char sys[MAX_PATH];
    GetSystemDirectoryA(sys, MAX_PATH);
    for (const char* name : candidates) {
        char path[MAX_PATH];
        wsprintfA(path, "%s\\%s", sys, name);
        R.mod = LoadLibraryA(path);
        if (R.mod) {
            Log("xinput: forwarding to %s", path);
            break;
        }
    }
    if (!R.mod) {
        Log("xinput: no system XInput found");
        return TRUE;
    }
    R.getState = (PFN_GetState)GetProcAddress(R.mod, "XInputGetState");
    R.getStateEx = (PFN_GetState)GetProcAddress(R.mod, (LPCSTR)100);
    R.setState = (PFN_SetState)GetProcAddress(R.mod, "XInputSetState");
    R.getCaps = (PFN_GetCaps)GetProcAddress(R.mod, "XInputGetCapabilities");
    R.enable = (PFN_Enable)GetProcAddress(R.mod, "XInputEnable");
    R.getBattery = (PFN_GetBattery)GetProcAddress(R.mod, "XInputGetBatteryInformation");
    R.getKeystroke = (PFN_GetKeystroke)GetProcAddress(R.mod, "XInputGetKeystroke");
    R.waitGuide = (PFN_WaitGuide)GetProcAddress(R.mod, (LPCSTR)101);
    R.cancelGuide = (PFN_DwordOnly)GetProcAddress(R.mod, (LPCSTR)102);
    R.powerOff = (PFN_DwordOnly)GetProcAddress(R.mod, (LPCSTR)103);
    return TRUE;
}

static void EnsureReal() { InitOnceExecuteOnce(&g_once, LoadReal, nullptr, nullptr); }

static DWORD GetStateCommon(PFN_GetState fn, DWORD user, XState* st) {
    EnsureReal();
    DWORD rc = fn ? fn(user, st) : ERROR_DEVICE_NOT_CONNECTED;
    if (user == 0 && st && rc == ERROR_SUCCESS) TransformPad(user, st->Gamepad);
    return rc;
}

static DWORD WINAPI Game_XInputGetState(DWORD user, XState* st) { return GetStateCommon(R.getState, user, st); }

static DWORD WINAPI Game_XInputGetCapabilities(DWORD user, DWORD flags, void* caps) {
    EnsureReal();
    return R.getCaps ? R.getCaps(user, flags, caps) : ERROR_DEVICE_NOT_CONNECTED;
}

// The Steam overlay detours the exported XInputGetState of whatever xinput1_3.dll is loaded (Steam Input), so the
// game's import slots are pointed at these internal entry points instead. They call the system XInput (which Steam
// may hook too, so Steam Input keeps working) and then apply the mod's controls layer. bio4.exe has no SteamStub,
// so its import table is final when we run; the slots are found by walking it rather than by fixed address.
void RedirectGameXInputImports() {
    const BYTE* base = (const BYTE*)ImageBase();
    const IMAGE_NT_HEADERS* nt = (const IMAGE_NT_HEADERS*)(base + ((const IMAGE_DOS_HEADER*)base)->e_lfanew);
    const IMAGE_DATA_DIRECTORY& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress) return;
    for (auto* imp = (const IMAGE_IMPORT_DESCRIPTOR*)(base + dir.VirtualAddress); imp->Name; ++imp) {
        if (lstrcmpiA((const char*)(base + imp->Name), "XINPUT1_3.dll") != 0) continue;
        auto* names = (const IMAGE_THUNK_DATA*)(base + (imp->OriginalFirstThunk ? imp->OriginalFirstThunk : imp->FirstThunk));
        auto* slots = (IMAGE_THUNK_DATA*)(base + imp->FirstThunk);
        for (; names->u1.AddressOfData; ++names, ++slots) {
            if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)) continue;
            const char* fn = (const char*)((const IMAGE_IMPORT_BY_NAME*)(base + names->u1.AddressOfData))->Name;
            void* target = nullptr;
            if (!lstrcmpA(fn, "XInputGetState")) target = (void*)Game_XInputGetState;
            else if (!lstrcmpA(fn, "XInputGetCapabilities")) target = (void*)Game_XInputGetCapabilities;
            if (!target) continue;
            DWORD old;
            VirtualProtect(&slots->u1.Function, 4, PAGE_READWRITE, &old);
            void* prev = (void*)slots->u1.Function;
            slots->u1.Function = (DWORD)(uintptr_t)target;
            VirtualProtect(&slots->u1.Function, 4, old, &old);
            Log("xinput: import %s %p -> %p", fn, prev, target);
        }
    }
}

extern "C" {

DWORD WINAPI Proxy_XInputGetState(DWORD user, XState* st) { return GetStateCommon(R.getState, user, st); }

DWORD WINAPI Proxy_XInputGetStateEx(DWORD user, XState* st) {
    EnsureReal();
    return GetStateCommon(R.getStateEx ? R.getStateEx : R.getState, user, st);
}

DWORD WINAPI Proxy_XInputSetState(DWORD user, void* vib) {
    EnsureReal();
    return R.setState ? R.setState(user, vib) : ERROR_DEVICE_NOT_CONNECTED;
}

DWORD WINAPI Proxy_XInputGetCapabilities(DWORD user, DWORD flags, void* caps) {
    return Game_XInputGetCapabilities(user, flags, caps);
}

void WINAPI Proxy_XInputEnable(BOOL enable) {
    EnsureReal();
    if (R.enable) R.enable(enable);
}

DWORD WINAPI Proxy_XInputGetDSoundAudioDeviceGuids(DWORD, GUID* render, GUID* capture) {
    if (render) ZeroMemory(render, sizeof(GUID));
    if (capture) ZeroMemory(capture, sizeof(GUID));
    return ERROR_SUCCESS;
}

DWORD WINAPI Proxy_XInputGetBatteryInformation(DWORD user, BYTE type, void* info) {
    EnsureReal();
    return R.getBattery ? R.getBattery(user, type, info) : ERROR_DEVICE_NOT_CONNECTED;
}

DWORD WINAPI Proxy_XInputGetKeystroke(DWORD user, DWORD reserved, void* ks) {
    EnsureReal();
    return R.getKeystroke ? R.getKeystroke(user, reserved, ks) : ERROR_EMPTY;
}

DWORD WINAPI Proxy_XInputWaitForGuideButton(DWORD user, DWORD flags, void* listen) {
    EnsureReal();
    return R.waitGuide ? R.waitGuide(user, flags, listen) : ERROR_DEVICE_NOT_CONNECTED;
}

DWORD WINAPI Proxy_XInputCancelGuideButtonWait(DWORD user) {
    EnsureReal();
    return R.cancelGuide ? R.cancelGuide(user) : ERROR_DEVICE_NOT_CONNECTED;
}

DWORD WINAPI Proxy_XInputPowerOffController(DWORD user) {
    EnsureReal();
    return R.powerOff ? R.powerOff(user) : ERROR_DEVICE_NOT_CONNECTED;
}

}  // extern "C"
