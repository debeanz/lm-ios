#pragma once
// Host <-> guest frame handoff. Guest code only runs while the host has an
// Aurora frame open: the host calls runUntilPresent() between
// aurora_begin_frame() and aurora_end_frame(), and the guest gives control
// back when it presents (VISetNextFrameBuffer).
#include "recomp/ctx.h"

namespace recomp::frame {

using Entry = void (*)();

void start(Entry boot);       // boot runs on a new host thread once the first frame opens
bool runUntilPresent();       // false once the guest has exited
// One host frame (between aurora_begin_frame and aurora_end_frame): runs the
// guest until it presents, or with frame interpolation on (settings), replays
// the guest's recorded frames instead while the guest runs on its own.
bool hostFrame();             // false once the guest has exited
void pace();                  // after aurora_end_frame (lockstep only; interpolated frames: pacing.h)
bool interpolating();         // host: frames are replayed tapes (decoupled guest)
u32 guestFrames();             // frames the game has finished (VISetNextFrameBuffer), fast-forwarded ones included
// Presentation target (F1 > Visuals > Frame rate): the slider, or the refresh rate of the
// display the window is on. 30 = the game's own rate without interpolation.
int targetFps();
// Phone (src/mobile): the screen (menu, touch controls, overlays) is never tied to the game's frames.
// The host always runs decoupled, at hostFps(): at least 60 even when the game is shown at its own
// 30 (gameFramesOnly(): the recorded frames are shown as they are, without in-betweens).
int hostFps();
bool gameFramesOnly();
float displayRefreshHz();     // 0 if unknown; host (main) thread
void stop();
bool gamePaused();
void setHostPaused(bool paused);  // host: window paused (no frames)

void present(Ctx& c, u32 xfb, bool black);  // guest side
// Guest side, before any GX work: while the presenting thread waits for the host to finish
// the frame, other guest threads keep running (present() gives up the guest CPU); one that
// reaches GX waits here until the guest's turn, since the host owns the Aurora frame.
void gxWaitForGuestTurn();
// Guest side: runs `wait` (host work that blocks on GX, e.g. draining the FIFO for
// GXDrawDone) with the guest CPU given up, like the console's GXDrawDone sleeping on its
// interrupt; other threads' GX calls wait until it is done.
void gxBlockingWait(void (*wait)());

// The US game hands each finished frame to VISetNextFrameBuffer. The PAL release
// re-submits the framebuffer from its post-retrace callback every retrace and paces its
// frames with an alarm instead, so there a frame is finished at GXCopyDisp.
bool presentOnCopy();
bool viBlack();  // last VISetBlack

}  // namespace recomp::frame
