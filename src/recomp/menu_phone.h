#pragma once
// Phone look of the F1 menu's widgets (menu_phone.cpp, docs/IOS.md "Settings menu"), after Dusk's
// menu. The menu's wrappers (menu.h: Checkbox, Combo, SliderFloat/Int, SeparatorText, ItemTooltip)
// call these when the phone layer is on: full-width rows with the label in tracked capitals and the
// value as text on the right, thumb-sized, usable with a controller (D-pad between rows, left/right
// changes, A toggles), and the PC tooltips shown in the description column for the option last
// tapped or focused.
#include <cstdarg>

struct ImDrawList;
struct ImVec2;
struct ImRect;

namespace recomp::menu::phone {

// "Mansion night" palette (ImU32, ImGui's IM_COL32 byte order), shared with the menu panel
// (menu.cpp) and the overlays.
namespace color {
constexpr unsigned rgba(unsigned r, unsigned g, unsigned b, unsigned a) { return a << 24 | b << 16 | g << 8 | r; }
constexpr unsigned kPanel = rgba(19, 18, 31, 237);
constexpr unsigned kBorder = rgba(75, 70, 112, 255);
constexpr unsigned kLine = rgba(220, 215, 255, 20);
constexpr unsigned kTabMuted = rgba(135, 129, 166, 255);
constexpr unsigned kText = rgba(231, 228, 242, 255);
constexpr unsigned kWhite = rgba(255, 255, 255, 255);
constexpr unsigned kAccent = rgba(230, 191, 98, 255);
constexpr unsigned kHeading = rgba(133, 127, 166, 255);
constexpr unsigned kRow = rgba(230, 225, 255, 9);
constexpr unsigned kRowSelected = rgba(230, 191, 98, 28);
constexpr unsigned kRowPressed = rgba(230, 191, 98, 46);
constexpr unsigned kDescription = rgba(214, 210, 230, 255);
}  // namespace color

bool checkbox(const char* label, bool* v);
bool combo(const char* label, int* current, const char* const items[], int count);
bool combo(const char* label, int* current, const char* zeroSeparated);
bool sliderFloat(const char* label, float* v, float min, float max, const char* format);
bool sliderInt(const char* label, int* v, int min, int max, const char* format);
void separatorText(const char* label);
// recomp::menu::Button / SmallButton on the phone: a row with the label and a chevron.
bool button(const char* label);
// recomp::menu::Note (the PC's grey TextDisabled lines): a page's introduction (before its first
// row) goes to the description column; later ones are small wrapped captions.
void note(const char* text);
// The menu's "(next launch)"-style status notes after an option: into that option's description.
void itemNote(const char* text);
// The tooltip of the last item: kept for the description column when that item is the selected one.
void itemTooltip(const char* fmt, va_list args);
// The default of the next row's value, when it is not the settings struct's own default (phone
// first-start defaults, a combo over a local index). Without it a row finds its default itself.
void setNextDefault(double value);

// Text with extra space between letters (Dusk's capitals), in the current font at `size` px.
void trackedText(ImDrawList* dl, ImVec2 pos, float size, unsigned col, const char* text, float tracking);
float trackedWidth(float size, const char* text, float tracking);

// Once per frame before the menu is drawn: the description shown is last frame's; reads the
// controller (D-pad / stick between rows, A, left / right) for the rows drawn this frame.
void beginFrame();
// Around a page's options (notes before its first row are its introduction).
void beginPage();
void endPage();
// The open choice list (a dropdown under its row, inside `bounds`): after the page, in the menu
// window. Tap or D-pad + A picks, B or a tap outside closes it.
void drawDropdown(const ImRect& bounds);
bool dropdownOpen();
// A row changed a setting since the last call (the menu then saves the settings).
bool consumeChanged();
const char* descriptionTitle();    // "" when none
const char* descriptionText();     // "" when none
const char* descriptionDefault();  // the selected row's default value, "" when unknown
const char* descriptionIntro();    // the page's introduction (shown while nothing is selected)
// Clears the selection (another tab or page opened).
void clearSelection();
// A slider is being dragged (this or the last frame): a moving finger stays on it instead of
// scrolling the list (touch_pad.cpp).
bool sliderHeld();
// A finger is down on a slider row (not yet dragging it): a sideways move goes to the slider.
bool sliderTouched();

}  // namespace recomp::menu::phone
