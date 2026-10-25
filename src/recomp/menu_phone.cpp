// Phone look of the F1 menu's widgets (menu_phone.h): moonlit-mansion rows after Dusk's menus.
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

constexpr ImU32 kAccent = IM_COL32(242, 197, 92, 255);
constexpr ImU32 kAccentSoft = IM_COL32(242, 197, 92, 56);
constexpr ImU32 kAccentFaint = IM_COL32(242, 197, 92, 10);
constexpr ImU32 kLavender = IM_COL32(143, 134, 179, 255);
constexpr ImU32 kLavenderDim = IM_COL32(143, 134, 179, 120);
constexpr ImU32 kKnob = IM_COL32(255, 243, 207, 255);
constexpr ImU32 kInk = IM_COL32(26, 20, 36, 255);
constexpr ImU32 kControlBg = IM_COL32(17, 16, 30, 170);

// The selected item (tapped, or focused with a controller): its tooltip goes to the strip.
ImGuiID sSelId = 0;
std::string sSelTitle;
std::string sDescTitle, sDescText;            // shown this frame (from last frame)
std::string sNextTitle, sNextText;            // collected this frame
bool sNextSet = false;
int sSliderHeldFrame = -10;    // ImGui frame a phone slider last had the finger (dragged sideways)
int sSliderActiveFrame = -10;  // ImGui frame a finger was last down on a phone slider
ImGuiID sClaimedId = 0;        // the slider the finger is dragging

// The text before "##" (ImGui's ID suffix).
std::string visible(const char* label) {
    const char* hash = std::strstr(label, "##");
    return hash ? std::string(label, hash) : std::string(label);
}

// Phone rows are full width: one placed with SameLine() after another starts its own line instead
// (there is no room beside a full-width row; e.g. Debug > Font test's "Latin last line").
void ownLine() {
    if (ImGui::GetCurrentWindow()->DC.IsSameLine) ImGui::NewLine();
}

float rowHeight() { return std::round(ImGui::GetFrameHeight() * 1.55f); }

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

// Full-width row: an invisible button of the row's size, the selection highlight (accent wash and a
// bar on the left) and the label. Returns the pressed state; rect gets the row.
bool row(const char* label, float h, ImRect& rect, bool& selectedOut) {
    ownLine();
    const float w = ImGui::GetContentRegionAvail().x;
    ImGui::PushID(label);
    const bool pressed = ImGui::InvisibleButton("##row", ImVec2(w, h), ImGuiButtonFlags_PressedOnClickRelease);
    ImGui::PopID();
    rect = ImRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
    const std::string title = visible(label);
    selectIfTouched(title);
    const bool sel = isSelected(ImGui::GetItemID());
    const bool held = ImGui::IsItemActive();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float r = ImGui::GetStyle().FrameRounding;
    if (sel || held) {
        dl->AddRectFilledMultiColor(rect.Min, rect.Max, held ? kAccentSoft : IM_COL32(242, 197, 92, 44), kAccentFaint,
                                    kAccentFaint, held ? kAccentSoft : IM_COL32(242, 197, 92, 44));
        dl->AddRectFilled(rect.Min, ImVec2(rect.Min.x + std::max(2.0f, r * 0.45f), rect.Max.y), kAccent);
    }
    const float pad = ImGui::GetStyle().FramePadding.x;
    const ImVec2 ts = ImGui::CalcTextSize(title.c_str());
    dl->AddText(ImVec2(rect.Min.x + pad + r * 0.6f, rect.Min.y + (h - ts.y) * 0.5f), ImGui::GetColorU32(ImGuiCol_Text),
                title.c_str());
    selectedOut = sel;
    return pressed;
}

// Controller: left/right on the focused item; returns -1, 0, +1 (with key repeat).
int sideStep() {
    if (!ImGui::IsItemFocused()) return 0;
    if (ImGui::IsKeyPressed(ImGuiKey_GamepadDpadLeft, true) || ImGui::IsKeyPressed(ImGuiKey_GamepadLStickLeft, true) ||
        ImGui::IsKeyPressed(ImGuiKey_LeftArrow, true))
        return -1;
    if (ImGui::IsKeyPressed(ImGuiKey_GamepadDpadRight, true) || ImGui::IsKeyPressed(ImGuiKey_GamepadLStickRight, true) ||
        ImGui::IsKeyPressed(ImGuiKey_RightArrow, true))
        return 1;
    return 0;
}

void arrow(ImDrawList* dl, ImVec2 c, float s, int dir, bool hot) {
    const ImU32 col = hot ? kAccent : IM_COL32(227, 223, 240, 230);
    const ImVec2 a(c.x + dir * s * 0.45f, c.y), b(c.x - dir * s * 0.35f, c.y - s * 0.5f), d(c.x - dir * s * 0.35f, c.y + s * 0.5f);
    dl->AddTriangleFilled(a, b, d, col);
}

// "◀ value ▶" on the right of a row; returns the step taken by the arrows (or 0).
int valueArrows(const ImRect& rect, float h, const char* value, bool pressed) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float pad = ImGui::GetStyle().FramePadding.x;
    const float box = h * 0.62f;
    const float valW = std::max(ImGui::CalcTextSize(value).x, h * 1.9f);
    const float right = rect.Max.x - pad;
    const ImRect next(ImVec2(right - box, rect.GetCenter().y - box * 0.5f), ImVec2(right, rect.GetCenter().y + box * 0.5f));
    const ImRect prev(ImVec2(next.Min.x - valW - box - pad, next.Min.y), ImVec2(next.Min.x - valW - pad, next.Max.y));
    const float rr = ImGui::GetStyle().FrameRounding * 0.8f;
    for (const ImRect* b : {&prev, &next}) {
        dl->AddRectFilled(b->Min, b->Max, kControlBg, rr);
        dl->AddRect(b->Min, b->Max, kLavenderDim, rr);
    }
    // A tap on an arrow (where the finger went down) steps that way; elsewhere on the row: forward.
    int step = 0;
    if (pressed) {
        const ImVec2 at = ImGui::GetIO().MouseClickedPos[0];
        const ImRect hitPrev(ImVec2(prev.Min.x - pad, rect.Min.y), ImVec2(prev.Max.x + pad * 0.5f, rect.Max.y));
        step = hitPrev.Contains(at) ? -1 : 1;
    }
    const bool held = ImGui::IsItemActive();
    const ImVec2 at = ImGui::GetIO().MouseClickedPos[0];
    arrow(dl, prev.GetCenter(), box * 0.42f, -1, held && prev.Contains(at));
    arrow(dl, next.GetCenter(), box * 0.42f, 1, held && !prev.Contains(at));
    const ImVec2 ts = ImGui::CalcTextSize(value);
    dl->AddText(ImVec2(prev.Max.x + (next.Min.x - prev.Max.x - ts.x) * 0.5f, rect.Min.y + (h - ts.y) * 0.5f),
                IM_COL32(207, 200, 232, 255), value);
    return step;
}

}  // namespace

bool checkbox(const char* label, bool* v) {
    const float h = rowHeight();
    ImRect rect;
    bool sel = false;
    bool pressed = row(label, h, rect, sel);
    const int side = sideStep();
    if (side != 0 && ((side > 0) != *v)) pressed = true;  // right turns on, left turns off
    if (pressed) *v = !*v;
    // On / Off pill on the right.
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float pad = ImGui::GetStyle().FramePadding.x;
    const float pw = h * 1.45f, ph = h * 0.58f;
    const ImVec2 mx(rect.Max.x - pad, rect.GetCenter().y + ph * 0.5f), mn(mx.x - pw, mx.y - ph);
    const float rr = ImGui::GetStyle().FrameRounding;
    if (*v) {
        dl->AddRectFilled(mn, mx, kAccent, rr);
    } else {
        dl->AddRectFilled(mn, mx, kControlBg, rr);
        dl->AddRect(mn, mx, kLavenderDim, rr);
    }
    const char* t = *v ? "On" : "Off";
    const ImVec2 ts = ImGui::CalcTextSize(t);
    dl->AddText(ImVec2((mn.x + mx.x - ts.x) * 0.5f, (mn.y + mx.y - ts.y) * 0.5f), *v ? kInk : IM_COL32(154, 147, 184, 255), t);
    if (pressed && port::settings().mobile.haptics) mobile::native::haptic(0);
    return pressed;
}

bool combo(const char* label, int* current, const char* const items[], int count) {
    if (count <= 0) return false;
    const float h = rowHeight();
    ImRect rect;
    bool sel = false;
    const bool pressed = row(label, h, rect, sel);
    const int idx = std::clamp(*current, 0, count - 1);
    int step = valueArrows(rect, h, items[idx] ? items[idx] : "", pressed);
    if (step == 0) step = sideStep();
    if (step == 0) return false;
    *current = ((idx + step) % count + count) % count;
    if (port::settings().mobile.haptics) mobile::native::haptic(0);
    return true;
}

bool combo(const char* label, int* current, const char* zeroSeparated) {
    const char* items[64];
    int n = 0;
    for (const char* p = zeroSeparated; *p && n < 64; p += std::strlen(p) + 1) items[n++] = p;
    return combo(label, current, items, n);
}

namespace {
// Two-line slider row: label and value on top, a thin rail with a glowing knob below. Works on a
// normalized position; `stepNorm` is one step of the controller's left/right.
bool sliderRow(const char* label, float& t, const char* valueText, float stepNorm) {
    ownLine();
    const float h = rowHeight();
    const float w = ImGui::GetContentRegionAvail().x;
    const std::string title = visible(label);
    ImGui::PushID(label);
    const ImVec2 start = ImGui::GetCursorScreenPos();
    const bool hit = ImGui::InvisibleButton("##slider", ImVec2(w, h * 1.45f));
    ImGui::PopID();
    const ImRect rect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
    selectIfTouched(title);
    const bool sel = isSelected(ImGui::GetItemID());
    const ImGuiID id = ImGui::GetItemID();
    const bool active = ImGui::IsItemActive();
    // A finger on the row takes the knob only once it moves sideways (or on a tap): a swipe up or
    // down that starts on a slider scrolls the list and leaves the value alone.
    const ImGuiIO& io = ImGui::GetIO();
    const bool pointer = io.MousePos.x > -1e6f;
    if (active) {
        sSliderActiveFrame = ImGui::GetFrameCount();
        const float dx = io.MousePos.x - io.MouseClickedPos[0].x, dy = io.MousePos.y - io.MouseClickedPos[0].y;
        if (sClaimedId != id && pointer && std::fabs(dx) > ImGui::GetFontSize() * 0.25f && std::fabs(dx) >= std::fabs(dy))
            sClaimedId = id;
    }
    const bool wasDragged = !active && sClaimedId == id;  // released after a drag: not a tap
    if (wasDragged) sClaimedId = 0;
    const bool held = active && sClaimedId == id;
    if (held) sSliderHeldFrame = ImGui::GetFrameCount();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float pad = ImGui::GetStyle().FramePadding.x, r = ImGui::GetStyle().FrameRounding;
    if (sel || held) {
        dl->AddRectFilledMultiColor(rect.Min, rect.Max, IM_COL32(242, 197, 92, 44), kAccentFaint, kAccentFaint, IM_COL32(242, 197, 92, 44));
        dl->AddRectFilled(rect.Min, ImVec2(rect.Min.x + std::max(2.0f, r * 0.45f), rect.Max.y), kAccent);
    }
    const float lineH = h * 0.8f;
    const ImVec2 ts = ImGui::CalcTextSize(title.c_str());
    dl->AddText(ImVec2(start.x + pad + r * 0.6f, start.y + (lineH - ts.y) * 0.5f), ImGui::GetColorU32(ImGuiCol_Text), title.c_str());
    const ImVec2 vs = ImGui::CalcTextSize(valueText);
    dl->AddText(ImVec2(rect.Max.x - pad - vs.x, start.y + (lineH - vs.y) * 0.5f), IM_COL32(207, 200, 232, 255), valueText);
    // Rail
    const float x0 = rect.Min.x + pad + r * 0.6f + h * 0.2f, x1 = rect.Max.x - pad - h * 0.2f;
    const float y = start.y + lineH + (rect.Max.y - start.y - lineH) * 0.45f;
    bool changed = false;
    // Dragging: the knob follows the finger. A tap (released without a scroll): the knob jumps there.
    const float fingerX = held && pointer ? io.MousePos.x : (hit && !wasDragged) ? io.MouseClickedPos[0].x : -FLT_MAX;
    if (fingerX > -1e6f) {
        const float nt = std::clamp((fingerX - x0) / std::max(x1 - x0, 1.0f), 0.0f, 1.0f);
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
        }
    }
    const float th = std::max(3.0f, h * 0.09f);
    dl->AddRectFilled(ImVec2(x0, y - th * 0.5f), ImVec2(x1, y + th * 0.5f), IM_COL32(143, 134, 179, 90), th);
    const float kx = x0 + (x1 - x0) * t;
    dl->AddRectFilled(ImVec2(x0, y - th * 0.5f), ImVec2(kx, y + th * 0.5f), kAccent, th);
    const float kr = h * (held ? 0.24f : 0.2f);
    dl->AddCircleFilled(ImVec2(kx, y), kr * 1.9f, IM_COL32(242, 197, 92, held ? 60 : 32), 32);
    dl->AddCircleFilled(ImVec2(kx, y), kr, kKnob, 32);
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
    char text[64];
    std::snprintf(text, sizeof(text), format ? format : "%.2f", *v);
    const float step = stepOf(format ? format : "%.2f");
    // Controller steps: one shown digit, but at most 100 presses across the range.
    const float stepNorm = std::max(step / span, 0.01f);
    if (!sliderRow(label, t, text, stepNorm)) return false;
    float nv = lo + t * span;
    nv = std::clamp(std::round(nv / step) * step, lo, hi);
    if (nv == *v) return false;
    *v = nv;
    return true;
}

bool sliderInt(const char* label, int* v, int min, int max, const char* format) {
    const int lo = std::min(min, max), hi = std::max(min, max);
    const float span = float(std::max(hi - lo, 1));
    float t = std::clamp(float(*v - lo) / span, 0.0f, 1.0f);
    char text[64];
    // (A slider's format may be a fixed label such as "Match window" without %d.)
    if (format && std::strstr(format, "%")) std::snprintf(text, sizeof(text), format, *v);
    else std::snprintf(text, sizeof(text), "%s", format ? format : "");
    if (!sliderRow(label, t, text, std::max(1.0f / span, 0.01f))) return false;
    const int nv = std::clamp(lo + int(std::lround(t * span)), lo, hi);
    if (nv == *v) return false;
    *v = nv;
    return true;
}

void separatorText(const char* label) {
    ownLine();
    // Section heading: small lavender capitals, a little air above (Dusk's group headings).
    ImGui::Dummy(ImVec2(0, ImGui::GetTextLineHeight() * 0.45f));
    std::string up = visible(label);
    for (char& ch : up) ch = char(std::toupper(static_cast<unsigned char>(ch)));
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float pad = ImGui::GetStyle().FramePadding.x;
    ImGui::GetWindowDrawList()->AddText(ImGui::GetFont(), ImGui::GetFontSize() * 0.82f, ImVec2(p.x + pad, p.y), kLavender, up.c_str());
    ImGui::Dummy(ImVec2(0, ImGui::GetFontSize() * 0.95f));
}

void itemTooltip(const char* fmt, va_list args) {
    // Any item (also plain ImGui buttons and combos) becomes the selection when touched.
    if (ImGui::IsItemActivated() || (ImGui::IsItemFocused() && navActive())) {
        if (ImGui::GetItemID() != sSelId) select(ImGui::GetItemID(), std::string());
    }
    if (ImGui::GetItemID() != sSelId || sNextSet) return;
    char buf[2048];
    std::vsnprintf(buf, sizeof(buf), fmt, args);
    // The PC tooltips are wrapped by hand for a mouse tooltip: let the strip wrap them instead.
    std::string text(buf);
    for (size_t i = 0; i < text.size(); i++)
        if (text[i] == '\n') text[i] = (i + 1 < text.size() && text[i + 1] == '\n') ? '\n' : ' ';
    sNextTitle = sSelTitle;
    sNextText = text;
    sNextSet = true;
}

void beginFrame() {
    if (sNextSet) {
        sDescTitle = sNextTitle;
        sDescText = sNextText;
    } else if (sSelId == 0) {
        sDescTitle.clear();
        sDescText.clear();
    } else {
        // Selected, but it has no tooltip: just its name.
        sDescTitle = sSelTitle;
        sDescText.clear();
    }
    sNextSet = false;
}

const char* descriptionTitle() { return sDescTitle.c_str(); }
const char* descriptionText() { return sDescText.c_str(); }

bool sliderHeld() { return ImGui::GetCurrentContext() && ImGui::GetFrameCount() - sSliderHeldFrame <= 1; }
bool sliderTouched() { return ImGui::GetCurrentContext() && ImGui::GetFrameCount() - sSliderActiveFrame <= 1; }

void clearSelection() {
    sSelId = 0;
    sSelTitle.clear();
    sDescTitle.clear();
    sDescText.clear();
    sNextSet = false;
}

}  // namespace recomp::menu::phone
