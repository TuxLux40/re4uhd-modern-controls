#pragma once
#include "common.h"

bool InitCameraHooks();
// Pad thread: reads the right stick for the free yaw and hides it from the game while the free camera runs.
void FreeCameraPad(XGamepad& pad);
bool FreeCameraActive();
