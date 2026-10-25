// Phone layer (mobile.h, docs/IOS.md): settings rows, display model, lifecycle, diagnostics.
#include "mobile/mobile.h"

#include "mobile/native.h"
#include "mobile/session_log.h"
#include "mobile/touch_pad.h"
#include "port/dol.h"
#include "port/install.h"
#include "port/settings.h"
#include "port/update.h"
#include "recomp/frame.h"
#include "recomp/menu.h"
#include "recomp/ui_scale.h"

#include <aurora/aurora.h>
#include <aurora/dvd.h>
#include <aurora/event.h>
#include <aurora/metalfx.h>
#include <dolphin/pad.h>
#include <dolphin/vi.h>
#include <imgui.h>
#include <imgui_internal.h>
#include <SDL3/SDL.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <mutex>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace mobile {
namespace {

using Clock = std::chrono::steady_clock;

// The phone display model (the same rows and values in every phone port).
constexpr const char* kResolutionLabels[] = {"Native", "2106x1084", "1920x1080", "1600x900", "1280x720"};
constexpr int kResolutionHeights[] = {0, 1084, 1080, 900, 720};
constexpr const char* kAspectLabels[] = {"iPhone Default", "1.943:1", "16:9", "4:3"};
constexpr float kAspectValues[] = {0.0f, 2106.0f / 1084.0f, 16.0f / 9.0f, 4.0f / 3.0f};
constexpr const char* kScalingLabels[] = {"Fit", "Stretch"};
constexpr const char* kFrameRateLabels[] = {"30 FPS", "60 FPS", "120 FPS (ProMotion)"};
constexpr int kFrameRates[] = {30, 60, 120};
constexpr const char* kUiSizeLabels[] = {"Small", "Default", "Large"};
constexpr float kUiSizeMultipliers[] = {0.85f, 1.0f, 1.2f};
constexpr const char* kOffOn[] = {"Off", "On"};
constexpr const char* kStickLabels[] = {"Fixed", "Floating"};

port::MobileSettings& ms() { return port::settings().mobile; }

const char* thermalName(int t) {
    switch (t) {
    case 0: return "nominal";
    case 1: return "fair";
    case 2: return "serious";
    case 3: return "critical";
    default: return "n/a";
    }
}

std::string memorySummary() {
    char buf[200];
    std::snprintf(buf, sizeof(buf), "footprint %lld MB, left %lld MB, thermal %s%s", native::footprintMB(),
                  native::availableMemoryMB(), thermalName(native::thermalState()), native::lowPowerMode() ? ", Low Power Mode" : "");
    return buf;
}

int nativeHeight() {
    const AuroraWindowSize ws = aurora_get_window_size();
    return int(std::min(ws.native_fb_width, ws.native_fb_height));
}

int frameRateIndex() {
    const int fps = port::settings().video.targetFps;
    return fps >= 100 ? 2 : fps >= 45 ? 1 : 0;
}

constexpr const char* kUpscalingLabels[] = {"Off", "MetalFX"};

// MetalFX upscaling chosen and possible on this device (Aurora 0240).
bool metalfxOn() { return ms().upscaling == 1 && aurora_metalfx_available(); }

// Render height of the Resolution row, never above the panel; 0 = the panel's own. With MetalFX,
// "Native" renders at 2/3 of the panel instead (at the panel's own size there is nothing to upscale).
int renderHeight() {
    const int idx = std::clamp(ms().resolution, 0, 4);
    const int panel = nativeHeight();
    if (idx == 0) return metalfxOn() && panel > 0 ? (panel * 2 / 3) & ~1 : 0;
    return panel > 0 ? std::min(kResolutionHeights[idx], panel) : kResolutionHeights[idx];
}

void logSettings(const char* why) {
    const auto& m = ms();
    const auto& v = port::settings().video;
    sessionlog::line("settings",
                     "%s: resolution '%s' (render height %d), aspect '%s', scaling '%s', upscaling '%s' (%s), %d fps, show fps %s, "
                     "UI '%s', touch %s (opacity %d%%, size %d%%, stick %s), vibration %s, enhanced lighting %s, MSAA %dx",
                     why, kResolutionLabels[std::clamp(m.resolution, 0, 4)], renderHeight(), kAspectLabels[std::clamp(m.aspect, 0, 3)],
                     kScalingLabels[std::clamp(m.scaling, 0, 1)], kUpscalingLabels[std::clamp(m.upscaling, 0, 1)],
                     aurora_metalfx_status(), v.targetFps, port::settings().enhancements.showFps ? "on" : "off",
                     kUiSizeLabels[std::clamp(m.uiSize, 0, 2)], m.touchControls ? "on" : "off", m.touchOpacity, m.touchSize,
                     m.floatingStick ? "floating" : "fixed", m.haptics ? "on" : "off",
                     port::settings().enhancedLighting.enabled ? "on" : "off", v.msaa);
}

// ---- Diagnostics -----------------------------------------------------------------------------

void logHeader() {
    const AuroraWindowSize ws = aurora_get_window_size();
    float l = 0, t = 0, r = 0, b = 0;
    safeAreaInsets(l, t, r, b);
    sessionlog::line("app", "==================== Luigi's Mansion Recompiled - iOS session log ====================");
    sessionlog::line("app", "Program:       %s (app %s)", port::update::buildVersion(), native::appVersion().c_str());
    sessionlog::line("app", "Device:        %s, %s, %lld MB RAM", native::deviceModel().c_str(), native::osVersion().c_str(),
                     native::physicalMemoryMB());
    sessionlog::line("app", "GPU:           %s", native::gpuName().c_str());
    sessionlog::line("app", "MetalFX:       %s", aurora_metalfx_available() ? "available" : aurora_metalfx_status());
    sessionlog::line("app", "Screen:        %ux%u px (%ux%u pt, scale %.2f), up to %d Hz, safe area insets L%.0f T%.0f R%.0f B%.0f",
                     ws.native_fb_width, ws.native_fb_height, ws.width, ws.height, ws.scale, native::maxRefreshRate(), l, t, r, b);
    sessionlog::line("app", "Memory:        %s", memorySummary().c_str());
    sessionlog::line("app", "Storage:       %lld MB free; Documents %s", native::freeDiskMB(),
                     port::displayPath(native::documentsDir()).c_str());
    sessionlog::line("app", "Log:           %s (previous: %s)", port::displayPath(sessionlog::path()).c_str(),
                     sessionlog::previousPath().empty() ? "none" : sessionlog::previousPath().filename().string().c_str());
    // How the previous run ended (recovery.cpp keeps it: clean exit, crash, killed while starting).
    std::ifstream state(port::configDir() / "last_run_state.ini");
    for (std::string line; std::getline(state, line);)
        if (!line.empty() && line[0] != ';') sessionlog::line("app", "Previous run:  %s", line.c_str());
    logSettings("settings at start");
    sessionlog::line("app", "=====================================================================================");
}

struct Perf {
    Clock::time_point windowStart = Clock::now(), last = Clock::now();
    int frames = 0, drops = 0;
    double worst = 0;
    u32 guestAtStart = 0;
} sPerf;

// Host loop steps (mobile.h frameBegin/phase): per-window samples and the slowest frame's split.
constexpr int kSteps = 5;
constexpr const char* kStepNames[kSteps] = {"pace", "begin", "game", "ui", "end"};
struct Steps {
    std::vector<float> ms[kSteps];
    float cur[kSteps] = {};
    float worstTotal = 0;
    float worst[kSteps] = {};
    Clock::time_point mark{};
    bool open = false;
} sSteps;

std::string stepSummary() {
    std::string out;
    char buf[96];
    for (int i = 0; i < kSteps; i++) {
        auto& v = sSteps.ms[i];
        if (v.empty()) continue;
        std::sort(v.begin(), v.end());
        std::snprintf(buf, sizeof(buf), "%s%s %.1f/%.1f/%.1f", out.empty() ? "" : ", ", kStepNames[i], v[v.size() / 2],
                      v[v.size() * 95 / 100], v.back());
        out += buf;
    }
    if (out.empty()) return "no game-loop samples";
    out = "ms median/p95/max: " + out + "; slowest frame " ;
    std::snprintf(buf, sizeof(buf), "%.0f ms =", sSteps.worstTotal);
    out += buf;
    for (int i = 0; i < kSteps; i++) {
        std::snprintf(buf, sizeof(buf), " %s %.1f", kStepNames[i], sSteps.worst[i]);
        out += buf;
    }
    return out;
}

void perfTick() {
    const auto t = Clock::now();
    const double dt = std::chrono::duration<double>(t - sPerf.last).count();
    sPerf.last = t;
    if (dt > 1.0) {  // a pause (background, picker): not a hitch
        sPerf = Perf{};
        sPerf.guestAtStart = recomp::frame::guestFrames();
        return;
    }
    const int target = std::max(port::settings().video.targetFps, 30);
    if (dt > 1.5 / double(target)) sPerf.drops++;
    sPerf.frames++;
    sPerf.worst = std::max(sPerf.worst, dt);
    const double span = std::chrono::duration<double>(t - sPerf.windowStart).count();
    if (span < 10.0) return;
    const u32 guest = recomp::frame::guestFrames();
    const char* pad = touch::physicalController();
    sessionlog::line("perf", "avg %.1f fps (target %d), game %.1f fps, dropped %d, worst frame %.0f ms, %s, input %s", sPerf.frames / span,
                     target, double(guest - sPerf.guestAtStart) / span, sPerf.drops, sPerf.worst * 1000.0, memorySummary().c_str(),
                     pad ? pad : touch::visible() ? "touch" : "none");
    sessionlog::line("perf", "game loop %s", stepSummary().c_str());
    sPerf = Perf{};
    sPerf.guestAtStart = guest;
    for (auto& v : sSteps.ms) v.clear();
    sSteps.worstTotal = 0;
}

void onNativeEvent(const char* what) {
    sessionlog::line("app", "%s; %s", what, memorySummary().c_str());
    if (std::strcmp(what, "will terminate") == 0) {
        port::saveSettings();
        sessionlog::flush();
    }
}

// ---- Settings rows ---------------------------------------------------------------------------

// One phone row: the title, then [<] value [>]; tapping the value steps forward. Wraps around.
bool row(const char* title, const char* const* values, int count, int& index) {
    bool changed = false;
    ImGui::PushID(title);
    ImGui::TableNextRow(0, ImGui::GetFrameHeight() * 1.6f);
    ImGui::TableNextColumn();
    ImGui::AlignTextToFramePadding();
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + ImGui::GetFrameHeight() * 0.3f);
    ImGui::TextUnformatted(title);
    ImGui::TableNextColumn();
    const float h = ImGui::GetFrameHeight() * 1.5f;
    const float valueW = std::max(ImGui::GetContentRegionAvail().x - 2.0f * h - 2.0f * ImGui::GetStyle().ItemSpacing.x, h);
    int next = index;
    if (ImGui::ArrowButtonEx("##prev", ImGuiDir_Left, ImVec2(h, h))) next = (index + count - 1) % count;
    ImGui::SameLine();
    if (ImGui::Button(values[std::clamp(index, 0, count - 1)], ImVec2(valueW, h))) next = (index + 1) % count;
    ImGui::SameLine();
    if (ImGui::ArrowButtonEx("##next", ImGuiDir_Right, ImVec2(h, h))) next = (index + 1) % count;
    if (next != index) {
        index = next;
        changed = true;
        sessionlog::line("settings", "%s -> %s", title, values[index]);
    }
    ImGui::PopID();
    return changed;
}

bool percentRow(const char* title, int from, int to, int step, int& value) {
    static char labels[32][8];
    static const char* ptrs[32];
    const int n = std::min((to - from) / step + 1, 32);
    for (int i = 0; i < n; i++) {
        std::snprintf(labels[i], sizeof(labels[i]), "%d%%", from + i * step);
        ptrs[i] = labels[i];
    }
    int idx = std::clamp((value - from) / step, 0, n - 1);
    if (!row(title, ptrs, n, idx)) return false;
    value = from + idx * step;
    return true;
}

bool boolRow(const char* title, bool& v) {
    int i = v ? 1 : 0;
    if (!row(title, kOffOn, 2, i)) return false;
    v = i == 1;
    return true;
}

// ---- Disc page -------------------------------------------------------------------------------

const char* regionName(const std::string& id) {
    return id == "GLME01" ? "USA (GLME01)" : id == "GLMP01" ? "Europe (GLMP01)" : "an unknown release";
}

bool discExtension(const fs::path& p) {
    std::string e = p.extension().string();
    std::transform(e.begin(), e.end(), e.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return e == ".iso" || e == ".gcm" || e == ".rvz" || e == ".wia" || e == ".ciso" || e == ".gcz" || e == ".nfs" || e == ".tgc";
}

// Opens a candidate and identifies its main.dol (then closes it again; main.cpp opens the one kept).
std::string discGameId(const fs::path& p, std::string& why) {
    const std::u8string u = p.u8string();
    const std::string s(reinterpret_cast<const char*>(u.data()), u.size());
    if (!aurora_dvd_open(s.c_str())) {
        why = "not a GameCube disc image this app can read";
        return {};
    }
    const port::dol::Version v = port::dol::load();
    aurora_dvd_close();
    const std::string id = v == port::dol::Version::GLME01 ? "GLME01" : v == port::dol::Version::GLMP01 ? "GLMP01" : "";
    if (id.empty()) why = "not a supported Luigi's Mansion release (only USA GLME01 and Europe GLMP01 rev 0)";
    return id;
}

}  // namespace

bool enabled() {
    static const bool sOn = [] {
        if (native::isIOS()) return true;
        const char* v = std::getenv("LM_MOBILE");
        return v && *v == '1';
    }();
    return sOn;
}

void beforeAurora() {
    if (!enabled()) return;
    auto& s = port::settings();
    if (s.mobile.version < 1) {
        // First start on this phone: 60 fps (interpolated; the game logic stays at 30), and a render
        // height by memory, as a quality tier (the GPU and memory budget follow the RAM).
        s.mobile.version = 1;
        s.video.targetFps = 60;
        s.video.matchDisplay = false;
        s.video.unlimitedFps = false;
        s.video.msaa = 1;
        s.ui.menuScale = 0;
        const long long ram = native::physicalMemoryMB();
        s.mobile.resolution = ram >= 7500 ? 2 : ram >= 5500 ? 3 : 4;
        port::saveSettings();
        sessionlog::line("settings", "first start: phone defaults (60 fps, %s for %lld MB RAM) saved",
                         kResolutionLabels[s.mobile.resolution], ram);
    }
    // Metal presents with the display's refresh: VSync is always on on a phone.
    s.video.vsync = true;
    s.video.fullscreen = true;
}

void init() {
    if (!enabled()) return;
    native::installObservers(&onNativeEvent);
    SDL_DisableScreenSaver();  // touch/controller play must not dim or lock the screen
    applyDisplay();
    logHeader();
}

bool handleEvent(const SDL_Event& ev) {
    if (!enabled()) return false;
    switch (ev.type) {
    case SDL_EVENT_WILL_ENTER_BACKGROUND: onBackground(); return false;
    case SDL_EVENT_DID_ENTER_FOREGROUND: onForeground(); return false;
    case SDL_EVENT_LOW_MEMORY: sessionlog::line("memory", "low-memory warning; %s", memorySummary().c_str()); return false;
    case SDL_EVENT_TERMINATING:
        sessionlog::line("app", "terminating");
        recomp::menu::shutdown();
        port::saveSettings();
        sessionlog::flush();
        return false;
    case SDL_EVENT_GAMEPAD_ADDED:
    case SDL_EVENT_GAMEPAD_REMOVED:
        sessionlog::line("input", "gamepad %s (id %u)", ev.type == SDL_EVENT_GAMEPAD_ADDED ? "added" : "removed", unsigned(ev.gdevice.which));
        return false;
    default: break;
    }
    return touch::handleEvent(ev);
}

void draw() {
    if (!enabled()) return;
    applyDisplay();  // cheap when nothing changed (Aurora ignores an unchanged scale)
    touch::draw();
    perfTick();
}

void onBackground() {
    touch::releaseAll();
    native::rumble(false);
    recomp::menu::shutdown();  // the F1 menu's pending changes
    port::saveSettings();
    sessionlog::line("app", "entering background: settings saved; %s", memorySummary().c_str());
    sessionlog::flush();
}

void frameBegin() {
    if (!enabled()) return;
    if (sSteps.open) {  // the previous iteration ended without its last step (paused, a hold frame)
        float total = 0;
        for (int i = 0; i < kSteps; i++) total += sSteps.cur[i];
        if (total > sSteps.worstTotal) {
            sSteps.worstTotal = total;
            std::copy(std::begin(sSteps.cur), std::end(sSteps.cur), std::begin(sSteps.worst));
        }
    }
    std::fill(std::begin(sSteps.cur), std::end(sSteps.cur), 0.0f);
    sSteps.mark = Clock::now();
    sSteps.open = true;
}

void phase(int step) {
    if (!enabled() || !sSteps.open || step < 0 || step >= kSteps) return;
    const auto now = Clock::now();
    const float ms = std::chrono::duration<float, std::milli>(now - sSteps.mark).count();
    sSteps.mark = now;
    sSteps.cur[step] = ms;
    if (sSteps.ms[step].size() < 20000) sSteps.ms[step].push_back(ms);
    if (step == kSteps - 1) {
        float total = 0;
        for (int i = 0; i < kSteps; i++) total += sSteps.cur[i];
        if (total > sSteps.worstTotal) {
            sSteps.worstTotal = total;
            std::copy(std::begin(sSteps.cur), std::end(sSteps.cur), std::begin(sSteps.worst));
        }
        sSteps.open = false;
    }
}

void onForeground() { sessionlog::line("app", "back in the foreground; %s", memorySummary().c_str()); }

void applyPad(PADStatus* status) { touch::applyPad(status); }

void onRumble(u32 chan, u32 cmd) {
    if (!enabled()) return;
    const bool on = chan == 0 && cmd == PAD_MOTOR_RUMBLE && touch::feeding() && ms().haptics;
    native::rumble(on);
}

float targetAspect() {
    const int i = std::clamp(ms().aspect, 0, 3);
    return kAspectValues[i];
}

bool fitScaling() { return ms().scaling == 0; }

float uiMultiplier() { return kUiSizeMultipliers[std::clamp(ms().uiSize, 0, 2)]; }

namespace {
int sRateLock = 0;
}

void applyDisplay() {
    const int h = renderHeight();
    VISetFrameBufferScale(h > 0 ? float(h) / 480.0f : 0.0f);
    aurora_set_metalfx(metalfxOn());  // cheap every frame (an atomic store)
    // Panel refresh held at the frame rate (30 FPS: the game's own frames, shown at 60 Hz).
    const int want = port::settings().video.targetFps >= 100 ? 120 : 60;
    if (want != sRateLock) {
        const int got = native::setDisplayRate(want);
        if (got > 0) sessionlog::line("display", "panel refresh held at %d Hz (asked %d)", got, want);
        sRateLock = got;
    }
}

float displayRateLock() { return float(sRateLock); }

void safeAreaInsets(float& left, float& top, float& right, float& bottom) {
    left = top = right = bottom = 0;
    SDL_Window* w = aurora_get_window();
    if (!w) return;
    int ww = 0, wh = 0;
    SDL_GetWindowSize(w, &ww, &wh);
    SDL_Rect r{};
    if (!SDL_GetWindowSafeArea(w, &r) || r.w <= 0 || r.h <= 0) return;
    // ImGui units are the window's logical units (points on iOS).
    const ImVec2 disp = ImGui::GetCurrentContext() ? ImGui::GetIO().DisplaySize : ImVec2(float(ww), float(wh));
    const float sx = ww > 0 ? disp.x / float(ww) : 1.0f, sy = wh > 0 ? disp.y / float(wh) : 1.0f;
    left = float(r.x) * sx;
    top = float(r.y) * sy;
    right = float(ww - (r.x + r.w)) * sx;
    bottom = float(wh - (r.y + r.h)) * sy;
}

bool drawSettingsTab() {
    auto& m = ms();
    auto& s = port::settings();
    bool display = false, any = false;
    ImGui::TextDisabled("Changes apply and save at once.");
    if (ImGui::BeginTable("phone", 2, ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("title", ImGuiTableColumnFlags_WidthStretch, 0.42f);
        ImGui::TableSetupColumn("value", ImGuiTableColumnFlags_WidthStretch, 0.58f);
        // Resolution, with the real panel size in its first entry.
        char nativeLabel[48];
        const AuroraWindowSize ws = aurora_get_window_size();
        std::snprintf(nativeLabel, sizeof(nativeLabel), "Native (%ux%u)", std::max(ws.native_fb_width, ws.native_fb_height),
                      std::min(ws.native_fb_width, ws.native_fb_height));
        const char* resLabels[5] = {nativeLabel, kResolutionLabels[1], kResolutionLabels[2], kResolutionLabels[3], kResolutionLabels[4]};
        display |= row("Resolution", resLabels, 5, m.resolution);
        display |= row("Aspect Ratio", kAspectLabels, 4, m.aspect);
        display |= row("Screen Scaling", kScalingLabels, 2, m.scaling);
        {
            // MetalFX (Aurora 0240): the game renders at the Resolution row's height (Native: 2/3 of
            // the panel) and Apple's spatial upscaler fills the screen. Disabled where unavailable.
            const bool fx = aurora_metalfx_available();
            ImGui::BeginDisabled(!fx);
            int up = fx ? m.upscaling : 0;
            if (row("Upscaling", kUpscalingLabels, 2, up)) {
                m.upscaling = up;
                display = true;
            }
            ImGui::EndDisabled();
            if (!fx) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TableNextColumn();
                ImGui::TextDisabled("MetalFX is not available on this device.");
            } else if (m.upscaling == 1) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TableNextColumn();
                ImGui::TextDisabled("Sharper picture from a lower render resolution (Native renders at 2/3).");
            }
        }
        int fr = frameRateIndex();
        if (row("Frame Rate", kFrameRateLabels, 3, fr)) {
            s.video.targetFps = kFrameRates[fr];
            s.video.matchDisplay = false;
            s.video.unlimitedFps = false;
            display = true;
        }
        any |= boolRow("Show FPS", s.enhancements.showFps);
        display |= row("UI Size", kUiSizeLabels, 3, m.uiSize);
        any |= boolRow("Touch Controls", m.touchControls);
        any |= percentRow("Touch Opacity", 20, 100, 5, m.touchOpacity);
        any |= percentRow("Touch Size", 70, 150, 10, m.touchSize);
        int stick = m.floatingStick ? 1 : 0;
        if (row("Control Stick", kStickLabels, 2, stick)) {
            m.floatingStick = stick == 1;
            any = true;
        }
        any |= boolRow("Vibration", m.haptics);
        ImGui::EndTable();
    }
    if (display) applyDisplay();
    if (display || any) {
        port::saveSettings();
        logSettings("changed");
    }

    ImGui::SeparatorText("About this device");
    ImGui::TextWrapped("%s, %s. %s.", native::deviceModel().c_str(), native::osVersion().c_str(), memorySummary().c_str());
    if (const char* pad = touch::physicalController()) ImGui::TextWrapped("Controller: %s (touch controls hidden).", pad);
    ImGui::SeparatorText("Logs");
    ImGui::TextWrapped("Every session is logged in detail and saved as it runs: Files app > On My iPhone > "
                       "Luigi's Mansion > Logs (the last 10 sessions). This session: %s",
                       sessionlog::path().filename().string().c_str());
    ImGui::TextWrapped("Saves, settings and disc images are in the same folder.");
    return false;  // already saved
}

std::string chooseDisc(const char* gameId) {
    const fs::path docs = native::documentsDir();
    std::vector<std::string> notes;
    auto scan = [&]() -> std::string {
        notes.clear();
        std::error_code ec;
        std::vector<fs::path> found;
        for (const auto& e : fs::directory_iterator(docs, ec))
            if (e.is_regular_file(ec) && discExtension(e.path())) found.push_back(e.path());
        std::sort(found.begin(), found.end());
        for (const auto& p : found) {
            std::string why;
            const std::string id = discGameId(p, why);
            sessionlog::line("disc", "%s: %s", p.filename().string().c_str(), id.empty() ? why.c_str() : id.c_str());
            if (id == gameId) return p.string();
            if (!id.empty()) why = std::string("Luigi's Mansion ") + regionName(id) + "; this app was built for " + regionName(gameId);
            notes.push_back(p.filename().string() + ": " + why);
        }
        return {};
    };
    if (std::string p = scan(); !p.empty()) return p;
    if (!native::isIOS()) return {};

    sessionlog::line("disc", "no %s disc image in %s: showing the disc page", gameId, port::displayPath(docs).c_str());
    recomp::ui::install(recomp::ui::Context::Setup);
    struct Pick {
        std::mutex m;
        bool done = false, busy = false;
        fs::path path;
        std::string error;
    };
    static Pick sPick;
    std::string status;
    for (;;) {
        for (const AuroraEvent* ev = aurora_update(); ev && ev->type != AURORA_NONE; ++ev) {
            if (ev->type == AURORA_SDL_EVENT) handleEvent(ev->sdl);
            if (ev->type == AURORA_EXIT) return {};
        }
        {
            std::lock_guard lk(sPick.m);
            if (sPick.done) {
                sPick.done = false;
                sPick.busy = false;
                if (!sPick.error.empty()) {
                    status = "Could not import the file: " + sPick.error;
                } else if (!sPick.path.empty()) {
                    std::string why;
                    const std::string id = discGameId(sPick.path, why);
                    sessionlog::line("disc", "picked %s: %s", sPick.path.filename().string().c_str(), id.empty() ? why.c_str() : id.c_str());
                    if (id == gameId) return sPick.path.string();
                    status = sPick.path.filename().string() + ": " +
                             (id.empty() ? why : std::string("Luigi's Mansion ") + regionName(id) + ", but this app was built for " + regionName(gameId));
                    std::error_code ec;
                    fs::remove(sPick.path, ec);  // our own copy: do not keep a wrong 1.4 GB file around
                }
            }
        }
        if (!aurora_begin_frame()) {
            SDL_Delay(16);
            continue;
        }
        const ImGuiViewport* vp = ImGui::GetMainViewport();
        float l = 0, t = 0, r = 0, b = 0;
        safeAreaInsets(l, t, r, b);
        ImGui::SetNextWindowPos(ImVec2(vp->Pos.x + l, vp->Pos.y + t));
        ImGui::SetNextWindowSize(ImVec2(vp->Size.x - l - r, vp->Size.y - t - b));
        ImGui::Begin("##disc", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);
        if (ImFont* big = recomp::ui::largeFont()) ImGui::PushFont(big);
        ImGui::TextUnformatted("Luigi's Mansion");
        if (recomp::ui::largeFont()) ImGui::PopFont();
        ImGui::Spacing();
        ImGui::TextWrapped("This app was built from your own Luigi's Mansion %s. Choose your disc image of the same "
                           "release (ISO, GCM, RVZ, WIA, CISO, GCZ). It is copied into the app once; the game reads "
                           "its pictures, sounds and levels from it.", regionName(gameId));
        ImGui::Spacing();
        bool busy;
        {
            std::lock_guard lk(sPick.m);
            busy = sPick.busy;
        }
        const float bh = ImGui::GetFrameHeight() * 2.0f;
        ImGui::BeginDisabled(busy);
        if (ImGui::Button(busy ? "Importing..." : "Choose disc image", ImVec2(ImGui::GetContentRegionAvail().x * 0.6f, bh))) {
            {
                std::lock_guard lk(sPick.m);
                sPick.busy = true;
            }
            status.clear();
            sessionlog::line("disc", "opening the Files picker");
            native::pickDiscImage([](const fs::path& p, const std::string& err) {
                std::lock_guard lk(sPick.m);
                sPick.path = p;
                sPick.error = err;
                sPick.done = true;
            });
        }
        ImGui::SameLine();
        if (ImGui::Button("Check again", ImVec2(ImGui::GetContentRegionAvail().x, bh))) {
            if (std::string p = scan(); !p.empty()) {
                ImGui::EndDisabled();
                ImGui::End();
                aurora_end_frame();
                return p;
            }
            status = notes.empty() ? "No disc image in the app's folder yet." : "";
        }
        ImGui::EndDisabled();
        ImGui::Spacing();
        ImGui::TextWrapped("You can also copy the disc image with the Files app into On My iPhone > Luigi's Mansion, "
                           "then tap Check again.");
        if (!status.empty()) {
            ImGui::Spacing();
            ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.4f, 1.0f), "%s", status.c_str());
        }
        for (const auto& n : notes) ImGui::TextDisabled("%s", n.c_str());
        ImGui::End();
        aurora_end_frame();
    }
}

}  // namespace mobile
