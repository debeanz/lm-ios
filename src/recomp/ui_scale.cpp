// Menu size (issue #20; settings interface.menu_scale, F1 > Visuals > General "Menu size").
//
// Dear ImGui here is 1.91 (fixed-size font atlas): a larger menu rebuilds the atlas with the
// default font (ProggyClean, made for 13 px) at round(13 x scale) physical pixels and replaces the
// style with ImGui's default style scaled once (ScaleAllSizes on a clean copy, never cumulative),
// so text is rasterized at its real size instead of a 13 px atlas being stretched (blurry). Both
// happen in Aurora's ImGui frame callback before NewFrame (Aurora patch 0238: the atlas is locked
// during a frame), so a change applies from the next frame.
//
// Auto: the window height (1080 lines = 100%, 4K = 200%), the display's scaling setting (Windows
// 150% = 150%) or 150% on a Steam Deck (7" 800p), whichever is largest, rounded down to a step;
// the setup screen (a full-window page) is larger by default (600 lines = 100%, as before). Any
// size is limited to what still fits the window (fitPercent).
//
// Windows with a size the player can change and imgui.ini keeps (the F1 menu, the model viewer,
// debug viewers) go through beforeWindow(): default and minimum size scale with the menu, a
// saved size is rescaled when the menu size changes (imgui.ini also records the scale it was saved
// at, section [LMUiScale]; a size saved by an older version is taken as 100%), and the window is
// kept on the screen.
#include "recomp/ui_scale.h"

#include "mobile/mobile.h"
#include "mobile/native.h"
#include "port/settings.h"

#include <aurora/aurora.h>
#include <aurora/imgui.h>
#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>

namespace recomp::ui {
namespace {

Context sContext = Context::Game;
bool sInstalled = false;
float sScale = 1.0f;
int sPercent = 100, sAuto = 100, sFit = kSteps[kStepCount - 1];
float sFontPx = 0.0f;       // atlas font size, physical pixels
float sFontDensity = 0.0f;  // physical pixels per ImGui unit the atlas was built for
ImFont* sLarge = nullptr;     // the same font kLarge times larger (largeFont)
constexpr float kLarge = 1.5f;
ImGuiStyle sBaseStyle;
bool sHaveBase = false;
float sStyleScale = 0.0f;

// Largest step at or below a scale (at least the smallest step).
int stepDown(float s) {
    int best = kSteps[0];
    for (int p : kSteps)
        if (float(p) <= s * 100.0f + 0.5f) best = p;
    return best;
}
int stepNearest(int pct) {
    int best = kSteps[0];
    for (int p : kSteps)
        if (std::abs(p - pct) < std::abs(best - pct)) best = p;
    return best;
}

bool steamDeck() {
    // Steam sets SteamDeck=1 on the Deck (game mode and desktop mode).
    static const bool sDeck = [] {
        const char* v = std::getenv("SteamDeck");
        return v && *v && std::strcmp(v, "0") != 0;
    }();
    return sDeck;
}

// Saved scale per window (imgui.ini [LMUiScale][<window name>] Scale=<f>), and the scale each
// window was last drawn at.
struct WinState {
    std::string name;
    float iniScale = 0.0f;  // from imgui.ini (0: none saved)
    float scale = 0.0f;     // last frame's (0: not drawn yet this session)
};
std::unordered_map<ImGuiID, WinState> sWins;
ImGuiID sAnchorWindow = 0;  // window to rescale about sAnchor at the next size change
ImVec2 sAnchor;

void* iniReadOpen(ImGuiContext*, ImGuiSettingsHandler*, const char* name) {
    WinState& w = sWins[ImHashStr(name)];
    w.name = name;
    return &w;
}
void iniReadLine(ImGuiContext*, ImGuiSettingsHandler*, void* entry, const char* line) {
    float f = 0.0f;
    if (std::sscanf(line, "Scale=%f", &f) == 1 && f > 0.1f && f < 10.0f) static_cast<WinState*>(entry)->iniScale = f;
}
void iniWriteAll(ImGuiContext*, ImGuiSettingsHandler* handler, ImGuiTextBuffer* buf) {
    for (const auto& [id, w] : sWins) {
        const float s = w.scale > 0.0f ? w.scale : w.iniScale;
        if (s <= 0.0f || w.name.empty()) continue;
        buf->appendf("[%s][%s]\nScale=%.3f\n\n", handler->TypeName, w.name.c_str(), s);
    }
}

void frameCallback(const AuroraWindowSize* size) {
    if (ImGui::GetCurrentContext() == nullptr || size == nullptr) return;
    ImGuiIO& io = ImGui::GetIO();
    const float logicalW = float(std::max(size->width, 1u)), logicalH = float(std::max(size->height, 1u));
    const float density = size->width > 0 && size->native_fb_width > 0 ? float(size->native_fb_width) / logicalW : 1.0f;
    // Display scaling relative to ImGui units (Windows: SDL's display scale, pixels = units;
    // macOS/Wayland: units are already scaled points).
    const float dpi = size->scale > 0.0f ? size->scale / density : 1.0f;

    float autoScale = std::max({1.0f, logicalH / 1080.0f, dpi});
    if (steamDeck()) autoScale = std::max(autoScale, 1.5f);
    if (sContext == Context::Setup) autoScale = std::max(autoScale, logicalH / 600.0f);
    // The menu needs about 560 x 330 units at 100% to be usable (tab bar, a few rows).
    float fit = std::min(logicalW / 560.0f, logicalH / 330.0f);
    if (mobile::enabled()) {
        // Phone (src/mobile): iPhone-sized text (about 16 pt with the system font) times the menu
        // text size setting; the menu panel scrolls, so it only has to fit sideways.
        autoScale = 1.25f * mobile::uiMultiplier();
        fit = std::min(logicalW / 420.0f, logicalH / 200.0f);
    }
    sFit = stepDown(fit);
    sAuto = std::min(stepDown(autoScale), sFit);
    const int setting = port::settings().ui.menuScale;
    int pct = setting <= 0 ? sAuto : std::min(stepNearest(setting), sFit);
    pct = std::max(pct, kSteps[0]);
    const float s = float(pct) / 100.0f;
    if (pct != sPercent || sFontPx == 0.0f) {
        std::printf("[ui] menu size %d%% (%s; auto %d%%, fits %d%%; window %.0fx%.0f, display scale %.2f)\n", pct,
                    setting <= 0 ? "auto" : "set", sAuto, sFit, logicalW, logicalH, size->scale);
        std::fflush(stdout);
    }
    sPercent = pct;
    sScale = s;

    // Font: rasterized at the size it is shown at, in physical pixels.
    const float fontPx = std::round(13.0f * s * density);
    if (fontPx != sFontPx || density != sFontDensity) {
        io.Fonts->Clear();
        // Phone: the system font (San Francisco, read from the device) instead of the small bitmap
        // font scaled up, which looked pixelated on a phone; anti-aliased, so no pixel snapping.
        // (Both branches lvalues: the atlas keeps pointing at this data after the callback returns,
        // so it must be native.cpp's static buffer, never a temporary copy.)
        std::string fontName;
        static const std::vector<unsigned char> kNoFont;
        const std::vector<unsigned char>& sys = mobile::enabled() ? mobile::native::systemFont(fontName) : kNoFont;
        if (!sys.empty()) {
            ImFontConfig cfg;
            cfg.FontDataOwnedByAtlas = false;  // the data stays in native.cpp's static buffer
            cfg.OversampleH = 2;
            cfg.OversampleV = 1;
            void* data = const_cast<unsigned char*>(sys.data());
            io.Fonts->AddFontFromMemoryTTF(data, int(sys.size()), fontPx, &cfg);  // Fonts[0]
            sLarge = io.Fonts->AddFontFromMemoryTTF(data, int(sys.size()), std::round(fontPx * kLarge), &cfg);
            static bool sLogged = false;
            if (!sLogged) {
                sLogged = true;
                std::printf("[ui] menu font: %s\n", fontName.c_str());
            }
        } else {
            if (mobile::enabled()) std::printf("[ui] menu font: ImGui's default (%s)\n", fontName.c_str());
            ImFontConfig cfg;
            cfg.OversampleH = cfg.OversampleV = 1;
            cfg.PixelSnapH = true;
            cfg.SizePixels = fontPx;
            io.Fonts->AddFontDefault(&cfg);  // Fonts[0]: ImGui's default
            cfg.SizePixels = std::round(13.0f * s * kLarge * density);
            sLarge = io.Fonts->AddFontDefault(&cfg);
        }
        io.FontGlobalScale = 1.0f / density;  // physical pixels back to ImGui units: 1:1 on screen
        sFontPx = fontPx;
        sFontDensity = density;
        // (left unbuilt: Aurora builds and uploads it after this callback)
    }

    // Style: ImGui's default sizes scaled once from a clean copy.
    if (!sHaveBase) {
        sBaseStyle = ImGui::GetStyle();
        sHaveBase = true;
    }
    if (s != sStyleScale) {
        ImGuiStyle st = sBaseStyle;
        st.ScaleAllSizes(s);
        // (ScaleAllSizes leaves line widths alone; the heading rule of SeparatorText grows with
        // the text, other borders stay 1 px.)
        st.SeparatorTextBorderSize = std::max(1.0f, std::round(sBaseStyle.SeparatorTextBorderSize * s));
        st.MouseCursorScale = std::max(1.0f, sBaseStyle.MouseCursorScale * s);
        if (mobile::enabled()) {
            // Phone: thumb-sized targets. TouchExtraPadding widens every widget's hit area without
            // moving it; taller frames and more row spacing keep neighbours from being hit; wide
            // scrollbars and slider grabs a thumb can hold; rounded shapes as elsewhere on iOS.
            st.TouchExtraPadding = ImVec2(6.0f * s, 4.0f * s);
            st.FramePadding = ImVec2(st.FramePadding.x * 1.6f, st.FramePadding.y * 1.9f);
            st.ItemSpacing = ImVec2(st.ItemSpacing.x * 1.4f, st.ItemSpacing.y * 1.8f);
            st.ItemInnerSpacing = ImVec2(st.ItemInnerSpacing.x * 1.5f, st.ItemInnerSpacing.y * 1.5f);
            st.ScrollbarSize = std::max(st.ScrollbarSize * 1.8f, 18.0f * s);
            st.GrabMinSize = std::max(st.GrabMinSize * 2.2f, 22.0f * s);
            st.WindowPadding = ImVec2(st.WindowPadding.x * 1.5f, st.WindowPadding.y * 1.2f);
            st.WindowRounding = 14.0f * s;
            st.ChildRounding = st.PopupRounding = 10.0f * s;
            st.FrameRounding = st.GrabRounding = 7.0f * s;
            st.TabRounding = 8.0f * s;
            st.ScrollbarRounding = 9.0f * s;
            st.WindowBorderSize = std::max(1.0f, std::round(1.5f * s));
            st.FrameBorderSize = 1.0f;
            st.PopupBorderSize = 1.0f;
            st.TabBarBorderSize = 2.0f;
            // "Mansion night" (the approved Dusk-style mockup; menu_phone.h has the same colours for
            // the menu's own rows): night-violet glass, a thin violet edge, moon-gold accent, quiet
            // controls. Every port window on the phone (menu, FPS, notices, shader preparation, disc
            // page) uses it.
            const auto rgb = [](int r, int g, int b, float a = 1.0f) { return ImVec4(r / 255.0f, g / 255.0f, b / 255.0f, a); };
            const ImVec4 accent = rgb(230, 191, 98), edge = rgb(75, 70, 112);
            const ImVec4 none = rgb(0, 0, 0, 0.0f), plate = rgb(230, 225, 255, 0.035f);
            ImVec4* c = st.Colors;
            c[ImGuiCol_Text] = rgb(231, 228, 242);
            c[ImGuiCol_TextDisabled] = rgb(133, 127, 166);
            c[ImGuiCol_WindowBg] = rgb(19, 18, 31, 0.93f);
            c[ImGuiCol_ChildBg] = none;
            c[ImGuiCol_PopupBg] = rgb(24, 23, 39, 0.98f);
            c[ImGuiCol_Border] = edge;
            c[ImGuiCol_BorderShadow] = none;
            c[ImGuiCol_FrameBg] = plate;
            c[ImGuiCol_FrameBgHovered] = rgb(230, 191, 98, 0.11f);
            c[ImGuiCol_FrameBgActive] = rgb(230, 191, 98, 0.18f);
            c[ImGuiCol_TitleBg] = rgb(24, 23, 39);
            c[ImGuiCol_TitleBgActive] = rgb(30, 28, 48);
            c[ImGuiCol_TitleBgCollapsed] = rgb(24, 23, 39, 0.8f);
            c[ImGuiCol_MenuBarBg] = plate;
            c[ImGuiCol_ScrollbarBg] = none;
            c[ImGuiCol_ScrollbarGrab] = rgb(220, 215, 255, 0.24f);
            c[ImGuiCol_ScrollbarGrabHovered] = rgb(220, 215, 255, 0.35f);
            c[ImGuiCol_ScrollbarGrabActive] = accent;
            c[ImGuiCol_CheckMark] = accent;
            c[ImGuiCol_SliderGrab] = accent;
            c[ImGuiCol_SliderGrabActive] = accent;
            c[ImGuiCol_Button] = plate;
            c[ImGuiCol_ButtonHovered] = rgb(230, 191, 98, 0.11f);  // (also the controller's focus)
            c[ImGuiCol_ButtonActive] = rgb(230, 191, 98, 0.18f);
            c[ImGuiCol_Header] = rgb(230, 191, 98, 0.11f);
            c[ImGuiCol_HeaderHovered] = rgb(230, 191, 98, 0.15f);
            c[ImGuiCol_HeaderActive] = rgb(230, 191, 98, 0.22f);
            c[ImGuiCol_Separator] = rgb(220, 215, 255, 0.08f);
            c[ImGuiCol_SeparatorHovered] = accent;
            c[ImGuiCol_SeparatorActive] = accent;
            c[ImGuiCol_ResizeGrip] = none;
            c[ImGuiCol_ResizeGripHovered] = none;
            c[ImGuiCol_ResizeGripActive] = none;
            c[ImGuiCol_Tab] = none;
            c[ImGuiCol_TabHovered] = rgb(230, 191, 98, 0.11f);
            c[ImGuiCol_TabSelected] = none;
            c[ImGuiCol_TabSelectedOverline] = accent;
            c[ImGuiCol_TabDimmed] = none;
            c[ImGuiCol_TabDimmedSelected] = none;
            c[ImGuiCol_TableHeaderBg] = plate;
            c[ImGuiCol_TableBorderStrong] = rgb(220, 215, 255, 0.10f);
            c[ImGuiCol_TableBorderLight] = rgb(220, 215, 255, 0.06f);
            c[ImGuiCol_TableRowBg] = none;
            c[ImGuiCol_TableRowBgAlt] = plate;
            c[ImGuiCol_TextSelectedBg] = rgb(230, 191, 98, 0.30f);
            // The menu's rows draw their own gold outline when selected; ImGui's nav frame would be a
            // second one around it (focused buttons show their hovered colour instead).
            c[ImGuiCol_NavCursor] = none;
            c[ImGuiCol_ModalWindowDimBg] = rgb(8, 7, 14, 0.6f);
            st.TabRounding = 0.0f;  // flat tabs with the accent underline
            st.WindowRounding = 10.0f * s;
            st.ChildRounding = st.PopupRounding = 8.0f * s;
            st.FrameRounding = st.GrabRounding = 6.0f * s;
            st.WindowBorderSize = st.PopupBorderSize = std::max(1.0f, std::round(s));
            st.FrameBorderSize = 0.0f;
        }
        ImGui::GetStyle() = st;
        sStyleScale = s;
    }
}

}  // namespace

void install(Context context) {
    sContext = context;
    if (sInstalled) return;
    sInstalled = true;
    if (ImGui::GetCurrentContext() != nullptr) {
        ImGuiSettingsHandler h;
        h.TypeName = "LMUiScale";
        h.TypeHash = ImHashStr("LMUiScale");
        h.ReadOpenFn = iniReadOpen;
        h.ReadLineFn = iniReadLine;
        h.WriteAllFn = iniWriteAll;
        ImGui::AddSettingsHandler(&h);
    }
    aurora_imgui_set_frame_callback(frameCallback);
}

float scale() { return sScale; }
ImFont* largeFont() { return sLarge; }
int autoPercent() { return sAuto; }
int fitPercent() { return sFit; }
int percent() { return sPercent; }

void setAnchor(const char* window, ImVec2 p) {
    sAnchorWindow = ImHashStr(window);
    sAnchor = p;
}

void beforeWindow(const char* name, ImVec2 defaultSize, ImVec2 minSize) {
    const float s = sScale;
    const ImVec2 ds = ImGui::GetIO().DisplaySize;
    const ImGuiID id = ImHashStr(name);
    WinState& st = sWins[id];
    if (st.name.empty()) st.name = name;
    const auto fitTo = [&](ImVec2 v) { return ImVec2(std::min(v.x, ds.x), std::min(v.y, ds.y)); };
    ImGui::SetNextWindowSize(fitTo(ImVec2(defaultSize.x * s, defaultSize.y * s)), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSizeConstraints(fitTo(ImVec2(minSize.x * s, minSize.y * s)), ds);

    ImGuiWindow* w = ImGui::FindWindowByID(id);
    ImVec2 pos, sz, scroll(0, 0);
    bool have = false;
    if (w != nullptr) {
        pos = w->Pos;
        sz = w->Size;
        scroll = w->Scroll;
        have = true;
    } else if (const ImGuiWindowSettings* ws = ImGui::FindWindowSettingsByID(id)) {
        // First Begin of the session: the size and place imgui.ini saved.
        pos = ImVec2(ws->Pos.x, ws->Pos.y);
        sz = ImVec2(ws->Size.x, ws->Size.y);
        have = sz.x > 0 && sz.y > 0;
    }
    // Scale the window was last sized at: this session's, else the one saved with it (a size from
    // a version without this option was made at 100%).
    const float prev = st.scale > 0.0f ? st.scale : have ? (st.iniScale > 0.0f ? st.iniScale : 1.0f) : s;
    if (have && std::fabs(prev - s) > 0.001f) {
        // Rescale about the anchor (the point under the mouse when the size was changed with it,
        // so what was under the mouse stays there), else about the window's centre.
        const float r = s / prev;
        const ImVec2 a = sAnchorWindow == id && w != nullptr ? sAnchor : ImVec2(pos.x + sz.x * 0.5f, pos.y + sz.y * 0.5f);
        ImVec2 nsz = fitTo(ImVec2(sz.x * r, sz.y * r));
        ImVec2 npos(a.x - (a.x - pos.x) * r, a.y - (a.y - pos.y) * r);
        npos.x = std::clamp(npos.x, 0.0f, std::max(0.0f, ds.x - nsz.x));
        npos.y = std::clamp(npos.y, 0.0f, std::max(0.0f, ds.y - nsz.y));
        ImGui::SetNextWindowSize(nsz, ImGuiCond_Always);
        ImGui::SetNextWindowPos(npos, ImGuiCond_Always);
        if (w != nullptr) ImGui::SetNextWindowScroll(ImVec2(scroll.x * r, scroll.y * r));
        ImGui::MarkIniSettingsDirty();
    } else if (w != nullptr) {
        // Keep it on the screen (a smaller window, a larger menu size, a saved place off-screen).
        const ImVec2 nsz = fitTo(sz);
        const ImVec2 npos(std::clamp(pos.x, 0.0f, std::max(0.0f, ds.x - nsz.x)),
                          std::clamp(pos.y, 0.0f, std::max(0.0f, ds.y - nsz.y)));
        if (npos.x != pos.x || npos.y != pos.y) ImGui::SetNextWindowPos(npos, ImGuiCond_Always);
    }
    if (st.scale != s && sAnchorWindow == id) sAnchorWindow = 0;
    st.scale = s;
}

}  // namespace recomp::ui
