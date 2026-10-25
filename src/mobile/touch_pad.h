#pragma once
// The on-screen GameCube controller (touch_pad.cpp). Internal to src/mobile.
#include <dolphin/types.h>

union SDL_Event;
struct PADStatus;

namespace mobile::touch {

// Host thread.
bool handleEvent(const SDL_Event& ev);  // true: a finger on a control (consumed)
void draw();                            // inside the ImGui frame, over everything else
void releaseAll();                      // app backgrounded, layout changed under a finger
bool visible();                         // drawn and feeding the game (no physical controller, not switched off)
const char* physicalController();       // name of a connected controller, or nullptr

// Guest thread (PADRead).
void applyPad(PADStatus* status);
bool feeding();  // the game reads the touch controller on port 1 right now

}  // namespace mobile::touch
