#pragma once
// Overlay settings menu (F1). See menu.cpp.
union SDL_Event;

namespace recomp::menu {
void init();                              // loads settings, applies window options
void handleEvent(const SDL_Event& ev);    // host event loop
void draw();                              // host thread, inside an Aurora frame
bool isOpen();                            // game input is suppressed while open
void toggle(const char* tab = nullptr);   // open (on that tab) or close, as F1 does (touch menu button)
// PADRead (guest thread): true while the buttons that closed the menu (e.g. B) are still held, so
// that press does not reach the game; false again once every button is released.
bool swallowPadAfterClose(unsigned short buttons);
bool quitRequested();                     // "Quit game" chosen in the menu
void shutdown();                          // saves changes not yet written (window closed with the menu open)
// LM_MENU_ID_CHECK sweep: tree nodes and collapsible sections in the menu call this right
// before their TreeNode() so the sweep sees (and checks) their contents too.
void openTreesForCheck();

// ImGui::Checkbox / Button / SmallButton for the menu's options: the same widgets, plus the
// LM_MENU_CLICK test aid (menu.cpp), which clicks an option by its label at a guest frame
// through the widget's own activation path, as a player's click would.
bool Checkbox(const char* label, bool* v);
bool Button(const char* label);
bool SmallButton(const char* label);
// ImGui::Combo / SeparatorText / SetItemTooltip for the menu's options: the same on a PC; on a phone
// the menu's own look (menu_phone.h: value rows, section headings, tooltips in the description strip).
bool Combo(const char* label, int* current, const char* const items[], int count, int popupMaxHeight = -1);
bool Combo(const char* label, int* current, const char* zeroSeparatedItems, int popupMaxHeight = -1);
void SeparatorText(const char* label);
// ImGui::TextDisabled for the menu's explanations: the same on a PC; on a phone a page's
// introduction goes to the description column and later ones become small wrapped captions.
void Note(const char* fmt, ...)
#if defined(__GNUC__) || defined(__clang__)
    __attribute__((format(printf, 1, 2)))
#endif
    ;
void ItemTooltip(const char* fmt, ...)
#if defined(__GNUC__) || defined(__clang__)
    __attribute__((format(printf, 1, 2)))
#endif
    ;

// ImGui::SliderFloat / SliderInt with "-" and "+" buttons in front: one click steps the value by
// one unit of its last shown digit (1 for ints and "%.0f", 0.1 for "%.1f", 0.01 for "%.2f", ...),
// clamped to the slider's range. Same arguments and return value as ImGui's (flags are
// ImGuiSliderFlags).
bool SliderFloat(const char* label, float* v, float min, float max, const char* format = "%.3f", int flags = 0);
bool SliderInt(const char* label, int* v, int min, int max, const char* format = "%d", int flags = 0);
}  // namespace recomp::menu
