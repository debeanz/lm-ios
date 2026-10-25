#pragma once
// iPhone / iPad layer (docs/IOS.md): the on-screen GameCube controller, the phone settings
// (F1 > iPhone), the phone display model (render height, aspect, Fit/Stretch, UI size), haptics,
// and the detailed session log. On iOS it is always on; on a PC, LM_MOBILE=1 turns it on for
// testing (the mouse acts as one finger, LM_MOBILE_SCREEN=WxH pretends the panel size).
//
// Threads: everything here runs on the host (main) thread except applyPad and onRumble, which the
// PADRead / PADControlMotor HLE calls on a guest thread.
#include <dolphin/types.h>

#include <string>

union SDL_Event;
struct PADStatus;

namespace mobile {

bool enabled();

// Before aurora_initialize: phone defaults on the first start (frame rate, quality tier from RAM),
// written to settings.ini right away.
void beforeAurora();
// After aurora_initialize, before the game starts: display model, touch controller.
void init();
// Host event loop, before the menu's own handling. True: the event was a touch the controller
// owns (it must not reach anything else).
bool handleEvent(const SDL_Event& ev);
// Host thread, inside the frame after the F1 menu: draws the controller and the FPS readout, and
// writes the periodic [perf] line into the log.
void draw();
// Host loop timing for the [perf] lines (main.cpp): call frameBegin() at the top of a game-loop
// iteration and phase(i) when step i ends: 0 frame pacing wait, 1 aurora_begin_frame (frame slot,
// drawable), 2 the game / interpolation replay, 3 menus and touch controls, 4 aurora_end_frame.
// The slowest frame of each 10 s window is logged with its steps.
void frameBegin();
void phase(int step);

// App lifecycle (SDL's background/foreground events): saves settings and flushes the log.
void onBackground();
void onForeground();

// Guest thread (PADRead): the touch controller's state merged into port 1.
void applyPad(PADStatus* status);
// Guest thread (PADControlMotor): the game's rumble on the phone's Taptic Engine while the touch
// controller is in use.
void onRumble(u32 chan, u32 cmd);

// Display model (widescreen.cpp, ui_scale.cpp).
float targetAspect();      // width / height chosen in Aspect Ratio; 0 = the device's
bool fitScaling();         // Screen Scaling = Fit
float uiMultiplier();      // UI Size: 0.85 / 1 / 1.2
// Applies Resolution (render height) and the frame rate now; called when a row changes.
void applyDisplay();
// The refresh rate the panel is held at (60 or 120; frame.cpp reports it to the frame pacer
// instead of the panel's maximum). 0 = not held (PC).
float displayRateLock();

// F1 > iPhone tab. Returns true when a setting changed (saved at once).
bool drawSettingsTab();

// The window's safe area as insets from each edge, in ImGui units (camera housing, rounded
// corners, home indicator). Zero on a PC.
void safeAreaInsets(float& left, float& top, float& right, float& bottom);

// The first start's disc image (iOS): a disc of the release this app was built for (gameId,
// "GLME01"/"GLMP01") found in the app's Documents folder, or chosen with the Files picker on a
// full-screen page. Returns its path; empty if there is no way to get one (PC).
std::string chooseDisc(const char* gameId);

}  // namespace mobile
