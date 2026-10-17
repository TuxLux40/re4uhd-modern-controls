// Game hooks and the controls layer. Milestone 0: the proxy loads, forwards XInput and logs the left stick, so the
// load path (Proton, WINEDLLOVERRIDES, re4_tweaks side by side) is proven before any game code is touched.
#include "common.h"

void TransformPad(DWORD, XGamepad& pad) {
    static DWORD last = 0;
    const DWORD now = GetTickCount();
    if (now - last >= 2000 && (pad.sThumbLX || pad.sThumbLY)) {
        last = now;
        Log("pad0: L(%d,%d) R(%d,%d) buttons %04X", pad.sThumbLX, pad.sThumbLY, pad.sThumbRX, pad.sThumbRY,
            pad.wButtons);
    }
}

void InitGameHooks() {}
