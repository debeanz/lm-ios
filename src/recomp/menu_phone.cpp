// Phone look of the F1 menu's widgets (menu_phone.h), after Dusk's menu: plain rows with the label
// in tracked capitals and the value as text on the right, a thin gold outline on the selected row,
// a dot before values changed from their default. "Mansion night" colours (user's choice).
#include "recomp/menu_phone.h"

#include "mobile/native.h"
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

namespace recomp::menu::phone {
namespace {

// The selected item (tapped, or focused with a controller): its tooltip and default go to the
// description column.
ImGuiID sSelId = 0;
std::string sSelTitle;
std::string sDescTitle, sDescText, sDescDefault;  // shown this frame (from last frame)
std::string sNextTitle, sNextText, sNextDefault;  // collected this frame
bool sNextSet = false, sNextDefaultSet = false;
int sSliderHeldFrame = -10;    // ImGui frame a phone slider last had the finger (dragged sideways)
int sSliderActiveFrame = -10;  // ImGui frame a finger was last down on a phone slider
ImGuiID sClaimedId = 0;        // the slider the finger is dragging
float sDragT0 = 0.0f;          // its position when the finger went down
bool sDefaultDone = false;     // the page's first row was made the controller's default focus
// setNextDefault(): the default of the next widget when it is not the settings struct's own.
bool sOverride = false;
double sOverrideValue = 0.0;

// The text before "##" (ImGui's ID suffix).
std::string visible(const char* label) {
    const char* hash = std::strstr(label, "##");
    return hash ? std::string(label, hash) : std::string(label);
}

std::string upper(std::string s) {
    for (char& ch : s) ch = char(std::toupper(static_cast<unsigned char>(ch)));
    return s;
}

// The first row of a page is where the controller starts (menu opened, section changed).
void defaultFocus() {
    if (sDefaultDone) return;
    sDefaultDone = true;
    ImGui::SetItemDefaultFocus();
}

// Phone rows are full width: one placed with SameLine() after another starts its own line instead
// (there is no room beside a full-width row; e.g. Debug > Font test's "Latin last line").
void ownLine() {
    if (ImGui::GetCurrentWindow()->DC.IsSameLine) ImGui::NewLine();
}

float fs() { return ImGui::GetFontSize(); }
float rowHeight() { return std::round(fs() * 2.45f); }
float padX() { return std::round(fs() * 0.85f); }

bool navActive() {
    const ImGuiContext& g = *GImGui;
    return g.NavCursorVisible && g.NavId != 0;
}

void select(ImGuiID id, const std::string& title) {
    sSelId = id;
    sSelTitle = title;
}

// The last item became the selection: tapped, or reached with the controller.
void selectIfTouched(const std::string& title) {
    if (ImGui::IsItemActivated() || (ImGui::IsItemFocused() && navActive())) select(ImGui::GetItemID(), title);
}

bool isSelected(ImGuiID id) { return id == sSelId; }

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

// Selected row: its default for the description column ("" when unknown).
void noteDefault(bool sel, const std::string& text) {
    if (!sel || sNextDefaultSet) return;
    sNextDefault = text;
    sNextDefaultSet = true;
}

// Row frame: an invisible button of the row's size, the faint row plate, the selection outline and
// the label in tracked capitals. Returns the pressed state.
bool row(const char* label, float h, ImRect& rect, bool& selOut) {
    ownLine();
    const float w = ImGui::GetContentRegionAvail().x;
    ImGui::PushID(label);
    const bool pressed = ImGui::InvisibleButton("##row", ImVec2(w, h), ImGuiButtonFlags_PressedOnClickRelease);
    defaultFocus();
    ImGui::PopID();
    rect = ImRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
    const std::string title = visible(label);
    selectIfTouched(title);
    const bool sel = isSelected(ImGui::GetItemID());
    const bool held = ImGui::IsItemActive();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float rr = std::round(h * 0.17f);
    dl->AddRectFilled(rect.Min, rect.Max, held ? color::kRowPressed : sel ? color::kRowSelected : color::kRow, rr);
    if (sel) dl->AddRect(rect.Min, rect.Max, color::kAccent, rr, 0, std::max(1.0f, fs() * 0.08f));
    const float size = std::round(fs() * 0.9f);
    trackedText(dl, ImVec2(rect.Min.x + padX(), rect.Min.y + (h - size) * 0.5f), size, color::kText, upper(title).c_str(),
                size * 0.07f);
    selOut = sel;
    return pressed;
}

// The value on the right of a row, with a dot when it is not the default.
void rowValue(const ImRect& rect, const char* value, bool changed) {
    std::string t = changed ? std::string("\xE2\x80\xA2 ") + value : std::string(value);  // U+2022 bullet
    const ImVec2 ts = ImGui::CalcTextSize(t.c_str());
    ImGui::GetWindowDrawList()->AddText(ImVec2(rect.Max.x - padX() - ts.x, rect.Min.y + (rect.GetHeight() - ts.y) * 0.5f),
                                        color::kText, t.c_str());
}

// Controller: left/right on the focused item; returns -1, 0, +1 (with key repeat).
int sideStep() {
    if (!ImGui::IsItemFocused()) return 0;
    // Left / right belong to the focused row (they change its value): ImGui's navigation reads them
    // only when nobody owns them, so it does not also move the focus sideways (to the header icons).
    const ImGuiID id = ImGui::GetItemID();
    for (const ImGuiKey k : {ImGuiKey_GamepadDpadLeft, ImGuiKey_GamepadDpadRight, ImGuiKey_GamepadLStickLeft,
                             ImGuiKey_GamepadLStickRight, ImGuiKey_LeftArrow, ImGuiKey_RightArrow})
        ImGui::SetKeyOwner(k, id);
    if (ImGui::IsKeyPressed(ImGuiKey_GamepadDpadLeft, true) || ImGui::IsKeyPressed(ImGuiKey_GamepadLStickLeft, true) ||
        ImGui::IsKeyPressed(ImGuiKey_LeftArrow, true))
        return -1;
    if (ImGui::IsKeyPressed(ImGuiKey_GamepadDpadRight, true) || ImGui::IsKeyPressed(ImGuiKey_GamepadLStickRight, true) ||
        ImGui::IsKeyPressed(ImGuiKey_RightArrow, true))
        return 1;
    return 0;
}

void tick() {
    if (port::settings().mobile.haptics) mobile::native::haptic(0);
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

void setNextDefault(double value) {
    sOverride = true;
    sOverrideValue = value;
}

bool checkbox(const char* label, bool* v) {
    ImRect rect;
    bool sel = false;
    bool pressed = row(label, rowHeight(), rect, sel);
    const int side = sideStep();
    if (side != 0 && ((side > 0) != *v)) pressed = true;  // right turns on, left turns off
    if (pressed) {
        *v = !*v;
        tick();
    }
    bool def = false;
    const bool known = defaultOf(v, def);
    rowValue(rect, *v ? "On" : "Off", known && def != *v);
    noteDefault(sel, known ? (def ? "On" : "Off") : "");
    return pressed;
}

bool combo(const char* label, int* current, const char* const items[], int count) {
    if (count <= 0) return false;
    ImRect rect;
    bool sel = false;
    const bool pressed = row(label, rowHeight(), rect, sel);
    int idx = std::clamp(*current, 0, count - 1);
    // A tap steps to the next value (Dusk: a click cycles); left/right on a controller step either way.
    int step = pressed ? 1 : sideStep();
    bool changed = false;
    if (step != 0) {
        idx = ((idx + step) % count + count) % count;
        *current = idx;
        changed = true;
        tick();
    }
    int def = 0;
    const bool known = defaultOf(current, def) && def >= 0 && def < count;
    rowValue(rect, items[idx] ? items[idx] : "", known && def != idx);
    noteDefault(sel, known && items[def] ? items[def] : "");
    return changed;
}

bool combo(const char* label, int* current, const char* zeroSeparated) {
    const char* items[64];
    int n = 0;
    for (const char* p = zeroSeparated; *p && n < 64; p += std::strlen(p) + 1) items[n++] = p;
    return combo(label, current, items, n);
}

namespace {
// Slider row: the same row as the others, value on the right, and a thin line along its bottom
// edge showing where the value sits. A sideways drag on the row changes it relative to where the
// finger went down (no jump to the finger); a swipe up or down scrolls the list instead. Works on
// a normalized position; `stepNorm` is one step of the controller's left/right.
bool sliderRow(const char* label, float& t, const char* valueText, bool notDefault, float stepNorm) {
    ownLine();
    const float h = rowHeight();
    const float w = ImGui::GetContentRegionAvail().x;
    const std::string title = visible(label);
    ImGui::PushID(label);
    ImGui::InvisibleButton("##slider", ImVec2(w, h));
    defaultFocus();
    ImGui::PopID();
    const ImRect rect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
    selectIfTouched(title);
    const ImGuiID id = ImGui::GetItemID();
    const bool sel = isSelected(id);
    const bool active = ImGui::IsItemActive();
    const ImGuiIO& io = ImGui::GetIO();
    const bool pointer = io.MousePos.x > -1e6f;
    if (ImGui::IsItemActivated()) sDragT0 = t;
    if (active) {
        sSliderActiveFrame = ImGui::GetFrameCount();
        const float dx = io.MousePos.x - io.MouseClickedPos[0].x, dy = io.MousePos.y - io.MouseClickedPos[0].y;
        if (sClaimedId != id && pointer && std::fabs(dx) > fs() * 0.25f && std::fabs(dx) >= std::fabs(dy)) sClaimedId = id;
    } else if (sClaimedId == id) {
        sClaimedId = 0;
    }
    const bool held = active && sClaimedId == id;
    if (held) sSliderHeldFrame = ImGui::GetFrameCount();

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float rr = std::round(h * 0.17f);
    dl->AddRectFilled(rect.Min, rect.Max, held ? color::kRowPressed : sel ? color::kRowSelected : color::kRow, rr);
    if (sel || held) dl->AddRect(rect.Min, rect.Max, color::kAccent, rr, 0, std::max(1.0f, fs() * 0.08f));
    const float size = std::round(fs() * 0.9f);
    trackedText(dl, ImVec2(rect.Min.x + padX(), rect.Min.y + (h - size) * 0.5f), size, color::kText, upper(title).c_str(),
                size * 0.07f);

    const float x0 = rect.Min.x + padX(), x1 = rect.Max.x - padX();
    bool changed = false;
    if (held && pointer) {
        const float nt = std::clamp(sDragT0 + (io.MousePos.x - io.MouseClickedPos[0].x) / std::max(x1 - x0, 1.0f), 0.0f, 1.0f);
        if (nt != t) {
            t = nt;
            changed = true;
        }
    }
    if (const int side = sideStep(); side != 0) {
        const float nt = std::clamp(t + float(side) * stepNorm, 0.0f, 1.0f);
        if (nt != t) {
            t = nt;
            changed = true;
            tick();
        }
    }
    rowValue(rect, valueText, notDefault);
    // Where the value sits: a hairline along the bottom of the row, brighter while selected.
    const float th = std::max(1.5f, fs() * 0.12f), y = rect.Max.y - th - std::max(1.0f, fs() * 0.1f);
    dl->AddRectFilled(ImVec2(x0, y), ImVec2(x1, y + th), color::kLine, th);
    dl->AddRectFilled(ImVec2(x0, y), ImVec2(x0 + (x1 - x0) * t, y + th),
                      sel || held ? color::kAccent : (color::kAccent & 0x00FFFFFFu) | (110u << 24), th);
    return changed;
}

float stepOf(const char* format) {
    const char* p = format ? std::strchr(format, '%') : nullptr;
    while (p && p[1] == '%') p = std::strchr(p + 2, '%');
    const char* dot = p ? std::strchr(p, '.') : nullptr;
    return dot ? std::pow(10.0f, -float(std::atoi(dot + 1))) : 1.0f;
}
}  // namespace

bool sliderFloat(const char* label, float* v, float min, float max, const char* format) {
    const float lo = std::min(min, max), hi = std::max(min, max), span = std::max(hi - lo, 1e-6f);
    float t = std::clamp((*v - lo) / span, 0.0f, 1.0f);
    const char* fmt = format ? format : "%.2f";
    char text[64];
    std::snprintf(text, sizeof(text), fmt, *v);
    const float step = stepOf(fmt);
    float def = 0;
    const bool known = defaultOf(v, def);
    const bool notDefault = known && std::fabs(def - *v) > step * 0.5f;
    bool sel = false;
    // Controller steps: one shown digit, but at most 100 presses across the range.
    const bool moved = sliderRow(label, t, text, notDefault, std::max(step / span, 0.01f));
    sel = isSelected(ImGui::GetItemID());
    if (known) {
        char d[64];
        std::snprintf(d, sizeof(d), fmt, def);
        noteDefault(sel, d);
    }
    if (!moved) return false;
    const float nv = std::clamp(std::round((lo + t * span) / step) * step, lo, hi);
    if (nv == *v) return false;
    *v = nv;
    return true;
}

bool sliderInt(const char* label, int* v, int min, int max, const char* format) {
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
    const bool moved = sliderRow(label, t, text, known && def != *v, std::max(1.0f / span, 0.01f));
    if (known) {
        char d[64];
        if (hasNumber) std::snprintf(d, sizeof(d), format, def);
        else std::snprintf(d, sizeof(d), "%d", def);
        noteDefault(isSelected(ImGui::GetItemID()), d);
    }
    if (!moved) return false;
    const int nv = std::clamp(lo + int(std::lround(t * span)), lo, hi);
    if (nv == *v) return false;
    *v = nv;
    return true;
}

void separatorText(const char* label) {
    ownLine();
    // Group heading (Dusk): small tracked capitals in the muted heading colour, air above.
    ImGui::Dummy(ImVec2(0, std::round(fs() * 0.55f)));
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float size = std::round(fs() * 0.78f);
    trackedText(ImGui::GetWindowDrawList(), ImVec2(p.x + std::round(fs() * 0.3f), p.y), size, color::kHeading,
                upper(visible(label)).c_str(), size * 0.12f);
    ImGui::Dummy(ImVec2(0, std::round(size + fs() * 0.35f)));
}

void itemTooltip(const char* fmt, va_list args) {
    // Any item (also plain ImGui buttons and combos) becomes the selection when touched.
    if (ImGui::IsItemActivated() || (ImGui::IsItemFocused() && navActive())) {
        if (ImGui::GetItemID() != sSelId) select(ImGui::GetItemID(), std::string());
    }
    if (ImGui::GetItemID() != sSelId || sNextSet) return;
    char buf[2048];
    std::vsnprintf(buf, sizeof(buf), fmt, args);
    // The PC tooltips are wrapped by hand for a mouse tooltip: let the column wrap them instead.
    std::string text(buf);
    for (size_t i = 0; i < text.size(); i++)
        if (text[i] == '\n') text[i] = (i + 1 < text.size() && text[i + 1] == '\n') ? '\n' : ' ';
    sNextTitle = sSelTitle;
    sNextText = text;
    sNextSet = true;
}

void beginFrame() {
    sDefaultDone = false;
    sOverride = false;
    if (sNextSet) {
        sDescTitle = sNextTitle;
        sDescText = sNextText;
    } else {
        sDescTitle = sSelId ? sSelTitle : std::string();  // selected without a tooltip: just its name
        sDescText.clear();
    }
    sDescDefault = sNextDefaultSet ? sNextDefault : std::string();
    sNextSet = sNextDefaultSet = false;
}

const char* descriptionTitle() { return sDescTitle.c_str(); }
const char* descriptionText() { return sDescText.c_str(); }
const char* descriptionDefault() { return sDescDefault.c_str(); }

bool sliderHeld() { return ImGui::GetCurrentContext() && ImGui::GetFrameCount() - sSliderHeldFrame <= 1; }
bool sliderTouched() { return ImGui::GetCurrentContext() && ImGui::GetFrameCount() - sSliderActiveFrame <= 1; }

void clearSelection() {
    sSelId = 0;
    sSelTitle.clear();
    sDescTitle.clear();
    sDescText.clear();
    sDescDefault.clear();
    sNextSet = sNextDefaultSet = false;
}

}  // namespace recomp::menu::phone
