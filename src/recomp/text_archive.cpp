// Text archive (F1 > Enhancements > Debug > Text archive) and the early lab script (F1 >
// Enhancements > Unused content > "Early E. Gadd lab script"). Docs: docs/UNUSED_CONTENT.md.
//
// Event archives (/Event/eventNN.szp) hold a script (text/eventNN.txt, Shift-JIS) and its
// messages (message/messageN.csv: entries separated by CR LF, lines inside an entry by LF). The
// script shows message n with <SPEAK>(n) and offers it as a menu entry with <CHOICE>"label"(n).
// Messages no script line refers to are never shown (e.g. Luigi's "Aaahhhh!" at the start of
// event01, the pianist's "do you know the name of this piece?" in event03, "Go, loser!!!!" in
// event85). Lines outside tags are the developers' notes (comments), skipped by the interpreter.
//
// event26 is an early, untranslated version of E. Gadd's lab script (retail: event08), with its
// own Japanese messages and the Professor still called "Oya-Mar". Nothing loads it.
//
// Text is drawn with the game's own font: the ResFONT the game embeds in the DOL (lbl_80306560,
// initiated by fn_8003EB44 for the in-game text): 359 glyphs of 32x32 cells in I4 sheets
// (GLY1), widths per glyph (WID1) and Shift-JIS code maps (MAP1: full-width Latin letters and
// digits, kana, punctuation, 64 kanji). Like JUTResFont with a two-byte font, single-byte
// (ASCII) text is drawn with the full-width glyphs. Kanji the font lacks are left blank.
#include "port/install.h"
#include "port/settings.h"
#include "recomp/hle.h"
#include "recomp/rarc.h"
#include "recomp/ui_scale.h"
#include "recomp/unused2.h"
#include "recomp/menu.h"
#include "recomp/version.h"

#include <aurora/imgui.h>
#include <imgui.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace recomp::unused2 {

std::vector<u8> readDiscItem(const char* disc, const char* inner);  // gallery.cpp

namespace {

u16 be16(const u8* p) { return u16(p[0] << 8 | p[1]); }
u32 be32(const u8* p) { return u32(p[0]) << 24 | u32(p[1]) << 16 | u32(p[2]) << 8 | p[3]; }

// ---- the game's font ----------------------------------------------------------------------
struct Font {
    bool ok = false;
    int cellW = 32, cellH = 32, leading = 34, ascent = 22, defWidth = 32;
    int glyphs = 0;
    std::vector<u8> alpha;            // glyph cells, glyphs * cellW * cellH
    std::vector<u8> kern, width;      // per glyph
    std::map<u16, int> code;          // Shift-JIS code -> glyph
};
Font sFont;
std::once_flag sFontOnce;

// Finds the ResFONT in guest memory (the DOL's data; USA lbl_80306560).
u32 findFont() {
    const u32 hint = VA(0x80306560);
    if (hint && !std::memcmp(hostp(hint), "FONTbfn1", 8)) return hint;
    for (u32 a = 0x80200000; a < 0x80400000; a += 0x20)
        if (!std::memcmp(hostp(a), "FONTbfn1", 8)) return a;
    return 0;
}

void loadFont() {
    const u32 base = findFont();
    if (!base) {
        log("[text] game font not found");
        return;
    }
    const u8* f = static_cast<const u8*>(hostp(base));
    const u32 size = be32(f + 8), blocks = be32(f + 12);
    u32 o = 0x20;
    for (u32 b = 0; b < blocks && o + 8 <= size; b++) {
        const u8* blk = f + o;
        const u32 bs = be32(blk + 4);
        if (!bs) break;
        if (!std::memcmp(blk, "INF1", 4)) {
            sFont.ascent = be16(blk + 0xA);
            sFont.defWidth = be16(blk + 0xE);
            sFont.leading = be16(blk + 0x10);
        } else if (!std::memcmp(blk, "MAP1", 4)) {
            const u16 method = be16(blk + 8), start = be16(blk + 0xA), end = be16(blk + 0xC), n = be16(blk + 0xE);
            if (method == 0) {  // linear: glyph = first + (code - start)
                for (u32 c = start; c <= end; c++) sFont.code[u16(c)] = be16(blk + 0x10) + int(c - start);
            } else if (method == 2) {  // table: one glyph per code
                for (u32 c = start; c <= end && 0x10 + 2 * (c - start) + 2 <= bs; c++) sFont.code[u16(c)] = be16(blk + 0x10 + 2 * (c - start));
            } else if (method == 3) {  // map: (code, glyph) pairs
                for (u32 i = 0; i < n && 0x10 + 4 * i + 4 <= bs; i++) sFont.code[be16(blk + 0x10 + 4 * i)] = be16(blk + 0x12 + 4 * i);
            }
        } else if (!std::memcmp(blk, "GLY1", 4)) {
            const int startG = be16(blk + 8), endG = be16(blk + 0xA);
            sFont.cellW = be16(blk + 0xC);
            sFont.cellH = be16(blk + 0xE);
            const u32 sheetSize = be32(blk + 0x10);
            const u16 fmt = be16(blk + 0x14), rows = be16(blk + 0x16), cols = be16(blk + 0x18);
            const u16 sw = be16(blk + 0x1A);
            if (fmt != 0) {  // I4 only (what the game's font uses)
                log("[text] font sheet format %u not supported", fmt);
                continue;
            }
            sFont.glyphs = endG - startG + 1;
            sFont.alpha.assign(size_t(sFont.glyphs) * sFont.cellW * sFont.cellH, 0);
            const int perSheet = rows * cols;
            for (int g = 0; g < sFont.glyphs; g++) {
                const u8* sheet = blk + 0x20 + sheetSize * u32(g / perSheet);
                const int cell = g % perSheet, cx = (cell % rows) * sFont.cellW, cy = (cell / rows) * sFont.cellH;
                for (int y = 0; y < sFont.cellH; y++)
                    for (int x = 0; x < sFont.cellW; x++) {
                        // I4: 8x8 tiles of 4-bit texels, tiles in rows of sw/8.
                        const int px = cx + x, py = cy + y;
                        const u32 tile = u32(py / 8) * (sw / 8) + u32(px / 8);
                        const u32 byte = tile * 32 + u32(py % 8) * 4 + u32(px % 8) / 2;
                        const u8 b = sheet[byte];
                        const u8 v = (px & 1) ? (b & 15) : (b >> 4);
                        sFont.alpha[(size_t(g) * sFont.cellH + y) * sFont.cellW + x] = u8(v * 17);
                    }
            }
        } else if (!std::memcmp(blk, "WID1", 4)) {
            const int startG = be16(blk + 8), endG = be16(blk + 0xA);
            sFont.kern.assign(size_t(endG + 1), 0);
            sFont.width.assign(size_t(endG + 1), u8(sFont.defWidth));
            for (int g = startG; g <= endG && 0xC + 2 * (g - startG) + 2 <= int(bs); g++) {
                sFont.kern[size_t(g)] = blk[0xC + 2 * (g - startG)];
                sFont.width[size_t(g)] = blk[0xD + 2 * (g - startG)];
            }
        }
        o += bs;
    }
    sFont.ok = sFont.glyphs > 0;
    log("[text] game font: %d glyphs %dx%d, %zu codes", sFont.glyphs, sFont.cellW, sFont.cellH, sFont.code.size());
}

// ASCII -> full-width Shift-JIS, as JUTResFont draws single-byte text with a two-byte font.
u16 fullWidth(u8 c) {
    if (c >= '0' && c <= '9') return u16(0x824F + (c - '0'));
    if (c >= 'A' && c <= 'Z') return u16(0x8260 + (c - 'A'));
    if (c >= 'a' && c <= 'z') return u16(0x8281 + (c - 'a'));
    static const struct {
        char c;
        u16 code;
    } kPunct[] = {{' ', 0x8140}, {'!', 0x8149}, {'"', 0x8168}, {'#', 0x8194}, {'$', 0x8190}, {'%', 0x8193}, {'&', 0x8195},
                  {'\'', 0x8166}, {'(', 0x8169}, {')', 0x816A}, {'*', 0x8196}, {'+', 0x817B}, {',', 0x8143}, {'-', 0x817C},
                  {'.', 0x8144}, {'/', 0x815E}, {':', 0x8146}, {';', 0x8147}, {'<', 0x8183}, {'=', 0x8181}, {'>', 0x8184},
                  {'?', 0x8148}, {'@', 0x8197}, {'[', 0x816D}, {']', 0x816E}, {'^', 0x814F}, {'_', 0x8151}, {'`', 0x814D},
                  {'{', 0x816F}, {'|', 0x8162}, {'}', 0x8170}, {'~', 0x8160}};
    for (const auto& p : kPunct)
        if (p.c == char(c)) return p.code;
    return 0x8140;
}

struct Rendered {
    ImTextureID tex{};
    int w = 0, h = 0;
};
std::map<std::string, Rendered> sRendered;

// Text colours of <COLOR>(n) (approximate: white, blue, green, orange).
const u8 kColors[][3] = {{255, 255, 255}, {120, 200, 255}, {120, 230, 120}, {255, 170, 60}};

// Draws Shift-JIS/ASCII text (LF = new line, <COLOR>(n) tags) with the game's font.
const Rendered& render(const std::string& text) {
    auto it = sRendered.find(text);
    if (it != sRendered.end()) return it->second;
    Rendered& r = sRendered[text];
    std::call_once(sFontOnce, loadFont);
    if (!sFont.ok) return r;
    struct G {
        int glyph, x, line, color;
    };
    std::vector<G> gs;
    int x = 0, line = 0, color = 0, maxX = 0;
    for (size_t i = 0; i < text.size();) {
        const u8 c = u8(text[i]);
        if (c == '\n') {
            maxX = std::max(maxX, x);
            x = 0, line++, i++;
            continue;
        }
        if (c == '\r') {
            i++;
            continue;
        }
        if (c == '<') {  // tag: <COLOR>(n) recolours, others are skipped
            const size_t e = text.find('>', i);
            if (e != std::string::npos) {
                const std::string tag = text.substr(i + 1, e - i - 1);
                i = e + 1;
                if (i < text.size() && text[i] == '(') {
                    const size_t p = text.find(')', i);
                    const int v = std::atoi(text.c_str() + i + 1);
                    if (tag == "COLOR") color = std::clamp(v, 0, 3);
                    if (p != std::string::npos) i = p + 1;
                }
                continue;
            }
        }
        u16 code;
        if ((c >= 0x81 && c <= 0x9F) || (c >= 0xE0 && c <= 0xFC)) {
            code = u16(c << 8 | (i + 1 < text.size() ? u8(text[i + 1]) : 0));
            i += 2;
        } else {
            code = fullWidth(c);
            i += 1;
        }
        auto m = sFont.code.find(code);
        const int g = m != sFont.code.end() && m->second < sFont.glyphs ? m->second : -1;
        const int adv = g >= 0 && size_t(g) < sFont.width.size() ? sFont.width[size_t(g)] : sFont.defWidth;
        if (g >= 0) gs.push_back({g, x - (size_t(g) < sFont.kern.size() ? sFont.kern[size_t(g)] : 0), line, color});
        x += adv;
    }
    maxX = std::max(maxX, x);
    r.w = std::max(maxX + 8, 8);
    r.h = (line + 1) * sFont.leading + 8;
    std::vector<u8> rgba(size_t(r.w) * r.h * 4, 0);
    for (const G& g : gs) {
        const int ox = g.x + 4, oy = g.line * sFont.leading + 4;
        for (int y = 0; y < sFont.cellH; y++)
            for (int xx = 0; xx < sFont.cellW; xx++) {
                const int px = ox + xx, py = oy + y;
                if (px < 0 || py < 0 || px >= r.w || py >= r.h) continue;
                const u8 a = sFont.alpha[(size_t(g.glyph) * sFont.cellH + y) * sFont.cellW + xx];
                u8* p = &rgba[(size_t(py) * r.w + px) * 4];
                if (a > p[3]) {
                    p[0] = kColors[g.color][0], p[1] = kColors[g.color][1], p[2] = kColors[g.color][2], p[3] = a;
                }
            }
    }
    r.tex = aurora_imgui_add_texture(u32(r.w), u32(r.h), rgba.data());
    return r;
}

void showGameText(const std::string& text, float scale) {
    const Rendered& r = render(text);
    if (!r.tex) {
        ImGui::TextUnformatted(text.c_str());
        return;
    }
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const ImVec2 sz(float(r.w) * scale, float(r.h) * scale);
    ImGui::GetWindowDrawList()->AddRectFilled(p, ImVec2(p.x + sz.x, p.y + sz.y), IM_COL32(16, 20, 40, 230), 4.0f);
    ImGui::Image(r.tex, sz);
}

// ---- event archives -------------------------------------------------------------------------
struct EventFile {
    int no = 0;
    std::string script;                 // Shift-JIS
    std::vector<std::string> messages;  // Shift-JIS
    std::vector<bool> referenced;
    std::vector<std::string> comments;  // developer notes (Shift-JIS)
};

std::vector<EventFile> sEvents;
int sScanNext = 0;  // next event number to read (incremental scan, a few per frame)
constexpr int kMaxEvent = 130;

void parseEvent(EventFile& e, const std::vector<u8>& raw) {
    rarc::Archive a;
    if (!a.parse(rarc::decompress(raw))) return;
    std::function<void(const rarc::Node&, const std::string&)> walk;
    walk = [&](const rarc::Node& n, const std::string& dir) {
        for (const rarc::Item& it : n.items) {
            if (it.dir) {
                walk(a.nodes[size_t(it.node)], dir + it.name + "/");
            } else if (it.name.size() > 4 && it.name.compare(it.name.size() - 4, 4, ".txt") == 0) {
                e.script.assign(it.data.begin(), it.data.end());
            } else if (it.name.size() > 4 && it.name.compare(it.name.size() - 4, 4, ".csv") == 0) {
                std::string s(it.data.begin(), it.data.end());
                size_t p = 0;
                while (p <= s.size()) {
                    const size_t q = s.find("\r\n", p);
                    e.messages.push_back(s.substr(p, q == std::string::npos ? std::string::npos : q - p));
                    if (q == std::string::npos) break;
                    p = q + 2;
                }
            }
        }
    };
    walk(a.nodes[0], "");
    e.referenced.assign(e.messages.size(), false);
    auto mark = [&](const char* tag, bool quoted) {
        for (size_t p = e.script.find(tag); p != std::string::npos; p = e.script.find(tag, p + 1)) {
            size_t q = p + std::strlen(tag);
            if (quoted) {  // <CHOICE>"a"(n)"b"(m)...<LISTEND>
                const size_t end = e.script.find("<LISTEND>", q);
                for (size_t k = q; k < end && k < e.script.size(); k++)
                    if (e.script[k] == '(') {
                        const int n = std::atoi(e.script.c_str() + k + 1);
                        if (n >= 0 && size_t(n) < e.referenced.size()) e.referenced[size_t(n)] = true;
                    }
            } else if (q < e.script.size() && e.script[q] == '(') {
                const int n = std::atoi(e.script.c_str() + q + 1);
                if (n >= 0 && size_t(n) < e.referenced.size()) e.referenced[size_t(n)] = true;
            }
        }
    };
    mark("<SPEAK>", false);
    mark("<CHOICE>", true);
    // Developer notes: text outside tags on a script line (Shift-JIS, not part of a message).
    size_t p = 0;
    while (p < e.script.size()) {
        size_t q = e.script.find('\n', p);
        if (q == std::string::npos) q = e.script.size();
        std::string l = e.script.substr(p, q - p);
        p = q + 1;
        if (!l.empty() && l.back() == '\r') l.pop_back();
        if (l.find("<SPEAK>") != std::string::npos || l.find("<WINDOW>") != std::string::npos) continue;
        std::string rest;
        for (size_t i = 0; i < l.size();) {
            const u8 c = u8(l[i]);
            if (c == '<') {
                const size_t e2 = l.find('>', i);
                if (e2 == std::string::npos) break;
                i = e2 + 1;
                while (i < l.size() && (l[i] == '(' || l[i] == '"')) {
                    const char close = l[i] == '(' ? ')' : '"';
                    const size_t c2 = l.find(close, i + 1);
                    i = c2 == std::string::npos ? l.size() : c2 + 1;
                }
                continue;
            }
            const bool lead = (c >= 0x81 && c <= 0x9F) || (c >= 0xE0 && c <= 0xFC);
            rest.append(l, i, lead ? 2 : 1);
            i += lead ? 2 : 1;
        }
        bool wide = false;
        for (char ch : rest) wide |= u8(ch) >= 0x81;
        if (wide) e.comments.push_back(rest);
    }
}

void scanSome() {
    for (int k = 0; k < 6 && sScanNext <= kMaxEvent; k++, sScanNext++) {
        char path[48];
        std::snprintf(path, sizeof path, "/Event/event%02d.szp", sScanNext);
        const rarc::Bytes raw = rarc::readDisc(path);
        if (raw.empty()) continue;
        EventFile e;
        e.no = sScanNext;
        parseEvent(e, raw);
        if (!e.messages.empty() || !e.script.empty()) sEvents.push_back(std::move(e));
    }
}

bool isFiller(const std::string& m) {
    std::string t;
    for (size_t i = 0; i < m.size(); i++) {
        if (u8(m[i]) == 0x81 && i + 1 < m.size() && u8(m[i + 1]) == 0x40) {  // full-width space
            i++;
            continue;
        }
        if (u8(m[i]) > ' ') t += m[i];
    }
    return t.empty() || t == "dummy" || t == "msg";
}

// ---- event26: translation (port's own) ------------------------------------------------------
const char* const kEvent26English[] = {
    /*0*/ "Well now, what will you do, Luigi?",
    /*1*/ "Training",
    /*2*/ "Go to the gallery",
    /*3*/ "Go to the mansion",
    /*4*/ "Shall we give it a look!",
    /*5*/ nullptr,
    /*6*/ "Mind how you go!",
    /*7*/ "Come back alive, Luigi!",
    nullptr, nullptr, nullptr,
    /*11*/ "Shall we give it a try!",
    nullptr,
    /*13*/ "...I see.\nSo your name is Luigi.\nMeeting you here must be fate of some kind.\nI look forward to working with you, Luigi!",
    /*14*/ "Hm? Never mind that, you say...\nYou want me to tell you what on earth\nthis place is?",
    /*15*/ "This is where I live, busy day and night\nwith my research on ghosts.\nAlso known as...\nProfessor Oya-Mar's Ghost Research Center!\n...Lab, for short.",
    /*16*/ "That aside, being suddenly handed\na mansion as a present...\nthat's a strange story...",
    /*17*/ "Does that look like a real mansion\nto you?",
    /*18*/ "I've lived here since I was twenty, so I know:\na mansion like that wasn't here\nuntil just the other day.\nNot a trace of it.\nAre you sure a fox didn't trick you?",
    nullptr, nullptr,
    /*21*/ "I've been waiting for you, Luigi!",
    /*22*/ "It seems your brother has been\ncaught up in the strange power\nof the Boo swarm.\nPoor fellow...",
    /*23*/ "Well, they don't seem to be the type\nto eat him.\nBut what's become of him? It worries me.\nSometimes people can't come back, you know.",
    /*24*/ "From what I hear, Boos grow stronger\nthe bigger the swarm...\nThey resonate with one another\nand unleash a strange power.\nYour brother must have fallen to it.",
    /*25*/ "If you want to save your brother,\nthere's only one way!\nCatch the Boos one by one\nand weaken that strange power!",
    /*26*/ "But Boos, true to their name,\nare shy, and good at hiding.\nYou won't even find them\nwithout a real effort...",
    /*27*/ "Heh, heh, heh!\nNo need to look so glum.\nThat Game Boy Horror of yours\nhas a hidden function.",
    /*28*/ "I had a feeling this might happen,\nso I secretly prepared for it.\n...It's called the \"Boo Radar\"!",
    /*29*/ "If a Boo is hiding nearby,\nit blinks red to let you know.\nUse it to search the whole mansion,\nLuigi!",
    /*30*/ "Now, no need to get all worked up.\nRelax, take your time, one at a time.\nUsing the C Stick and the Control Stick well\nis the trick to catching Boos.\nDon't forget it!",
    /*31*/ "Well, then.\nBy the way...",
    /*32*/ "Getting a haunted mansion as a present -\nwho knows if it's a dream or an illusion -\nisn't much of a gift at all!",
    /*33*/ "Speaking of bad luck, I saw another\nyoung fellow who looks just like you,\nwearing red. He ran off before\nI could talk to him and never came back.",
    /*34*/ "What...?",
    /*35*/ "That was your brother?\n...This is serious!",
    /*36*/ "Anyone with no way to fight ghosts\nis a perfect target for them.\nHurry and go save your brother!",
    /*37*/ "...All right, I'll lend you a hand.\nI'll teach you how to deal with ghosts.\nFollow me, Luigi!",
    /*38*/ "Go to the Hidden Mansion",
};

// Notes in the event26 script, translated (the rest are commands).
const struct {
    const char* jp;
    const char* en;
} kEvent26Notes[] = {
    {"\x83\x66\x83\x6F\x83\x4F\x97\x70", "For debugging (four flag sets at the top)"},  // デバグ用
    {"\x83\x65\x83\x8C\x83\x54\x82\xCC\x89\xF0\x95\xFA\x82\xF0\x82\xB5\x82\xBD\x82\xA9\x94\xBB\x92\xE8", "Check: were the Boos released?"},  // テレサの解放をしたか判定
    {"\x90\xE0\x96\xBE\x8F\x49\x82\xA6\x82\xBD\x82\xA9\x94\xBB\x92\xE8", "Check: has the explanation been given?"},  // 説明終えたか判定
    {"\x90\xE0\x96\xBE\x82\xF0\x8F\x49\x82\xA6\x82\xBD\x94\xBB\x92\xE8", "Mark: explanation given"},  // 説明を終えた判定
    {"\x8F\xED\x92\x93\x8E\xB7\x8E\x96\x82\xCC\x94\x7A\x92\x75\x83\x74\x83\x89\x83\x4F", "Flag that places the resident butler"},  // 常駐執事の配置フラグ
    {"\x83\x66\x83\x74\x83\x48\x83\x8B\x83\x67\x82\xC6\x93\xAF\x82\xB6", "Same as the default"},  // デフォルトと同じ
    {"\x8F\x89\x8A\xFA\x92\x6E\x89\xBA\x83\x89\x83\x7B\x82\xA9\x81\x41\x83\x66\x83\x74\x83\x48\x83\x8B\x83\x67\x82\xA9\x82\xF0\x94\xBB\x95\xCA", "Decide: first visit to the underground lab, or the default?"},  // 初期地下ラボか、デフォルトかを判別
    {"\x8F\x89\x8A\xFA\x92\x6E\x89\xBA\x83\x89\x83\x7B\x82\xF0\x8F\x49\x82\xA6\x82\xBD\x94\xBB\x92\xE8", "Mark: first lab visit done"},  // 初期地下ラボを終えた判定
    {"\x8F\x89\x8A\xFA\x92\x6E\x89\xBA\x83\x89\x83\x7B", "First underground-lab visit"},  // 初期地下ラボ
    {"\x82\xA8\x82\xB5\x82\xE1\x82\xD7\x82\xE8\x83\x4A\x83\x81\x83\x89", "Talking camera"},  // おしゃべりカメラ
    {"\x83\x66\x83\x74\x83\x48\x83\x8B\x83\x67\x83\x67\x83\x8C\x81\x5B\x83\x6A\x83\x93\x83\x4F\x83\x43\x83\x78\x83\x93\x83\x67\x8B\x4E\x93\xAE\x82\xCC\x82\xBD\x82\xDF\x82\xCC\x83\x74\x83\x89\x83\x4F", "Flag that starts the default training event"},  // デフォルトトレーニングイベント起動のためのフラグ
    {"\x83\x66\x83\x74\x83\x48\x83\x8B\x83\x67", "Default"},  // デフォルト
    {"\x97\xFB\x8F\x4B", "Training"},  // 練習
    {"\x89\xAE\x95\x7E\x82\xC9\x8D\x73\x82\xAD", "Go to the mansion"},  // 屋敷に行く
    {"\x90\xEA\x97\x70\x83\x5E\x83\x4F\x81\x69\x83\x66\x83\x82\x81\x60\x83\x8F\x81\x5B\x83\x76\x81\x60\x83\x8F\x83\x43\x83\x76\x81\x6A", "Dedicated tag (demo, warp, wipe)"},  // 専用タグ（デモ〜ワープ〜ワイプ）
    {"\x83\x4D\x83\x83\x83\x89\x83\x8A\x81\x5B\x82\xC9\x8D\x73\x82\xAD", "Go to the gallery"},  // ギャラリーに行く
    {"\x97\xA0\x89\xAE\x95\x7E\x82\xC9\x8D\x73\x82\xAD", "Go to the Hidden Mansion (ura-yashiki)"},  // 裏屋敷に行く
};

const char* translateNote(const std::string& jp) {
    for (const auto& n : kEvent26Notes)
        if (jp.find(n.jp) != std::string::npos) return n.en;
    return nullptr;
}

bool sTextOpen = false;

}  // namespace

void textArchiveButton() {
    if (recomp::menu::Button(sTextOpen ? "Close text archive###textarchive" : "Text archive (unused lines)###textarchive")) sTextOpen = !sTextOpen;
    recomp::menu::ItemTooltip("Opens a window with the game's text, unused lines included, drawn in the game font.");
}

void textArchiveWindow() {
    static const bool sEnv = std::getenv("LM_TEXT_ARCHIVE") != nullptr;
    static bool sEnvOnce = false;
    if (sEnv && !sEnvOnce) sEnvOnce = sTextOpen = true;
    if (!sTextOpen) return;
    scanSome();
    recomp::ui::beforeWindow("Text archive", ImVec2(820, 600), ImVec2(320, 240));
    if (!ImGui::Begin("Text archive", &sTextOpen)) {
        ImGui::End();
        return;
    }
    static float sScale = 0.6f;
    recomp::menu::SliderFloat("Text size", &sScale, 0.3f, 1.0f, "%.2f");
    if (sScanNext <= kMaxEvent) ImGui::TextDisabled("Reading event archives... %d / %d", sScanNext, kMaxEvent);
    static const char* sEnvTab = std::getenv("LM_TEXT_ARCHIVE");
    auto tabFlags = [&](const char* name) {
        return sEnvTab && !std::strcmp(sEnvTab, name) ? ImGuiTabItemFlags_SetSelected : 0;
    };
    if (ImGui::BeginTabBar("textarchive")) {
        if (ImGui::BeginTabItem("Unused lines", nullptr, tabFlags("lines"))) {
            ImGui::TextDisabled("Messages that no <SPEAK> or <CHOICE> of their event script refers to (never shown).");
            ImGui::BeginChild("lines");
            for (const EventFile& e : sEvents)
                for (size_t i = 0; i < e.messages.size(); i++) {
                    if (e.referenced[i] || isFiller(e.messages[i])) continue;
                    ImGui::Text("event%02d  message %zu", e.no, i);
                    if (e.no == 3 && i == 12) {
                        ImGui::SameLine();
                        ImGui::TextDisabled("(the pianist's tune quiz - the tune is Totaka's Song)");
                    }
                    showGameText(e.messages[i], sScale);
                    if (e.no == 26 && i < std::size(kEvent26English) && kEvent26English[i]) ImGui::TextWrapped("%s", kEvent26English[i]);
                }
            ImGui::EndChild();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Early lab script (event26)", nullptr, tabFlags("event26"))) {
            ImGui::TextWrapped("An early, Japanese version of E. Gadd's lab script (retail: event08). The Professor "
                               "is still \"Oya-Mar\" (Oyama). English translations are the port's own.");
            ImGui::BeginChild("ev26");
            for (const EventFile& e : sEvents) {
                if (e.no != 26) continue;
                for (size_t i = 0; i < e.messages.size(); i++) {
                    if (isFiller(e.messages[i])) continue;
                    ImGui::Text("message %zu%s", i, e.referenced[i] ? "" : "  (not used by the script)");
                    showGameText(e.messages[i], sScale);
                    if (i < std::size(kEvent26English) && kEvent26English[i]) ImGui::TextWrapped("%s", kEvent26English[i]);
                    ImGui::Separator();
                }
                recomp::menu::SeparatorText("Developer notes in the script");
                for (const std::string& n : e.comments) {
                    showGameText(n, sScale * 0.8f);
                    if (const char* en = translateNote(n)) {
                        ImGui::SameLine();
                        ImGui::TextWrapped("%s", en);
                    }
                }
            }
            ImGui::EndChild();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Developer notes", nullptr, tabFlags("notes"))) {
            ImGui::TextDisabled("Japanese notes the developers left in the event scripts (skipped by the interpreter).\n"
                                "Drawn with the game font, which only has 64 kanji: missing ones are left blank.");
            ImGui::BeginChild("notes");
            for (const EventFile& e : sEvents) {
                if (e.comments.empty()) continue;
                recomp::menu::SeparatorText(("event" + std::string(e.no < 10 ? "0" : "") + std::to_string(e.no)).c_str());
                for (const std::string& n : e.comments) showGameText(n, sScale * 0.8f);
            }
            ImGui::EndChild();
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    ImGui::End();
}

}  // namespace recomp::unused2
