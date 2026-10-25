// Gallery of unused textures (F1 > Enhancements > Debug > Gallery) and the "Ending pictures"
// option (F1 > Enhancements > Unused content). Docs: docs/UNUSED_CONTENT.md ("Second batch").
//
// Ending: scene 6 (ENDING, lbl_8021DEE8: setup fn_8000D1CC, update fn_8000D624, draw fn_8000E050)
// shows the house the money bought, the rank and a caption whose file depends on the rank
// (lbl_804D8100 = rank letter, 1 H (worst) .. 8 A (best): 1 -> mes_bE "The haunted mansion
// disappeared without a trace...", 2..7 -> mes_gE "Welcome to Luigi's NEW Mansion!", 8 -> mes_VE
// "Congratulations!"). The disc also has Ending/luigi_V.bti (Luigi with two V signs), luigi_g.bti
// (one V sign) and luigi_b.bti (sitting with a flower) that nothing loads; the option shows the
// flower picture for H-D, one V sign for C-B, two for A (user's mapping). With the option on, the picture
// for the rank is drawn with the message's own fade (+0x6C of the scene's layer block
// lbl_803A3B40) through the scene's 2D quad helper (fn_80057ACC colour, fn_80057AD8 texture quad).
//
// Gallery: textures decoded on the host (GX texture formats) and shown with ImGui.
#include "port/install.h"
#include "port/settings.h"
#include "recomp/guest_util.h"
#include "recomp/hle.h"
#include "recomp/menu.h"
#include "recomp/rarc.h"
#include "recomp/skip.h"
#include "recomp/ui_scale.h"
#include "recomp/unused2.h"
#include "recomp/version.h"
#include "recomp/widescreen.h"

#include <aurora/imgui.h>
#include <imgui.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

namespace recomp::unused2 {

// ---- host-side GX texture decoding --------------------------------------------------------
namespace {

u16 be16(const u8* p) { return u16(p[0] << 8 | p[1]); }
u32 be32(const u8* p) { return u32(p[0]) << 24 | u32(p[1]) << 16 | u32(p[2]) << 8 | p[3]; }

struct Rgba {
    u8 r, g, b, a;
};
Rgba fromRgb565(u16 v) {
    return {u8(((v >> 11) & 31) * 255 / 31), u8(((v >> 5) & 63) * 255 / 63), u8((v & 31) * 255 / 31), 255};
}
Rgba fromRgb5a3(u16 v) {
    if (v & 0x8000) return {u8(((v >> 10) & 31) * 255 / 31), u8(((v >> 5) & 31) * 255 / 31), u8((v & 31) * 255 / 31), 255};
    return {u8(((v >> 8) & 15) * 17), u8(((v >> 4) & 15) * 17), u8((v & 15) * 17), u8(((v >> 12) & 7) * 255 / 7)};
}

}  // namespace

// Decodes a .bti (header at `d`) to RGBA8. Formats: I4 I8 IA4 IA8 RGB565 RGB5A3 RGBA8 C4 C8 C14X2 CMPR.
bool decodeBti(const std::vector<u8>& d, int* outW, int* outH, std::vector<u8>& rgba) {
    if (d.size() < 0x20) return false;
    const u8 fmt = d[0];
    const int w = be16(&d[2]), h = be16(&d[4]);
    const u8 palFmt = d[9];
    const int npal = be16(&d[0xA]);
    const u32 palOff = be32(&d[0xC]), imgOff = be32(&d[0x1C]);
    if (!w || !h || w > 2048 || h > 2048 || imgOff >= d.size()) return false;
    std::vector<Rgba> pal;
    for (int i = 0; i < npal && palOff + 2 * i + 2 <= d.size(); i++) {
        const u16 v = be16(&d[palOff + 2 * i]);
        pal.push_back(palFmt == 1 ? fromRgb565(v) : palFmt == 2 ? fromRgb5a3(v) : Rgba{u8(v & 255), u8(v & 255), u8(v & 255), u8(v >> 8)});
    }
    auto palAt = [&](u32 i) { return i < pal.size() ? pal[i] : Rgba{0, 0, 0, 0}; };
    rgba.assign(size_t(w) * h * 4, 0);
    auto put = [&](int x, int y, Rgba c) {
        if (x >= w || y >= h) return;
        u8* p = &rgba[(size_t(y) * w + x) * 4];
        p[0] = c.r, p[1] = c.g, p[2] = c.b, p[3] = c.a;
    };
    int bw = 4, bh = 4;
    switch (fmt) {
    case 0: case 8: case 14: bw = 8, bh = 8; break;
    case 1: case 2: case 9: bw = 8, bh = 4; break;
    default: break;
    }
    size_t o = imgOff;
    auto need = [&](size_t n) { return o + n <= d.size(); };
    for (int by = 0; by < h; by += bh)
        for (int bx = 0; bx < w; bx += bw) {
            switch (fmt) {
            case 0: case 8:  // I4, C4
                if (!need(32)) return false;
                for (int y = 0; y < 8; y++)
                    for (int x = 0; x < 8; x += 2) {
                        const u8 b = d[o++];
                        for (int k = 0; k < 2; k++) {
                            const u8 n = k ? (b & 15) : (b >> 4);
                            put(bx + x + k, by + y, fmt == 0 ? Rgba{u8(n * 17), u8(n * 17), u8(n * 17), u8(n * 17)} : palAt(n));
                        }
                    }
                break;
            case 1: case 9:  // I8, C8
                if (!need(32)) return false;
                for (int y = 0; y < 4; y++)
                    for (int x = 0; x < 8; x++) {
                        const u8 b = d[o++];
                        put(bx + x, by + y, fmt == 1 ? Rgba{b, b, b, b} : palAt(b));
                    }
                break;
            case 2:  // IA4
                if (!need(32)) return false;
                for (int y = 0; y < 4; y++)
                    for (int x = 0; x < 8; x++) {
                        const u8 b = d[o++];
                        const u8 i = u8((b & 15) * 17);
                        put(bx + x, by + y, {i, i, i, u8((b >> 4) * 17)});
                    }
                break;
            case 3:  // IA8
                if (!need(32)) return false;
                for (int y = 0; y < 4; y++)
                    for (int x = 0; x < 4; x++, o += 2) put(bx + x, by + y, {d[o + 1], d[o + 1], d[o + 1], d[o]});
                break;
            case 4: case 5: case 10:  // RGB565, RGB5A3, C14X2
                if (!need(32)) return false;
                for (int y = 0; y < 4; y++)
                    for (int x = 0; x < 4; x++, o += 2) {
                        const u16 v = be16(&d[o]);
                        put(bx + x, by + y, fmt == 4 ? fromRgb565(v) : fmt == 5 ? fromRgb5a3(v) : palAt(v & 0x3FFF));
                    }
                break;
            case 6:  // RGBA8: AR plane then GB plane
                if (!need(64)) return false;
                for (int k = 0; k < 16; k++)
                    put(bx + k % 4, by + k / 4, {d[o + 2 * k + 1], d[o + 32 + 2 * k], d[o + 32 + 2 * k + 1], d[o + 2 * k]});
                o += 64;
                break;
            case 14:  // CMPR: 2x2 sub-blocks of DXT1
                if (!need(32)) return false;
                for (int sb = 0; sb < 4; sb++, o += 8) {
                    const int sx = bx + (sb % 2) * 4, sy = by + (sb / 2) * 4;
                    const u16 c0 = be16(&d[o]), c1 = be16(&d[o + 2]);
                    const u32 bits = be32(&d[o + 4]);
                    Rgba p[4] = {fromRgb565(c0), fromRgb565(c1), {}, {}};
                    if (c0 > c1) {
                        p[2] = {u8((2 * p[0].r + p[1].r) / 3), u8((2 * p[0].g + p[1].g) / 3), u8((2 * p[0].b + p[1].b) / 3), 255};
                        p[3] = {u8((p[0].r + 2 * p[1].r) / 3), u8((p[0].g + 2 * p[1].g) / 3), u8((p[0].b + 2 * p[1].b) / 3), 255};
                    } else {
                        p[2] = {u8((p[0].r + p[1].r) / 2), u8((p[0].g + p[1].g) / 2), u8((p[0].b + p[1].b) / 2), 255};
                        p[3] = {0, 0, 0, 0};
                    }
                    for (int k = 0; k < 16; k++) put(sx + k % 4, sy + k / 4, p[(bits >> (30 - 2 * k)) & 3]);
                }
                break;
            default:
                return false;
            }
        }
    *outW = w;
    *outH = h;
    return true;
}

// A file from the disc, optionally inside a (possibly compressed) RARC archive.
std::vector<u8> readDiscItem(const char* disc, const char* inner) {
    rarc::Bytes data = rarc::readDisc(disc);
    if (data.empty()) return {};
    data = rarc::decompress(data);
    if (!inner) return data;
    rarc::Archive a;
    if (!a.parse(data)) return {};
    const rarc::Item* it = a.find(inner);
    return it && !it->dir ? rarc::decompress(it->data) : std::vector<u8>{};
}

// ---- ending pictures ------------------------------------------------------------------------
namespace {

#define kEndRank VA(0x804D8100)    // rank 1..8 = letters H..A (1 worst, 8 best; see endPicFor)
#define kEndLayers VA(0x803A3B40)  // the ending scene's layer block
#define kEndFrame VA(0x804D8108)   // the ending scene's frame counter

bool endingOn() {
    static const bool e = [] {
        const char* v = std::getenv("LM_ENDING_PICTURES");
        return v && *v && *v != '0';
    }();
    return e || port::settings().unused2.endingPictures;
}

struct EndPic {
    const char* file;
    u32 bti = 0;     // guest copy of the whole .bti
    u32 texObj = 0;  // GXTexObj (0x20)
    bool tried = false;
};
EndPic sEndPics[3] = {{"/Ending/luigi_V.bti"}, {"/Ending/luigi_g.bti"}, {"/Ending/luigi_b.bti"}};
u32 sColor = 0;  // u32 colour words for fn_80057ACC

// lbl_804D8100 is the rank letter: fn_8000D1CC loads Ending/ENGLISH/ra_<X>E.bti from the table at
// lbl_8021DC28 + 0x1A0 + 0x20 * rank: 1 H, 2 G, 3 F, 4 E, 5 D, 6 C, 7 B, 8 A. Pictures (user's
// mapping): sad Luigi with the flower for H-D, one V sign for C-B, two V signs for A.
EndPic* endPicFor(u32 rank) { return &sEndPics[rank <= 5 ? 2 : rank >= 8 ? 0 : 1]; }

bool loadEndPic(Ctx& c, EndPic& p) {
    if (p.tried) return p.bti != 0;
    p.tried = true;
    const std::vector<u8> d = readDiscItem(p.file, nullptr);
    if (d.size() < 0x20) {
        log("[gallery] %s not found", p.file);
        return false;
    }
    p.bti = extraAlloc(u32(d.size()), 32);
    p.texObj = extraAlloc(0x20, 32);
    if (!p.bti || !p.texObj) return false;
    std::memcpy(hostp(p.bti), d.data(), d.size());
    const u8* h = d.data();
    gu::call(c, GU_SYM("GXInitTexObj"), {p.texObj, p.bti + be32(h + 0x1C), be16(h + 2), be16(h + 4), h[0], h[6], h[7], 0});
    log("[gallery] %s loaded (%ux%u format %u)", p.file, be16(h + 2), be16(h + 4), h[0]);
    return true;
}

}  // namespace

void endingFrame(Ctx& c) {
    // Test aid: LM_TEST_ENDING=<rank>@<guest frame> switches to the ending scene with that rank.
    static int sRank = -1, sAt = -1;
    static bool sParsed = false;
    if (!sParsed) {
        sParsed = true;
        if (const char* v = std::getenv("LM_TEST_ENDING")) std::sscanf(v, "%d@%d", &sRank, &sAt);
    }
    if (sRank > 0 && int(skip::guestFrames()) == sAt) {
        wr32(kEndRank, u32(sRank));
        wr32(VA(0x803A3AE4), 6);  // requested scene: ENDING
        log("[gallery] test: ending scene, rank %d", sRank);
    }
}

// ---- gallery window -------------------------------------------------------------------------
namespace {

struct GalleryItem {
    const char* group;
    const char* title;
    const char* disc;
    const char* inner;  // path inside the archive, or nullptr
    const char* note;
    // filled on first draw
    bool tried = false;
    ImTextureID tex{};
    int w = 0, h = 0;
};

GalleryItem sItems[] = {
    {"Ending", "luigi_V (rank A)", "/Ending/luigi_V.bti", nullptr, "Luigi with two V signs; shown for rank A"},
    {"Ending", "luigi_g (ranks C, B)", "/Ending/luigi_g.bti", nullptr, "One V sign; shown for ranks C and B"},
    {"Ending", "luigi_b (ranks H-D)", "/Ending/luigi_b.bti", nullptr, "Sitting with a flower; shown for ranks H to D"},
    {"Ending", "mes_b1 (Japanese)", "/Ending/mes_b1.bti", nullptr,
     "Unused Japanese worst-rank caption. Translation: \"Hmm... Reality was harsh. Luigi's Mansion fades away like a dream.\""},
    {"Ending", "sosite (Japanese)", "/Ending/sosite.bti", nullptr, "Unused Japanese caption. Translation: \"And then... a present from the Professor!\""},
    {"Room names", "e_kyukei", "/Game/game_usa.szp", "kawano/roomname/e_kyukei.bti", "\"Butler's Room\" name card (kyukei = rest room); never shown"},
    {"Room names", "e_roof", "/Game/game_usa.szp", "kawano/roomname/e_roof.bti", "\"Roof\" name card; never shown"},
    {"Game Boy Horror", "gb_sight_ob_key", "/Game/game_usa.szp", "kawano/base/timg/gb_sight_ob_key.bti", "Scan-target icon for a key"},
    {"Game Boy Horror", "gb_sight_ob_teresa", "/Game/game_usa.szp", "kawano/base/timg/gb_sight_ob_teresa.bti", "Scan-target icon for a Boo"},
    {"Game Boy Horror", "gb_sight_ob_quest", "/Game/game_usa.szp", "kawano/base/timg/gb_sight_ob_quest.bti", "Scan-target question mark (for comparison)"},
    {"Other", "hand1 (res_kwnx)", "/Kawano/res_kwnx.szp", "hand1.bti", "A pointing hand cursor in the menu archive"},
    {"Other", "bg_kupa", "/Nakamura/up/bg_kupa.bti", nullptr, "Unreferenced Bowser background"},
    {"Other", "e_tx_erased_2a", "/Kawano/ENGLISH/res_slct.szp", "timg/e_tx_erased_2a.bti", "File-select message never shown"},
    {"Other", "e_tx_erased_3a", "/Kawano/ENGLISH/res_slct.szp", "timg/e_tx_erased_3a.bti", "File-select message never shown"},
    {"Other", "m_window3", "/Game/game_usa.szp", "kawano/dmman/m_window3.bti", "Unreferenced message window"},
    {"Other", "obam1 (ghost portrait)", "/model/obake01.szp", "bti/obam1.bti", "Ghost archives carry a portrait nothing loads"},
    {"Other", "obam1_kg (silhouette)", "/model/obake01.szp", "bti/obam1_kg.bti", nullptr},
};

bool sGalleryOpen = false;

void loadItem(GalleryItem& it) {
    if (it.tried) return;
    it.tried = true;
    std::vector<u8> d = readDiscItem(it.disc, it.inner);
    std::vector<u8> rgba;
    if (d.empty() || !decodeBti(d, &it.w, &it.h, rgba)) {
        log("[gallery] %s%s%s: not found or not decodable", it.disc, it.inner ? "#" : "", it.inner ? it.inner : "");
        return;
    }
    it.tex = aurora_imgui_add_texture(u32(it.w), u32(it.h), rgba.data());
}

}  // namespace

void galleryButton() {
    if (recomp::menu::Button(sGalleryOpen ? "Close gallery###gallery" : "Gallery (unused textures)###gallery")) sGalleryOpen = !sGalleryOpen;
    recomp::menu::ItemTooltip("Opens a window with unused textures and pictures left on the disc.");
}

void galleryWindow() {
    static const bool sEnv = std::getenv("LM_GALLERY") != nullptr;
    static bool sEnvOnce = false;
    if (sEnv && !sEnvOnce) sEnvOnce = sGalleryOpen = true;
    if (!sGalleryOpen) return;
    recomp::ui::beforeWindow("Gallery: unused textures", ImVec2(720, 560), ImVec2(320, 240));
    if (!ImGui::Begin("Gallery: unused textures", &sGalleryOpen)) {
        ImGui::End();
        return;
    }
    static float sZoom = 1.0f;
    recomp::menu::SliderFloat("Zoom", &sZoom, 0.25f, 3.0f, "%.2fx");
    const char* group = nullptr;
    for (GalleryItem& it : sItems) {
        if (!group || std::strcmp(group, it.group)) {
            group = it.group;
            recomp::menu::SeparatorText(group);
        }
        loadItem(it);
        ImGui::TextUnformatted(it.title);
        if (it.note) {
            ImGui::SameLine();
            ImGui::TextDisabled("%s", it.note);
        }
        if (it.tex) {
            // Checkerboard-free dark backdrop so white and transparent parts both show.
            const ImVec2 p = ImGui::GetCursorScreenPos();
            const ImVec2 sz(float(it.w) * sZoom, float(it.h) * sZoom);
            ImGui::GetWindowDrawList()->AddRectFilled(p, ImVec2(p.x + sz.x, p.y + sz.y), IM_COL32(40, 44, 56, 255));
            ImGui::Image(it.tex, sz);
            ImGui::TextDisabled("%dx%d  %s%s%s", it.w, it.h, it.disc, it.inner ? " : " : "", it.inner ? it.inner : "");
        } else {
            ImGui::TextDisabled("(not on this disc)");
        }
    }
    ImGui::End();
}

}  // namespace recomp::unused2

namespace recomp {

// Ending scene draw. The unused Luigi picture for the rank fades in with the message.
HLE(fn_8000E050) {
    using namespace unused2;
    callOriginal(c, "fn_8000E050");
    if (!endingOn()) return;
    const u32 rank = rd32(kEndRank);
    const u32 layers = kEndLayers;
    if (!gu::inMem1(rd32(layers + 0x54))) return;  // the message is not loaded yet
    EndPic& p = *endPicFor(rank);
    if (!loadEndPic(c, p)) return;
    const u8 alpha = u8(rd16(layers + 0x6C));  // the message layer alpha (s16, low byte used)
    if (!alpha || s32(rd32(kEndFrame)) >= 0x7BC) return;  // 0x7BC: the scene's own fade-out starts
    if (!sColor) sColor = extraAlloc(8, 4);
    const int w = rd16(p.bti + 2), h = rd16(p.bti + 4);
    // Bottom-right corner of the ending's background picture, bottom edge flush with the screen's
    // bottom (the pictures end at Luigi's knees; the edge hides the cut), right-aligned with a
    // small margin. The game's 2D is a 640x480 frame kept at 4:3 and centered in widescreen
    // (widescreen.h); full-screen pictures follow backgroundMode(): pillarboxed they keep that
    // frame (right edge 640), stretched or zoomed they reach the screen's right edge, at
    // 320 + 320 / factor() in the game's coordinates.
    constexpr int kMargin = 16;
    const bool wide = ws::active() && ws::backgroundMode() != ws::Background::Pillarbox;
    const int right = wide ? 320 + int(320.0f / ws::factor() + 0.5f) : 640;
    const int x = right - kMargin - w, y = 480 - h;
    wr32(sColor, rd32(VA(0x804CFF88)));  // the scene's shadow colour
    gu::call(c, GU_SYM("fn_80057ACC"), {sColor});
    gu::call(c, GU_SYM("fn_80057AD8"), {p.texObj, u32(x + 2), u32(y + 2), u32(w), u32(h), alpha});
    wr32(sColor, rd32(VA(0x804CFF84)));  // and its normal colour
    gu::call(c, GU_SYM("fn_80057ACC"), {sColor});
    gu::call(c, GU_SYM("fn_80057AD8"), {p.texObj, u32(x), u32(y), u32(w), u32(h), alpha});
}

}  // namespace recomp
