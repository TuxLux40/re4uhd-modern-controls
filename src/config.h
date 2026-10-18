#pragma once
#include "common.h"

enum RunMode { RUN_BUTTON = 0, RUN_ALWAYS = 1, RUN_OUTER_RING = 2 };
enum ButtonLayout { BUTTONS_ORIGINAL = 0, BUTTONS_RE5 = 1 };

struct Config {
    // [Movement]
    int run = RUN_ALWAYS;
    float outerRing = 0.85f;  // stick deflection that counts as the outer ring (RUN_OUTER_RING)
    float turnSpeed = 9.0f;   // radians per second Leon turns toward the stick
    int buttons = BUTTONS_RE5;  // [Buttons] Layout
    int gameType = 1;           // [Buttons] GameType: the controller type chosen in the game's options (1-3)
    // [Aim]
    bool aimWalk = true;        // left stick walks while aiming, right stick aims
    float aimWalkSpeed = 1300;  // game units per second at full deflection
    float reloadSpeed = 3900;   // the same while reloading without aiming (about Leon's running speed)
    bool crosshair = true;      // crosshair at the laser's end point
    bool centerDot = true;      // a small dot in the middle of the screen outside of aiming too (with CenteredAim)
    bool hideActionPrompt = true;   // hide the door/item prompt (the action icon would show X; A acts)
    bool autoQte = true;           // quick-time events pass on their own (their prompts are hidden with the action prompts)
    bool centeredAim = true;
    bool keepViewOnAim = true;   // raising the gun keeps the view's direction (remake style) instead of looking where Leon looks    // while aiming the camera looks where the laser points: crosshair in the centre
    float aimShoulderRight = 400.0f;   // aim camera: sideways from Leon's centre (game units, + = right)
    float aimShoulderBack = 750.0f;    // behind the shoulder spot along the view
    float aimShoulderHeight = 1550.0f; // shoulder spot above Leon's feet
    bool protectAshley = true;  // Leon's guns and knife don't damage Ashley
    // [Camera]
    float defaultPitch = 0.0f;  // radians, + = looks further down than the game's camera (ini: degrees)
    float height = 0.0f;        // raises the camera, game units
    float distance = 0.6f;      // free camera's distance to Leon, share of the original
    float shoulder = 300.0f;    // moves the free camera to the right (game units), over Leon's shoulder
    float yawSpeed = 3.0f;      // radians per second at full deflection
    float pitchSpeed = 1.8f;
    float autoFollow = 1.4f;       // radians per second the camera eases behind Leon while he moves (0 = off)
    float autoFollowDelay = 1.0f;  // seconds without right-stick input before it starts
    bool invertX = false;
    bool invertY = false;
};

const Config& Cfg();
void LoadConfig();
// Re-reads RE4ModernControls.ini when it changed on disk (checked at most once a second).
void ConfigTick();
