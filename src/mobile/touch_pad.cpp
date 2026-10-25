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
#include "recomp/menu_phone.h"
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
// Moulded plastic in the GameCube controller's palette: teal-green A, red B, light-grey kidney X/Y,
// indigo Z, yellow C-Stick, mid-grey stick, D-pad, triggers and START (original drawings, no
// artwork from the console).

struct Rgb {
    float r, g, b;
};
constexpr Rgb kWhite{1, 1, 1};
constexpr Rgb kBlack{0, 0, 0};
constexpr Rgb kGreenA{0.10f, 0.62f, 0.47f};
constexpr Rgb kRedB{0.84f, 0.16f, 0.19f};
constexpr Rgb kGreyXY{0.80f, 0.81f, 0.85f};
constexpr Rgb kIndigoZ{0.37f, 0.29f, 0.78f};
constexpr Rgb kYellowC{0.98f, 0.77f, 0.10f};
constexpr Rgb kGreyBody{0.55f, 0.56f, 0.60f};
constexpr Rgb kGreyLight{0.70f, 0.71f, 0.75f};
constexpr Rgb kSocket{0.13f, 0.13f, 0.16f};

Rgb mix(Rgb a, Rgb b, float t) { return {a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t}; }
ImU32 col(Rgb c, float a) {
    return ImGui::GetColorU32(ImVec4(std::clamp(c.r, 0.0f, 1.0f), std::clamp(c.g, 0.0f, 1.0f), std::clamp(c.b, 0.0f, 1.0f),
                                     std::clamp(a, 0.0f, 1.0f)));
}
Rgb lighter(Rgb c, float t) { return mix(c, kWhite, t); }
Rgb darker(Rgb c, float t) { return mix(c, kBlack, t); }

// Light from the top left: a surface facing the light (outward normal n, screen y down) is
// brighter, one facing away darker. `relief` sets how strongly the shape is modelled.
Rgb shade(Rgb base, ImVec2 n, float relief = 1.0f) {
    const float d = (-n.x * 0.42f - n.y * 0.91f) * relief;  // -1..1
    return d >= 0 ? lighter(base, 0.42f * d) : darker(base, 0.38f * -d);
}

ImVec2 add(ImVec2 a, ImVec2 b) { return ImVec2(a.x + b.x, a.y + b.y); }
ImVec2 sub(ImVec2 a, ImVec2 b) { return ImVec2(a.x - b.x, a.y - b.y); }
ImVec2 mul(ImVec2 a, float k) { return ImVec2(a.x * k, a.y * k); }
ImVec2 norm(ImVec2 v) {
    const float l = std::hypot(v.x, v.y);
    return l > 1e-5f ? ImVec2(v.x / l, v.y / l) : ImVec2(0, -1);
}

// ---- Shapes ----------------------------------------------------------------------------------

using Pts = std::vector<ImVec2>;

Pts circlePts(ImVec2 c, float r, int seg = 48) {
    Pts p;
    for (int i = 0; i < seg; i++) {
        const float a = 2.0f * kPi * float(i) / float(seg);
        p.push_back(ImVec2(c.x + std::cos(a) * r, c.y + std::sin(a) * r));
    }
    return p;
}

// Rounded rectangle (convex), corners of radius rad.
Pts roundRectPts(ImVec2 c, float w, float h, float rad, int perCorner = 8) {
    rad = std::min(rad, std::min(w, h) * 0.5f);
    const float x0 = c.x - w * 0.5f + rad, x1 = c.x + w * 0.5f - rad, y0 = c.y - h * 0.5f + rad, y1 = c.y + h * 0.5f - rad;
    const ImVec2 centres[4] = {{x1, y1}, {x0, y1}, {x0, y0}, {x1, y0}};
    Pts p;
    for (int k = 0; k < 4; k++)
        for (int i = 0; i <= perCorner; i++) {
            const float a = (float(k) + float(i) / float(perCorner)) * kPi * 0.5f;
            p.push_back(ImVec2(centres[k].x + std::cos(a) * rad, centres[k].y + std::sin(a) * rad));
        }
    return p;
}

Pts offset(const Pts& in, ImVec2 d) {
    Pts p = in;
    for (auto& v : p) v = add(v, d);
    return p;
}

ImVec2 centroid(const Pts& p) {
    ImVec2 s(0, 0);
    for (const auto& v : p) s = add(s, v);
    return mul(s, 1.0f / float(std::max<size_t>(p.size(), 1)));
}

// Convex shape filled as moulded plastic: each rim vertex lit by its outward direction, the middle
// a little brighter (the dome), so the shape reads as a raised, rounded part.
void plastic(ImDrawList* dl, const Pts& p, Rgb base, float alpha, float relief = 1.0f, float dome = 0.10f) {
    if (p.size() < 3) return;
    const ImVec2 uv = dl->_Data->TexUvWhitePixel;
    const ImVec2 c = centroid(p);
    const int n = int(p.size());
    dl->PrimReserve(n * 3, n + 1);
    const unsigned base0 = dl->_VtxCurrentIdx;
    dl->PrimWriteVtx(add(c, ImVec2(0, 0)), uv, col(lighter(base, dome), alpha));
    for (const auto& v : p) dl->PrimWriteVtx(v, uv, col(shade(base, norm(sub(v, c)), relief), alpha));
    for (int i = 0; i < n; i++) {
        dl->PrimWriteIdx(ImDrawIdx(base0));
        dl->PrimWriteIdx(ImDrawIdx(base0 + 1 + i));
        dl->PrimWriteIdx(ImDrawIdx(base0 + 1 + (i + 1) % n));
    }
}

void flat(ImDrawList* dl, const Pts& p, ImU32 color) {
    if (p.size() >= 3) dl->AddConvexPolyFilled(p.data(), int(p.size()), color);
}

void outline(ImDrawList* dl, const Pts& p, ImU32 color, float w) {
    if (p.size() >= 2) dl->AddPolyline(p.data(), int(p.size()), color, ImDrawFlags_Closed, w);
}

// Soft drop shadow of a convex shape: a few widening, fading copies below it.
void dropShadow(ImDrawList* dl, const Pts& p, float depth, float alpha) {
    const ImVec2 c = centroid(p);
    for (int i = 3; i >= 0; i--) {
        Pts s = p;
        const float grow = 1.0f + 0.035f * float(i);
        for (auto& v : s) v = add(add(c, mul(sub(v, c), grow)), ImVec2(0, depth * (0.6f + 0.25f * float(i))));
        flat(dl, s, col(kBlack, alpha * (i == 0 ? 0.35f : 0.12f)));
    }
}

// ---- Lettering -------------------------------------------------------------------------------
// Bold rounded stroke letters drawn as thick lines (sharp at any size; the menu font is a small
// bitmap font). Glyphs in a unit box (x 0..1 across the letter, y 0..1 down), arcs as point runs.

struct Stroke {
    Pts pts;  // unit coordinates
};

void arcInto(Pts& out, float cx, float cy, float rx, float ry, float fromDeg, float toDeg, int seg = 12) {
    for (int i = 0; i <= seg; i++) {
        const float a = (fromDeg + (toDeg - fromDeg) * float(i) / float(seg)) * kPi / 180.0f;
        out.push_back(ImVec2(cx + std::cos(a) * rx, cy + std::sin(a) * ry));
    }
}

std::vector<Stroke> glyph(char ch) {
    std::vector<Stroke> g;
    auto line = [&](std::initializer_list<ImVec2> l) { g.push_back({Pts(l)}); };
    switch (ch) {
    case 'A': line({{0.0f, 1.0f}, {0.5f, 0.0f}, {1.0f, 1.0f}}); line({{0.22f, 0.66f}, {0.78f, 0.66f}}); break;
    case 'B': {
        Stroke s;
        s.pts = {{0.0f, 0.5f}, {0.0f, 0.0f}, {0.5f, 0.0f}};
        arcInto(s.pts, 0.5f, 0.25f, 0.42f, 0.25f, -90.0f, 90.0f);
        s.pts.push_back({0.0f, 0.5f});
        s.pts.push_back({0.55f, 0.5f});
        arcInto(s.pts, 0.55f, 0.75f, 0.45f, 0.25f, -90.0f, 90.0f);
        s.pts.push_back({0.0f, 1.0f});
        s.pts.push_back({0.0f, 0.5f});
        g.push_back(s);
        break;
    }
    case 'C': {
        Stroke s;
        arcInto(s.pts, 0.55f, 0.5f, 0.5f, 0.5f, -45.0f, -315.0f, 20);
        g.push_back(s);
        break;
    }
    case 'L': line({{0.0f, 0.0f}, {0.0f, 1.0f}, {0.9f, 1.0f}}); break;
    case 'R': {
        Stroke s;
        s.pts = {{0.0f, 1.0f}, {0.0f, 0.0f}, {0.5f, 0.0f}};
        arcInto(s.pts, 0.5f, 0.27f, 0.45f, 0.27f, -90.0f, 90.0f);
        s.pts.push_back({0.0f, 0.54f});
        g.push_back(s);
        line({{0.42f, 0.54f}, {0.95f, 1.0f}});
        break;
    }
    case 'S': {
        Stroke s;
        arcInto(s.pts, 0.5f, 0.26f, 0.46f, 0.26f, -20.0f, -270.0f, 14);
        arcInto(s.pts, 0.5f, 0.76f, 0.5f, 0.24f, -90.0f, 160.0f, 16);
        g.push_back(s);
        break;
    }
    case 'T': line({{0.0f, 0.0f}, {1.0f, 0.0f}}); line({{0.5f, 0.0f}, {0.5f, 1.0f}}); break;
    case 'X': line({{0.0f, 0.0f}, {1.0f, 1.0f}}); line({{1.0f, 0.0f}, {0.0f, 1.0f}}); break;
    case 'Y': line({{0.0f, 0.0f}, {0.5f, 0.5f}, {1.0f, 0.0f}}); line({{0.5f, 0.5f}, {0.5f, 1.0f}}); break;
    case 'Z': line({{0.0f, 0.0f}, {1.0f, 0.0f}, {0.0f, 1.0f}, {1.0f, 1.0f}}); break;
    default: break;
    }
    return g;
}

float glyphWidth(char ch) { return ch == 'L' ? 0.55f : ch == 'T' ? 0.62f : ch == 'S' ? 0.58f : 0.66f; }

// Text centred on c, cap height h, stroke width h * weight; raised: a darker copy below the letters
// and a lighter one above (moulded into the plastic).
void strokeText(ImDrawList* dl, ImVec2 c, float h, const char* text, Rgb face, float alpha, float weight = 0.17f,
                bool raised = true) {
    const float t = std::max(1.5f, h * weight);
    const float gap = h * 0.24f;
    float total = 0;
    for (const char* p = text; *p; p++) total += glyphWidth(*p) * h + (p[1] ? gap : 0.0f);
    // Each segment on its own with a round end at every point: a thick polyline gets mitred
    // (spiky) corners at sharp joins (the A's apex, the X's crossing, the Z's corners).
    const int roundSegs = std::clamp(int(t * 1.2f), 12, 40);
    const auto draw = [&](ImVec2 shift, ImU32 color, float width) {
        float x = c.x - total * 0.5f;
        for (const char* p = text; *p; p++) {
            const float w = glyphWidth(*p) * h;
            for (const auto& s : glyph(*p)) {
                Pts pts;
                for (const auto& u : s.pts) pts.push_back(ImVec2(x + u.x * w + shift.x, c.y - h * 0.5f + u.y * h + shift.y));
                for (size_t i = 0; i + 1 < pts.size(); i++) dl->AddLine(pts[i], pts[i + 1], color, width);
                for (const auto& v : pts) dl->AddCircleFilled(v, width * 0.5f, color, roundSegs);
            }
            x += w + gap;
        }
    };
    if (raised) draw(ImVec2(0, t * 0.40f), col(darker(face, 0.65f), alpha * 0.75f), t);  // moulded: a shade below
    draw(ImVec2(0, 0), col(face, alpha), t);
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
    float ripple = -1;  // seconds since the last press (the ring that spreads out), -1 = none
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
    add({.kind = Kind::Stick, .name = "Control Stick", .anchor = Anchor::BL, .off = {300, 275}, .size = 140, .accent = kGreyBody});
    add({.kind = Kind::DPad, .name = "D-pad", .anchor = Anchor::BL, .off = {250, 645}, .size = 200, .accent = kGreyBody});
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
         .accent = kGreyLight, .text = "L"});
    add({.kind = Kind::Trigger, .name = "R", .bit = PAD_TRIGGER_R, .anchor = Anchor::TR, .off = {-200, -95}, .size = 250, .size2 = 104,
         .accent = kGreyLight, .text = "R", .rightTrigger = true});
    add({.kind = Kind::Pill, .name = "Z", .bit = PAD_TRIGGER_Z, .anchor = Anchor::TR, .off = {-200, -222}, .size = 196, .size2 = 76,
         .accent = kIndigoZ, .tinted = true, .text = "Z"});
    // Middle
    add({.kind = Kind::Round, .name = "START", .bit = PAD_BUTTON_START, .anchor = Anchor::TC, .off = {70, -72}, .size = 84,
         .accent = kGreyLight, .text = "START"});
    add({.kind = Kind::Round, .name = "Menu", .anchor = Anchor::TC, .off = {-70, -72}, .size = 76, .accent = kGreyLight, .menu = true});

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

void haptic(int style = 0) {  // 0 light, 1 medium (the face buttons: a firmer click)
    if (ms().haptics) native::haptic(style);
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
    if (c.kind != Kind::Stick && c.kind != Kind::DPad) c.ripple = 0.0f;
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
        haptic(c.face ? 1 : 0);
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

// Finger that missed every control: ImGui's mouse (F1 menu, disc page, notices). A drag of more than
// a few points becomes a scroll (scrollFor: the menu moves its lists by the finger's own distance,
// then glides on after a flick), even when it started on a row; only a slider being dragged keeps
// the finger. (It used to become mouse-wheel steps, which ImGui feeds one per frame: laggy, and rows
// grabbed the finger, so only the scrollbar scrolled.)
ImVec2 sScrollAccum{}, sScrollVel{};
double sLastMoveTime = 0;
bool sFling = false;

void mouseDown(SDL_FingerID f, ImVec2 p) {
    if (sMouseDown) return;  // one mouse finger at a time
    ImGuiIO& io = ImGui::GetIO();
    sMouseDown = true;
    sMouseFinger = f;
    sMouseStart = sMouseLast = p;
    sMouseScrolling = false;
    sFling = false;  // a touch stops a glide
    sScrollVel = ImVec2(0, 0);
    sScrollAccum = ImVec2(0, 0);
    sLastMoveTime = now();
    io.AddMouseSourceEvent(ImGuiMouseSource_TouchScreen);
    io.AddMousePosEvent(p.x, p.y);
    io.AddMouseButtonEvent(0, true);
}

void mouseMove(ImVec2 p) {
    ImGuiIO& io = ImGui::GetIO();
    const float slop = 9.0f * recomp::ui::scale();
    const float dx = std::fabs(p.x - sMouseStart.x), dy = std::fabs(p.y - sMouseStart.y);
    const bool toSlider = recomp::menu::phone::sliderHeld() || (recomp::menu::phone::sliderTouched() && dx >= dy);
    if (!sMouseScrolling && !toSlider && (dy > slop || dx > slop)) {
        sMouseScrolling = true;
        // Lift the button away from the row so nothing is clicked.
        io.AddMouseSourceEvent(ImGuiMouseSource_TouchScreen);
        io.AddMousePosEvent(-FLT_MAX, -FLT_MAX);
        io.AddMouseButtonEvent(0, false);
        sLastMoveTime = now();
    }
    const double t = now();
    if (sMouseScrolling) {
        const ImVec2 d(p.x - sMouseLast.x, p.y - sMouseLast.y);
        sScrollAccum = ImVec2(sScrollAccum.x + d.x, sScrollAccum.y + d.y);
        const double dt = t - sLastMoveTime;
        if (dt > 0.0005) {
            const float k = 0.4f;  // smoothed finger speed (points per second) for the glide
            sScrollVel = ImVec2(sScrollVel.x + (float(d.x / dt) - sScrollVel.x) * k, sScrollVel.y + (float(d.y / dt) - sScrollVel.y) * k);
        }
    } else {
        io.AddMouseSourceEvent(ImGuiMouseSource_TouchScreen);
        io.AddMousePosEvent(p.x, p.y);
    }
    sLastMoveTime = t;
    sMouseLast = p;
}

void mouseUp() {
    if (!sMouseDown) return;
    ImGuiIO& io = ImGui::GetIO();
    io.AddMouseSourceEvent(ImGuiMouseSource_TouchScreen);
    if (!sMouseScrolling) io.AddMouseButtonEvent(0, false);
    // Touch has no hover: park the pointer so nothing stays highlighted.
    io.AddMousePosEvent(-FLT_MAX, -FLT_MAX);
    // A flick glides on; a finger that stopped before lifting does not.
    sFling = sMouseScrolling && now() - sLastMoveTime < 0.08 && std::hypot(sScrollVel.x, sScrollVel.y) > 60.0f;
    if (!sFling) sScrollVel = ImVec2(0, 0);
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
                if (menuOpen) continue;  // the menu has its own "Back to game" button
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
// Each control: a soft drop shadow, the part's darker side wall (its height), the lit plastic top
// lifted above it, a crisp edge, a glint, raised lettering. A press sinks the top onto its wall,
// darkens it a little, flashes it and sends a ripple out from the part.

constexpr float kRippleSeconds = 0.38f;

// The label colour for a part: light lettering on colourful plastic, dark on grey.
Rgb letterColour(Rgb base, bool colourful) { return colourful ? lighter(base, 0.80f) : darker(base, 0.62f); }

void ripple(ImDrawList* dl, ImVec2 c, float r, float t, Rgb accent, float a) {
    if (t < 0.0f || t > kRippleSeconds) return;
    const float k = t / kRippleSeconds;
    const float ease = 1.0f - (1.0f - k) * (1.0f - k);
    dl->AddCircle(c, r * (1.0f + 0.65f * ease), col(lighter(accent, 0.35f), 0.55f * (1.0f - k) * std::max(a, 0.5f)), 48,
                  std::max(1.5f, r * 0.10f * (1.0f - k)));
}

void glint(ImDrawList* dl, ImVec2 c, ImVec2 radius, float alpha) {
    if (alpha <= 0.01f) return;
    dl->AddEllipseFilled(c, radius, col(kWhite, alpha * 0.45f), -0.45f, 24);
    dl->AddEllipseFilled(c, mul(radius, 0.55f), col(kWhite, alpha * 0.35f), -0.45f, 20);
}

void paintRound(ImDrawList* dl, const Control& c, float a) {
    const float p = c.anim, r = c.r;
    const bool colourful = c.tinted;
    const Rgb base = darker(c.accent, 0.10f * p);
    const float height = r * 0.10f;
    const ImVec2 top = add(c.c, ImVec2(0, -height * (1.0f - p)));
    const Pts footprint = circlePts(c.c, r);
    dropShadow(dl, footprint, r * 0.10f, 0.55f * a);
    plastic(dl, footprint, darker(c.accent, 0.45f), a, 0.6f, 0.0f);  // side wall
    const Pts body = circlePts(top, r * (1.0f - 0.03f * p));
    plastic(dl, body, base, a, 1.0f, 0.12f);
    outline(dl, body, col(darker(c.accent, 0.55f), 0.9f * a), std::max(1.5f, r * 0.045f));
    if (p > 0.01f) dl->AddCircleFilled(top, r, col(kWhite, 0.16f * p * a), 48);
    glint(dl, add(top, ImVec2(-0.34f * r, -0.40f * r)), ImVec2(0.30f * r, 0.15f * r), a * (1.0f - 0.6f * p));
    ripple(dl, c.c, r, c.ripple, c.accent, a);
    if (c.menu) {
        const float bw = r * 0.44f, bh = std::max(2.0f, r * 0.085f);
        for (int i = -1; i <= 1; i++) {
            const ImVec2 m = add(top, ImVec2(0, float(i) * r * 0.27f));
            dl->AddRectFilled(ImVec2(m.x - bw, m.y - bh + bh * 0.6f), ImVec2(m.x + bw, m.y + bh + bh * 0.6f),
                              col(lighter(base, 0.55f), 0.5f * a), bh);  // raised: light edge below the groove
            dl->AddRectFilled(ImVec2(m.x - bw, m.y - bh), ImVec2(m.x + bw, m.y + bh), col(darker(base, 0.6f), a), bh);
        }
    } else if (c.text) {
        const bool word = std::strlen(c.text) > 1;
        strokeText(dl, top, r * (word ? 0.30f : 0.84f), c.text, letterColour(base, colourful), std::clamp(a * 1.2f + 0.15f, 0.0f, 1.0f),
                   word ? 0.19f : 0.17f);
    }
}

// X / Y: a curved bean around A. One mesh: every outline point (outer arc, rounded end, inner arc,
// other end) is joined to the matching point of the centre line (the arc at radius R, the ends'
// centres for the rounded ends), lit by its outward direction; the centre line is the dome's crest.
// (Shading the ends as separate discs showed as circles at both ends.)
struct Bean {
    Pts edge, spine, normal;
};

Bean bean(ImVec2 c, float R, float th, float a0, float a1) {
    Bean b;
    const int seg = 28, cap = 12;
    const auto dir = [](float ang) { return ImVec2(std::cos(ang), std::sin(ang)); };
    const auto push = [&](ImVec2 e, ImVec2 s, ImVec2 n) {
        b.edge.push_back(e);
        b.spine.push_back(s);
        b.normal.push_back(n);
    };
    for (int i = 0; i <= seg; i++) {  // outer arc, a0 -> a1
        const ImVec2 d = dir(a0 + (a1 - a0) * float(i) / float(seg));
        push(add(c, mul(d, R + th * 0.5f)), add(c, mul(d, R)), d);
    }
    const ImVec2 e1 = add(c, mul(dir(a1), R));
    for (int i = 1; i < cap; i++) {  // rounded end at a1
        const ImVec2 d = dir(a1 + kPi * float(i) / float(cap));
        push(add(e1, mul(d, th * 0.5f)), e1, d);
    }
    for (int i = seg; i >= 0; i--) {  // inner arc, a1 -> a0
        const ImVec2 d = dir(a0 + (a1 - a0) * float(i) / float(seg));
        push(add(c, mul(d, R - th * 0.5f)), add(c, mul(d, R)), mul(d, -1.0f));
    }
    const ImVec2 e0 = add(c, mul(dir(a0), R));
    for (int i = 1; i < cap; i++) {  // rounded end at a0
        const ImVec2 d = dir(a0 + kPi + kPi * float(i) / float(cap));
        push(add(e0, mul(d, th * 0.5f)), e0, d);
    }
    return b;
}

Pts kidneyOutline(ImVec2 c, float R, float th, float a0, float a1) { return bean(c, R, th, a0, a1).edge; }

void kidneyFill(ImDrawList* dl, ImVec2 c, float R, float th, float a0, float a1, Rgb base, float alpha, float relief) {
    const Bean b = bean(c, R, th, a0, a1);
    if (relief <= 0.0f) {  // shadow, flash: one flat shape
        dl->AddConcavePolyFilled(b.edge.data(), int(b.edge.size()), col(base, alpha));
        return;
    }
    const ImVec2 uv = dl->_Data->TexUvWhitePixel;
    const int n = int(b.edge.size());
    const ImU32 crest = col(lighter(base, 0.10f), alpha);
    dl->PrimReserve(n * 6, n * 2);
    const unsigned v0 = dl->_VtxCurrentIdx;
    for (int i = 0; i < n; i++) {
        dl->PrimWriteVtx(b.edge[size_t(i)], uv, col(shade(base, b.normal[size_t(i)], relief), alpha));
        dl->PrimWriteVtx(b.spine[size_t(i)], uv, crest);
    }
    for (int i = 0; i < n; i++) {
        const unsigned e = v0 + unsigned(i) * 2, s = e + 1;
        const unsigned en = v0 + unsigned((i + 1) % n) * 2, sn = en + 1;
        dl->PrimWriteIdx(ImDrawIdx(e));
        dl->PrimWriteIdx(ImDrawIdx(en));
        dl->PrimWriteIdx(ImDrawIdx(sn));
        dl->PrimWriteIdx(ImDrawIdx(e));
        dl->PrimWriteIdx(ImDrawIdx(sn));
        dl->PrimWriteIdx(ImDrawIdx(s));
    }
}

void paintKidney(ImDrawList* dl, const Control& c, float a) {
    const float p = c.anim;
    const float th = c.size2 * c.k;
    const float a0 = -c.arcTo * kPi / 180.0f, a1 = -c.arcFrom * kPi / 180.0f;  // screen space (y down)
    const float height = th * 0.16f;
    const ImVec2 top = add(c.c, ImVec2(0, -height * (1.0f - p)));
    const Rgb base = darker(c.accent, 0.10f * p);
    // shadow (several soft copies), side wall, top
    for (int i = 2; i >= 0; i--)
        kidneyFill(dl, add(c.c, ImVec2(0, th * (0.10f + 0.06f * float(i)))), c.r, th * (1.0f + 0.08f * float(i)), a0, a1, kBlack,
                   a * (i == 0 ? 0.32f : 0.10f), 0.0f);
    kidneyFill(dl, c.c, c.r, th, a0, a1, darker(c.accent, 0.45f), a, 0.6f);
    kidneyFill(dl, top, c.r, th * (1.0f - 0.04f * p), a0, a1, base, a, 1.0f);
    outline(dl, kidneyOutline(top, c.r, th, a0, a1), col(darker(c.accent, 0.55f), 0.9f * a), std::max(1.5f, th * 0.05f));
    const float mid = (a0 + a1) * 0.5f;
    const ImVec2 m = add(top, mul(ImVec2(std::cos(mid), std::sin(mid)), c.r));
    if (p > 0.01f) kidneyFill(dl, top, c.r, th, a0, a1, kWhite, 0.16f * p * a, 0.0f);
    glint(dl, add(m, ImVec2(-th * 0.18f, -th * 0.22f)), ImVec2(th * 0.24f, th * 0.10f), a * (1.0f - 0.6f * p));
    ripple(dl, m, th * 0.6f, c.ripple, c.accent, a);
    strokeText(dl, m, th * 0.50f, c.text, letterColour(base, false), std::clamp(a * 1.2f + 0.15f, 0.0f, 1.0f));
}

void paintRect(ImDrawList* dl, const Control& c, float a, bool trigger) {
    const float p = c.anim;
    const float w = c.w, h = c.h;
    const float rad = trigger ? h * 0.42f : h * 0.5f;
    const float height = h * 0.13f;
    const Rgb base = darker(c.accent, 0.10f * p);
    const ImVec2 top = add(c.c, ImVec2(0, -height * (1.0f - p)));
    const Pts footprint = roundRectPts(c.c, w, h, rad);
    dropShadow(dl, footprint, h * 0.12f, 0.5f * a);
    plastic(dl, footprint, darker(c.accent, 0.45f), a, 0.6f, 0.0f);
    const Pts body = roundRectPts(top, w * (1.0f - 0.02f * p), h * (1.0f - 0.03f * p), rad);
    plastic(dl, body, base, a, 1.0f, 0.10f);
    outline(dl, body, col(darker(c.accent, 0.55f), 0.9f * a), std::max(1.5f, h * 0.05f));
    if (p > 0.01f) flat(dl, body, col(kWhite, 0.15f * p * a));
    glint(dl, add(top, ImVec2(-w * 0.28f, -h * 0.22f)), ImVec2(w * 0.16f, h * 0.10f), a * (1.0f - 0.6f * p));
    ripple(dl, c.c, std::max(w, h) * 0.5f, c.ripple, c.accent, a);
    strokeText(dl, add(top, ImVec2(0, trigger ? -h * 0.06f : 0.0f)), h * 0.42f, c.text, letterColour(base, c.tinted),
               std::clamp(a * 1.2f + 0.15f, 0.0f, 1.0f));
}

// The D-pad: a raised plus in a round dish; the pressed arm dips and the pad leans toward it.
Pts crossPts(ImVec2 c, float r, float arm) {
    const float e = r * 0.92f;
    return {{c.x - arm, c.y - e}, {c.x + arm, c.y - e}, {c.x + arm, c.y - arm}, {c.x + e, c.y - arm}, {c.x + e, c.y + arm},
            {c.x + arm, c.y + arm}, {c.x + arm, c.y + e}, {c.x - arm, c.y + e}, {c.x - arm, c.y + arm}, {c.x - e, c.y + arm},
            {c.x - e, c.y - arm}, {c.x - arm, c.y - arm}};
}

void paintDPad(ImDrawList* dl, Control& c, float a) {
    const float r = c.r, arm = r * 0.31f;
    const float dt = ImGui::GetIO().DeltaTime;
    ImVec2 lean(0, 0);
    for (int i = 0; i < 4; i++) {
        c.armAnim[i] = c.dir[i] ? std::min(1.0f, c.armAnim[i] + dt * 16.0f) : std::max(0.0f, c.armAnim[i] - dt * 10.0f);
        const float ang = (-90.0f + 90.0f * float(i)) * kPi / 180.0f;  // up, right, down, left (screen)
        lean = add(lean, mul(ImVec2(std::cos(ang), std::sin(ang)), c.armAnim[i] * r * 0.035f));
    }
    // dish
    const Pts dish = circlePts(c.c, r * 1.12f);
    dropShadow(dl, dish, r * 0.05f, 0.30f * a);
    plastic(dl, dish, darker(kGreyBody, 0.55f), a * 0.85f, -0.7f, 0.0f);  // concave
    // shadow + side wall + top of the plus
    const float height = r * 0.07f;
    const ImVec2 top = add(add(c.c, ImVec2(0, -height)), lean);
    flat(dl, crossPts(add(c.c, ImVec2(0, r * 0.06f)), r, arm * 1.04f), col(kBlack, 0.40f * a));
    const Rgb wall = darker(kGreyBody, 0.45f);
    plastic(dl, roundRectPts(c.c, arm * 2, r * 1.84f, arm * 0.35f), wall, a, 0.5f, 0.0f);
    plastic(dl, roundRectPts(c.c, r * 1.84f, arm * 2, arm * 0.35f), wall, a, 0.5f, 0.0f);
    plastic(dl, roundRectPts(top, arm * 2, r * 1.84f, arm * 0.35f), kGreyBody, a, 1.0f, 0.10f);
    plastic(dl, roundRectPts(top, r * 1.84f, arm * 2, arm * 0.35f), kGreyBody, a, 1.0f, 0.10f);
    outline(dl, crossPts(top, r, arm), col(darker(kGreyBody, 0.55f), 0.9f * a), std::max(1.5f, r * 0.03f));
    for (int i = 0; i < 4; i++) {
        const float ang = (-90.0f + 90.0f * float(i)) * kPi / 180.0f;
        const ImVec2 d(std::cos(ang), std::sin(ang));
        const ImVec2 n(-d.y, d.x);
        if (c.armAnim[i] > 0.01f) {
            const ImVec2 p0 = add(top, mul(d, arm * 1.05f)), p1 = add(top, mul(d, r * 0.90f));
            dl->AddRectFilled(ImVec2(std::min(p0.x, p1.x) - std::fabs(n.x) * arm, std::min(p0.y, p1.y) - std::fabs(n.y) * arm),
                              ImVec2(std::max(p0.x, p1.x) + std::fabs(n.x) * arm, std::max(p0.y, p1.y) + std::fabs(n.y) * arm),
                              col(kBlack, 0.22f * c.armAnim[i] * a), arm * 0.35f);
        }
        // engraved arrow: dark groove with a light lower edge
        const float s = r * 0.12f;
        const ImVec2 tip = add(top, mul(d, r * 0.72f));
        const ImVec2 back = sub(tip, mul(d, s * 1.5f));
        const ImVec2 t0 = tip, t1 = add(back, mul(n, s)), t2 = sub(back, mul(n, s));
        dl->AddTriangleFilled(add(t0, ImVec2(0, s * 0.18f)), add(t1, ImVec2(0, s * 0.18f)), add(t2, ImVec2(0, s * 0.18f)),
                              col(lighter(kGreyBody, 0.5f), 0.7f * a));
        dl->AddTriangleFilled(t0, t1, t2, col(darker(kGreyBody, 0.6f), a));
    }
    glint(dl, add(top, ImVec2(-arm * 0.4f, -r * 0.62f)), ImVec2(arm * 0.45f, arm * 0.18f), a);
    dl->AddCircleFilled(top, arm * 0.42f, col(darker(kGreyBody, 0.25f), a), 24);  // centre dimple
    dl->AddCircle(top, arm * 0.42f, col(darker(kGreyBody, 0.55f), 0.8f * a), 24, std::max(1.0f, arm * 0.06f));
}

void paintStick(ImDrawList* dl, Control& c, float a) {
    const float p = c.anim, r = c.r;
    const bool floatingIdle = !c.cStick && ms().floatingStick && !c.pressed;
    const float baseA = floatingIdle ? a * 0.7f : a;
    // socket with its gate: an octagon (Control Stick) or a circle (C-Stick), as on the controller
    const Pts socket = circlePts(c.c, r);
    dropShadow(dl, socket, r * 0.05f, 0.30f * baseA);
    plastic(dl, socket, kGreyBody, baseA * 0.9f, 0.9f, 0.0f);
    outline(dl, socket, col(darker(kGreyBody, 0.6f), 0.9f * baseA), std::max(1.5f, r * 0.025f));
    Pts gate;
    if (c.cStick) {
        gate = circlePts(c.c, r * 0.80f, 40);
    } else {
        for (int i = 0; i < 8; i++) {
            const float ang = (22.5f + 45.0f * float(i)) * kPi / 180.0f;
            gate.push_back(add(c.c, mul(ImVec2(std::cos(ang), std::sin(ang)), r * 0.82f)));
        }
    }
    plastic(dl, gate, kSocket, baseA, -0.8f, 0.0f);  // a recess: lit from below
    outline(dl, gate, col(lighter(kGreyBody, 0.3f), 0.8f * baseA), std::max(1.5f, r * 0.03f));
    // knob: follows the finger, springs back when released
    if (!c.pressed) {
        const float t = std::clamp(ImGui::GetIO().DeltaTime * 22.0f, 0.0f, 1.0f);
        c.knob = mul(c.knob, 1.0f - t);
    }
    const ImVec2 kc = add(c.c, c.knob);
    const float kr = r * (c.cStick ? 0.46f : 0.56f);
    const Rgb knob = c.cStick ? kYellowC : kGreyLight;
    dropShadow(dl, circlePts(add(kc, ImVec2(0, kr * 0.08f)), kr), kr * 0.18f, 0.55f * a);
    plastic(dl, circlePts(add(kc, ImVec2(0, kr * 0.10f)), kr), darker(knob, 0.45f), a, 0.6f, 0.0f);  // stem side
    plastic(dl, circlePts(kc, kr), darker(knob, 0.08f * p), a, 1.0f, 0.08f);
    outline(dl, circlePts(kc, kr), col(darker(knob, 0.55f), 0.9f * a), std::max(1.5f, kr * 0.05f));
    // concave thumb cap with a ring of grip bumps (Control Stick) / a plain dish (C-Stick)
    plastic(dl, circlePts(kc, kr * 0.70f), knob, a, -0.9f, 0.0f);
    if (!c.cStick)
        for (int i = 0; i < 16; i++) {
            const float ang = 2.0f * kPi * float(i) / 16.0f;
            const ImVec2 b = add(kc, mul(ImVec2(std::cos(ang), std::sin(ang)), kr * 0.85f));
            dl->AddCircleFilled(add(b, ImVec2(0, kr * 0.02f)), kr * 0.065f, col(darker(knob, 0.5f), 0.6f * a), 10);
            dl->AddCircleFilled(b, kr * 0.055f, col(lighter(knob, 0.35f), 0.8f * a), 10);
        }
    glint(dl, add(kc, ImVec2(-kr * 0.40f, -kr * 0.48f)), ImVec2(kr * 0.26f, kr * 0.12f), a);
    if (p > 0.01f) dl->AddCircle(kc, kr * 1.12f, col(lighter(knob, 0.3f), 0.5f * p * a), 40, kr * 0.12f);
    if (c.text) strokeText(dl, kc, kr * 0.62f, c.text, darker(kYellowC, 0.55f), std::clamp(a * 1.2f + 0.15f, 0.0f, 1.0f), 0.2f);
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

bool scrollFor(const ImRect& region, ImVec2& delta, ImVec2& start) {
    start = sMouseStart;
    if (!region.Contains(sMouseStart)) return false;
    if (sMouseScrolling) {
        delta = sScrollAccum;
        sScrollAccum = ImVec2(0, 0);
        return delta.x != 0.0f || delta.y != 0.0f;
    }
    if (!sFling) return false;
    // Glide after a flick: the finger's speed, slowing down exponentially (about like iOS lists).
    const float dt = std::min(ImGui::GetIO().DeltaTime, 0.05f);
    delta = ImVec2(sScrollVel.x * dt, sScrollVel.y * dt);
    const float decay = std::exp(-dt * 4.2f);
    sScrollVel = ImVec2(sScrollVel.x * decay, sScrollVel.y * decay);
    if (std::hypot(sScrollVel.x, sScrollVel.y) < 25.0f) {
        sFling = false;
        sScrollVel = ImVec2(0, 0);
    }
    return true;
}
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
                if (!sCtl[i].owned && hit(sCtl[i], p, true) && !recomp::menu::isOpen()) {
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
        if (c.ripple >= 0.0f) c.ripple = c.ripple + dt > kRippleSeconds ? -1.0f : c.ripple + dt;
        if (menuOpen) continue;  // hidden under the menu (its "Back to game" button closes it)
        if (!sVisible && !c.menu) continue;
        const float alpha = c.menu && !sVisible ? std::max(a, 0.85f) : a;
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
