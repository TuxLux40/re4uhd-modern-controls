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

// Crash report: names the module and offset of a fault, plus return addresses found on the stack, so a crash can be
// pinned on the game, this mod or another mod (Proton doesn't keep a crash log in Game Mode). First-chance exceptions
// the game handles itself also pass here, so only the first few are logged.
static bool DescribeAddress(uintptr_t a, char* out, size_t n) {
    HMODULE m = nullptr;
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            (LPCSTR)a, &m) || !m)
        return false;
    char path[MAX_PATH];
    GetModuleFileNameA(m, path, MAX_PATH);
    const char* name = strrchr(path, '\\');
    _snprintf(out, n, "%s+%lX", name ? name + 1 : path, (unsigned long)(a - (uintptr_t)m));
    out[n - 1] = 0;
    return true;
}

static LONG CALLBACK CrashLogger(EXCEPTION_POINTERS* ep) {
    static volatile LONG count = 0;
    const DWORD code = ep->ExceptionRecord->ExceptionCode;
    if (code != EXCEPTION_ACCESS_VIOLATION && code != EXCEPTION_ILLEGAL_INSTRUCTION &&
        code != EXCEPTION_INT_DIVIDE_BY_ZERO && code != EXCEPTION_STACK_OVERFLOW && code != EXCEPTION_PRIV_INSTRUCTION)
        return EXCEPTION_CONTINUE_SEARCH;
    if (InterlockedIncrement(&count) > 6) return EXCEPTION_CONTINUE_SEARCH;
    const CONTEXT* c = ep->ContextRecord;
    char where[MAX_PATH + 32] = "?";
    DescribeAddress(c->Eip, where, sizeof(where));
    const ULONG_PTR* info = ep->ExceptionRecord->ExceptionInformation;
    Log("EXCEPTION %08lX at %08lX (%s), %s %08lX | eax %08lX ebx %08lX ecx %08lX edx %08lX esi %08lX edi %08lX "
        "ebp %08lX esp %08lX thread %lu",
        code, c->Eip, where, info[0] ? "writing" : "reading", (unsigned long)info[1], c->Eax, c->Ebx, c->Ecx, c->Edx,
        c->Esi, c->Edi, c->Ebp, c->Esp, GetCurrentThreadId());
    const uintptr_t* sp = (const uintptr_t*)c->Esp;
    int found = 0;
    for (int i = 0; i < 256 && found < 12; ++i) {
        if (!Readable((void*)(sp + i), sizeof(uintptr_t))) break;
        const uintptr_t v = sp[i];
        char m[MAX_PATH + 32];
        if (v > 0x10000 && DescribeAddress(v, m, sizeof(m))) {
            Log("  stack+%03X %08lX %s", i * 4, (unsigned long)v, m);
            ++found;
        }
    }
    return EXCEPTION_CONTINUE_SEARCH;
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
        AddVectoredExceptionHandler(0, CrashLogger);
        HANDLE t = CreateThread(nullptr, 0, InitThread, nullptr, 0, nullptr);
        if (t) CloseHandle(t);
    }
    return TRUE;
}
