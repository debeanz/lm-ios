// Second batch of restored unused content: shared wiring and the F1 menu sections
// (docs/UNUSED_CONTENT.md, "Second batch"). The features live in their own files
// (play_as_mario.cpp, luigi_states.cpp, jukebox.cpp, gallery.cpp, text_archive.cpp,
// placements.cpp, spawn.cpp); this file calls them from the existing hooks and draws their options.
#include "recomp/unused2.h"

#include "port/install.h"
#include "port/settings.h"
#include "recomp/coop.h"
#include "recomp/dev_tools.h"
#include "recomp/guest_util.h"
#include "recomp/hidden_mansion.h"
#include "recomp/hle.h"
#include "recomp/runtime.h"
#include "recomp/version.h"

#include "recomp/menu.h"

#include <imgui.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <mutex>

namespace recomp::unused2 {

namespace {
constexpr u32 kExtraBase = 0x819A0000, kExtraEnd = 0x81F00000;  // below: hleAlloc (runtime.cpp)
u32 sExtraNext = kExtraBase;
std::mutex sExtraM;
bool sDirty = false;

void tip(const char* text) {
    recomp::menu::ItemTooltip("%s", text);
}
void note(const char* text) {
    ImGui::SameLine();
    ImGui::TextDisabled("(%s)", text);
}
}  // namespace

u32 extraAlloc(u32 size, u32 align) {
    std::lock_guard lk(sExtraM);
    const u32 p = (sExtraNext + align - 1) & ~(align - 1);
    if (p + size > kExtraEnd) {
        log("[unused2] extra guest memory exhausted (%u bytes wanted)", size);
        return 0;
    }
    sExtraNext = p + size;
    std::memset(hostp(p), 0, size);
    return p;
}

// ---- disc overlay ----------------------------------------------------------------------------
// Files served rebuilt while an option asks for them. The decision is made when the game opens
// the file, so turning an option on or off applies the next time the file is loaded.
namespace {
constexpr s32 kOverlayBase = 0x50000000;  // virtual FST entries (HM 0x40000000, rooms 0x48000000, maps 0x4C000000)
struct Served {
    const char* path;
    std::vector<u8> (*build)();
    u32 (*wanted)();  // 0: serve the disc file; otherwise a key of the options the copy depends on
    std::vector<u8> bytes;
    u32 builtKey = 0;
};
Served sServed[] = {
    {"/Map/map2.szp", buildMapArchive2, mapArchive2Key},
    {"/Map/map3.szp", buildMapArchive3, mapArchive3Key},
};
std::mutex sServedM;
}  // namespace

s32 overlayEntry(const char* guestPath) {
    std::lock_guard lk(sServedM);
    for (size_t i = 0; i < std::size(sServed); i++) {
        Served& s = sServed[i];
        if (std::strcmp(guestPath, s.path)) continue;
        // A map the Hidden Mansion overlay serves (the other release's hidden mansion) wins: the
        // rebuilt copy is made from this disc's normal mansion.
        if (!std::strncmp(guestPath, "/Map/", 5) && hm::redirect(guestPath)) return -1;
        const u32 key = s.wanted();
        if (!key) continue;
        if (s.builtKey != key) {
            s.builtKey = key;
            s.bytes = s.build();
        }
        if (s.bytes.empty()) return -1;
        log("[unused2] serving rebuilt %s", s.path);
        return kOverlayBase + s32(i);
    }
    return -1;
}
bool isOverlayEntry(s32 entry) { return entry >= kOverlayBase && entry < kOverlayBase + s32(std::size(sServed)); }
u32 overlayLength(s32 entry) {
    std::lock_guard lk(sServedM);
    return isOverlayEntry(entry) ? u32(sServed[entry - kOverlayBase].bytes.size()) : 0;
}
s32 overlayRead(s32 entry, void* dst, s32 length, s32 offset) {
    std::lock_guard lk(sServedM);
    if (!isOverlayEntry(entry)) return -1;
    const std::vector<u8>& b = sServed[entry - kOverlayBase].bytes;
    if (offset < 0 || length < 0 || size_t(offset) > b.size()) return -1;
    const size_t n = std::min(size_t(length), b.size() - size_t(offset));
    std::memcpy(dst, b.data() + offset, n);
    if (n < size_t(length)) std::memset(static_cast<u8*>(dst) + n, 0, size_t(length) - n);
    return s32(n);
}

void onMapSetup(Ctx& c) {
    statesMapSetup(c);
    placementsMapSetup(c);
}

void onFrame(Ctx& c) {
    static int sBorder = -1;  // patch 50: the bordered 3D view (a USA and a European line)
    const int border = port::settings().unused2.borderedView;
    if (border != sBorder) {
        sBorder = border;
        setPatch(50, border != 0);
    }
    marioFrame(c);
    endingFrame(c);
    statesFrame(c);
    jukeboxFrame(c);
    spawnFrame(c);
    placementsFrame(c);
}

void onModelLoaded(Ctx& c, u32 entry) { marioModelLoaded(c, entry); }

}  // namespace recomp::unused2

namespace recomp {
// Game-scene view setup (fn_80010F20): two views while the Game Boy Horror camera asks for its
// view (r13-0x7F70 = 2, "Game Boy Horror camera" option), then view 0 = screen, view 1 = the GBH
// screen rectangle the HUD reported (fn_80010EBC: lbl_804D0008/0A, lbl_804D8162/64), view 2 the
// full-screen 2D view. The bordered view (patch 50) branches to the case that only sets view 0's
// framed rectangle, so with both options on, views 1 and 2 kept stale rectangles and the GBH
// camera's view covered the screen. Their rectangles are written first, as the two-view case
// does; the bordered case then sets view 0 (views +0xD0 x, +0xD4 y, +0xD8 w, +0xDC h).
HLE(fn_80010F20) {
    if (g_patches[50] && rd32(VA(0x804CFFF0)) == 2) {
        const u32 views = VA(0x803A4C80);
        const u32 v1 = rd32(views + 4), v2 = rd32(views + 8);
        if (gu::inMem1(v1) && gu::inMem1(v2)) {
            wr32(v1 + 0xD0, u32(s32(s16(rd16(VA(0x804D0008))))));
            wr32(v1 + 0xD4, u32(s32(s16(rd16(VA(0x804D000A))))));
            wr32(v1 + 0xD8, u32(s32(s16(rd16(VA(0x804D8162))))));
            wr32(v1 + 0xDC, u32(s32(s16(rd16(VA(0x804D8164))))));
            wr32(v2 + 0xD0, 1);
            wr32(v2 + 0xD4, 1);
            wr32(v2 + 0xD8, 0x27F);
            wr32(v2 + 0xDC, 0x1DF);
        }
    }
    callOriginal(c, "fn_80010F20");
}
}  // namespace recomp

namespace recomp::unused2 {

void unusedSection(UnusedGroup group) {
    auto& u = port::settings().unused2;
    switch (group) {
    case UnusedGroup::Player:
        sDirty |= recomp::menu::Checkbox("Play as Mario", &u.playAsMario);
        tip("The player uses the unused Mario model left in the game (bmario: the character table has a\n"
            "whole Mario player set that nothing places). Mario has Luigi's skeleton, so Luigi's\n"
            "animations, Poltergust, flashlight and Game Boy Horror work unchanged. Mario's model has\n"
            "no eye animations (he does not blink); the voice stays Luigi's (the game has no Mario\n"
            "voice clips for these actions). Also used by the cutscene Luigis.");
        note("next map load");
        break;
    case UnusedGroup::Characters:
        sDirty |= recomp::menu::Checkbox("Unused ghost placements", &u.ghostPlacements);
        tip("Places ghosts on paths the mansion's map data has but no ghost uses: Slim Bankshot in the\n"
            "Hidden Room (hustler_01), two of Mr. Bones' skeletons in the Courtyard (skul_23_1/2), a\n"
            "ghost in room 8 (topoo_8_1), nine more Ghost Guys and a ghost on the roof (heypo_60_4..12,\n"
            "poo_60_1), and three more ghost generators in the training room (mg_path).");
        note("next mansion / training load");
        sDirty |= recomp::menu::Checkbox("Pink mice", &u.pinkMice);
        tip("The mice use the unused rat2 colour set (pink) and its parameter file rat2.prm.");
        sDirty |= recomp::menu::Checkbox("Blue Ghost Guys", &u.blueGhostGuys);
        tip("Two kinds of Ghost Guys use the unused heypo9 / heypo10 colour sets and parameter files\n"
            "(blue / cyan glow).");
        sDirty |= recomp::menu::Checkbox("Extra Speedy Spirits 16-20 (not original)", &u.extraSpeedySpirits);
        tip("NOT ORIGINAL CONTENT: the game has Speedy Spirit types 16-20 that were never placed and have\n"
            "no rewards. This places them in five rooms that have none (chosen by the port) with rewards\n"
            "modelled on the retail ones. They appear like the others when the room's lights are off.");
        note("next mansion load");
        break;
    case UnusedGroup::Screens:
        sDirty |= recomp::menu::Checkbox("Bordered view", &u.borderedView);
        tip("The game's view setup has a third, unused layout: a fixed 560x400 3D view (40,30 to\n"
            "560,430) with a black border, like a TV-safe window. The HUD stays where it is.");
        sDirty |= recomp::menu::Checkbox("Ending pictures of Luigi", &u.endingPictures);
        tip("The ending's rank screen shows one of three unused pictures of Luigi next to its\n"
            "message: sitting with a flower (ranks H to D), one V sign (C, B) or two V signs (A).");
        break;
    case UnusedGroup::TwoPlayer:
        sDirty |= recomp::menu::Checkbox("Mario as player 2", &u.marioP2);
        tip("In the 2-player mode, player 2 is Mario (the unused bmario model) instead of a\n"
            "second Luigi. Needs \"2-player mode (unfinished)\".");
        break;
    }
    if (sDirty) {
        port::saveSettings();
        sDirty = false;
    }
}

void gameplaySection() {
    auto& u = port::settings().unused2;
    recomp::menu::SeparatorText("Unfinished Luigi states");
    ImGui::TextDisabled("Restored from code left in the game. All off by default.");
    sDirty |= recomp::menu::Checkbox("Poison mushrooms poison Luigi", &u.poisonState);
    tip("Touching a poison mushroom no longer shrinks Luigi. Instead he plays the unused poisoned\n"
        "animation (doku_lp, with the dark \"poisoned\" eyes), stands still and loses health slowly.\n"
        "The mushroom still takes 10 coins. Poison alone never takes Luigi's last point of health.");
    {
        ImGui::BeginDisabled(!u.poisonState);
        ImGui::Indent();
        sDirty |= recomp::menu::SliderFloat("Poison damage", &u.poisonDamage, 1.0f, 100.0f, "%.0f HP", ImGuiSliderFlags_AlwaysClamp);
        tip("Total health the poison takes (it never takes Luigi's last point).");
        sDirty |= recomp::menu::SliderFloat("Poison duration", &u.poisonSeconds, 1.0f, 20.0f, "%.1f s", ImGuiSliderFlags_AlwaysClamp);
        tip("How long the poison lasts; the damage is spread over this time.");
        ImGui::Unindent();
        ImGui::EndDisabled();
    }
    sDirty |= recomp::menu::Checkbox("Crawling fear", &u.crawlFear);
    tip("When a ghost startles Luigi from behind, he falls into the game's unused panic state:\n"
        "about 5 seconds sitting and scooting along the floor (unused cow1wait / cow1move\n"
        "animations), unable to use the Poltergust, taking half damage, then he gets up.\n"
        "The game's fear system has this mode but nothing ever triggers it.");
    sDirty |= recomp::menu::Checkbox("Vacuum overheat", &u.overheat);
    tip("Cut feature: vacuuming too long overheats the Poltergust. The heat shows on the element\n"
        "meter (fire colours) while it is above zero. When it is full the Poltergust catches fire,\n"
        "Luigi is burned (the game's own on-fire state) and the vacuum stops working until it\n"
        "has cooled to half; the meter shakes meanwhile. Not vacuuming cools it, holding L\n"
        "(blowing air or spraying) cools it faster.");
    {
        ImGui::BeginDisabled(!u.overheat);
        ImGui::Indent();
        sDirty |= recomp::menu::SliderFloat("Time to overheat", &u.overheatFillSeconds, 5.0f, 120.0f, "%.0f s of vacuuming", ImGuiSliderFlags_AlwaysClamp);
        tip("Continuous vacuuming until the Poltergust overheats.");
        sDirty |= recomp::menu::SliderFloat("Cool-down time", &u.overheatCoolSeconds, 5.0f, 120.0f, "%.0f s (full to empty)", ImGuiSliderFlags_AlwaysClamp);
        tip("Time for a full meter to cool back to empty while not vacuuming.");
        sDirty |= recomp::menu::SliderFloat("Cooling while blowing (L)", &u.overheatBlowCool, 1.0f, 10.0f, "%.1fx faster", ImGuiSliderFlags_AlwaysClamp);
        tip("How much faster it cools while holding L (blowing air or spraying).");
        sDirty |= recomp::menu::SliderInt("Burn damage", &u.overheatDamage, 0, 50, "%d HP", ImGuiSliderFlags_AlwaysClamp);
        tip("Health Luigi loses when the Poltergust catches fire.");
        ImGui::Unindent();
        ImGui::EndDisabled();
    }
    if (sDirty) {
        port::saveSettings();
        sDirty = false;
    }
}
void debugSection() {
    recomp::menu::SeparatorText("Unused content viewers");
    galleryButton();
    ImGui::SameLine();
    textArchiveButton();
    spawnSection(recomp::dev::inGameScene());
    particleSection(recomp::dev::inGameScene());
}
void drawOverlays() {
    galleryWindow();
    textArchiveWindow();
}

}  // namespace recomp::unused2
