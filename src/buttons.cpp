// RE5-style button layout (Buttons/Layout=RE5), applied to the pad before the game sees it, during gameplay only:
//   LT aim, RT fire (also without aiming: Leon raises the gun, shoots at the screen's centre and lowers it again
//   shortly after the last shot), LB knife, RB Ashley, A action, X reload (also without aiming: a short aim, reload,
//   lower), B held = walk (the game's run button; runs by default), Y inventory, Back map, Start options.
// Those short aims keep the camera where it is (QuickAim); the aim button zooms in.
// The game's own controller types (Buttons/GameType, numbered as in the game's Controller Setup screen; Type III read
// off that screen, I and II from StrategyWiki, whose numbering differs):
//              aim  fire      reload  knife  Ashley  action  run  map  status     options
//   Type I     RT   RT+X      RT+A    LT     RB      X       A    Y    Start      Back
//   Type II    RB   RB+X      RB+A    LB     RT      X       A    Y    Start      Back
//   Type III   LT   LT+RT     LT+A    LB     RB      X       A    Y    Start      Back
// Each button keeps the meaning it had when it went down, so a press that opens a menu (where the game's layout
// applies) doesn't turn into another button there. The sticks and the d-pad pass through.
#include <stdio.h>
#include <string.h>
#include "common.h"
#include "config.h"
#include "camera.h"
#include "re4.h"

bool GameplayView();
bool CameraControlRunning();

// Leon's aim routine (r_no_0 0, r_no_1 6 gun / 0xB knife) while the world runs: the sniper's scope and the shooting
// gallery use another camera than the shoulder one (GameplayView is off there), but the RE5 layout has to stay.
// Returns r_no_2 of the gun stance (0 raising, 1 ready, 2 firing, 4 reloading...), 0 for the knife, -1 when not aiming.
int AimStance() {
    const uintptr_t pl = *(uintptr_t*)Va(re4::kPlayerPtr);
    if (pl < 0x10000 || !Readable((void*)(pl + re4::pl::RNo0), 4) || !CameraControlRunning()) return -1;
    const uint8_t r0 = *(uint8_t*)(pl + re4::pl::RNo0), r1 = *(uint8_t*)(pl + re4::pl::RNo0 + 1);
    if (r0 != 0) return -1;
    if (r1 == 0xB) return 0;
    return r1 == 6 ? *(uint8_t*)(pl + re4::pl::RNo0 + 2) : -1;
}

// The weapon in Leon's hands can reload now (not full, ammo left): cPlayer+0x7D8 (cPlWep) -> +0x34 cObjWep, whose
// virtual reloadable() (vtable +0x4C) is what the game's own reload check calls (0x0067C9A0 path). Unknown (no weapon
// object) counts as yes.
static bool CanReload() {
    const uintptr_t pl = *(uintptr_t*)Va(re4::kPlayerPtr);
    if (pl < 0x10000 || !Readable((void*)(pl + 0x7D8), 4)) return true;
    const uintptr_t pw = *(uintptr_t*)(pl + 0x7D8);
    if (pw < 0x10000 || !Readable((void*)(pw + 0x34), 4)) return true;
    const uintptr_t wep = *(uintptr_t*)(pw + 0x34);
    if (wep < 0x10000 || !Readable((void*)wep, 4)) return true;
    void** vt = *(void***)wep;
    if (!Readable(vt + 19, 4)) return true;
    typedef bool(__thiscall* PFN_Reloadable)(uintptr_t);
    const bool ok = ((PFN_Reloadable)vt[19])(wep);
    return ok;
}

// Shots without the aim button: the gun needs to come up before the first shot counts (Leon's aim stance reports
// ready, r_no_2 1; kRaiseMax in case it never does; a quick tap still fires once it is up), and stays up kLinger after the trigger is let go, so the next shot is immediate.
static const DWORD kRaiseMax = 500, kLinger = 600, kTap = 80;
static volatile bool g_quickAim = false, g_quickReload = false;
bool QuickAim() { return g_quickAim; }
bool QuickReload() { return g_quickReload; }  // a reload with X alone: the free camera stays

// cPlayer::keyReload (0x0067C9A0, cdecl bool()): the game's reload check while aiming; it clears PlReloadDirect
// (0x00C666DD). With the flag set, cPlayer::isKamae counts Leon as not aiming while he reloads, so the game skips its
// aim camera (re4_tweaks' ReloadWithoutZoom does the same). Set for reloads started with X alone.
typedef bool(__cdecl* PFN_KeyReload)();
static PFN_KeyReload g_origKeyReload = nullptr;
static bool __cdecl HookKeyReload() {
    const bool r = g_origKeyReload();
    if (r && g_quickReload) *(volatile uint8_t*)Va(0x008666DD) = 1;
    return r;
}
static const uint8_t kSigKeyReload[] = {0xA1, 0x08, 0x2F, 0xC6, 0x00, 0x33, 0xC9, 0x83, 0xE0, 0x10};
bool HookAt(uint32_t rva, const uint8_t* sig, size_t n, void* detour, void** orig, const char* name);
void InitButtonHooks() {
    HookAt(0x0027C9A0, kSigKeyReload, sizeof(kSigKeyReload), (void*)HookKeyReload, (void**)&g_origKeyReload,
           "cPlayer::keyReload");
}

static const WORD kRemapped = XB_START | XB_BACK | XB_LB | XB_RB | XB_A | XB_B | XB_X | XB_Y;
static const BYTE kTrigger = 40;

enum Act { AIM, FIRE, RELOAD, KNIFE, ASHLEY, ACTION, RUN, MAP, INVENTORY, OPTIONS, ACT_COUNT };

// The pad button the game expects for each action. The game's bindings live in BIO4\input.ini, one section per
// controller type (###XINPUT_CONTROLLER_A/B/C = Type I/II/III), lines like "KEY_CK = QKEY_GC_B11". The on-screen prompts
// follow that file too, so editing it (action on A) changes both; reading it keeps the mod in step with any edit.
// Button numbers: B5 RT, B6 RB, B7 LB, B8 LT, B9 A, B10 B, B11 X, B12 Y, B13 Start, B14 Back.
struct Bind { WORD btn; BYTE lt, rt; };
static Bind g_bind[ACT_COUNT];
static const char* const kActKey[ACT_COUNT] = {"KEY_KAMAE", "KEY_FIRE",   "KEY_RUN", "KEY_KAMAE_KNIFE", "KEY_ASHLEY",
                                               "KEY_CK",    "KEY_RUN",    "KEY_MAP", "KEY_ST",          "KEY_OPTION"};

static bool GcBind(int n, Bind& b) {
    static const WORD kBtn[] = {0, 0, XB_RB, XB_LB, 0, XB_A, XB_B, XB_X, XB_Y, XB_START, XB_BACK};  // B4..B14
    b = Bind{};
    if (n == 5) b.rt = 255;
    else if (n == 8) b.lt = 255;
    else if (n >= 6 && n <= 14 && kBtn[n - 4]) b.btn = kBtn[n - 4];
    else return false;
    return true;
}

static void LoadBindings(int type) {
    // Defaults: the game's shipped layouts (Type III read off its Controller Setup screen, I and II from StrategyWiki).
    static const int kDefault[3][ACT_COUNT] = {
        {5, 11, 9, 8, 6, 11, 9, 12, 13, 14},   // Type I: aim RT, fire X, run/reload A, knife LT
        {6, 11, 9, 7, 5, 11, 9, 12, 13, 14},   // Type II: aim RB, fire X, knife LB, Ashley RT
        {8, 5, 9, 7, 6, 11, 9, 12, 13, 14},    // Type III: aim LT, fire RT, knife LB, Ashley RB
    };
    const int t = type < 1 || type > 3 ? 0 : type - 1;
    for (int a = 0; a < ACT_COUNT; ++a) GcBind(kDefault[t][a], g_bind[a]);
    char path[MAX_PATH], section[32];
    wsprintfA(path, "%s..\\BIO4\\input.ini", ModDir());
    wsprintfA(section, "###XINPUT_CONTROLLER_%c", 'A' + t);
    FILE* f = fopen(path, "r");
    if (!f) {
        Log("buttons: %s not readable, using the shipped Type %d layout", path, t + 1);
        return;
    }
    bool in = false, seen[ACT_COUNT] = {};
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        char key[64];
        int n;
        if (line[0] == '#') {
            in = strncmp(line, section, strlen(section)) == 0;
            continue;
        }
        if (!in || sscanf(line, " %63[A-Z0-9_] = QKEY_GC_B%d", key, &n) != 2) continue;
        for (int a = 0; a < ACT_COUNT; ++a) {
            Bind b;
            if (!seen[a] && strcmp(key, kActKey[a]) == 0 && GcBind(n, b)) g_bind[a] = b, seen[a] = true;  // first wins
        }
    }
    fclose(f);
    Log("buttons: Type %d bindings from input.ini: action %04x, run %04x, aim %04x/%d/%d, fire %04x/%d/%d", t + 1,
        g_bind[ACTION].btn, g_bind[RUN].btn, g_bind[AIM].btn, g_bind[AIM].lt, g_bind[AIM].rt, g_bind[FIRE].btn,
        g_bind[FIRE].lt, g_bind[FIRE].rt);
}

struct Out {
    WORD buttons;
    BYTE lt, rt;
    void Do(Act a) {
        const Bind& b = g_bind[a];
        buttons |= b.btn;
        if (b.lt) lt = b.lt;
        if (b.rt) rt = b.rt;
    }
};

void RemapButtons(XGamepad& p) {
    g_quickAim = g_quickReload = false;
    if (Cfg().buttons != BUTTONS_RE5) return;
    static int loadedType = -1;
    if (loadedType != Cfg().gameType) {
        loadedType = Cfg().gameType;
        LoadBindings(loadedType);
    }
    static WORD prev = 0, latched = 0;
    static bool prevLt = false, prevRt = false, ltLatched = false, rtLatched = false;
    static DWORD reloadStart = 0;  // the reload without aiming in progress (0 = none)
    static DWORD hipStart = 0, hipLast = 0, hipReady = 0;  // the shooting without aiming: start, trigger last held, gun up
    const bool game = GameplayView() || AimStance() >= 0;
    const WORD in = p.wButtons;
    const bool lt = p.bLeftTrigger > kTrigger, rt = p.bRightTrigger > kTrigger;
    // Latch presses that start in gameplay; a latch ends when the button is released.
    if (game) latched |= in & ~prev & kRemapped;
    latched &= in;
    // The aim latches while held, not only on the press: an aim pressed during a kick or a hit (no gameplay camera)
    // and held into gameplay otherwise counted as no aim, so every shot was a hip-fire shot with its camera pull-back.
    if (game && lt) ltLatched = true;
    if (!lt) ltLatched = false;
    if (game && rt && !prevRt) rtLatched = true;
    if (!rt) rtLatched = false;
    const WORD pressed = in & ~prev;
    prev = in, prevLt = lt, prevRt = rt;
    if (!latched && !ltLatched && !rtLatched && !reloadStart && !hipStart) return;

    const bool aim = ltLatched, knife = (latched & XB_LB) != 0;
    Out o = {(WORD)(in & ~latched), ltLatched ? (BYTE)0 : p.bLeftTrigger, rtLatched ? (BYTE)0 : p.bRightTrigger};
    if (aim) o.Do(AIM);
    if (knife) o.Do(KNIFE);
    if (rtLatched && (aim || knife)) o.Do(FIRE);
    // Shooting without aiming: hold the aim while firing and a little after.
    const DWORD now = GetTickCount();
    if (aim || knife || !game) {
        hipStart = 0;  // the aim button takes over
    } else {
        if (rtLatched && !hipStart) hipStart = now;
        if (rtLatched) hipLast = now;
        if (hipStart && !rtLatched && now - hipLast > kLinger) hipStart = 0;
    }
    if (hipStart) {
        o.Do(AIM);
        if (!hipReady && (AimStance() >= 1 || now - hipStart >= kRaiseMax)) hipReady = now;
        if (hipReady && (rtLatched || now - hipReady < kTap)) o.Do(FIRE);
    }
    if (!hipStart) hipReady = 0;
    if (latched & XB_RB) o.Do(ASHLEY);
    if ((latched & XB_A) && !aim && !knife) o.Do(ACTION);
    if ((latched & XB_B) && !aim) o.Do(RUN);
    if (latched & XB_X) {
        if (aim) o.Do(RELOAD);
        else if ((pressed & XB_X) && !knife && !reloadStart && CanReload()) reloadStart = GetTickCount();
    }
    if (latched & XB_Y) o.Do(INVENTORY);
    if (latched & XB_BACK) o.Do(MAP);
    if (latched & XB_START) o.Do(OPTIONS);
    // Reload without aiming: hold the aim, press reload once the gun is up, keep the aim while Leon reloads.
    if (reloadStart) {
        const DWORD t = GetTickCount() - reloadStart;
        if (aim || knife || t > 1700 || !game) {
            reloadStart = 0;  // aiming takes over, or done
        } else {
            o.Do(AIM);
            if (t >= 250 && t < 400) o.Do(RELOAD);
        }
    }
    g_quickAim = (hipStart || reloadStart) && !aim;
    g_quickReload = reloadStart && !hipStart && !aim;
    p.wButtons = o.buttons;
    p.bLeftTrigger = o.lt;
    p.bRightTrigger = o.rt;
}
