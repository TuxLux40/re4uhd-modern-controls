// Test kit: read the running game's state from a command file, for testing the mod without a debugger (on Linux,
// Proton's game process can't be attached to or read from outside). The scripted pad (RE4MC_input.txt, game.cpp)
// does the input side.
//
// The channel follows boshjerns' RE4 SDK test kit (src/testkit.cpp, docs/sdk/testkit.md,
// https://github.com/boshjerns/re4-boshjerns-sdk, MIT, Copyright (c) 2026 boshjerns): write the commands to
// Bin32/RE4MC_cmd.txt under another name first and rename it into place; once per frame (after the game's input
// update, in every state: gameplay, the case, menus, title, loading) the mod renames it to RE4MC_cmd.done, runs the
// commands and writes Bin32/RE4MC_cmd-out.txt, ending with "done <n> ok <n> errors". Every line also goes to the log.
// Commands, one per line ('#' starts a comment):
//   status                 Leon (routine bytes, position, facing, HP), Ashley, the camera mode, eye, target and fov,
//                          the mod's camera (camera.cpp CameraStatus) and the held key bits
//   peek <va hex> [bytes]  hex dump (default 64, at most 1024); addresses as Ghidra shows them (image base 0x400000)
//   f32 <va hex> [count]   floats (default 4, at most 64)
//   u32 <va hex> [count]   32-bit words in hex (default 4, at most 64)
//   ptr <va hex> <off hex>... follows pointers: reads the u32 at va, adds the next offset, reads again ...; prints each
//   log <text>             a marker line in the log
#include "common.h"
#include "re4.h"
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

using namespace re4;

void CameraStatus(char* out, size_t n);  // camera.cpp

namespace {
FILE* g_out = nullptr;
int g_ok = 0, g_err = 0;

void Out(bool ok, const char* cmd, const char* fmt, ...) {
    char buf[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    ok ? ++g_ok : ++g_err;
    if (g_out) fprintf(g_out, "%s %s: %s\n", ok ? "ok " : "ERR", cmd, buf);
    Log("testkit: %s %s: %s", ok ? "ok" : "ERR", cmd, buf);
}

// A game address (as Ghidra shows it) -> this process, or 0 if unreadable.
uintptr_t Addr(uint32_t va, size_t n) {
    const uintptr_t p = Va(va - 0x400000);
    return Readable((void*)p, n) ? p : 0;
}

void Status(const char* cmd) {
    char b[1600];
    int k = 0;
    const uintptr_t pl = *(uintptr_t*)Va(kPlayerPtr);
    if (pl >= 0x10000 && Readable((void*)pl, 0x500)) {
        const float* p = (const float*)(pl + pl::Pos);
        k += snprintf(b + k, sizeof b - k, "leon rno %02X%02X%02X%02X pos %.0f %.0f %.0f yaw %.3f | ", At<uint8_t>(pl, pl::RNo0),
                      At<uint8_t>(pl, pl::RNo0 + 1), At<uint8_t>(pl, pl::RNo0 + 2), At<uint8_t>(pl, pl::RNo0 + 3), p[0], p[1],
                      p[2], At<float>(pl, pl::Ang + 4));
    } else {
        k += snprintf(b + k, sizeof b - k, "no player | ");
    }
    const uintptr_t as = *(uintptr_t*)Va(kAshleyPtr);
    if (as >= 0x10000 && Readable((void*)as, 0x200)) {
        const float* p = (const float*)(as + pl::Pos);
        k += snprintf(b + k, sizeof b - k, "ashley pos %.0f %.0f %.0f | ", p[0], p[1], p[2]);
    }
    const uintptr_t cam = Va(kCamCtrl);
    const float* e = (const float*)(cam + cc::Point);
    k += snprintf(b + k, sizeof b - k, "cam mode %u eye %.0f %.0f %.0f tgt %.0f %.0f %.0f fov %.1f | ", At<uint8_t>(cam, cc::Mode),
                  e[0], e[1], e[2], e[3], e[4], e[5], e[7]);
    char c[400];
    CameraStatus(c, sizeof c);
    k += snprintf(b + k, sizeof b - k, "%s | keys %llX", c, (unsigned long long)*(uint64_t*)Va(kKeyBtnOn));
    Out(true, cmd, "%s", b);
}

void Run(char* line) {
    char* nl = strpbrk(line, "#\r\n");
    if (nl) *nl = 0;
    char* argv[12];
    int argc = 0;
    for (char* t = strtok(line, " \t"); t && argc < 12; t = strtok(nullptr, " \t")) argv[argc++] = t;
    if (!argc) return;
    const char* cmd = argv[0];
    if (!strcmp(cmd, "status")) {
        Status(cmd);
    } else if (!strcmp(cmd, "log")) {
        Out(true, cmd, "%s", argc > 1 ? argv[1] : "");
    } else if (!strcmp(cmd, "peek") && argc >= 2) {
        const uint32_t va = strtoul(argv[1], nullptr, 16);
        size_t n = argc > 2 ? strtoul(argv[2], nullptr, 0) : 64;
        if (n > 1024) n = 1024;
        const uintptr_t p = Addr(va, n);
        if (!p) return Out(false, cmd, "%08X: not readable", va);
        char b[3200];
        int k = 0;
        for (size_t i = 0; i < n; ++i) k += snprintf(b + k, sizeof b - k, "%s%02X", i ? (i % 16 ? " " : " | ") : "", ((uint8_t*)p)[i]);
        Out(true, cmd, "%08X: %s", va, b);
    } else if ((!strcmp(cmd, "f32") || !strcmp(cmd, "u32")) && argc >= 2) {
        const uint32_t va = strtoul(argv[1], nullptr, 16);
        size_t n = argc > 2 ? strtoul(argv[2], nullptr, 0) : 4;
        if (n > 64) n = 64;
        const uintptr_t p = Addr(va, n * 4);
        if (!p) return Out(false, cmd, "%08X: not readable", va);
        char b[1600];
        int k = 0;
        for (size_t i = 0; i < n; ++i)
            k += cmd[0] == 'f' ? snprintf(b + k, sizeof b - k, "%s%g", i ? " " : "", ((float*)p)[i])
                               : snprintf(b + k, sizeof b - k, "%s%08X", i ? " " : "", ((uint32_t*)p)[i]);
        Out(true, cmd, "%08X: %s", va, b);
    } else if (!strcmp(cmd, "ptr") && argc >= 2) {
        uint32_t a = strtoul(argv[1], nullptr, 16);
        char b[600];
        int k = snprintf(b, sizeof b, "%08X", a);
        for (int i = 2; i <= argc; ++i) {
            const uintptr_t p = Addr(a, 4);
            if (!p) return Out(false, cmd, "%s: not readable", b);
            a = *(uint32_t*)p + (i < argc ? strtoul(argv[i], nullptr, 16) : 0);
            k += snprintf(b + k, sizeof b - k, " -> %08X", a);
        }
        Out(true, cmd, "%s", b);
    } else {
        Out(false, cmd, "unknown command or missing argument");
    }
}
}  // namespace

// Once per frame from the game's input update (camera.cpp HookKeyConvert); looks for the file at most 5 times a second.
void TestkitTick() {
    static DWORD last = 0;
    const DWORD now = GetTickCount();
    if (now - last < 200) return;
    last = now;
    char path[MAX_PATH], done[MAX_PATH], out[MAX_PATH];
    wsprintfA(path, "%sRE4MC_cmd.txt", ModDir());
    if (GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES) return;
    wsprintfA(done, "%sRE4MC_cmd.done", ModDir());
    wsprintfA(out, "%sRE4MC_cmd-out.txt", ModDir());
    DeleteFileA(done);
    if (!MoveFileA(path, done)) return;
    FILE* f = fopen(done, "r");
    if (!f) return;
    g_out = fopen(out, "w");
    g_ok = g_err = 0;
    char line[512];
    while (fgets(line, sizeof line, f)) Run(line);
    fclose(f);
    if (g_out) {
        fprintf(g_out, "done %d ok %d errors\n", g_ok, g_err);
        fclose(g_out);
        g_out = nullptr;
    }
}
