// On-screen GameCube controller (docs/IOS.md "Touch controls"). The phone kit's behaviour (same in
// every port) with the GameCube's layout:
//  - left: Control Stick in an octagonal gate (fixed, or floating where the thumb lands), D-pad above;
//  - right: big A, small B to its lower left, kidney-shaped X (right) and Y (top) around A,
//    C-Stick to the lower left of the cluster;
//  - top: analog L and R triggers in the corners, Z under R, START and the menu button in the middle.
// Smoked-glass look drawn with ImGui (no image files): tinted bodies, rims, top shine, a glow and a
// small press-in on touch, haptic taps. Constant opacity (no idle dimming). Hidden while a physical
// controller is connected. Fingers that do not land on a control go to ImGui as a mouse (the F1
// menu, the disc page), so menus are fully usable by touch; a vertical drag scrolls.
//
// Press timing: a tap is seen by at least one PADRead even if the finger lifts before the game
// reads the pad (pulse bits). The D-pad sends one press per new direction, then, after 0.4 s of
// holding, repeat presses every 0.15 s (menus step once per tap, never twice).
#include "mobile/touch_pad.h"

#include "mobile/mobile.h"
#include "mobile/native.h"
#include "mobile/session_log.h"
#include "port/settings.h"
#include "recomp/menu.h"
#include "recomp/ui_scale.h"

#include <aurora/aurora.h>
#include <dolphin/pad.h>
#include <imgui.h>
#include <imgui_internal.h>
#include <SDL3/SDL.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace mobile::touch {
namespace {

constexpr float kPi = 3.14159265f;
constexpr float kRefHeight = 1080.0f;     // layout units: a 1080-high reference inside the safe area
constexpr SDL_FingerID kMouseFinger = ~SDL_FingerID(0);  // PC test mode: the mouse is one finger

using Clock = std::chrono::steady_clock;
double now() { return std::chrono::duration<double>(Clock::now().time_since_epoch()).count(); }

// ---- Shared with the guest thread ------------------------------------------------------------

std::atomic<u16> sHeld{0};     // buttons held by fingers
std::atomic<u16> sPulse{0};    // presses the next PADRead must see once (taps, D-pad steps)
std::atomic<int> sStickX{0}, sStickY{0}, sCX{0}, sCY{0};  // -100..100
std::atomic<int> sTrigL{0}, sTrigR{0};                    // 0..255
std::atomic<bool> sFeeding{false};

void press(u16 bit, bool down) {
    if (down) {
        sHeld.fetch_or(bit);
        sPulse.fetch_or(bit);
    } else {
        sHeld.fetch_and(u16(~bit));
    }
}

// ---- Colours ---------------------------------------------------------------------------------

struct Rgb {
    float r, g, b;
};
constexpr Rgb kBody{0.075f, 0.08f, 0.10f};  // smoked glass
constexpr Rgb kNeutral{0.92f, 0.94f, 1.0f};
constexpr Rgb kGreenA{0.16f, 0.74f, 0.52f};
constexpr Rgb kRedB{0.90f, 0.22f, 0.24f};
constexpr Rgb kGreyXY{0.78f, 0.80f, 0.86f};
constexpr Rgb kPurpleZ{0.50f, 0.36f, 0.92f};
constexpr Rgb kYellowC{1.0f, 0.80f, 0.16f};
constexpr Rgb kGrey{0.70f, 0.72f, 0.78f};

Rgb mix(Rgb a, Rgb b, float t) { return {a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t}; }
ImU32 col(Rgb c, float a) {
    return ImGui::GetColorU32(ImVec4(c.r, c.g, c.b, std::clamp(a, 0.0f, 1.0f)));
}

// ---- Drawing helpers -------------------------------------------------------------------------

// Filled circle with a vertical gradient (top colour to bottom colour): the glass body's light.
void gradientDisc(ImDrawList* dl, ImVec2 c, float r, ImU32 top, ImU32 bottom, int seg = 48) {
    const ImVec2 uv = dl->_Data->TexUvWhitePixel;
    dl->PrimReserve(seg * 3, seg + 1);
    const unsigned base = dl->_VtxCurrentIdx;
    const ImVec4 t = ImGui::ColorConvertU32ToFloat4(top), b = ImGui::ColorConvertU32ToFloat4(bottom);
    auto lerp = [&](float k) {
        return ImGui::ColorConvertFloat4ToU32(
            ImVec4(t.x + (b.x - t.x) * k, t.y + (b.y - t.y) * k, t.z + (b.z - t.z) * k, t.w + (b.w - t.w) * k));
    };
    dl->PrimWriteVtx(c, uv, lerp(0.5f));
    for (int i = 0; i < seg; i++) {
        const float a = 2.0f * kPi * float(i) / float(seg);
        dl->PrimWriteVtx(ImVec2(c.x + std::cos(a) * r, c.y + std::sin(a) * r), uv, lerp((std::sin(a) + 1.0f) * 0.5f));
    }
    for (int i = 0; i < seg; i++) {
        dl->PrimWriteIdx(ImDrawIdx(base));
        dl->PrimWriteIdx(ImDrawIdx(base + 1 + i));
        dl->PrimWriteIdx(ImDrawIdx(base + 1 + (i + 1) % seg));
    }
}

// Glossy highlight on the upper part of a disc: bright at the top, gone by the middle.
void shine(ImDrawList* dl, ImVec2 c, float r, float alpha) {
    if (alpha <= 0.0f) return;
    gradientDisc(dl, ImVec2(c.x, c.y - r * 0.18f), r * 0.78f, col({1, 1, 1}, alpha), col({1, 1, 1}, 0.0f));
}

// Soft drop shadow: a few widening, fading discs.
void shadow(ImDrawList* dl, ImVec2 c, float r, float alpha) {
    for (int i = 0; i < 4; i++)
        dl->AddCircleFilled(ImVec2(c.x, c.y + r * 0.06f), r * (1.02f + 0.06f * float(i)), col({0, 0, 0}, alpha * 0.22f), 40);
}

void label(ImDrawList* dl, ImVec2 c, float px, const char* text, ImU32 color) {
    ImFont* font = ImGui::GetFont();
    const ImVec2 sz = font->CalcTextSizeA(px, FLT_MAX, 0.0f, text);
    const ImVec2 p(c.x - sz.x * 0.5f, c.y - sz.y * 0.5f);
    dl->AddText(font, px, ImVec2(p.x, p.y + px * 0.05f), col({0, 0, 0}, 0.55f * (ImGui::ColorConvertU32ToFloat4(color).w)), text);
    dl->AddText(font, px, p, color, text);
}

// ---- Controls --------------------------------------------------------------------------------

enum class Kind { Round, Kidney, Trigger, Pill, DPad, Stick };
enum class Anchor { BL, BR, TL, TR, TC };

struct Control {
    Kind kind;
    const char* name;  // for the log
    u16 bit = 0;       // PAD_BUTTON_* / PAD_TRIGGER_*
    Anchor anchor;
    ImVec2 off;        // reference units from the anchor (x right, y up)
    float size;        // diameter (Round, DPad), radius (Stick, Kidney arc), width (Trigger, Pill)
    float size2 = 0;   // height (Trigger, Pill), stroke width (Kidney), arc start (deg) unused
    float arcFrom = 0, arcTo = 0;  // Kidney: arc in degrees, counter-clockwise from +x (y up)
    Rgb accent{1, 1, 1};
    bool tinted = false;  // body tinted with the accent (A, B, Z, C) instead of neutral glass
    const char* text = nullptr;
    bool face = false;    // A/B/X/Y: a finger slides from one to another
    bool cStick = false;
    bool menu = false;    // opens/closes the F1 menu (no game button)
    bool rightTrigger = false;

    // Layout (screen units, ImGui).
    ImVec2 c{};
    float r = 0, w = 0, h = 0, k = 1;
    ImVec2 home{};  // stick: fixed centre (floating sticks return here)
    // State.
    SDL_FingerID finger = 0;
    bool owned = false;
    bool pressed = false;
    float anim = 0;  // 0..1 press animation
    ImVec2 knob{};   // stick knob offset (screen units)
    // D-pad
    bool dir[4] = {};  // up, right, down, left
    double since[4] = {}, nextPulse[4] = {};
    float armAnim[4] = {};
};

std::vector<Control> sCtl;
float sLastScale = -1, sLastW = -1, sLastH = -1;
bool sLastFloating = false;
ImVec4 sSafe{};  // x, y, w, h (ImGui units)
ImVec4 sZone{};  // floating stick zone

bool sVisible = false;
bool sPadConnected = false;
std::string sPadName;
double sPadPoll = 0;

// ImGui mouse finger (fingers that miss every control).
bool sMouseDown = false;
SDL_FingerID sMouseFinger = 0;
ImVec2 sMouseStart{}, sMouseLast{};
bool sMouseScrolling = false;
std::map<SDL_FingerID, int> sOwner;  // finger -> control index
std::vector<SDL_FingerID> sIgnoreUntilUp;  // fingers whose control moved under them (layout rebuilt)

const port::MobileSettings& ms() { return port::settings().mobile; }

ImVec2 place(Anchor a, ImVec2 off, float k) {
    const float x0 = sSafe.x, y0 = sSafe.y, w = sSafe.z, h = sSafe.w;
    switch (a) {
    case Anchor::BL: return ImVec2(x0 + off.x * k, y0 + h - off.y * k);
    case Anchor::BR: return ImVec2(x0 + w + off.x * k, y0 + h - off.y * k);
    case Anchor::TL: return ImVec2(x0 + off.x * k, y0 - off.y * k);
    case Anchor::TR: return ImVec2(x0 + w + off.x * k, y0 - off.y * k);
    case Anchor::TC: return ImVec2(x0 + w * 0.5f + off.x * k, y0 - off.y * k);
    }
    return ImVec2(x0, y0);
}

void build() {
    // Changing Touch Size / Control Stick mode moves controls under a finger that is still down:
    // release everything and ignore those fingers until they lift.
    if (!sOwner.empty()) {
        for (const auto& [f, i] : sOwner) sIgnoreUntilUp.push_back(f);
        releaseAll();
        sessionlog::line("touch", "touch controls rebuilt: held controls released until the finger lifts");
    }
    sCtl.clear();
    auto add = [](Control c) { sCtl.push_back(c); };
    // Left hand
    add({.kind = Kind::Stick, .name = "Control Stick", .anchor = Anchor::BL, .off = {300, 275}, .size = 140, .accent = kGrey});
    add({.kind = Kind::DPad, .name = "D-pad", .anchor = Anchor::BL, .off = {250, 645}, .size = 200, .accent = kNeutral});
    // Right hand: A with B, X and Y around it, C-Stick lower left
    const ImVec2 a{-300, 310};
    add({.kind = Kind::Round, .name = "A", .bit = PAD_BUTTON_A, .anchor = Anchor::BR, .off = a, .size = 175, .accent = kGreenA,
         .tinted = true, .text = "A", .face = true});
    add({.kind = Kind::Round, .name = "B", .bit = PAD_BUTTON_B, .anchor = Anchor::BR, .off = {a.x - 150, a.y - 115}, .size = 105,
         .accent = kRedB, .tinted = true, .text = "B", .face = true});
    add({.kind = Kind::Kidney, .name = "X", .bit = PAD_BUTTON_X, .anchor = Anchor::BR, .off = a, .size = 145, .size2 = 66,
         .arcFrom = -38, .arcTo = 48, .accent = kGreyXY, .text = "X", .face = true});
    add({.kind = Kind::Kidney, .name = "Y", .bit = PAD_BUTTON_Y, .anchor = Anchor::BR, .off = a, .size = 145, .size2 = 66,
         .arcFrom = 92, .arcTo = 168, .accent = kGreyXY, .text = "Y", .face = true});
    add({.kind = Kind::Stick, .name = "C-Stick", .anchor = Anchor::BR, .off = {-640, 170}, .size = 88, .accent = kYellowC,
         .tinted = true, .text = "C", .cStick = true});
    // Shoulders
    add({.kind = Kind::Trigger, .name = "L", .bit = PAD_TRIGGER_L, .anchor = Anchor::TL, .off = {200, -95}, .size = 250, .size2 = 104,
         .accent = kGrey, .text = "L"});
    add({.kind = Kind::Trigger, .name = "R", .bit = PAD_TRIGGER_R, .anchor = Anchor::TR, .off = {-200, -95}, .size = 250, .size2 = 104,
         .accent = kGrey, .text = "R", .rightTrigger = true});
    add({.kind = Kind::Pill, .name = "Z", .bit = PAD_TRIGGER_Z, .anchor = Anchor::TR, .off = {-200, -222}, .size = 196, .size2 = 76,
         .accent = kPurpleZ, .tinted = true, .text = "Z"});
    // Middle
    add({.kind = Kind::Round, .name = "START", .bit = PAD_BUTTON_START, .anchor = Anchor::TC, .off = {70, -72}, .size = 84,
         .accent = kNeutral, .text = "START"});
    add({.kind = Kind::Round, .name = "Menu", .anchor = Anchor::TC, .off = {-70, -72}, .size = 76, .accent = kNeutral, .menu = true});

    const float k = sSafe.w / kRefHeight * float(ms().touchSize) / 100.0f;
    for (auto& c : sCtl) {
        c.k = k;
        c.c = place(c.anchor, c.off, k);
        c.home = c.c;
        switch (c.kind) {
        case Kind::Round:
        case Kind::DPad: c.r = c.size * 0.5f * k; break;
        case Kind::Stick:
        case Kind::Kidney: c.r = c.size * k; break;
        case Kind::Trigger:
        case Kind::Pill:
            c.w = c.size * k;
            c.h = c.size2 * k;
            break;
        }
    }
    // Floating Control Stick zone: the lower-left 40% x 58% of the safe area (invisible).
    sZone = ImVec4(sSafe.x, sSafe.y + sSafe.w * 0.42f, sSafe.z * 0.40f, sSafe.w * 0.58f);
    sLastScale = float(ms().touchSize);
    sLastFloating = ms().floatingStick;
    sessionlog::line("touch", "layout built: safe area %.0f,%.0f %.0fx%.0f, scale %.3f (Touch Size %d%%), Control Stick %s",
                     sSafe.x, sSafe.y, sSafe.z, sSafe.w, k, ms().touchSize, ms().floatingStick ? "floating" : "fixed");
}

void updateSafeArea() {
    const ImVec2 disp = ImGui::GetIO().DisplaySize;
    float l = 0, t = 0, r = 0, b = 0;
    safeAreaInsets(l, t, r, b);
    sSafe = ImVec4(l, t, std::max(disp.x - l - r, 1.0f), std::max(disp.y - t - b, 1.0f));
    if (sSafe.z != sLastW || sSafe.w != sLastH || float(ms().touchSize) != sLastScale || ms().floatingStick != sLastFloating) {
        sLastW = sSafe.z;
        sLastH = sSafe.w;
        build();
    }
}

// ---- Hit tests -------------------------------------------------------------------------------

float dist(ImVec2 a, ImVec2 b) { return std::hypot(a.x - b.x, a.y - b.y); }

bool hit(const Control& c, ImVec2 p, bool forStart) {
    switch (c.kind) {
    case Kind::Round: return dist(p, c.c) <= c.r * 1.18f;
    case Kind::DPad: return dist(p, c.c) <= c.r * 1.12f;
    case Kind::Stick:
        if (!c.cStick && ms().floatingStick && forStart)
            return p.x >= sZone.x && p.x <= sZone.x + sZone.z && p.y >= sZone.y && p.y <= sZone.y + sZone.w;
        return dist(p, c.home) <= c.r * 1.25f;
    case Kind::Kidney: {
        const float d = dist(p, c.c);
        const float half = c.size2 * c.k * 0.5f * 1.3f;
        if (std::fabs(d - c.r) > half) return false;
        const float ang = std::atan2(-(p.y - c.c.y), p.x - c.c.x) * 180.0f / kPi;  // y up
        const float margin = 8.0f;
        auto within = [&](float a) { return a >= c.arcFrom - margin && a <= c.arcTo + margin; };
        return within(ang) || within(ang + 360.0f) || within(ang - 360.0f);
    }
    case Kind::Trigger:
    case Kind::Pill:
        return std::fabs(p.x - c.c.x) <= c.w * 0.5f * 1.15f && std::fabs(p.y - c.c.y) <= c.h * 0.5f * 1.2f;
    }
    return false;
}

// ---- Input -----------------------------------------------------------------------------------

void haptic() {
    if (ms().haptics) native::haptic(0);
}

void setStick(Control& c, ImVec2 p) {
    ImVec2 d(p.x - c.c.x, p.y - c.c.y);
    const float len = std::hypot(d.x, d.y);
    if (len > c.r) d = ImVec2(d.x / len * c.r, d.y / len * c.r);
    c.knob = d;
    const int x = int(std::lround(d.x / c.r * 100.0f));
    const int y = int(std::lround(-d.y / c.r * 100.0f));  // up is positive on the pad
    (c.cStick ? sCX : sStickX).store(x);
    (c.cStick ? sCY : sStickY).store(y);
}

constexpr u16 kDirBits[4] = {PAD_BUTTON_UP, PAD_BUTTON_RIGHT, PAD_BUTTON_DOWN, PAD_BUTTON_LEFT};
constexpr const char* kDirNames[4] = {"up", "right", "down", "left"};
constexpr double kHoldDelay = 0.4, kRepeat = 0.15;
constexpr float kDiagonalFrom = 33.0f;  // degrees off a cardinal direction before a diagonal

void dpadDrag(Control& c, ImVec2 p) {
    const ImVec2 v(p.x - c.c.x, -(p.y - c.c.y));
    bool next[4] = {};
    if (std::hypot(v.x, v.y) > c.r * 0.18f) {
        float ang = std::atan2(v.y, v.x) * 180.0f / kPi;  // 0 right, 90 up
        if (ang < 0) ang += 360.0f;
        const int kq = int(std::lround(ang / 90.0f)) % 4;  // 0 right, 1 up, 2 left, 3 down
        static constexpr int kToSlot[4] = {1, 0, 3, 2};
        float off = ang - float(kq) * 90.0f;
        if (off > 180) off -= 360;
        if (off < -180) off += 360;
        next[kToSlot[kq]] = true;
        if (std::fabs(off) > kDiagonalFrom) next[kToSlot[(kq + (off > 0 ? 1 : 3)) % 4]] = true;
    }
    const double t = now();
    for (int i = 0; i < 4; i++) {
        if (next[i] && !c.dir[i]) {
            c.dir[i] = true;
            c.since[i] = t;
            c.nextPulse[i] = t + kHoldDelay;
            sPulse.fetch_or(kDirBits[i]);  // exactly one press per new direction
            haptic();
            sessionlog::line("touch", "dpad %s tap", kDirNames[i]);
        } else if (!next[i] && c.dir[i]) {
            c.dir[i] = false;
        }
    }
}

// Steady repeat while a D-pad direction stays held (host frame).
void dpadRepeat(Control& c) {
    const double t = now();
    for (int i = 0; i < 4; i++) {
        if (!c.dir[i] || t < c.nextPulse[i]) continue;
        if (c.nextPulse[i] == c.since[i] + kHoldDelay) sessionlog::line("touch", "dpad %s hold-repeat", kDirNames[i]);
        c.nextPulse[i] = t + kRepeat;
        sPulse.fetch_or(kDirBits[i]);
    }
}

void controlDown(int idx, SDL_FingerID f, ImVec2 p) {
    Control& c = sCtl[size_t(idx)];
    c.owned = true;
    c.finger = f;
    c.pressed = true;
    sOwner[f] = idx;
    switch (c.kind) {
    case Kind::Stick:
        if (!c.cStick && ms().floatingStick) c.c = p;
        setStick(c, p);
        break;
    case Kind::DPad: dpadDrag(c, p); break;
    case Kind::Trigger:
        press(c.bit, true);
        (c.rightTrigger ? sTrigR : sTrigL).store(255);
        haptic();
        break;
    default:
        if (c.menu) {
            haptic();
            break;  // acts on release
        }
        press(c.bit, true);
        haptic();
        break;
    }
    sessionlog::line("touch", "%s down", c.name);
}

void controlUp(int idx) {
    Control& c = sCtl[size_t(idx)];
    const bool was = c.pressed;
    c.owned = false;
    c.pressed = false;
    switch (c.kind) {
    case Kind::Stick:
        c.knob = ImVec2(0, 0);
        c.c = c.home;
        (c.cStick ? sCX : sStickX).store(0);
        (c.cStick ? sCY : sStickY).store(0);
        break;
    case Kind::DPad:
        for (bool& d : c.dir) d = false;
        break;
    case Kind::Trigger:
        press(c.bit, false);
        (c.rightTrigger ? sTrigR : sTrigL).store(0);
        break;
    default:
        if (c.menu) {
            if (was) {
                recomp::menu::toggle("iPhone");
                sessionlog::line("touch", "menu button: F1 menu %s", recomp::menu::isOpen() ? "opened" : "closed");
            }
            break;
        }
        press(c.bit, false);
        break;
    }
    sessionlog::line("touch", "%s up", c.name);
}

// Finger that missed every control: ImGui's mouse (F1 menu, disc page, notices). A vertical drag of
// more than a few points that does not start on an active widget (slider) scrolls instead of
// clicking.
void mouseDown(SDL_FingerID f, ImVec2 p) {
    if (sMouseDown) return;  // one mouse finger at a time
    ImGuiIO& io = ImGui::GetIO();
    sMouseDown = true;
    sMouseFinger = f;
    sMouseStart = sMouseLast = p;
    sMouseScrolling = false;
    io.AddMouseSourceEvent(ImGuiMouseSource_TouchScreen);
    io.AddMousePosEvent(p.x, p.y);
    io.AddMouseButtonEvent(0, true);
}

void mouseMove(ImVec2 p) {
    ImGuiIO& io = ImGui::GetIO();
    const float slop = 10.0f * recomp::ui::scale();
    if (!sMouseScrolling && std::fabs(p.y - sMouseStart.y) > slop && !ImGui::IsAnyItemActive()) {
        sMouseScrolling = true;
        // Lift the button away from the widget so nothing is clicked, then scroll where the finger is.
        io.AddMouseSourceEvent(ImGuiMouseSource_TouchScreen);
        io.AddMousePosEvent(-FLT_MAX, -FLT_MAX);
        io.AddMouseButtonEvent(0, false);
    }
    io.AddMouseSourceEvent(ImGuiMouseSource_TouchScreen);
    io.AddMousePosEvent(p.x, p.y);
    if (sMouseScrolling) {
        const float step = std::max(ImGui::GetFontSize() * 5.0f, 1.0f);  // ImGui scrolls about 5 lines per wheel unit
        io.AddMouseWheelEvent(0.0f, (p.y - sMouseLast.y) / step);
    }
    sMouseLast = p;
}

void mouseUp() {
    if (!sMouseDown) return;
    ImGuiIO& io = ImGui::GetIO();
    io.AddMouseSourceEvent(ImGuiMouseSource_TouchScreen);
    if (!sMouseScrolling) io.AddMouseButtonEvent(0, false);
    // Touch has no hover: park the pointer so nothing stays highlighted.
    io.AddMousePosEvent(-FLT_MAX, -FLT_MAX);
    sMouseDown = false;
    sMouseScrolling = false;
}

bool begin(SDL_FingerID f, ImVec2 p) {
    sIgnoreUntilUp.erase(std::remove(sIgnoreUntilUp.begin(), sIgnoreUntilUp.end(), f), sIgnoreUntilUp.end());
    if (sVisible) {
        const bool menuOpen = recomp::menu::isOpen();
        int found = -1;
        // Buttons before sticks (sticks and the floating zone are the largest targets).
        for (int pass = 0; pass < 2 && found < 0; pass++)
            for (size_t i = 0; i < sCtl.size(); i++) {
                const Control& c = sCtl[i];
                if (c.owned || (pass == 0) == (c.kind == Kind::Stick)) continue;
                if (menuOpen && !c.menu) continue;  // with the menu open only its button shows
                if (hit(c, p, true)) {
                    found = int(i);
                    break;
                }
            }
        if (found >= 0) {
            controlDown(found, f, p);
            return true;
        }
    }
    mouseDown(f, p);
    return false;
}

bool move(SDL_FingerID f, ImVec2 p) {
    if (std::find(sIgnoreUntilUp.begin(), sIgnoreUntilUp.end(), f) != sIgnoreUntilUp.end()) return true;
    const auto it = sOwner.find(f);
    if (it == sOwner.end()) {
        if (sMouseDown && sMouseFinger == f) mouseMove(p);
        return false;
    }
    Control& c = sCtl[size_t(it->second)];
    switch (c.kind) {
    case Kind::Stick: setStick(c, p); break;
    case Kind::DPad: dpadDrag(c, p); break;
    case Kind::Round:
    case Kind::Kidney:
        // Sliding onto another face button moves the press there (A -> B for a quick follow-up).
        if (c.face && !hit(c, p, false)) {
            for (size_t i = 0; i < sCtl.size(); i++) {
                Control& o = sCtl[i];
                if (!o.face || o.owned || !hit(o, p, false)) continue;
                const int from = it->second;
                controlUp(from);
                sOwner.erase(f);
                controlDown(int(i), f, p);
                break;
            }
        }
        break;
    default: break;
    }
    return true;
}

bool end(SDL_FingerID f) {
    const bool ignored = std::find(sIgnoreUntilUp.begin(), sIgnoreUntilUp.end(), f) != sIgnoreUntilUp.end();
    sIgnoreUntilUp.erase(std::remove(sIgnoreUntilUp.begin(), sIgnoreUntilUp.end(), f), sIgnoreUntilUp.end());
    const auto it = sOwner.find(f);
    if (it == sOwner.end()) {
        if (sMouseDown && sMouseFinger == f) mouseUp();
        return ignored;
    }
    const int idx = it->second;
    sOwner.erase(it);
    controlUp(idx);
    return true;
}

// ---- Painting --------------------------------------------------------------------------------

void paintRound(ImDrawList* dl, const Control& c, float a) {
    const float p = c.anim;
    const float r = c.r * (1.0f - 0.08f * p);
    const Rgb body = c.tinted ? mix(kBody, c.accent, 0.42f) : kBody;
    shadow(dl, c.c, r, 0.55f * a * (1.0f - 0.4f * p));
    if (p > 0.01f) dl->AddCircle(c.c, r * 1.12f, col(c.accent, 0.8f * p * std::max(a, 0.6f)), 48, r * 0.16f);
    gradientDisc(dl, c.c, r, col(mix(body, {1, 1, 1}, 0.10f + 0.12f * p), (0.82f + 0.12f * p) * a + 0.05f),
                 col(body, (0.80f + 0.12f * p) * a + 0.05f));
    if (p > 0.01f) dl->AddCircleFilled(c.c, r, col(c.accent, 0.32f * p), 48);
    shine(dl, c.c, r, 0.16f * a * (1.0f - 0.5f * p));
    const Rgb rim = mix({1, 1, 1}, c.accent, c.tinted ? 0.45f : 0.1f);
    dl->AddCircle(c.c, r, col(rim, (0.55f + 0.45f * p) * a + 0.12f), 48, std::max(1.5f, r * 0.045f));
    const float la = std::clamp(a * 1.25f + 0.2f, 0.0f, 1.0f);
    if (c.menu) {
        // three bars
        const float bw = r * 0.46f, bh = std::max(2.0f, r * 0.075f);
        for (int i = -1; i <= 1; i++)
            dl->AddRectFilled(ImVec2(c.c.x - bw, c.c.y + i * r * 0.26f - bh), ImVec2(c.c.x + bw, c.c.y + i * r * 0.26f + bh),
                              col(kNeutral, la), bh);
    } else if (c.text) {
        const bool small = std::strlen(c.text) > 1;
        label(dl, c.c, r * (small ? 0.42f : 1.0f), c.text, col(c.tinted ? Rgb{1, 1, 1} : kNeutral, la));
    }
}

void paintKidney(ImDrawList* dl, const Control& c, float a) {
    const float p = c.anim;
    const float th = c.size2 * c.k * (1.0f - 0.08f * p);
    const float a0 = -c.arcTo * kPi / 180.0f, a1 = -c.arcFrom * kPi / 180.0f;  // screen space (y down)
    auto arc = [&](float radius, ImU32 color, float thickness) {
        dl->PathArcTo(c.c, radius, a0, a1, 36);
        dl->PathStroke(color, 0, thickness);
    };
    auto ends = [&](float rr, ImU32 color, bool filled, float thickness = 1.0f) {
        for (float ang : {a0, a1}) {
            const ImVec2 e(c.c.x + std::cos(ang) * c.r, c.c.y + std::sin(ang) * c.r);
            if (filled) dl->AddCircleFilled(e, rr, color, 24);
            else dl->AddCircle(e, rr, color, 24, thickness);
        }
    };
    // shadow, body, glow, shine, rim
    arc(c.r, col({0, 0, 0}, 0.35f * a), th * 1.18f);
    ends(th * 0.59f, col({0, 0, 0}, 0.35f * a), true);
    if (p > 0.01f) {
        arc(c.r, col(c.accent, 0.55f * p * std::max(a, 0.6f)), th * 1.32f);
        ends(th * 0.66f, col(c.accent, 0.55f * p * std::max(a, 0.6f)), true);
    }
    const ImU32 body = col(mix(kBody, {1, 1, 1}, 0.06f + 0.12f * p), (0.84f + 0.1f * p) * a + 0.05f);
    arc(c.r, body, th);
    ends(th * 0.5f, body, true);
    arc(c.r + th * 0.18f, col({1, 1, 1}, 0.10f * a), th * 0.28f);
    const float rimW = std::max(1.5f, th * 0.06f);
    const ImU32 rim = col({1, 1, 1}, (0.55f + 0.45f * p) * a + 0.12f);
    arc(c.r + th * 0.5f, rim, rimW);
    arc(c.r - th * 0.5f, rim, rimW);
    ends(th * 0.5f, rim, false, rimW);
    const float mid = -(c.arcFrom + c.arcTo) * 0.5f * kPi / 180.0f;
    label(dl, ImVec2(c.c.x + std::cos(mid) * c.r, c.c.y + std::sin(mid) * c.r), th * 0.62f, c.text,
          col(kNeutral, std::clamp(a * 1.25f + 0.2f, 0.0f, 1.0f)));
}

void paintRect(ImDrawList* dl, const Control& c, float a, bool trigger) {
    const float p = c.anim;
    const float s = 1.0f - 0.06f * p;
    const float w = c.w * s, h = c.h * s;
    const ImVec2 mn(c.c.x - w * 0.5f, c.c.y - h * 0.5f), mx(c.c.x + w * 0.5f, c.c.y + h * 0.5f);
    const float round = trigger ? h * 0.32f : h * 0.5f;
    const Rgb body = c.tinted ? mix(kBody, c.accent, 0.42f) : kBody;
    for (int i = 0; i < 3; i++) {
        const float g = h * 0.05f * float(i + 1);
        dl->AddRectFilled(ImVec2(mn.x - g, mn.y - g + h * 0.06f), ImVec2(mx.x + g, mx.y + g + h * 0.06f), col({0, 0, 0}, 0.16f * a),
                          round + g);
    }
    if (p > 0.01f)
        dl->AddRect(ImVec2(mn.x - h * 0.08f, mn.y - h * 0.08f), ImVec2(mx.x + h * 0.08f, mx.y + h * 0.08f),
                    col(c.accent, 0.7f * p * std::max(a, 0.6f)), round + h * 0.08f, 0, h * 0.14f);
    dl->AddRectFilledMultiColor(mn, mx, col(mix(body, {1, 1, 1}, 0.12f + 0.12f * p), (0.84f + 0.1f * p) * a + 0.05f),
                                col(mix(body, {1, 1, 1}, 0.12f + 0.12f * p), (0.84f + 0.1f * p) * a + 0.05f),
                                col(body, (0.80f + 0.1f * p) * a + 0.05f), col(body, (0.80f + 0.1f * p) * a + 0.05f));
    // (AddRectFilledMultiColor has square corners: round them off with the body colour's rim pass.)
    dl->AddRect(mn, mx, col(mix({1, 1, 1}, c.accent, c.tinted ? 0.45f : 0.1f), (0.55f + 0.45f * p) * a + 0.12f), round, 0,
                std::max(1.5f, h * 0.05f));
    if (trigger) {
        // analog travel: a bar along the bottom fills while held
        const float fill = (c.rightTrigger ? sTrigR : sTrigL).load() / 255.0f;
        if (fill > 0)
            dl->AddRectFilled(ImVec2(mn.x + round, mx.y - h * 0.16f), ImVec2(mn.x + round + (w - 2 * round) * fill, mx.y - h * 0.08f),
                              col(c.accent, 0.8f * a + 0.1f), h * 0.04f);
    }
    label(dl, c.c, h * 0.5f, c.text, col(c.tinted ? Rgb{1, 1, 1} : kNeutral, std::clamp(a * 1.25f + 0.2f, 0.0f, 1.0f)));
}

void paintDPad(ImDrawList* dl, Control& c, float a) {
    const float r = c.r;
    const float arm = r * 0.30f;  // half width of each bar
    shadow(dl, c.c, r * 1.08f, 0.35f * a);
    gradientDisc(dl, c.c, r * 1.10f, col(mix(kBody, {1, 1, 1}, 0.06f), 0.42f * a + 0.03f), col(kBody, 0.42f * a + 0.03f));
    dl->AddCircle(c.c, r * 1.10f, col({1, 1, 1}, 0.30f * a + 0.05f), 48, std::max(1.5f, r * 0.03f));
    const ImU32 body = col(mix(kBody, {1, 1, 1}, 0.08f), 0.85f * a + 0.06f);
    const float round = arm * 0.35f;
    dl->AddRectFilled(ImVec2(c.c.x - arm, c.c.y - r * 0.92f), ImVec2(c.c.x + arm, c.c.y + r * 0.92f), body, round);
    dl->AddRectFilled(ImVec2(c.c.x - r * 0.92f, c.c.y - arm), ImVec2(c.c.x + r * 0.92f, c.c.y + arm), body, round);
    const float dt = ImGui::GetIO().DeltaTime;
    for (int i = 0; i < 4; i++) {
        c.armAnim[i] = c.dir[i] ? std::min(1.0f, c.armAnim[i] + dt * 14.0f) : std::max(0.0f, c.armAnim[i] - dt * 14.0f);
        const float ang = (-90.0f + 90.0f * float(i)) * kPi / 180.0f;  // up, right, down, left (screen)
        const ImVec2 d(std::cos(ang), std::sin(ang));
        const ImVec2 n(-d.y, d.x);
        if (c.armAnim[i] > 0.01f) {
            const ImVec2 p0(c.c.x + d.x * arm * 1.1f, c.c.y + d.y * arm * 1.1f), p1(c.c.x + d.x * r * 0.92f, c.c.y + d.y * r * 0.92f);
            dl->AddRectFilled(ImVec2(std::min(p0.x, p1.x) - std::fabs(n.x) * arm, std::min(p0.y, p1.y) - std::fabs(n.y) * arm),
                              ImVec2(std::max(p0.x, p1.x) + std::fabs(n.x) * arm, std::max(p0.y, p1.y) + std::fabs(n.y) * arm),
                              col({1, 1, 1}, 0.30f * c.armAnim[i]), round);
        }
        // arrow
        const float s = r * 0.13f * (1.0f + 0.12f * c.armAnim[i]);
        const ImVec2 tip(c.c.x + d.x * r * 0.72f, c.c.y + d.y * r * 0.72f);
        const ImVec2 back(tip.x - d.x * s * 1.5f, tip.y - d.y * s * 1.5f);
        dl->AddTriangleFilled(tip, ImVec2(back.x + n.x * s, back.y + n.y * s), ImVec2(back.x - n.x * s, back.y - n.y * s),
                              col({1, 1, 1}, std::clamp((0.75f + 0.25f * c.armAnim[i]) * a * 1.2f + 0.1f, 0.0f, 1.0f)));
    }
    const ImU32 rim = col({1, 1, 1}, 0.55f * a + 0.12f);
    const float rw = std::max(1.5f, r * 0.025f);
    dl->AddRect(ImVec2(c.c.x - arm, c.c.y - r * 0.92f), ImVec2(c.c.x + arm, c.c.y + r * 0.92f), rim, round, 0, rw);
    dl->AddRect(ImVec2(c.c.x - r * 0.92f, c.c.y - arm), ImVec2(c.c.x + r * 0.92f, c.c.y + arm), rim, round, 0, rw);
    dl->AddCircleFilled(c.c, arm * 0.55f, col({0.25f, 0.26f, 0.3f}, 0.8f * a), 24);
}

void paintStick(ImDrawList* dl, Control& c, float a) {
    const float p = c.anim;
    const bool floatingIdle = !c.cStick && ms().floatingStick && !c.pressed;
    const float baseA = floatingIdle ? a * 0.7f : a;
    const float r = c.r;
    shadow(dl, c.c, r * 1.1f, 0.35f * baseA);
    gradientDisc(dl, c.c, r, col(mix(kBody, {1, 1, 1}, 0.10f), 0.55f * baseA + 0.06f), col(kBody, 0.55f * baseA + 0.06f));
    dl->AddCircle(c.c, r, col({1, 1, 1}, 0.45f * baseA + 0.06f), 48, std::max(1.5f, r * 0.03f));
    // The gate: an octagon (Control Stick) or a circle (C-Stick), as on the controller.
    const ImU32 gate = col(c.cStick ? c.accent : Rgb{1, 1, 1}, 0.45f * baseA + 0.05f);
    if (!c.cStick) {
        for (int i = 0; i < 8; i++) {
            const float a0 = (22.5f + 45.0f * float(i)) * kPi / 180.0f, a1 = (22.5f + 45.0f * float(i + 1)) * kPi / 180.0f;
            dl->PathLineTo(ImVec2(c.c.x + std::cos(a0) * r * 0.80f, c.c.y + std::sin(a0) * r * 0.80f));
            if (i == 7) dl->PathLineTo(ImVec2(c.c.x + std::cos(a1) * r * 0.80f, c.c.y + std::sin(a1) * r * 0.80f));
        }
        dl->PathStroke(gate, ImDrawFlags_Closed, std::max(1.5f, r * 0.025f));
    } else {
        dl->AddCircle(c.c, r * 0.80f, gate, 48, std::max(1.5f, r * 0.025f));
    }
    // Knob follows the finger; springs back when released.
    if (!c.pressed) {
        const float t = std::clamp(ImGui::GetIO().DeltaTime * 22.0f, 0.0f, 1.0f);
        c.knob = ImVec2(c.knob.x * (1.0f - t), c.knob.y * (1.0f - t));
    }
    const ImVec2 kc(c.c.x + c.knob.x, c.c.y + c.knob.y);
    const float kr = r * (c.cStick ? 0.50f : 0.55f) * (1.0f + 0.06f * p);
    shadow(dl, kc, kr, 0.6f * a);
    if (p > 0.01f) dl->AddCircle(kc, kr * 1.15f, col(c.cStick ? c.accent : Rgb{1, 1, 1}, 0.45f * p), 40, kr * 0.18f);
    const Rgb knob = c.cStick ? mix(kBody, c.accent, 0.75f) : Rgb{0.30f, 0.31f, 0.36f};
    gradientDisc(dl, kc, kr, col(mix(knob, {1, 1, 1}, 0.18f), 0.85f * a + 0.1f), col(mix(knob, {0, 0, 0}, 0.25f), 0.85f * a + 0.1f));
    if (!c.cStick) {
        // ridged thumb cap
        for (int i = 0; i < 12; i++) {
            const float ang = 2.0f * kPi * float(i) / 12.0f;
            dl->AddCircleFilled(ImVec2(kc.x + std::cos(ang) * kr * 0.62f, kc.y + std::sin(ang) * kr * 0.62f), kr * 0.07f,
                                col({0, 0, 0}, 0.25f * a), 10);
        }
    }
    shine(dl, kc, kr, 0.12f * a);
    dl->AddCircle(kc, kr, col({1, 1, 1}, (0.45f + 0.4f * p) * a + 0.08f), 40, std::max(1.5f, kr * 0.06f));
    if (c.text) label(dl, kc, kr * 0.9f, c.text, col({1, 1, 1}, std::clamp(a * 1.25f + 0.2f, 0.0f, 1.0f)));
}

void pollPhysicalPad() {
    const double t = now();
    if (t < sPadPoll) return;
    sPadPoll = t + 0.5;
    int count = 0;
    SDL_JoystickID* ids = SDL_GetGamepads(&count);
    std::string name;
    if (ids && count > 0)
        if (const char* n = SDL_GetGamepadNameForID(ids[0])) name = n;
    if (ids) SDL_free(ids);
    // LM_MOBILE_IGNORE_PADS=1 (test aid): keep the touch controller with a pad attached.
    static const bool sIgnore = std::getenv("LM_MOBILE_IGNORE_PADS") != nullptr;
    const bool connected = count > 0 && !sIgnore;
    if (connected != sPadConnected) {
        sPadConnected = connected;
        sPadName = name;
        sessionlog::line("touch", connected ? "physical controller connected (%s) -> hiding touch controls"
                                            : "physical controller disconnected -> showing touch controls",
                         name.c_str());
    }
}

}  // namespace

bool visible() { return sVisible; }
bool feeding() { return sFeeding.load(); }
const char* physicalController() { return sPadConnected ? sPadName.c_str() : nullptr; }

void releaseAll() {
    for (const auto& [f, idx] : sOwner) {
        if (sCtl[size_t(idx)].menu) sCtl[size_t(idx)].pressed = false;  // a cancelled tap opens nothing
        controlUp(idx);
    }
    sOwner.clear();
    for (auto& c : sCtl) {
        c.owned = false;
        c.pressed = false;
        for (bool& d : c.dir) d = false;
    }
    sHeld = 0;
    sStickX = sStickY = sCX = sCY = 0;
    sTrigL = sTrigR = 0;
    if (sMouseDown) mouseUp();
}

bool handleEvent(const SDL_Event& ev) {
    if (ImGui::GetCurrentContext() == nullptr) return false;
    const ImVec2 disp = ImGui::GetIO().DisplaySize;
    switch (ev.type) {
    case SDL_EVENT_FINGER_DOWN: return begin(ev.tfinger.fingerID, ImVec2(ev.tfinger.x * disp.x, ev.tfinger.y * disp.y));
    case SDL_EVENT_FINGER_MOTION: return move(ev.tfinger.fingerID, ImVec2(ev.tfinger.x * disp.x, ev.tfinger.y * disp.y));
    case SDL_EVENT_FINGER_UP:
    case SDL_EVENT_FINGER_CANCELED: return end(ev.tfinger.fingerID);
    default: break;
    }
    // PC test mode: the left mouse button is a finger on the controls (ImGui already sees the mouse).
    if (!native::isIOS()) {
        if (ev.type == SDL_EVENT_MOUSE_BUTTON_DOWN && ev.button.button == SDL_BUTTON_LEFT) {
            const ImVec2 p(ev.button.x, ev.button.y);
            for (size_t i = 0; i < sCtl.size() && sVisible; i++)
                if (!sCtl[i].owned && hit(sCtl[i], p, true) && (!recomp::menu::isOpen() || sCtl[i].menu)) {
                    controlDown(int(i), kMouseFinger, p);
                    return true;
                }
        } else if (ev.type == SDL_EVENT_MOUSE_MOTION && sOwner.count(kMouseFinger)) {
            return move(kMouseFinger, ImVec2(ev.motion.x, ev.motion.y));
        } else if (ev.type == SDL_EVENT_MOUSE_BUTTON_UP && ev.button.button == SDL_BUTTON_LEFT && sOwner.count(kMouseFinger)) {
            return end(kMouseFinger);
        }
    }
    return false;
}

void draw() {
    pollPhysicalPad();
    updateSafeArea();
    const bool show = ms().touchControls && !sPadConnected;
    if (show != sVisible) {
        sVisible = show;
        if (!show) releaseAll();
        sessionlog::line("touch", "touch controls %s", show ? "shown" : sPadConnected ? "hidden (controller)" : "hidden (switched off)");
    }
    const bool menuOpen = recomp::menu::isOpen();
    sFeeding = sVisible && !menuOpen;
    // Without the on-screen controls (controller connected, or switched off) a phone still needs a
    // way into the F1 menu: the menu button stays.
    const float a = std::clamp(float(ms().touchOpacity) / 100.0f, 0.1f, 1.0f);
    const float dt = ImGui::GetIO().DeltaTime;
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    for (auto& c : sCtl) {
        if (c.kind == Kind::DPad) dpadRepeat(c);
        c.anim = c.pressed ? std::min(1.0f, c.anim + dt * 18.0f) : std::max(0.0f, c.anim - dt * 9.0f);
        if (menuOpen && !c.menu) continue;
        if (!sVisible && !c.menu) continue;
        const float alpha = c.menu && (menuOpen || !sVisible) ? std::max(a, 0.85f) : a;
        switch (c.kind) {
        case Kind::Round: paintRound(dl, c, alpha); break;
        case Kind::Kidney: paintKidney(dl, c, alpha); break;
        case Kind::Trigger: paintRect(dl, c, alpha, true); break;
        case Kind::Pill: paintRect(dl, c, alpha, false); break;
        case Kind::DPad: paintDPad(dl, c, alpha); break;
        case Kind::Stick: paintStick(dl, c, alpha); break;
        }
    }
}

void applyPad(PADStatus* st) {
    if (!sFeeding.load()) return;
    PADStatus& s = st[0];
    const u16 buttons = u16(sHeld.load() | sPulse.exchange(0));
    // No physical controller on port 1 reads as "no controller": the touch controller is one.
    s.err = PAD_ERR_NONE;
    s.button |= buttons;
    auto stick = [](s8& x, s8& y, int tx, int ty) {
        if (tx * tx + ty * ty > int(x) * int(x) + int(y) * int(y)) {
            x = s8(std::clamp(tx, -127, 127));
            y = s8(std::clamp(ty, -127, 127));
        }
    };
    stick(s.stickX, s.stickY, sStickX.load(), sStickY.load());
    stick(s.substickX, s.substickY, sCX.load(), sCY.load());
    s.triggerLeft = u8(std::max<int>(s.triggerLeft, sTrigL.load()));
    s.triggerRight = u8(std::max<int>(s.triggerRight, sTrigR.load()));
    if (buttons & PAD_BUTTON_A) s.analogA = 255;
    if (buttons & PAD_BUTTON_B) s.analogB = 255;
}

}  // namespace mobile::touch
