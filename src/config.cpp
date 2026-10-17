// RE4ModernControls.ini next to the DLL. Written with the defaults if missing, re-read live when it changes.
#include "config.h"
#include <stdio.h>
#include <stdlib.h>

static Config g_cfg;
static FILETIME g_lastWrite = {};
static DWORD g_lastCheck = 0;

const Config& Cfg() { return g_cfg; }

static const char kDefaultIni[] =
    "; RE4 Modern Controls. Changes apply while the game runs (saved file is re-read within a second).\r\n"
    "\r\n"
    "[Movement]\r\n"
    "; Always   = Leon runs whenever the stick is pushed; hold the game's run button to walk\r\n"
    "; OuterRing = run when the stick is pushed past OuterRing, walk below it\r\n"
    "; Button   = the game's own run button (vanilla)\r\n"
    "Run=Always\r\n"
    "OuterRing=0.85\r\n"
    "; Degrees per second Leon turns toward the stick direction\r\n"
    "TurnSpeed=515\r\n"
    "\r\n"
    "[Camera]\r\n"
    "; Where the camera rests vertically, in degrees relative to the game's camera (+ = higher, looking down)\r\n"
    "DefaultPitch=12\r\n"
    "; Raises the whole camera (game units; Leon's shoulders are about 1500 above his feet)\r\n"
    "Height=250\r\n"
    "; Degrees per second at full right-stick deflection\r\n"
    "YawSpeed=170\r\n"
    "PitchSpeed=100\r\n"
    "InvertX=0\r\n"
    "InvertY=0\r\n";

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
    c.outerRing = ReadFloat(path, "Movement", "OuterRing", 0.85f);
    c.turnSpeed = ReadFloat(path, "Movement", "TurnSpeed", 515.0f) * kDeg;
    c.defaultPitch = ReadFloat(path, "Camera", "DefaultPitch", 12.0f) * kDeg;
    c.height = ReadFloat(path, "Camera", "Height", 250.0f);
    c.yawSpeed = ReadFloat(path, "Camera", "YawSpeed", 170.0f) * kDeg;
    c.pitchSpeed = ReadFloat(path, "Camera", "PitchSpeed", 100.0f) * kDeg;
    c.invertX = ReadFloat(path, "Camera", "InvertX", 0.0f) != 0.0f;
    c.invertY = ReadFloat(path, "Camera", "InvertY", 0.0f) != 0.0f;
    g_cfg = c;
    Log("config: run %d outer %.2f turn %.2f rad/s | cam pitch %.3f height %.0f yaw %.2f pitch %.2f rad/s inv %d%d",
        c.run, c.outerRing, c.turnSpeed, c.defaultPitch, c.height, c.yawSpeed, c.pitchSpeed, c.invertX, c.invertY);
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
