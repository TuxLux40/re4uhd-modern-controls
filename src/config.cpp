// RE4ModernControls.ini next to the DLL. Written with the defaults if missing, re-read live when it changes.
#include "config.h"
#include <stdio.h>
#include <stdlib.h>

static Config g_cfg;
static FILETIME g_lastWrite = {};
static DWORD g_lastCheck = 0;

const Config& Cfg() { return g_cfg; }

// src/default.ini, embedded by build.sh as build/default_ini.h. Every option is commented out, so the defaults in
// the code apply (and improve with updates) until a player or the installer sets one.
#include "default_ini.h"

static void IniPath(char* out) { wsprintfA(out, "%sRE4ModernControls.ini", ModDir()); }

static float ReadFloat(const char* path, const char* sec, const char* key, float def) {
    char buf[64], defbuf[64];
    sprintf(defbuf, "%g", def);
    GetPrivateProfileStringA(sec, key, defbuf, buf, sizeof(buf), path);
    return (float)atof(buf);
}

static const float kDeg = 0.01745329252f;

void LoadConfig() {
    char path[MAX_PATH];
    IniPath(path);
    if (GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES) {
        HANDLE h = CreateFileA(path, GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h != INVALID_HANDLE_VALUE) {
            DWORD n;
            WriteFile(h, kDefaultIni, sizeof(kDefaultIni) - 1, &n, nullptr);
            CloseHandle(h);
            Log("config: wrote defaults to %s", path);
        }
    }
    WIN32_FILE_ATTRIBUTE_DATA fa;
    if (GetFileAttributesExA(path, GetFileExInfoStandard, &fa)) g_lastWrite = fa.ftLastWriteTime;

    Config c;
    char run[32];
    GetPrivateProfileStringA("Movement", "Run", "Always", run, sizeof(run), path);
    c.run = !lstrcmpiA(run, "Button") ? RUN_BUTTON : !lstrcmpiA(run, "OuterRing") ? RUN_OUTER_RING : RUN_ALWAYS;
    char layout[32];
    GetPrivateProfileStringA("Buttons", "Layout", "RE5", layout, sizeof(layout), path);
    c.buttons = !lstrcmpiA(layout, "Original") ? BUTTONS_ORIGINAL : BUTTONS_RE5;
    c.gameType = (int)ReadFloat(path, "Buttons", "GameType", 1.0f);
    if (c.gameType < 1 || c.gameType > 3) c.gameType = 1;
    c.outerRing = ReadFloat(path, "Movement", "OuterRing", 0.85f);
    c.turnSpeed = ReadFloat(path, "Movement", "TurnSpeed", 515.0f) * kDeg;
    c.aimWalk = ReadFloat(path, "Aim", "Walk", 1.0f) != 0.0f;
    c.aimWalkSpeed = ReadFloat(path, "Aim", "WalkSpeed", 1300.0f);
    c.crosshair = ReadFloat(path, "Aim", "Crosshair", 1.0f) != 0.0f;
    c.centeredAim = ReadFloat(path, "Aim", "CenteredAim", 1.0f) != 0.0f;
    c.centerDot = ReadFloat(path, "Aim", "CenterDot", 1.0f) != 0.0f;
    c.hideActionPrompt = ReadFloat(path, "Buttons", "HideActionPrompt", 1.0f) != 0.0f;
    c.autoQte = ReadFloat(path, "Buttons", "AutoQTE", 1.0f) != 0.0f;
    c.keepViewOnAim = ReadFloat(path, "Aim", "KeepViewOnAim", 1.0f) != 0.0f;
    c.aimShoulderRight = ReadFloat(path, "Aim", "ShoulderRight", 400.0f);
    c.aimShoulderBack = ReadFloat(path, "Aim", "ShoulderBack", 750.0f);
    c.aimShoulderHeight = ReadFloat(path, "Aim", "ShoulderHeight", 1550.0f);
    c.protectAshley = ReadFloat(path, "Aim", "ProtectAshley", 1.0f) != 0.0f;
    c.defaultPitch = ReadFloat(path, "Camera", "DefaultPitch", 12.0f) * kDeg;
    c.height = ReadFloat(path, "Camera", "Height", 0.0f);
    c.shoulder = ReadFloat(path, "Camera", "Shoulder", 300.0f);
    c.distance = ReadFloat(path, "Camera", "Distance", 0.6f);
    if (c.distance < 0.2f) c.distance = 0.2f;
    if (c.distance > 1.0f) c.distance = 1.0f;
    c.yawSpeed = ReadFloat(path, "Camera", "YawSpeed", 170.0f) * kDeg;
    c.pitchSpeed = ReadFloat(path, "Camera", "PitchSpeed", 100.0f) * kDeg;
    c.autoFollow = ReadFloat(path, "Camera", "AutoFollow", 80.0f) * kDeg;
    c.autoFollowDelay = ReadFloat(path, "Camera", "AutoFollowDelay", 1.0f);
    c.invertX = ReadFloat(path, "Camera", "InvertX", 0.0f) != 0.0f;
    c.invertY = ReadFloat(path, "Camera", "InvertY", 0.0f) != 0.0f;
    g_cfg = c;
    Log("config: aim walk %d speed %.0f crosshair %d centered %d protect ashley %d", c.aimWalk, c.aimWalkSpeed,
        c.crosshair, c.centeredAim, c.protectAshley);
    Log("config: run %d outer %.2f turn %.2f rad/s | cam pitch %.3f height %.0f yaw %.2f pitch %.2f rad/s inv %d%d",
        c.run, c.outerRing, c.turnSpeed, c.defaultPitch, c.height, c.yawSpeed, c.pitchSpeed, c.invertX, c.invertY);
    Log("config: auto-follow %.2f rad/s after %.1f s", c.autoFollow, c.autoFollowDelay);
}

void ConfigTick() {
    const DWORD now = GetTickCount();
    if (now - g_lastCheck < 1000) return;
    g_lastCheck = now;
    char path[MAX_PATH];
    IniPath(path);
    WIN32_FILE_ATTRIBUTE_DATA fa;
    if (!GetFileAttributesExA(path, GetFileExInfoStandard, &fa)) return;
    if (CompareFileTime(&fa.ftLastWriteTime, &g_lastWrite) != 0) {
        Log("config: file changed, reloading");
        LoadConfig();
    }
}
