// Jukebox (F1 > Sound > Jukebox): plays any of the sequenced music tracks through the game's own
// music player. Nothing plays until the player presses a Play button.
//
// Tracks: the BARC block of /AudioRes/JaiInit.aaf (sequence archive JaiArcS.arc) lists 89
// sequences; entry 0 is the sound-effect sequence LuigiSe.bms, the other 88 are music
// (names cut to 13 characters by the format). Some entries point at the same sequence data as an
// earlier one (aliases, e.g. 9 Mario.com = 5). The game starts music with fn_80188760(0x80000000 |
// index) (e.g. 0x12 GoodNight when Luigi's health runs out); event scripts use <BGM>(n), which
// maps n through the table lbl_8038EAB4 (54 entries) in fn_8018A470. fn_8018A570 stops the
// current music. Silent picks fixed (docs/UNUSED_CONTENT.md "Jukebox"): both music handles are
// stopped, tracks the player refuses (info flag 0x80000000) start the way the game starts them,
// a stream's pause of the DVD task thread is lifted so the pick can load, and Mario.com is moved
// to the instrument bank it was written for (hook at the end of the file).
//
// "Not heard in retail" marks the tracks that no event script's <BGM> command, no constant music
// id in the code (fn_80188760 / fn_801889FC calls) and no data table selects (USA analysis,
// docs/UNUSED_CONTENT.md "Jukebox"); a track reached only through a computed id could be missed.
#include "port/install.h"
#include "recomp/audio.h"
#include "recomp/guest_util.h"
#include "recomp/hle.h"
#include "recomp/rarc.h"
#include "recomp/skip.h"
#include "recomp/unused2.h"
#include "recomp/version.h"

#include "recomp/menu.h"

#include <imgui.h>
#include <imgui_internal.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

namespace recomp::unused2 {
namespace {

struct Track {
    int index;
    std::string name;
    u32 offset;    // sequence data offset in JaiArcS.arc
    u32 size = 0;  // and its length
    int aliasOf = -1;
};

std::vector<Track> sTracks;
std::once_flag sLoad;
std::atomic<int> sPlay{-1};
std::atomic<bool> sStop{false};
int sNowPlaying = -1;                // last track picked in the menu
std::atomic<u32> sMusicId{0};       // music the game's player runs (guest thread)
std::atomic<u32> sSubId{0};         // and its second handle (room / light layers)
std::atomic<bool> sPending{false};  // a pick the guest has not started yet
bool sUnusedOnly = false;

// Music ids the retail game selects (USA): event <BGM> table entries the scripts use, constant
// ids in code, data tables (see the file comment). Everything else is listed as not heard.
const int kUsed[] = {0x01, 0x02, 0x03, 0x04, 0x07, 0x08, 0x0C, 0x0E, 0x0F, 0x10, 0x12, 0x13, 0x14, 0x15, 0x16,
                     0x17, 0x18, 0x19, 0x1A, 0x1B, 0x1C, 0x1D, 0x1E, 0x21, 0x22, 0x24, 0x25, 0x26, 0x27, 0x29,
                     0x2A, 0x2B, 0x2C, 0x2D, 0x2E, 0x2F, 0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39,
                     0x3A, 0x3B, 0x3C, 0x3D, 0x3E, 0x3F, 0x40, 0x41, 0x42, 0x43, 0x44, 0x45, 0x47, 0x48, 0x49,
                     0x4A, 0x4B, 0x4C, 0x4D, 0x4E, 0x50, 0x51, 0x52, 0x53, 0x57, 0x58,
                     // ids built as 0x80000000 + n anywhere in the code (e.g. the ending's rank
                     // jingles Ochi01-03, 0x54-0x56, chosen in fn_8000D1CC), kept as heard
                     0x1F, 0x54, 0x55, 0x56};

bool usedIndex(int i) {
    for (int u : kUsed)
        if (u == i) return true;
    return false;
}

// Heard when the track, or any entry playing the same sequence data, is selected somewhere.
bool usedInRetail(const Track& t) {
    for (const Track& o : sTracks)
        if ((o.index == t.index || o.offset == t.offset) && usedIndex(o.index)) return true;
    return false;
}

void load() {
    const rarc::Bytes aaf = rarc::readDisc("/AudioRes/JaiInit.aaf");
    for (size_t i = 0; i + 0x20 <= aaf.size(); i += 4) {
        if (std::memcmp(&aaf[i], "BARC----", 8)) continue;
        auto be = [&](size_t o) { return u32(aaf[o]) << 24 | u32(aaf[o + 1]) << 16 | u32(aaf[o + 2]) << 8 | aaf[o + 3]; };
        const u32 count = be(i + 0xC);
        for (u32 k = 0; k < count && i + 0x40 + 0x20 * k <= aaf.size(); k++) {
            const size_t e = i + 0x20 + 0x20 * k;
            Track t;
            t.index = int(k);
            t.name.assign(reinterpret_cast<const char*>(&aaf[e]), strnlen(reinterpret_cast<const char*>(&aaf[e]), 14));
            t.offset = be(e + 0x18);
            t.size = be(e + 0x1C);
            for (const Track& p : sTracks)
                if (p.offset == t.offset && p.aliasOf < 0) t.aliasOf = p.index;
            sTracks.push_back(t);
        }
        break;
    }
    log("[jukebox] %zu sequences in JaiInit.aaf", sTracks.size());
}

// Test aid LM_JUKEBOX_UI=<sequence|stop>@<guest frame>[,...]: presses the menu's own Play / Stop
// button (ImGui item activation; the F1 menu must be open on the Sound tab, e.g. LM_MENU=Sound),
// so the whole UI path is exercised. Logs "[jukebox-ui] click".
namespace ui {
struct Click {
    int target;  // sequence index, 0 = the Stop button
    u32 frame;
};
std::vector<Click> sClicks = [] {
    std::vector<Click> out;
    const char* v = std::getenv("LM_JUKEBOX_UI");
    for (const char* p = v; p && *p;) {
        Click c{};
        char what[16] = {};
        if (std::sscanf(p, "%15[^@]@%u", what, &c.frame) == 2) {
            c.target = std::strcmp(what, "stop") == 0 ? 0 : std::atoi(what);
            out.push_back(c);
        }
        const char* comma = std::strchr(p, ',');
        if (!comma) break;
        p = comma + 1;
    }
    return out;
}();
int sStep = -1;  // frames into the current click (-1 = idle)

int target() { return !sClicks.empty() && skip::guestFrames() >= sClicks.front().frame ? sClicks.front().target : -1; }

// After the target's button: scroll it into view, then activate it.
void atButton(int index) {
    if (target() != index) return;
    if (sStep < 0) {
        ImGui::SetScrollHereY(0.5f);
        sStep = 0;
        return;
    }
    // Activated like a click (ImGui's nav activation goes through the button's own behaviour,
    // independent of where the real mouse is).
    if (++sStep == 3) ImGui::ActivateItemByID(ImGui::GetItemID());
    else if (sStep == 5) {
        log("[jukebox-ui] click %d frame %u", index, skip::guestFrames());
        sClicks.erase(sClicks.begin());
        sStep = -1;
    }
}
}  // namespace ui

}  // namespace

namespace {
// Music handles of the game's player: the main one (lbl_804D91A4) and the second one next to it
// (lbl_804D91A8), which room / light / Toad layers use (fn_801889FC starts tracks with info flag
// 0x08000000 there through fn_80188EBC). fn_8018A570 stops only the main one, so a layer picked
// earlier (e.g. Kinopio_Light, 30, or Light.com, 78) kept playing through Stop and later picks.
u32 subHandleAddr() { return VA(0x804D91A8); }

void stopMusic(Ctx& c) {
    gu::call(c, GU_SYM("fn_8018A570"), {0});
    const u32 sub = rd32(subHandleAddr());
    if (gu::inMem1(sub)) gu::call(c, GU_SYM("stop__8JAISoundFUl"), {sub, 0});
}
}  // namespace

void jukeboxFrame(Ctx& c) {
    // Test aid: LM_JUKEBOX=<sequence>@<guest frame>.
    static int sEnvSeq = -1, sEnvFrame = -1;
    static bool sEnvParsed = false;
    if (!sEnvParsed) {
        sEnvParsed = true;
        if (const char* v = std::getenv("LM_JUKEBOX")) std::sscanf(v, "%d@%d", &sEnvSeq, &sEnvFrame);
    }
    if (sEnvSeq > 0 && int(skip::guestFrames()) == sEnvFrame) sPlay = sEnvSeq;
    // Test aid: LM_JUKEBOX_SWEEP=<first frame>[,<frames per track>[,<first>-<last> | <n>+<n>+...]]
    // plays every music track (aliases skipped) or the listed ones in turn; each start is logged
    // with its LM_AUDIO_WAV_RAW sample position (tools: level per track from the WAV).
    static int sSweepAt = -1, sSweepStep = 300;
    static std::vector<int> sSweepList;
    static size_t sSweepPos = 0;
    static bool sSweepParsed = false;
    if (!sSweepParsed) {
        sSweepParsed = true;
        if (const char* v = std::getenv("LM_JUKEBOX_SWEEP")) {
            int first = 1, last = 1000;
            std::sscanf(v, "%d,%d", &sSweepAt, &sSweepStep);
            const char* sel = std::strchr(v, ',');
            sel = sel ? std::strchr(sel + 1, ',') : nullptr;
            if (sel && std::strchr(sel, '+')) {
                for (const char* p = sel + 1; *p; p++)
                    if (p == sel + 1 || p[-1] == '+') sSweepList.push_back(std::atoi(p));
            } else {
                if (sel) std::sscanf(sel + 1, "%d-%d", &first, &last);
                std::call_once(sLoad, load);
                for (const Track& t : sTracks)
                    if (t.index >= first && t.index <= last && t.index > 0 && t.aliasOf < 0) sSweepList.push_back(t.index);
            }
        }
    }
    if (sSweepAt >= 0 && int(skip::guestFrames()) >= sSweepAt && sSweepStep > 0) {
        std::call_once(sLoad, load);
        if (sSweepPos < sSweepList.size() && sSweepList[sSweepPos] < int(sTracks.size())) {
            const int k = sSweepList[sSweepPos++];
            log("[jukebox] sweep %d %s raw %llu", k, sTracks[k].name.c_str(), audio::outRawFrames());
            sPlay = k;
        } else {
            log("[jukebox] sweep end raw %llu", audio::outRawFrames());
            sStop = true;
            sSweepStep = 0;
        }
        sSweepAt += sSweepStep;
    }
    if (sStop.exchange(false)) stopMusic(c);
    const int n = sPlay.exchange(-1);
    if (n > 0) {
        stopMusic(c);
        // r4 is the fade-in time in frames (handed to JAIBasic's start as its fade argument;
        // every game call passes 0). Left out, it was whatever the hooked caller had in r4
        // (often a pointer): the music started at volume 0 and faded in over months.
        const u32 id = 0x80000000u | u32(n);
        // A streamed track (the title's or the mansion's .afc ambience) pauses JAudio's DVD task
        // thread while it plays (fn_8019480C -> DVDT_Pause; its end calls DVDT_UnPause), so a pick
        // whose sequence data is not already in a buffer waited for the stream to end and stayed
        // silent (seen at the title screen once a track had ended there). Let the load through:
        // reads do not compete for a drive here; the stream's own UnPause later is a no-op.
        if (rd32(VA(0x804D93B8)) == 1) {  // DVDT_PAUSE_FLAG
            gu::call(c, VA(0x8019E620));  // DVDT_UnPause
            log("[jukebox] DVD task thread was paused by a stream: resumed for the pick");
        }
        const u32 jai = rd32(VA(0x804D7828));  // the game's JAIBasic
        const u32 info = gu::inMem1(jai) ? gu::call(c, GU_SYM("getSoundInfoFromID__8JAIBasicFUl"), {jai, id}) : 0;
        const u32 flags = gu::inMem1(info) ? rd32(info) : 0;
        if (flags & 0x80000000u) {
            // The music player (fn_801889FC) refuses sounds with info flag 0x80000000: the game
            // starts those itself, e.g. Room_Baby (31) from fn_80189858 straight on the second
            // handle with fn_8018D6C0(jai, id, &handle, 0, 0, 4). Same here (Room_Baby, Room_Space).
            gu::call(c, VA(0x8018D6C0), {jai, id, subHandleAddr(), 0, 0, 4});
        } else {
            gu::call(c, GU_SYM("fn_80188760"), {id, 0});
        }
        log("[jukebox] playing sequence %d (info flags %08X)", n, flags);
    }
    // Log what the music player runs (main handle lbl_804D91A4 +8 = sound id), for testing.
    static u32 sLastId = 0;
    const u32 h = rd32(VA(0x804D91A4));
    const u32 id = gu::inMem1(h) ? rd32(h + 8) : 0;
    const u32 sh = rd32(subHandleAddr());
    const u32 sid = gu::inMem1(sh) ? rd32(sh + 8) : 0;
    sMusicId = id;
    sSubId = sid;
    if (n > 0) sPending = false;
    if (id != sLastId) {
        sLastId = id;
        log("[jukebox] music now %08X", id);
    }
    static u32 sLastSub = 0;
    if (sid != sLastSub) {
        sLastSub = sid;
        log("[jukebox] second music handle now %08X", sid);
    }
}

void soundSection() {
    recomp::menu::SeparatorText("Jukebox");
    if (ui::target() >= 0) ImGui::SetScrollHereY(0.0f);  // LM_JUKEBOX_UI: the section in view
    std::call_once(sLoad, load);
    if (sTracks.empty()) {
        recomp::menu::Note("No music table found on the disc.");
        return;
    }
    recomp::menu::Note("Plays the game's sequenced music (the mansion's streamed ambience is separate).\n"
                        "The game may change the music again on its own, e.g. when Luigi enters a room.");
    if (recomp::menu::Button("Stop music")) {
        sStop = true;
        log("[jukebox] menu: stop");
        sNowPlaying = -1;
    }
    ui::atButton(0);
    recomp::menu::ItemTooltip("Stops the sequenced music the jukebox (or the game) is playing.");
    ImGui::SameLine();
    recomp::menu::Checkbox("Only tracks not heard in retail", &sUnusedOnly);
    recomp::menu::ItemTooltip("Lists only the music the retail game never plays (USA version).");
    const bool us = !ver::pal();
    // "Playing" only while the game's music player still runs the picked track (it ended, was
    // stopped, or the game started other music: back to "Play").
    if (sNowPlaying > 0 && !sPending && sMusicId != (0x80000000u | u32(sNowPlaying)) && sSubId != (0x80000000u | u32(sNowPlaying)))
        sNowPlaying = -1;
    if (ImGui::BeginTable("jukebox", 3, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY,
                          ImVec2(0, ImGui::GetTextLineHeightWithSpacing() * 14))) {
        for (const Track& t : sTracks) {
            if (t.index == 0) continue;  // sound effects
            const bool unused = us && !usedInRetail(t);
            if (sUnusedOnly && !unused) continue;
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            char label[32];
            std::snprintf(label, sizeof label, "%s###jb%d", sNowPlaying == t.index ? "Playing" : "Play", t.index);
            if (recomp::menu::SmallButton(label)) {
                sPending = true;
                sPlay = t.index;
                log("[jukebox] menu: play %d", t.index);
                sNowPlaying = t.index;
            }
            ui::atButton(t.index);
            ImGui::TableNextColumn();
            ImGui::Text("%2d  %s", t.index, t.name.c_str());
            ImGui::TableNextColumn();
            // Total.com (17) is 32 bytes: tempo, one rest, end. Nothing to hear (a "silence" cue).
            if (t.size && t.size <= 0x20) recomp::menu::Note("empty (no notes)");
            else if (t.aliasOf >= 0) recomp::menu::Note("same as %d", t.aliasOf);
            else if (unused) ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.3f, 1.0f), "not heard in retail");
        }
        ImGui::EndTable();
    }
}

}  // namespace recomp::unused2

namespace recomp {
namespace {
// Mario.com (sequence 5, alias 9, never played by the game) was written for an older bank
// layout. A track's instrument is register 6 (bank << 8 | program; `a4 20 b` sets the bank,
// `a4 21 p` the program, `ac 06 bb pp` both, child tracks inherit it, a root starts at 0x00F0).
// Mario.com selects no bank anywhere: its tracks set `ac 06 00 79/7a/7b/7d` (bank 0, programs
// 121-125) or, track 1, only `a4 21 78` (program 120 on the inherited bank 0). Bank 0 is the
// sound-effect bank, whose programs 120-123 and 125 do not exist, so every note was dropped and
// the track stayed silent. Bank 1 is the only bank with all of programs 120-125. The sequence
// is patched where JAudio takes it (Jaq_SetSeqData_Limit, direct data): bank 0 -> 1 in the
// `ac 06` writes, and track 1's opening SyncCPU(0) (`e7 00 00`, a no-op for music) becomes
// `a4 20 01`. Each site is checked before it is changed; same data on US and PAL.
struct SeqPatch {
    u32 offset;
    u8 from[3], to[3];
};
constexpr u8 kMarioSig[] = {0x80, 0x06, 0x98, 0x00, 0x46, 0xC1, 0x00, 0x00, 0x06, 0x19, 0xFE, 0x00, 0x78};
constexpr SeqPatch kMarioPatches[] = {
    {0x645, {0xAC, 0x06, 0x00}, {0xAC, 0x06, 0x01}},  {0x64E, {0xAC, 0x06, 0x00}, {0xAC, 0x06, 0x01}},
    {0x657, {0xAC, 0x06, 0x00}, {0xAC, 0x06, 0x01}},  {0x2105, {0xAC, 0x06, 0x00}, {0xAC, 0x06, 0x01}},
    {0x2135, {0xAC, 0x06, 0x00}, {0xAC, 0x06, 0x01}}, {0x2167, {0xAC, 0x06, 0x00}, {0xAC, 0x06, 0x01}},
    {0x17BD, {0xE7, 0x00, 0x00}, {0xA4, 0x20, 0x01}},
};
constexpr u32 kMarioSize = 0x21E0;

void fixSequence(u32 data) {
    if (!gu::inMem1(data) || !gu::inMem1(data + kMarioSize - 1)) return;
    for (u32 i = 0; i < sizeof kMarioSig; i++)
        if (rd8(data + i) != kMarioSig[i]) return;
    int changed = 0;
    for (const SeqPatch& p : kMarioPatches) {
        bool match = true;
        for (int i = 0; i < 3; i++) match = match && rd8(data + p.offset + i) == p.from[i];
        if (!match) continue;  // already patched (data reused) or different data
        for (int i = 0; i < 3; i++) wr8(data + p.offset + i, p.to[i]);
        changed++;
    }
    if (changed) log("[jukebox] Mario.com at %08X: %d bank selects moved to bank 1", data, changed);
}
}  // namespace

// r3 track (0 = new root), r4 data, r5 size / id, r6 mode (0 = data at r4).
HLE(Jaq_SetSeqData_Limit) {
    static const bool sLog = std::getenv("LM_SEQ_LOG") != nullptr;  // test aid: every sequence start
    if (sLog)
        log("[seq] Jaq_SetSeqData_Limit track %08X data %08X r5 %08X mode %u first %08X", c.r[3], c.r[4], c.r[5], c.r[6],
            gu::inMem1(c.r[4]) ? rd32(c.r[4]) : 0);
    if (c.r[6] == 0) fixSequence(c.r[4]);
    callOriginal(c, "Jaq_SetSeqData_Limit");
}

}  // namespace recomp
