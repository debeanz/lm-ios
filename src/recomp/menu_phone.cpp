// Phone look of the F1 menu's widgets (menu_phone.h), after Dusk's menu: plain rows with the label
// in tracked capitals and the value as text on the right, a thin gold outline on the selected row,
// a dot before values changed from their default, dropdown lists for choices, sliders with a knob.
// "Mansion night" colours (user's choice).
//
// Controller: the menu moves its own selection (beginFrame): every row registers in drawing order,
// the D-pad / left stick moves between them, A activates, left/right change the value, B closes a
// dropdown. ImGui's own gamepad navigation never reached the rows inside the scrolling list on the
// device, so it is not used for them.
#include "recomp/menu_phone.h"

#include "mobile/native.h"
#include "mobile/touch_pad.h"
#include "port/settings.h"

#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>
#include <cctype>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace recomp::menu::phone {
namespace {

// --- selection --------------------------------------------------------------------------------
int sRowCount = 0, sLastRowCount = 0;  // focusable rows this frame / last frame
int sFocus = -1;                       // the selected row (tapped or reached with the controller)
bool sFocusMoved = false;              // moved by the controller this frame: scroll it into view
bool sController = false;              // the selection was last moved with a controller
bool sActivate = false;                // A this frame (taken by the selected row)
int sSide = 0;                         // left / right this frame (taken by the selected row)
ImGuiID sLastRowId = 0;                // the last row's item id (its tooltip / note go with it)
bool sLastRowFocused = false;
bool sLastHidden = false;              // the last row was hidden (disabled): its tooltip is dropped
ImGuiID sSelOtherId = 0;               // a plain ImGui item that was tapped (description only)
std::string sTitle;                    // the selected row's name

// --- description column (collected this frame, shown next frame) ---------------------------------
std::string sDescTitle, sDescText, sDescDefault, sDescIntro;
std::string sNextText, sNextDefault, sNextIntro, sNextNote;
bool sNextSet = false, sNextDefaultSet = false;
bool sInPage = false;

// --- sliders ---------------------------------------------------------------------------------------
int sSliderHeldFrame = -10;    // ImGui frame a phone slider last had the finger (dragged sideways)
int sSliderActiveFrame = -10;  // ImGui frame a finger was last down on a phone slider
ImGuiID sClaimedId = 0;        // the slider the finger is dragging
int sTickStep = -1;            // the step the haptic tick last fired at while dragging

// --- dropdown ------------------------------------------------------------------------------------
ImGuiID sDropId = 0;               // the choice row whose list is open
std::vector<std::string> sDropItems;
int sDropCurrent = 0, sDropFocus = 0;
ImRect sDropAnchor;
ImGuiID sDropResultId = 0;
int sDropResult = -1;
bool sDropOpenedNow = false;
bool sDropKeys = false;      // the list was moved with the controller (show its focus)
float sDropScroll = 0.0f;
bool sSwallowTap = false;    // a tap closed the list: the rows ignore it until the finger lifts

// A row changed a value this frame (menu.cpp marks the settings for saving: some pages save only
// on ImGui's "edited and released", which these rows do not report).
bool sChanged = false;

// setNextDefault(): the default of the next widget when it is not the settings struct's own.
bool sOverride = false;
double sOverrideValue = 0.0;

std::string visible(const char* label) {
    const char* hash = std::strstr(label, "##");
    return hash ? std::string(label, hash) : std::string(label);
}

std::string upper(std::string s) {
    for (char& ch : s) ch = char(std::toupper(static_cast<unsigned char>(ch)));
    return s;
}

float fs() { return ImGui::GetFontSize(); }
float rowHeight() { return std::round(fs() * 2.25f); }
float padX() { return std::round(fs() * 0.85f); }
float hair() { return std::max(1.0f, std::round(fs() * 0.08f)); }

void tick() {
    if (port::settings().mobile.haptics) mobile::native::haptic(0);
}

// Options that cannot be used right now (BeginDisabled: a master switch off, another disc's
// feature) are left out on the phone instead of greyed out: shorter, calmer pages.
bool hiddenNow() {
    if ((GImGui->CurrentItemFlags & ImGuiItemFlags_Disabled) == 0) return false;
    sLastHidden = true;
    sLastRowId = 0;
    return true;
}

// While the choice list is open (and for the tap that closed it) the rows below take no taps.
bool blocked() { return sDropId != 0 || sSwallowTap; }

// Phone rows are full width: one placed with SameLine() after another starts its own line instead.
void ownLine() {
    if (ImGui::GetCurrentWindow()->DC.IsSameLine) ImGui::NewLine();
}

// A focusable row was just submitted (its InvisibleButton is the last item): registers it, makes a
// tap (or a slider drag) select it, scrolls it into view when the controller moved to it. Returns
// whether it is the selected row.
bool registerRow(const std::string& title, bool touched) {
    const int index = sRowCount++;
    const ImGuiID id = ImGui::GetItemID();
    if (touched) {  // a finished tap (a finger that scrolled the list lifted elsewhere: not one)
        sFocus = index;
        sController = false;
    }
    const bool focused = index == sFocus;
    if (focused) {
        sTitle = title;
        if (sFocusMoved)
            ImGui::ScrollToRect(ImGui::GetCurrentWindow(), ImRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax()),
                                ImGuiScrollFlags_KeepVisibleEdgeY);
    }
    sLastRowId = id;
    sLastRowFocused = focused;
    sLastHidden = false;
    return focused;
}

// The selected row's A / left / right (each taken once).
bool takeActivate(bool focused) {
    if (!focused || !sActivate) return false;
    sActivate = false;
    return true;
}
int takeSide(bool focused) {
    if (!focused || sSide == 0) return 0;
    const int s = sSide;
    sSide = 0;
    return s;
}

// The default of a setting the widget edits in place: the same member of a default-constructed
// Settings (every member has its first-install default). False for values outside the settings
// (a local copy, e.g. a combo over an enum), which then show no dot and no default.
const port::Settings& defaults() {
    static const port::Settings d{};
    return d;
}
template <typename T>
bool defaultOf(const T* v, T& out) {
    if (sOverride) {
        sOverride = false;
        out = T(sOverrideValue);
        return true;
    }
    const auto* base = reinterpret_cast<const char*>(&port::settings());
    const auto* p = reinterpret_cast<const char*>(v);
    if (p < base || p + sizeof(T) > base + sizeof(port::Settings)) return false;
    std::memcpy(&out, reinterpret_cast<const char*>(&defaults()) + (p - base), sizeof(T));
    return true;
}

void noteDefault(bool focused, const std::string& text) {
    if (!focused || sNextDefaultSet) return;
    sNextDefault = text;
    sNextDefaultSet = true;
}

void plate(const ImRect& rect, bool focused, bool held) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float rr = std::round(fs() * 0.42f);
    dl->AddRectFilled(rect.Min, rect.Max, held ? color::kRowPressed : focused ? color::kRowSelected : color::kRow, rr);
    if (focused) dl->AddRect(rect.Min, rect.Max, color::kAccent, rr, 0, hair());
}

void label(const ImRect& rect, float lineH, const std::string& title) {
    const float size = std::round(fs() * 0.9f);
    trackedText(ImGui::GetWindowDrawList(), ImVec2(rect.Min.x + padX(), rect.Min.y + (lineH - size) * 0.5f), size, color::kText,
                upper(title).c_str(), size * 0.07f);
}

// The value on the right of a row (vertically centred in the first `lineH`), with the gold dot of a
// changed value drawn as a shape (the menu font has no bullet glyph: it showed as "?").
float value(const ImRect& rect, float lineH, const char* text, bool changed, unsigned col = color::kText, float rightInset = 0) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 ts = ImGui::CalcTextSize(text);
    const float x = rect.Max.x - padX() - rightInset - ts.x, y = rect.Min.y + (lineH - ts.y) * 0.5f;
    dl->AddText(ImVec2(x, y), col, text);
    if (changed) dl->AddCircleFilled(ImVec2(x - fs() * 0.55f, rect.Min.y + lineH * 0.5f), fs() * 0.17f, color::kAccent, 12);
    return x;
}

// A thin chevron: pointing down (a choice row's list) or right (a button).
void chevron(ImDrawList* dl, ImVec2 c, float s, unsigned col, bool down) {
    const ImVec2 p[3] = {down ? ImVec2(c.x - s, c.y - s * 0.5f) : ImVec2(c.x - s * 0.5f, c.y - s),
                         down ? ImVec2(c.x, c.y + s * 0.5f) : ImVec2(c.x + s * 0.5f, c.y),
                         down ? ImVec2(c.x + s, c.y - s * 0.5f) : ImVec2(c.x - s * 0.5f, c.y + s)};
    dl->AddPolyline(p, 3, col, 0, std::max(1.5f, fs() * 0.11f));
}

}  // namespace

void trackedText(ImDrawList* dl, ImVec2 pos, float size, unsigned col, const char* text, float tracking) {
    ImFont* font = ImGui::GetFont();
    const char* end = text + std::strlen(text);
    float x = pos.x;
    for (const char* p = text; p < end;) {
        unsigned int c = 0;
        const int n = ImTextCharFromUtf8(&c, p, end);
        if (n <= 0) break;
        dl->AddText(font, size, ImVec2(std::round(x), pos.y), col, p, p + n);
        x += font->CalcTextSizeA(size, FLT_MAX, 0.0f, p, p + n).x + tracking;
        p += n;
    }
}

float trackedWidth(float size, const char* text, float tracking) {
    ImFont* font = ImGui::GetFont();
    const char* end = text + std::strlen(text);
    float w = 0;
    int count = 0;
    for (const char* p = text; p < end;) {
        unsigned int c = 0;
        const int n = ImTextCharFromUtf8(&c, p, end);
        if (n <= 0) break;
        w += font->CalcTextSizeA(size, FLT_MAX, 0.0f, p, p + n).x;
        count++;
        p += n;
    }
    return w + tracking * float(std::max(count - 1, 0));
}

void setNextDefault(double v) {
    sOverride = true;
    sOverrideValue = v;
}

bool checkbox(const char* labelText, bool* v) {
    if (hiddenNow()) return false;
    ownLine();
    const float h = rowHeight();
    const std::string title = visible(labelText);
    ImGui::PushID(labelText);
    bool pressed = ImGui::InvisibleButton("##row", ImVec2(ImGui::GetContentRegionAvail().x, h), ImGuiButtonFlags_PressedOnClickRelease) &&
                   !blocked();
    ImGui::PopID();
    const ImRect rect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
    const bool focused = registerRow(title, pressed);
    pressed |= takeActivate(focused);
    const int side = takeSide(focused);
    if (side != 0 && ((side > 0) != *v)) pressed = true;  // right turns on, left turns off
    if (pressed) {
        *v = !*v;
        sChanged = true;
        tick();
    }
    plate(rect, focused, ImGui::IsItemActive());
    label(rect, h, title);
    bool def = false;
    const bool known = defaultOf(v, def);
    value(rect, h, *v ? "On" : "Off", known && def != *v, *v ? color::kAccent : color::kText);
    noteDefault(focused, known ? (def ? "On" : "Off") : "");
    return pressed;
}

bool combo(const char* labelText, int* current, const char* const items[], int count) {
    if (count <= 0 || hiddenNow()) return false;
    ownLine();
    const float h = rowHeight();
    const std::string title = visible(labelText);
    ImGui::PushID(labelText);
    const bool pressed = ImGui::InvisibleButton("##row", ImVec2(ImGui::GetContentRegionAvail().x, h), ImGuiButtonFlags_PressedOnClickRelease) &&
                         !blocked();
    const ImGuiID id = ImGui::GetItemID();
    ImGui::PopID();
    const ImRect rect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
    const bool focused = registerRow(title, pressed);
    int idx = std::clamp(*current, 0, count - 1);
    bool changed = false;
    // A choice made in the open list (drawDropdown, last frame).
    if (sDropResultId == id) {
        sDropResultId = 0;
        if (sDropResult >= 0 && sDropResult < count && sDropResult != idx) {
            idx = sDropResult;
            changed = true;
        }
    }
    // Tap / A: the list opens under the row. Left / right on a controller step without it.
    if ((pressed || takeActivate(focused)) && sDropId == 0) {
        sDropId = id;
        sDropCurrent = sDropFocus = idx;
        sDropOpenedNow = true;
        sDropKeys = sController;
        tick();
    }
    if (const int side = takeSide(focused); side != 0) {
        idx = ((idx + side) % count + count) % count;
        changed = true;
        tick();
    }
    if (sDropId == id) {  // the list shows this frame's labels at this frame's row position
        sDropItems.assign(items, items + count);
        for (auto& s : sDropItems) if (s.empty()) s = " ";
        sDropCurrent = idx;
        sDropAnchor = rect;
    }
    if (changed) {
        *current = idx;
        sChanged = true;
    }
    const bool open = sDropId == id;
    plate(rect, focused || open, ImGui::IsItemActive());
    label(rect, h, title);
    int def = 0;
    const bool known = defaultOf(current, def) && def >= 0 && def < count;
    const float chev = fs() * 1.3f;
    value(rect, h, items[idx] ? items[idx] : "", known && def != idx, open ? color::kAccent : color::kText, chev);
    chevron(ImGui::GetWindowDrawList(), ImVec2(rect.Max.x - padX() - chev * 0.35f, rect.GetCenter().y), fs() * 0.28f,
            open ? color::kAccent : color::kHeading, true);
    noteDefault(focused, known && items[def] ? items[def] : "");
    return changed;
}

bool combo(const char* labelText, int* current, const char* zeroSeparated) {
    const char* items[64];
    int n = 0;
    for (const char* p = zeroSeparated; *p && n < 64; p += std::strlen(p) + 1) items[n++] = p;
    return combo(labelText, current, items, n);
}

bool button(const char* labelText) {
    if (hiddenNow()) return false;
    ownLine();
    const float h = rowHeight();
    const std::string title = visible(labelText);
    ImGui::PushID(labelText);
    bool pressed = ImGui::InvisibleButton("##button", ImVec2(ImGui::GetContentRegionAvail().x, h), ImGuiButtonFlags_PressedOnClickRelease) &&
                   !blocked();
    ImGui::PopID();
    const ImRect rect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
    const bool focused = registerRow(title, pressed);
    pressed |= takeActivate(focused);
    plate(rect, focused, ImGui::IsItemActive());
    label(rect, h, title);
    chevron(ImGui::GetWindowDrawList(), ImVec2(rect.Max.x - padX() - fs() * 0.3f, rect.GetCenter().y), fs() * 0.3f,
            focused ? color::kAccent : color::kHeading, false);
    if (pressed) tick();
    return pressed;
}

namespace {
// Slider row: the label and the value on top, a track with a knob below. A sideways drag anywhere
// on the row moves the knob to the finger (a swipe up or down scrolls the list instead); a tap on
// the track jumps there. While dragging, the knob grows, a bubble with the value rides above it and
// each step ticks the Taptic Engine. Controller: left / right step. Works on a normalized
// position; `steps` is the number of steps across the range (0: continuous).
bool sliderRow(const char* labelText, float& t, const char* valueText, bool notDefault, int steps, const char* bubble) {
    ownLine();
    const float lineH = rowHeight(), h = std::round(fs() * 3.6f);
    const std::string title = visible(labelText);
    ImGui::PushID(labelText);
    const bool tapped = ImGui::InvisibleButton("##slider", ImVec2(ImGui::GetContentRegionAvail().x, h), ImGuiButtonFlags_PressedOnClickRelease) &&
                        !blocked();
    ImGui::PopID();
    const ImRect rect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
    const ImGuiID id = ImGui::GetItemID();
    const bool active = ImGui::IsItemActive() && !blocked();
    const ImGuiIO& io = ImGui::GetIO();
    const bool pointer = io.MousePos.x > -1e6f;
    if (active) {
        sSliderActiveFrame = ImGui::GetFrameCount();
        const float dx = io.MousePos.x - io.MouseClickedPos[0].x, dy = io.MousePos.y - io.MouseClickedPos[0].y;
        if (sClaimedId != id && pointer && std::fabs(dx) > fs() * 0.25f && std::fabs(dx) >= std::fabs(dy)) sClaimedId = id;
    }
    const bool wasDragged = !active && sClaimedId == id;
    if (wasDragged) sClaimedId = 0;
    const bool held = active && sClaimedId == id;
    if (held) sSliderHeldFrame = ImGui::GetFrameCount();
    const bool focused = registerRow(title, held || tapped);

    const float x0 = rect.Min.x + padX() + fs() * 0.4f, x1 = rect.Max.x - padX() - fs() * 0.4f;
    const float ty = rect.Min.y + lineH + (h - lineH) * 0.38f;
    bool changed = false;
    const auto moveTo = [&](float nt) {
        nt = std::clamp(nt, 0.0f, 1.0f);
        if (nt == t) return;
        t = nt;
        changed = true;
    };
    if (held && pointer) moveTo((io.MousePos.x - x0) / std::max(x1 - x0, 1.0f));
    // A tap on the track (not on the label line) jumps there.
    if (tapped && !wasDragged && io.MouseClickedPos[0].y > rect.Min.y + lineH * 0.8f)
        moveTo((io.MouseClickedPos[0].x - x0) / std::max(x1 - x0, 1.0f));
    if (const int side = takeSide(focused); side != 0) {
        moveTo(t + float(side) * (steps > 0 ? 1.0f / float(steps) : 0.01f));
        tick();
    }
    // A tick per step while the finger drags (every 5% for continuous sliders).
    const int step = int(std::lround(t * float(steps > 0 && steps <= 100 ? steps : 20)));
    if (held && changed && step != sTickStep) tick();
    sTickStep = held ? step : -1;

    ImDrawList* dl = ImGui::GetWindowDrawList();
    plate(rect, focused || held, false);
    label(rect, lineH, title);
    value(rect, lineH, valueText, notDefault, held ? color::kAccent : color::kText);
    // Track: rounded, gold up to the knob; little marks at each step when there are few.
    const float th = std::round(fs() * 0.3f), kx = x0 + (x1 - x0) * t;
    dl->AddRectFilled(ImVec2(x0, ty - th * 0.5f), ImVec2(x1, ty + th * 0.5f), color::rgba(220, 215, 255, 38), th);
    dl->AddRectFilled(ImVec2(x0, ty - th * 0.5f), ImVec2(kx, ty + th * 0.5f), color::kAccent, th);
    if (steps > 1 && steps <= 12)
        for (int i = 1; i < steps; i++) {
            const float mx = x0 + (x1 - x0) * float(i) / float(steps);
            dl->AddCircleFilled(ImVec2(mx, ty), th * 0.28f, mx <= kx ? color::rgba(26, 22, 10, 160) : color::rgba(220, 215, 255, 90), 8);
        }
    // Knob: a moon-white disc in a gold ring; bigger, with a soft halo, while it is held.
    const float kr = fs() * (held ? 0.78f : 0.6f);
    if (held || focused) dl->AddCircleFilled(ImVec2(kx, ty), kr * 1.75f, color::rgba(230, 191, 98, held ? 52 : 26), 32);
    dl->AddCircleFilled(ImVec2(kx, ty), kr, color::rgba(255, 246, 222, 255), 32);
    dl->AddCircle(ImVec2(kx, ty), kr, color::kAccent, 32, std::max(1.5f, fs() * 0.13f));
    if (held && bubble && *bubble) {
        // The value in a gold bubble above the knob (clear of the finger).
        const ImVec2 bs = ImGui::CalcTextSize(bubble);
        const float bw = bs.x + fs() * 1.0f, bh = bs.y + fs() * 0.5f;
        float bx = std::clamp(kx - bw * 0.5f, rect.Min.x, rect.Max.x - bw);
        const float by = ty - kr - fs() * 0.5f - bh;
        dl->PushClipRectFullScreen();
        dl->AddRectFilled(ImVec2(bx, by), ImVec2(bx + bw, by + bh), color::kAccent, bh * 0.5f);
        dl->AddTriangleFilled(ImVec2(kx - fs() * 0.3f, by + bh - 1), ImVec2(kx + fs() * 0.3f, by + bh - 1), ImVec2(kx, by + bh + fs() * 0.3f),
                              color::kAccent);
        dl->AddText(ImVec2(bx + (bw - bs.x) * 0.5f, by + (bh - bs.y) * 0.5f), color::rgba(30, 24, 12, 255), bubble);
        dl->PopClipRect();
    }
    return changed;
}

float stepOf(const char* format) {
    const char* p = format ? std::strchr(format, '%') : nullptr;
    while (p && p[1] == '%') p = std::strchr(p + 2, '%');
    const char* dot = p ? std::strchr(p, '.') : nullptr;
    return dot ? std::pow(10.0f, -float(std::atoi(dot + 1))) : 1.0f;
}
}  // namespace

bool sliderFloat(const char* labelText, float* v, float min, float max, const char* format) {
    if (hiddenNow()) return false;
    const float lo = std::min(min, max), hi = std::max(min, max), span = std::max(hi - lo, 1e-6f);
    float t = std::clamp((*v - lo) / span, 0.0f, 1.0f);
    const char* fmt = format ? format : "%.2f";
    char text[64];
    std::snprintf(text, sizeof(text), fmt, *v);
    const float step = stepOf(fmt);
    float def = 0;
    const bool known = defaultOf(v, def);
    const int steps = int(std::lround(span / step));
    const bool moved = sliderRow(labelText, t, text, known && std::fabs(def - *v) > step * 0.5f, steps <= 100 ? steps : 0, text);
    if (known) {
        char d[64];
        std::snprintf(d, sizeof(d), fmt, def);
        noteDefault(sLastRowFocused, d);
    }
    if (!moved) return false;
    const float nv = std::clamp(std::round((lo + t * span) / step) * step, lo, hi);
    if (nv == *v) return false;
    *v = nv;
    sChanged = true;
    return true;
}

bool sliderInt(const char* labelText, int* v, int min, int max, const char* format) {
    if (hiddenNow()) return false;
    const int lo = std::min(min, max), hi = std::max(min, max);
    const float span = float(std::max(hi - lo, 1));
    float t = std::clamp(float(*v - lo) / span, 0.0f, 1.0f);
    // (A slider's format may be a fixed label such as "Match window" without %d.)
    const bool hasNumber = format && std::strstr(format, "%");
    char text[64];
    if (hasNumber) std::snprintf(text, sizeof(text), format, *v);
    else std::snprintf(text, sizeof(text), "%s", format ? format : "");
    int def = 0;
    const bool known = defaultOf(v, def);
    const bool moved = sliderRow(labelText, t, text, known && def != *v, hi - lo, text);
    if (known) {
        char d[64];
        if (hasNumber) std::snprintf(d, sizeof(d), format, def);
        else std::snprintf(d, sizeof(d), "%d", def);
        noteDefault(sLastRowFocused, d);
    }
    if (!moved) return false;
    const int nv = std::clamp(lo + int(std::lround(t * span)), lo, hi);
    if (nv == *v) return false;
    *v = nv;
    sChanged = true;
    return true;
}

void separatorText(const char* text) {
    if ((GImGui->CurrentItemFlags & ImGuiItemFlags_Disabled) != 0) return;
    ownLine();
    // Group heading (Dusk): small tracked capitals in the muted heading colour, air above.
    ImGui::Dummy(ImVec2(0, std::round(fs() * 0.55f)));
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float size = std::round(fs() * 0.78f);
    trackedText(ImGui::GetWindowDrawList(), ImVec2(p.x + std::round(fs() * 0.3f), p.y), size, color::kHeading,
                upper(visible(text)).c_str(), size * 0.12f);
    ImGui::Dummy(ImVec2(0, std::round(size + fs() * 0.35f)));
}

void note(const char* text) {
    if (!text || !*text || (GImGui->CurrentItemFlags & ImGuiItemFlags_Disabled) != 0) return;
    std::string s(text);
    for (size_t i = 0; i < s.size(); i++)
        if (s[i] == '\n') s[i] = ' ';
    // A page's introduction (notes before its first row) goes to the description column, shown
    // while no option is selected; later notes are small captions in the list.
    if (sInPage && sRowCount == 0) {
        if (!sNextIntro.empty()) sNextIntro += ' ';
        sNextIntro += s;
        return;
    }
    ownLine();
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(color::kHeading));
    ImGui::PushTextWrapPos(ImGui::GetCursorPos().x + ImGui::GetContentRegionAvail().x - padX());
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::round(fs() * 0.3f));
    ImGui::SetWindowFontScale(0.88f);
    ImGui::TextWrapped("%s", s.c_str());
    ImGui::SetWindowFontScale(1.0f);
    ImGui::PopTextWrapPos();
    ImGui::PopStyleColor();
}

void itemNote(const char* text) {
    // A row's status note ("next launch", "needs the European game"): into its description.
    if (sLastHidden || !text || !*text) return;
    if (ImGui::GetItemID() == sLastRowId && sLastRowFocused && sNextNote.empty()) sNextNote = text;
}

void itemTooltip(const char* fmt, va_list args) {
    if (sLastHidden) return;  // the row before it was left out: so is its description
    const ImGuiID id = ImGui::GetItemID();
    bool mine;
    if (id != 0 && id == sLastRowId) {
        mine = sLastRowFocused;
    } else {
        // A plain ImGui item (a combo list, a radio button): selected by a tap only.
        if (ImGui::IsItemActivated()) {
            sSelOtherId = id;
            sFocus = -1;
        }
        mine = id != 0 && id == sSelOtherId;
        if (mine) sTitle.clear();
    }
    if (!mine || sNextSet) return;
    char buf[2048];
    std::vsnprintf(buf, sizeof(buf), fmt, args);
    // The PC tooltips are wrapped by hand for a mouse tooltip: let the column wrap them instead.
    std::string text(buf);
    for (size_t i = 0; i < text.size(); i++)
        if (text[i] == '\n') text[i] = (i + 1 < text.size() && text[i + 1] == '\n') ? '\n' : ' ';
    sNextText = text;
    sNextSet = true;
}

void beginFrame() {
    // Description: last frame's.
    sDescTitle = sFocus >= 0 || sSelOtherId ? sTitle : std::string();
    sDescText = sNextSet ? sNextText : std::string();
    if (!sNextNote.empty()) sDescText += (sDescText.empty() ? "" : "\n\n") + sNextNote;
    sDescDefault = sNextDefaultSet ? sNextDefault : std::string();
    sDescIntro = sNextIntro;
    sNextSet = sNextDefaultSet = false;
    sNextNote.clear();
    sNextIntro.clear();
    sOverride = false;

    if (sSwallowTap && !ImGui::GetIO().MouseDown[0] && !ImGui::GetIO().MouseReleased[0]) sSwallowTap = false;
    sLastRowCount = sRowCount;
    sRowCount = 0;
    sLastRowId = 0;
    sLastHidden = false;
    if (sFocus >= sLastRowCount) sFocus = sLastRowCount - 1;
    sFocusMoved = false;
    sActivate = false;
    sSide = 0;
    // Controller: up / down between rows (with repeat), A, left / right. Not while a list or another
    // popup is open (the list reads them itself, drawDropdown).
    const bool popup = ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel);
    if (popup || sDropId != 0 || sLastRowCount == 0) return;
    const auto pressed = [](ImGuiKey a, ImGuiKey b) { return ImGui::IsKeyPressed(a, true) || ImGui::IsKeyPressed(b, true); };
    int move = 0;
    if (pressed(ImGuiKey_GamepadDpadDown, ImGuiKey_GamepadLStickDown)) move = 1;
    if (pressed(ImGuiKey_GamepadDpadUp, ImGuiKey_GamepadLStickUp)) move = -1;
    if (move != 0) {
        sFocus = sFocus < 0 ? 0 : std::clamp(sFocus + move, 0, sLastRowCount - 1);
        sFocusMoved = sController = true;
        sSelOtherId = 0;
        tick();
    }
    if (ImGui::IsKeyPressed(ImGuiKey_GamepadFaceDown, false)) {
        if (sFocus < 0) {
            sFocus = 0;
            sFocusMoved = true;
        } else {
            sActivate = true;
        }
        sController = true;
    }
    if (pressed(ImGuiKey_GamepadDpadLeft, ImGuiKey_GamepadLStickLeft)) sSide = -1;
    if (pressed(ImGuiKey_GamepadDpadRight, ImGuiKey_GamepadLStickRight)) sSide = 1;
    if (sSide != 0 && sFocus < 0) sSide = 0;
}

void beginPage() { sInPage = true; }
void endPage() { sInPage = false; }

void drawDropdown(const ImRect& bounds) {
    if (sDropId == 0) return;
    // Drawn over everything (the foreground list) and driven by hand: an ImGui popup opened from
    // the list's child window never showed on the device (build 14).
    const ImGuiIO& io = ImGui::GetIO();
    const float itemH = std::round(fs() * 2.3f), pad = std::round(fs() * 0.35f);
    const int n = int(sDropItems.size());
    float w = 0;
    for (const auto& s : sDropItems) w = std::max(w, ImGui::CalcTextSize(s.c_str()).x);
    w = std::clamp(w + padX() * 2 + fs() * 1.4f, sDropAnchor.GetWidth() * 0.45f, sDropAnchor.GetWidth());
    const float full = float(n) * itemH + 2 * pad;
    // Under the row, right-aligned with it; above it when there is more room there.
    const float below = bounds.Max.y - sDropAnchor.Max.y - pad, above = sDropAnchor.Min.y - bounds.Min.y - pad;
    const bool down = below >= std::min(full, fs() * 8.0f) || below >= above;
    const float h = std::min(full, std::max(down ? below : above, itemH * 2.5f));
    const float x = sDropAnchor.Max.x - w, y = down ? sDropAnchor.Max.y + pad * 0.6f : sDropAnchor.Min.y - pad * 0.6f - h;
    const ImRect box(ImVec2(x, y), ImVec2(x + w, y + h));
    const float maxScroll = std::max(0.0f, full - h);
    const auto keepVisible = [&](int i) {
        const float top = pad + float(i) * itemH;
        if (top < sDropScroll + pad) sDropScroll = top - pad;
        if (top + itemH > sDropScroll + h - pad) sDropScroll = top + itemH - h + pad;
        sDropScroll = std::clamp(sDropScroll, 0.0f, maxScroll);
    };
    if (sDropOpenedNow) {
        sDropScroll = 0;
        keepVisible(sDropCurrent);
    }

    int chosen = -1;
    bool close = false;
    if (!sDropOpenedNow) {
        // Controller: up / down, A picks, B closes without a change.
        int move = 0;
        if (ImGui::IsKeyPressed(ImGuiKey_GamepadDpadDown, true) || ImGui::IsKeyPressed(ImGuiKey_GamepadLStickDown, true)) move = 1;
        if (ImGui::IsKeyPressed(ImGuiKey_GamepadDpadUp, true) || ImGui::IsKeyPressed(ImGuiKey_GamepadLStickUp, true)) move = -1;
        if (move) {
            sDropFocus = std::clamp(sDropFocus + move, 0, n - 1);
            sDropKeys = true;
            keepVisible(sDropFocus);
            tick();
        }
        if (ImGui::IsKeyPressed(ImGuiKey_GamepadFaceDown, false)) chosen = sDropFocus;
        if (ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false)) close = true;
        // Touch: a swipe inside the list scrolls it; a tap on a value picks it; a tap anywhere else
        // closes the list (and goes nowhere else: sSwallowTap until the finger lifts).
        ImVec2 drag, start;
        if (mobile::touch::scrollFor(box, drag, start)) sDropScroll = std::clamp(sDropScroll - drag.y, 0.0f, maxScroll);
        if (io.MouseClicked[0] && !box.Contains(io.MouseClickedPos[0])) {
            close = true;
            sSwallowTap = true;
        }
        if (io.MouseReleased[0] && ImGui::IsMousePosValid(&io.MousePos) && box.Contains(io.MouseClickedPos[0]) &&
            box.Contains(io.MousePos)) {
            const int i = int((io.MouseClickedPos[0].y - box.Min.y - pad + sDropScroll) / itemH);
            const int j = int((io.MousePos.y - box.Min.y - pad + sDropScroll) / itemH);
            if (i == j && i >= 0 && i < n) chosen = i;
        }
    }

    ImDrawList* dl = ImGui::GetForegroundDrawList();
    const float rr = std::round(fs() * 0.5f);
    dl->AddRectFilled(ImVec2(box.Min.x, box.Min.y + fs() * 0.25f), ImVec2(box.Max.x, box.Max.y + fs() * 0.25f), color::rgba(0, 0, 0, 90), rr);
    dl->AddRectFilled(box.Min, box.Max, color::rgba(28, 26, 44, 252), rr);
    dl->AddRect(box.Min, box.Max, color::kBorder, rr, 0, hair());
    dl->PushClipRect(ImVec2(box.Min.x + hair(), box.Min.y + hair()), ImVec2(box.Max.x - hair(), box.Max.y - hair()), true);
    const bool fingerDown = io.MouseDown[0] && box.Contains(io.MouseClickedPos[0]) && ImGui::IsMousePosValid(&io.MousePos);
    for (int i = 0; i < n; i++) {
        const ImRect r(ImVec2(box.Min.x + pad, box.Min.y + pad + float(i) * itemH - sDropScroll),
                       ImVec2(box.Max.x - pad, box.Min.y + pad + float(i + 1) * itemH - sDropScroll));
        if (r.Max.y < box.Min.y || r.Min.y > box.Max.y) continue;
        const bool cur = i == sDropCurrent, foc = i == sDropFocus && sDropKeys, held = fingerDown && r.Contains(io.MousePos);
        const float ir = std::round(fs() * 0.35f);
        if (held || foc) dl->AddRectFilled(r.Min, r.Max, held ? color::kRowPressed : color::kRowSelected, ir);
        if (foc) dl->AddRect(r.Min, r.Max, color::kAccent, ir, 0, hair());
        const char* text = sDropItems[size_t(i)].c_str();
        const ImVec2 ts = ImGui::CalcTextSize(text);
        dl->AddText(ImVec2(r.Min.x + padX(), r.Min.y + (itemH - ts.y) * 0.5f), cur ? color::kAccent : color::kText, text);
        if (cur) {  // a check mark on the current value
            const float c = fs() * 0.28f, cx = r.Max.x - padX() - c, cy = r.GetCenter().y;
            const ImVec2 p[3] = {ImVec2(cx - c, cy), ImVec2(cx - c * 0.3f, cy + c * 0.7f), ImVec2(cx + c, cy - c * 0.7f)};
            dl->AddPolyline(p, 3, color::kAccent, 0, std::max(1.5f, fs() * 0.12f));
        }
    }
    dl->PopClipRect();

    sDropOpenedNow = false;
    if (chosen >= 0) {
        sDropResultId = sDropId;
        sDropResult = chosen;
        tick();
        close = true;
    }
    if (close) sDropId = 0;
}

bool dropdownOpen() { return sDropId != 0; }
bool consumeChanged() { return std::exchange(sChanged, false); }

const char* descriptionTitle() { return sDescTitle.c_str(); }
const char* descriptionText() { return sDescText.c_str(); }
const char* descriptionDefault() { return sDescDefault.c_str(); }
const char* descriptionIntro() { return sDescIntro.c_str(); }

bool sliderHeld() { return ImGui::GetCurrentContext() && ImGui::GetFrameCount() - sSliderHeldFrame <= 1; }
bool sliderTouched() { return ImGui::GetCurrentContext() && ImGui::GetFrameCount() - sSliderActiveFrame <= 1; }

void clearSelection() {
    sFocus = sController ? 0 : -1;  // a controller starts the new page at its first row
    sFocusMoved = sController;
    sSelOtherId = 0;
    sTitle.clear();
    sDescTitle.clear();
    sDescText.clear();
    sDescDefault.clear();
    sNextSet = sNextDefaultSet = false;
    sDropId = 0;
}

}  // namespace recomp::menu::phone
