#pragma once
// Phone look of the F1 menu's widgets (menu_phone.cpp, docs/IOS.md "Settings menu"). The menu's
// wrappers (menu.h: Checkbox, Combo, SliderFloat/Int, SeparatorText, ItemTooltip) call these when
// the phone layer is on: full-width rows with the label on the left and the value on the right,
// thumb-sized, usable with a controller (D-pad between rows, left/right changes, A toggles), and
// the PC tooltips shown in the menu's description strip for the option last tapped or focused.
#include <cstdarg>

namespace recomp::menu::phone {

bool checkbox(const char* label, bool* v);
bool combo(const char* label, int* current, const char* const items[], int count);
bool combo(const char* label, int* current, const char* zeroSeparated);
bool sliderFloat(const char* label, float* v, float min, float max, const char* format);
bool sliderInt(const char* label, int* v, int min, int max, const char* format);
void separatorText(const char* label);
// The tooltip of the last item: kept for the description strip when that item is the selected one.
void itemTooltip(const char* fmt, va_list args);

// Once per frame before the menu is drawn: the description shown is last frame's.
void beginFrame();
const char* descriptionTitle();  // "" when none
const char* descriptionText();   // "" when none
// Clears the selection (another tab or page opened).
void clearSelection();
// A slider is being dragged (this or the last frame): a moving finger stays on it instead of
// scrolling the list (touch_pad.cpp).
bool sliderHeld();
// A finger is down on a slider row (not yet dragging it): a sideways move goes to the slider.
bool sliderTouched();

}  // namespace recomp::menu::phone
