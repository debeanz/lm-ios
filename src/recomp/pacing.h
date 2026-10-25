#pragma once
// Presentation pacing for interpolated frame rates (docs/INTERPOLATION.md, "Pass 4: what reaches
// the screen"). The host loop starts each interpolated frame on an even schedule and the frame's
// interpolation position is computed for that scheduled time, so equal time steps on screen carry
// equal motion steps:
//  - fixed refresh (or VRR at its ceiling): every n-th display refresh, n chosen so that the
//    frame's cost fits (120 Hz: 120, 60, 40 fps...), started at the vblank (a vblank monitor
//    thread), never an uneven rate such as 87 fps that would show frames for 1 or 2 refreshes;
//  - variable refresh rate (G-SYNC / FreeSync, detected from the display's refresh timing or set in
//    F1): an even grid at the target, capped a little below the refresh rate so frames stay inside
//    the VRR range, slowed evenly when the frame cost does not fit;
//  - the GPU never runs more than one frame behind, so the time from a frame's start to its display
//    stays nearly constant (the display time = scheduled time + a constant).
#include "recomp/ctx.h"

#include <chrono>

namespace recomp::pacing {

using Clock = std::chrono::steady_clock;

// Host loop, interpolated presentation only (frame::interpolating()).
void beforeFrame();  // before aurora_begin_frame: waits for this frame's scheduled start
void afterBegin();   // after a successful aurora_begin_frame (LM_PACING_LOG)
void afterReplay();  // after the game frame / interpolated replay was issued (LM_PACING_LOG)
// Before aurora_end_frame: on an even grid (VRR, or VSync off), holds a frame that was built faster
// than usual until the usual time after its start, so it is presented a constant time after the
// time its picture is for (frames built in 3 ms and 10 ms were presented 7 ms apart in time but
// showed motion 8.6 ms apart: on a VRR display smooth motion juddered by +-4 ms, e.g. a boss arena's
// animated sky with a still camera). LM_PRESENT_HOLD=0: off (test aid).
void beforeEnd();
void afterFrame();   // after aurora_end_frame: frame cost bookkeeping
// Any thread: the display just refreshed (iOS display link, src/mobile): the vblank signal of the
// fixed-refresh schedule where the platform has no vblank monitor of its own.
void displayRefreshed();
// The time the frame being built is shown for (its scheduled start; the constant display latency
// is absorbed by the interpolation delay). Clock::now() when not pacing.
Clock::time_point frameTime();
// For F1: a short description of the current pacing ("120 fps (VRR)", "60 fps = 120 Hz / 2 ...").
const char* status();
bool vrrActive();
// Running under Gamescope (Steam Deck game mode): presentation always waits for the display's
// refresh there (VSync on; main.cpp), issue #11.
bool gamescope();
// VSync as the swap chain uses it: the setting, or always under Gamescope (LM_GAMESCOPE_VSYNC=0 test
// aid keeps the setting).
bool vsyncOn();
// With VSync at a fixed refresh rate, the rate a target of fps is shown at: the refresh rate or the
// even fraction of it at or below the target (60 fps at 90 Hz -> 45). 0 when frames are not tied to
// the refresh (VSync off, variable refresh, unknown refresh rate).
float fixedRefreshFps(int fps);
float presentedFps();
// Per-frame GPU completion tracking (Aurora 0197 present timing: a helper thread polls each frame's
// OnSubmittedWorkDone): Windows only. On Linux/Vulkan it made the GPU stop completing work within
// about 20 s (issue #2, NVIDIA: a frozen picture on Wayland, a failed fence or device lost on X11;
// at 30 fps through keep-warm, above 30 through frame pacing; likely also the Steam Deck freeze
// above 30 fps, issue #11). Without it frames are paced by the timer and the governor (start
// intervals), with no frame-ahead limit and no GPU cost figures. LM_PRESENT_TIMING=0/1 overrides.
bool presentTimingAvailable();
// GPU keep-warm (F1 > Visuals > Rendering "Keep GPU clocks up in menus") can run: it is sized from
// the tracked GPU cost, so wherever the tracking is.
bool keepWarmAvailable();
// The setting and the system allow it.
bool keepWarmOn();

}  // namespace recomp::pacing
