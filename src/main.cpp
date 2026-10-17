// Loader: log file, image base, and the init thread that installs the hooks once the game runs.
#include "common.h"
#include <stdarg.h>
#include <stdio.h>
#include <share.h>
#include "MinHook.h"

static CRITICAL_SECTION g_logLock;
static FILE* g_log = nullptr;
static char g_modDir[MAX_PATH];
static uintptr_t g_imageBase = 0;

const char* ModDir() { return g_modDir; }
uintptr_t ImageBase() { return g_imageBase; }

void Log(const char* fmt, ...) {
    if (!g_log) return;
    EnterCriticalSection(&g_logLock);
    SYSTEMTIME st;
    GetLocalTime(&st);
    fprintf(g_log, "[%02d:%02d:%02d.%03d] ", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(g_log, fmt, ap);
    va_end(ap);
    fputc('\n', g_log);
    fflush(g_log);
    LeaveCriticalSection(&g_logLock);
}

static void OpenLog(HINSTANCE inst) {
    GetModuleFileNameA(inst, g_modDir, MAX_PATH);
    char* slash = strrchr(g_modDir, '\\');
    if (slash) slash[1] = 0;
    char path[MAX_PATH];
    wsprintfA(path, "%sRE4ModernControls.log", g_modDir);
    g_log = _fsopen(path, "w", _SH_DENYWR);
}

// The PE header's TimeDateStamp and SizeOfImage identify the exe build in the log.
static void LogExeIdentity() {
    const BYTE* base = (const BYTE*)g_imageBase;
    const IMAGE_NT_HEADERS* nt = (const IMAGE_NT_HEADERS*)(base + ((const IMAGE_DOS_HEADER*)base)->e_lfanew);
    Log("bio4.exe at %p, timestamp %08lX, image size %08lX", base, nt->FileHeader.TimeDateStamp,
        nt->OptionalHeader.SizeOfImage);
}

void InitGameHooks();
void InitCrosshairHooks();

static DWORD WINAPI InitThread(LPVOID) {
    if (MH_Initialize() != MH_OK) Log("MinHook init failed");
    InitCrosshairHooks();  // first: it has to catch the game creating its D3D9 device
    RedirectGameXInputImports();
    InitGameHooks();
    return 0;
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(inst);
        InitializeCriticalSection(&g_logLock);
        OpenLog(inst);
        g_imageBase = (uintptr_t)GetModuleHandleA(nullptr);
        Log("RE4 Modern Controls v" RE4MC_VERSION_STR " loading (pid %lu)", GetCurrentProcessId());
        LogExeIdentity();
        HANDLE t = CreateThread(nullptr, 0, InitThread, nullptr, 0, nullptr);
        if (t) CloseHandle(t);
    }
    return TRUE;
}
