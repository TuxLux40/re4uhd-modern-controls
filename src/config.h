#pragma once
#include "common.h"

enum RunMode { RUN_BUTTON = 0, RUN_ALWAYS = 1, RUN_OUTER_RING = 2 };

struct Config {
    // [Movement]
    int run = RUN_ALWAYS;
    float outerRing = 0.85f;  // stick deflection that counts as the outer ring (RUN_OUTER_RING)
    float turnSpeed = 9.0f;   // radians per second Leon turns toward the stick
    // [Aim]
    bool aimWalk = true;        // left stick walks while aiming, right stick aims
    float aimWalkSpeed = 900;   // game units per second at full deflection
    // [Camera]
    float defaultPitch = 0.0f;  // radians, + = looks further down than the game's camera (ini: degrees)
    float height = 0.0f;        // raises the camera, game units
    float yawSpeed = 3.0f;      // radians per second at full deflection
    float pitchSpeed = 1.8f;
    bool invertX = false;
    bool invertY = false;
};

const Config& Cfg();
void LoadConfig();
// Re-reads RE4ModernControls.ini when it changed on disk (checked at most once a second).
void ConfigTick();
