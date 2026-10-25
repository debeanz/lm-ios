// Skippable cutscenes and fast-forward; see skip.h.
//
// What the game tells us (US GLME01 addresses; the European game's via VA(), same layout):
//   sCurScene 0x804CFF98 points at the running scene's descriptor: OPENING
//     (0x8021DBD8, the forest/mansion-exterior intro after the file select) or
//     MAIN GAME (0x8021DB68), ...
//   Event interpreter object 0x803D2D40 (fn_8006385C runs it every frame):
//     +0x04 last tag executed (index into the tag table at 0x80339C6C),
//     +0x0C read cursor into the script text, 0 when no event runs,
//     +0x1C wait state: 3 = <ANYKEY> (message window waits for A), 7 = <CHOICE>
//       (a menu waits for the player), 4 = <WAIT>(n), ...
//   <MENUOFF>/<MENUON> set/clear 0x804D8088 (r13+0x128): HUD hidden (also set
//     around scene changes).
//   <LUIGISTOP> sets bit 0x10 of Luigi's +0x40 flags, <LUIGIFREE> clears it.
//   <SAVE> requests the save screen through 0x804D80C4 (r13+0x164) = 5.
// A cutscene can be skipped while the opening scene runs, or while an event
// script runs with the HUD hidden or Luigi stopped. Choices and the save screen
// suspend the skip (the player answers them); it resumes afterwards.
#include "recomp/skip.h"

#include "mobile/mobile.h"
#include "port/install.h"
#include "port/settings.h"
#include "recomp/gx_bridge.h"
#include "recomp/hle.h"
#include "recomp/menu.h"
#include "recomp/version.h"

#include <dolphin/pad.h>
#include <imgui.h>
#include <SDL3/SDL.h>

#include <atomic>
#include <bit>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace recomp::gx {
bool g_discardDraws = false;
}

namespace recomp::skip {
namespace {

using clock = std::chrono::steady_clock;

// USA addresses; VA() gives the running version's (version.h).
#define kCurScene VA(0x804CFF98)
#define kMenuOff VA(0x804D8088)
#define kModeRequest VA(0x804D80C4)
#define kEnManagerPtr VA(0x804D8C60)  // r13+0xD00
#define kCharacterTable VA(0x803D48A0)
#define kEvent VA(0x803D2D40)
#define kSceneMainGame VA(0x8021DB68)
#define kSceneOpening VA(0x8021DBD8)
#define kOpeningNext VA(0x804CFFAC)  // scene id the opening hands over to
constexpr u32 kSceneIdMainGame = 2;
constexpr u32 kStateAnyKey = 3, kStateWait = 4, kStateChoice = 7;
// A skip gives up when the script has not moved for this many frames outside a
// <WAIT>: it is waiting for something only the player can do.
constexpr u32 kStallFrames = 1800;
constexpr u32 kModeSave = 5;

bool envFlag(const char* name) {
    const char* v = std::getenv(name);
    return v && *v && *v != '0';
}
const bool sLog = envFlag("LM_SKIP_LOG");
const bool sTracePos = sLog && std::getenv("LM_SKIP_LOG")[0] == '2';  // also a snapshot every 30 frames
const bool sForceOn = envFlag("LM_SKIP_CUTSCENES");
const bool sForceFast = envFlag("LM_FF");  // testing: fast-forward all the time
// LM_FF_FROM=<guest frame> (testing, with LM_FF): fast-forward only from that frame on (past the
// title screen and menus, which do not take scripted input at fast-forward speed).
const unsigned sForceFastFrom = std::getenv("LM_FF_FROM") ? unsigned(std::atoi(std::getenv("LM_FF_FROM"))) : 0;
bool sTestFast = false;  // setTestFastForward
const int sAutoFrames = [] {
    const char* v = std::getenv("LM_SKIP_AUTO");
    return v ? std::atoi(v) : 0;
}();
int sAutoCount = [] {
    const char* v = std::getenv("LM_SKIP_AUTO");
    const char* colon = v ? std::strchr(v, ':') : nullptr;
    return colon ? std::atoi(colon + 1) : -1;
}();
const double sSpeed = [] {
    const char* v = std::getenv("LM_FF_SPEED");
    const double s = v ? std::atof(v) : 0.0;
    return s >= 1.0 ? s : 32.0;
}();

// Both releases: the European scene/event state has the same layout (addresses via VA()).
bool enabled() { return sForceOn || port::settings().enhancements.skipCutscenes; }

struct Status {
    u32 scene = 0;
    bool event = false;       // an event script is running
    u32 tag = 0, state = 0;   // interpreter: last tag, wait state
    u32 cursor = 0;           // interpreter: read position in the script
    bool menuOff = false;     // HUD hidden by the script
    bool luigiStopped = false;
    u32 modeRequest = 0;
    u32 openingNext = 0;      // opening scene: scene id that follows (1 = attract demo)

    bool operator==(const Status&) const = default;
};

u32 luigi() {
    const u32 mgrHolder = rd32(kEnManagerPtr);
    if (mgrHolder < 0x80000000u || mgrHolder >= 0x81800000u) return 0;
    const u32 mgr = rd32(mgrHolder + 8);
    if (mgr < 0x80000000u || mgr >= 0x81800000u) return 0;
    const u32 idx = rd32(mgr + 0xE08);  // character 0's slot
    if (idx >= 0x400) return 0;
    const u32 obj = rd32(kCharacterTable + 4 * idx);
    return obj >= 0x80000000u && obj < 0x81800000u ? obj : 0;
}

Status sample() {
    Status s;
    s.scene = rd32(kCurScene);
    if (s.scene == kSceneOpening) s.openingNext = rd32(kOpeningNext);
    if (s.scene != kSceneMainGame) return s;
    s.cursor = rd32(kEvent + 0x0C);
    s.event = s.cursor != 0;
    s.tag = rd32(kEvent + 0x04);
    s.state = rd32(kEvent + 0x1C);
    s.menuOff = rd32(kMenuOff) != 0;
    s.modeRequest = rd32(kModeRequest);
    if (const u32 l = luigi()) s.luigiStopped = (rd32(l + 0x40) & 0x10) != 0;
    return s;
}

// Cutscene: the opening, or an event that has taken control away from the player.
bool cutscene(const Status& s) {
    // The title screen plays the opening as its attract demo, where START already
    // returns to the title; only the opening of a new game is skipped.
    if (s.scene == kSceneOpening) return s.openingNext == kSceneIdMainGame;
    return s.scene == kSceneMainGame && s.event && (s.menuOff || s.luigiStopped);
}
// The player has to answer: a choice menu or the save screen.
bool needsPlayer(const Status& s) { return s.state == kStateChoice || s.modeRequest == kModeSave; }

enum class Mode { Off, Skipping, Suspended };
Mode sMode = Mode::Off;
bool sOpeningSkip = false;  // the skip started in the opening scene
int sFreeFrames = 0;        // consecutive frames the player had control during a skip
u32 sStallFrames = 0;       // frames the script has not advanced during a skip
bool sGaveUp = false;       // this event's skip was abandoned (until the event ends)
Status sStatus;
u32 sFrame = 0;

// Guest pad state.
bool sStartLatched = false;  // START went down since the last guest frame
bool sStartHeld = false;
bool sMaskStart = false;     // the game must not see START (cutscene can be skipped)

// Batching of fast-forwarded frames into one host frame.
bool sNewBatch = true;
clock::time_point sBatchStart;
constexpr auto kBatchBudget = std::chrono::milliseconds(12);
// Phone (src/mobile): game frames cost more CPU there, so a 12 ms batch held about one frame and
// most of the time went to the shown frame in between (a skip ran at 2x). The screen is covered
// while skipping, so fewer shown frames cost nothing visible.
constexpr auto kPhoneBatchBudget = std::chrono::milliseconds(40);
// Per skip: frames shown between batches, and guest time spent on the skipped ones (the log line).
u32 sShownInSkip = 0;
double sSkippedSeconds = 0;

// Shared with the host thread.
std::atomic<bool> sDevFastForward{false};  // Tab held
std::atomic<bool> sSkipping{false};        // hidden fast-forward (cutscene skip)
std::atomic<bool> sSkippable{false};       // prompt can be shown
std::atomic<bool> sFast{false};            // any fast-forward
std::atomic<clock::rep> sUnmuteTime{0};

void setFast(bool fast) {
    if (sFast.exchange(fast) && !fast) sUnmuteTime = clock::now().time_since_epoch().count();
}

const clock::time_point sStartTime = clock::now();
float seconds(clock::time_point t) { return std::chrono::duration<float>(t - sStartTime).count(); }
u32 sSkipStartFrame = 0;
clock::time_point sSkipStartTime;

void logStatus(const Status& s) {
    log("[skip] %.2fs frame %u scene %08X/%u event %d tag %u state %u menuOff %d luigiStop %d mode %u -> %s",
        seconds(clock::now()), sFrame, s.scene, s.openingNext, s.event, s.tag, s.state, s.menuOff, s.luigiStopped,
        s.modeRequest, sMode == Mode::Off ? "off" : sMode == Mode::Skipping ? "skipping" : "suspended");
}

// LM_SKIP_LOG: a fingerprint of the state a cutscene leaves behind, logged 60 frames
// after every event, to compare a skipped run with a watched one.
u32 sSnapshotFrame = 0;
void logSnapshot() {
    char flags[65] = {};
    for (int i = 0; i < 32; i++) std::snprintf(flags + 2 * i, 3, "%02X", rd8(kEvent + 0x659 + i));
    const u32 l = luigi();
    log("[skip] snapshot frame %u: flags %s luigi %08X pos (%.3f %.3f %.3f) flags40 %08X menuOff %u", sFrame, flags, l,
        l ? rdf32(l + 0x44) : 0.0f, l ? rdf32(l + 0x48) : 0.0f, l ? rdf32(l + 0x4C) : 0.0f, l ? rd32(l + 0x40) : 0,
        rd32(kMenuOff));
}

// LM_SKIP_TEST=<frame>:flag:<n>;<frame>:pos:<x>,<y>,<z>;... (testing): at a guest
// frame, sets event flag n (flag-triggered events start by themselves; see the
// map's jmp/eventinfo table) or puts Luigi at a position.
void testActions() {
    static const char* spec = std::getenv("LM_SKIP_TEST");
    if (!spec) return;
    for (const char* p = spec; p && *p; p = std::strchr(p, ';') ? std::strchr(p, ';') + 1 : nullptr) {
        unsigned frame = 0, flag = 0;
        float x, y, z;
        if (std::sscanf(p, "%u:flag:%u", &frame, &flag) == 2 && frame == sFrame && flag < 256) {
            wr8(kEvent + 0x659 + flag / 8, rd8(kEvent + 0x659 + flag / 8) | (1u << (flag % 8)));
            log("[skip] test: flag %u on", flag);
        } else if (std::sscanf(p, "%u:pos:%f,%f,%f", &frame, &x, &y, &z) == 4 && frame == sFrame) {
            if (const u32 l = luigi()) {
                wr32(l + 0x44, std::bit_cast<u32>(x));
                wr32(l + 0x48, std::bit_cast<u32>(y));
                wr32(l + 0x4C, std::bit_cast<u32>(z));
                log("[skip] test: Luigi at (%.1f %.1f %.1f)", x, y, z);
            }
        }
    }
}

void update() {
    testActions();
    const Status s = sample();
    if (sLog) {
        if (sStatus.event && !s.event) sSnapshotFrame = sFrame + 60;
        if (sSnapshotFrame == sFrame || (sTracePos && sFrame % 30 == 0)) logSnapshot();
    }
    const bool on = enabled();
    if (!s.event) sGaveUp = false;
    const bool isCutscene = on && cutscene(s) && !sGaveUp;
    const Mode before = sMode;

    // LM_SKIP_AUTO=<n>[:<count>] (testing): press START by itself n frames into every
    // cutscene (only the first <count> ones).
    static int sCutsceneFrames = 0;
    sCutsceneFrames = isCutscene && sMode == Mode::Off ? sCutsceneFrames + 1 : 0;
    if (sAutoFrames > 0 && sCutsceneFrames == sAutoFrames && sAutoCount != 0) {
        sStartLatched = true;
        sAutoCount--;
    }

    switch (sMode) {
    case Mode::Off:
        if (isCutscene && sStartLatched && !needsPlayer(s)) {
            sMode = Mode::Skipping;
            sOpeningSkip = s.scene == kSceneOpening;
            sFreeFrames = 0;
            sStallFrames = 0;
        }
        break;
    case Mode::Skipping:
    case Mode::Suspended:
        if (!on) {
            sMode = Mode::Off;
        } else if (sOpeningSkip) {
            // One skip covers the opening scene; the mansion's first event is its own cutscene.
            if (s.scene != kSceneOpening) sMode = Mode::Off;
        } else if (s.scene != kSceneMainGame || !s.event) {
            sMode = Mode::Off;
        } else {
            // Luigi may be freed for a moment between two parts of one script; only
            // give up once the player has had control for a few frames.
            sFreeFrames = cutscene(s) ? 0 : sFreeFrames + 1;
            const bool still = s.cursor == sStatus.cursor && s.state == sStatus.state && s.state != kStateWait;
            sStallFrames = still && sMode == Mode::Skipping ? sStallFrames + 1 : 0;
            if (sFreeFrames >= 3) {
                sMode = Mode::Off;
            } else if (sStallFrames >= kStallFrames) {
                sMode = Mode::Off;
                sGaveUp = true;
                log("[skip] the event is waiting for the player; skip abandoned");
            } else {
                sMode = needsPlayer(s) ? Mode::Suspended : Mode::Skipping;
            }
        }
        break;
    }
    if (before == Mode::Off && sMode != Mode::Off) {
        sSkipStartFrame = sFrame;
        sSkipStartTime = clock::now();
        sShownInSkip = 0;
        sSkippedSeconds = 0;
        log("[skip] skipping %s", sOpeningSkip ? "the opening" : "an event");
    } else if (before != Mode::Off && sMode == Mode::Off) {
        const u32 frames = sFrame - sSkipStartFrame;
        const u32 hidden = frames > sShownInSkip ? frames - sShownInSkip : 0;
        log("[skip] done: %u frames in %.2f s (%u shown between batches, %.2f ms per skipped frame)", frames,
            std::chrono::duration<float>(clock::now() - sSkipStartTime).count(), sShownInSkip,
            hidden ? sSkippedSeconds * 1000.0 / hidden : 0.0);
    }
    sStartLatched = false;
    sMaskStart = isCutscene && sMode != Mode::Suspended;

    if (sLog && (!(s == sStatus) || sMode != before)) logStatus(s);
    sStatus = s;
    sSkipping = sMode == Mode::Skipping;
    sSkippable = isCutscene && sMode == Mode::Off && !needsPlayer(s);
    setFast(sMode == Mode::Skipping || sDevFastForward.load() || (sForceFast && sFrame >= sForceFastFrom) || sTestFast);
}

}  // namespace

u32 guestFrames() { return sFrame; }
void setTestFastForward(bool on) { sTestFast = on; }

bool onGuestFrame(Ctx&) {
    sFrame++;
    update();
    static clock::time_point sLastFrame = clock::now();
    const auto now = clock::now();
    const auto frameTime = now - sLastFrame;
    sLastFrame = now;
    if (sFast.load()) {
        if (gx::g_discardDraws) sSkippedSeconds += std::chrono::duration<double>(frameTime).count();
        if (sNewBatch) {
            sNewBatch = false;
            sBatchStart = now;
        }
        static const auto sBudget = mobile::enabled() ? std::chrono::duration_cast<clock::duration>(kPhoneBatchBudget)
                                                      : std::chrono::duration_cast<clock::duration>(kBatchBudget);
        if (now - sBatchStart < sBudget) {
            gx::g_discardDraws = true;  // the next frame is not shown
            return true;
        }
        sShownInSkip++;
    }
    // Hand this frame to the host; the next one is drawn normally.
    gx::g_discardDraws = false;
    sNewBatch = true;
    return false;
}

void filterPad(PADStatus* pads, int count) {
    if (count <= 0) return;
    PADStatus& p = pads[0];
    const bool start = p.err == PAD_ERR_NONE && (p.button & PAD_BUTTON_START);
    if (start && !sStartHeld) sStartLatched = true;
    sStartHeld = start;
    if (sMaskStart) p.button &= ~PAD_BUTTON_START;
    if (sMode != Mode::Skipping) return;
    // While skipping, the player's input is ignored and message windows are
    // dismissed: A goes down for 4 frames out of 8 while the script waits for a key.
    // Controller 1 reads as connected, so this also works with no pad plugged in.
    p = PADStatus{};
    p.err = PAD_ERR_NONE;
    if (!sOpeningSkip && sStatus.state == kStateAnyKey && (sFrame & 4)) p.button = PAD_BUTTON_A;
}

double retraceSpeed() { return sFast.load(std::memory_order_relaxed) ? sSpeed : 1.0; }

float audioGain() {
    if (sFast.load(std::memory_order_relaxed)) return 0.0f;
    const clock::rep t = sUnmuteTime.load(std::memory_order_relaxed);
    if (!t) return 1.0f;
    const float since = std::chrono::duration<float>(clock::now() - clock::time_point(clock::duration(t))).count();
    return since >= 0.5f ? 1.0f : since / 0.5f;  // fade back in
}

void drawOverlay() {
    // Developer fast-forward: hold Tab (not while typing into the menu).
    const bool* keys = SDL_GetKeyboardState(nullptr);
    const bool tab = port::settings().enhancements.fastForwardKey && keys && keys[SDL_SCANCODE_TAB] && !menu::isOpen() &&
                     !ImGui::GetIO().WantCaptureKeyboard;
    sDevFastForward = tab;

    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    const ImVec2 size = ImGui::GetIO().DisplaySize;
    const float scale = size.y / 720.0f;
    auto text = [&](const char* s, ImVec2 anchor, ImVec2 pivot, ImU32 color, float px) {
        ImFont* font = ImGui::GetFont();
        const ImVec2 ts = font->CalcTextSizeA(px, FLT_MAX, 0.0f, s);
        const ImVec2 pos(anchor.x - ts.x * pivot.x, anchor.y - ts.y * pivot.y);
        dl->AddText(font, px, ImVec2(pos.x + 2, pos.y + 2), IM_COL32(0, 0, 0, (color >> 24) & 0xFF), s);
        dl->AddText(font, px, pos, color, s);
    };

    if (sSkipping.load()) {
        dl->AddRectFilled(ImVec2(0, 0), size, IM_COL32(0, 0, 0, 255));
        text("Skipping...", ImVec2(size.x - 24 * scale, size.y - 20 * scale), ImVec2(1, 1), IM_COL32(220, 220, 220, 255),
             22 * scale);
        return;
    }
    if (tab) {
        text(">> Fast-forward", ImVec2(size.x - 16 * scale, 12 * scale), ImVec2(1, 0), IM_COL32(255, 255, 255, 230),
             20 * scale);
    }
    // No "Press START to skip" prompt: the option is described in the F1 menu instead.
}

}  // namespace recomp::skip
