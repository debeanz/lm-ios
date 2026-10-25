// F1 > Enhancements > Silly: Luigi options. All visual only and off by default (retail look).
//
// Head size, body size, wobbly head, paper Luigi: S3D models (.mdl) keep one world matrix per
// joint at S3DModel +0x14 (0x30 bytes, 3x4). Every path that animates a model (calcJoints
// fn_800589B0, the actor loop fn_800613C8, fn_800689BC) recomputes them from the .key and then
// calls fn_80058850, which builds the skinning (envelope) matrices from them. Hooking
// fn_80058850 changes the joints of Luigi's models right before that, so the skin, the rigid
// face parts (bound to the head joint), the per-pass view/normal matrices (calcView
// fn_80059710, also for the shadow maps) and everything attached to his joints (Poltergust,
// flashlight, Game Boy Horror) follow, and frame interpolation records and blends the changed
// matrices like any animation. Head = the joint subtree from the neck joint up (luige.mdl:
// joint 51 and its children 52..58: head, nose, mustache, hat), found per model from the joints
// the hat material's vertices are bound to; scaled (and wobbled) about the neck joint. Body =
// every joint, about joint 0 (between his feet). Paper = the body transform flattened along his
// facing axis (joint 0's model-space +Z) to 6 %: normal from the front and back, a sheet when he
// turns sideways. Wobble = a damped spring per model instance, stepped once per game frame and
// driven by the acceleration of the neck joint; its offset tilts the head away from the motion
// and stretches it a little along its own axis.
//
// Clothes / Poltergust colours: every Luigi model shares the same textures (CMPR): the cap
// (128x128, green with the white "L"), one texture for the shirt and overalls (128x128 in the
// game's models, 128x256 in the opening/file-select ones: green shirt between the blue straps),
// and the shoes (64x64 brown); the Poltergust body has its own red textures (vbody, the ending's
// db_sojiki). They are recognised by content (64-bit FNV-1a of the texels, identical on the USA
// and PAL discs) when the game initialises a texture object (GXInitTexObj), and Aurora is handed
// a recoloured host copy instead: per CMPR block each endpoint colour that belongs to the piece
// (hue range + saturation: green = shirt/hat, blue = overalls, brown = shoes, red = Poltergust) gets
// the picked colour at the endpoint's brightness relative to the piece's mean brightness, so the
// shading stays and white (the "L", the stitching) or grey parts are untouched. Aurora caches
// textures by content, so a new colour is a new texture and the old one simply goes unused.
#include "recomp/silly.h"

#include "port/settings.h"
#include "recomp/frame.h"
#include "recomp/guest_util.h"
#include "recomp/hle.h"
#include "recomp/menu.h"
#include "recomp/model_viewer.h"
#include "recomp/unused2.h"

#include <imgui.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace recomp::silly {
namespace {

// ---- settings and test aids ------------------------------------------------------------------

float envFloat(const char* n, float def) {
    const char* v = std::getenv(n);
    return v && *v ? float(std::atof(v)) : def;
}
bool envColor(const char* n, unsigned& out) {
    const char* v = std::getenv(n);
    if (!v || !*v) return false;
    char* end = nullptr;
    const unsigned long x = std::strtoul(v, &end, 16);
    if (end == v) return false;
    out = unsigned(x & 0xFFFFFF);
    return true;
}

struct Outfit {
    const char* name;
    unsigned rgb[4];  // hat, shirt, overalls, shoes; 0xFF000000 = original
};
constexpr unsigned kOrig = 0xFF000000u;
constexpr Outfit kOutfits[] = {
    {"Classic", {kOrig, kOrig, kOrig, kOrig}},
    {"Mario", {0xE01818, 0xE01818, 0x2848C8, 0x6A3A1E}},
    {"Wario", {0xF0D020, 0xF0D020, 0x7A2CA0, 0x2E8B30}},
    {"Waluigi", {0x6A2C9A, 0x6A2C9A, 0x24243C, 0xE07A10}},
    {"Fire Luigi", {0xF4F4F4, 0xF4F4F4, 0x3CA03C, 0x6A3A1E}},
};

// Test aids (CLAUDE.md): LM_HEAD_SCALE=<f>, LM_BODY_SCALE=<f>, LM_WOBBLE=<strength>, LM_PAPER=1,
// LM_HAT_COLOR / LM_SHIRT_COLOR / LM_OVERALLS_COLOR / LM_SHOES_COLOR / LM_POLTERGUST_COLOR=RRGGBB,
// LM_OUTFIT=mario|wario|waluigi|fire. Applied to the settings once (not saved unless the menu saves).
void applyEnvOnce() {
    static std::once_flag sOnce;
    std::call_once(sOnce, [] {
        auto& s = port::settings().sillyLuigi;
        s.headScale = std::clamp(envFloat("LM_HEAD_SCALE", s.headScale), 0.25f, 3.0f);
        s.bodyScale = std::clamp(envFloat("LM_BODY_SCALE", s.bodyScale), 0.25f, 3.0f);
        if (const char* w = std::getenv("LM_WOBBLE"); w && *w) {
            s.wobble = std::atof(w) > 0.0f;
            if (s.wobble) s.wobbleStrength = std::clamp(float(std::atof(w)), 0.25f, 3.0f);
        }
        if (const char* p = std::getenv("LM_PAPER"); p && *p) s.paper = *p != '0';
        if (const char* o = std::getenv("LM_OUTFIT"); o && *o) {
            auto prefix = [](const char* name, const char* p) {
                for (; *p; p++, name++)
                    if (!*name || (*name | 0x20) != (*p | 0x20)) return false;
                return true;
            };
            for (const Outfit& f : kOutfits)
                if (prefix(f.name, o))
                    for (int i = 0; i < 4; i++) {
                        s.colorOn[i] = f.rgb[i] != kOrig;
                        if (f.rgb[i] != kOrig) s.colorRgb[i] = f.rgb[i];
                    }
        }
        const char* names[port::kSillyPieceCount] = {"LM_HAT_COLOR", "LM_SHIRT_COLOR", "LM_OVERALLS_COLOR",
                                                     "LM_SHOES_COLOR", "LM_POLTERGUST_COLOR"};
        for (int i = 0; i < port::kSillyPieceCount; i++)
            if (envColor(names[i], s.colorRgb[i])) s.colorOn[i] = true;
    });
}

const port::SillyLuigiSettings& cfg() {
    applyEnvOnce();
    return port::settings().sillyLuigi;
}

u64 fnv(const u8* p, u32 n) {
    u64 h = 0xCBF29CE484222325ull;
    for (u32 i = 0; i < n; i++) h = (h ^ p[i]) * 0x100000001B3ull;
    return h;
}

bool inGuest(u32 a) { return a >= 0x80000000u && a < 0x83FF0000u; }

// ---- known textures ----------------------------------------------------------------------------

enum class Role { Hat, ShirtOveralls, Shoes, Poltergust };
struct KnownTex {
    u64 hash;
    u8 mdlFmt;  // .mdl format code: 7 RGB565, 10 CMPR
    u16 w, h;
    Role role;
};
// FNV-1a of the texel bytes; same on the USA and PAL discs.
constexpr KnownTex kKnown[] = {
    {0xDD6D1F66D5A8C4F6ull, 10, 128, 128, Role::Hat},            // luige tex 6 (every Luigi model)
    {0x6C8A7672154D01D8ull, 10, 128, 128, Role::ShirtOveralls},  // luige tex 3 (game, cutscene, ending)
    {0x50DD8326A2453F6Dull, 10, 128, 256, Role::ShirtOveralls},  // opening / file select Luigi
    {0xE2C7092E77C81AADull, 10, 64, 64, Role::Shoes},            // luige tex 0
    {0x234BB0F052809EBCull, 10, 64, 64, Role::Poltergust},       // vbody
    {0x01C5DBC059DB3561ull, 10, 128, 128, Role::Poltergust},     // vbody
    {0x7FE0316AD8B080DEull, 7, 16, 16, Role::Poltergust},        // vbody (resident archive)
    {0xF72720A9044CFF17ull, 10, 16, 16, Role::Poltergust},       // db_sojiki (ending)
    {0xA76F3A0BB267C0D7ull, 7, 32, 32, Role::Poltergust},        // db_sojiki
    {0x202271F7B3929D77ull, 10, 32, 64, Role::Poltergust},       // db_sojiki
};
constexpr u64 kHatHash = 0xDD6D1F66D5A8C4F6ull;

u32 texBytes(u8 mdlFmt, u32 w, u32 h) {
    if (mdlFmt == 10) return ((w + 7) / 8) * ((h + 7) / 8) * 32;
    if (mdlFmt == 7) return ((w + 3) / 4) * ((h + 3) / 4) * 32;
    return 0;
}

// ---- matrices ----------------------------------------------------------------------------------

struct M34 {
    float m[3][4];
};
struct V3 {
    float x, y, z;
};
V3 operator+(V3 a, V3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
V3 operator-(V3 a, V3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
V3 operator*(V3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
float dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
V3 cross(V3 a, V3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
float len(V3 a) { return std::sqrt(dot(a, a)); }

M34 ident() {
    M34 r{};
    r.m[0][0] = r.m[1][1] = r.m[2][2] = 1.0f;
    return r;
}
M34 mul(const M34& a, const M34& b) {
    M34 r{};
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 4; j++) {
            float v = a.m[i][0] * b.m[0][j] + a.m[i][1] * b.m[1][j] + a.m[i][2] * b.m[2][j];
            if (j == 3) v += a.m[i][3];
            r.m[i][j] = v;
        }
    return r;
}
V3 trans(const M34& a) { return {a.m[0][3], a.m[1][3], a.m[2][3]}; }
// Linear part L applied about point p: T(p) * L * T(-p).
M34 about(const M34& lin, V3 p) {
    M34 r = lin;
    const float q[3] = {p.x, p.y, p.z};
    for (int i = 0; i < 3; i++) r.m[i][3] = q[i] - (lin.m[i][0] * q[0] + lin.m[i][1] * q[1] + lin.m[i][2] * q[2]);
    return r;
}
M34 scaleAlong(V3 u, float s) {  // I + (s - 1) u u^T, u unit
    M34 r = ident();
    const float v[3] = {u.x, u.y, u.z};
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++) r.m[i][j] += (s - 1.0f) * v[i] * v[j];
    return r;
}
M34 rotation(V3 axis, float angle) {  // axis unit
    const float c = std::cos(angle), s = std::sin(angle), t = 1.0f - c;
    const float x = axis.x, y = axis.y, z = axis.z;
    M34 r{};
    r.m[0][0] = t * x * x + c, r.m[0][1] = t * x * y - s * z, r.m[0][2] = t * x * z + s * y;
    r.m[1][0] = t * x * y + s * z, r.m[1][1] = t * y * y + c, r.m[1][2] = t * y * z - s * x;
    r.m[2][0] = t * x * z - s * y, r.m[2][1] = t * y * z + s * x, r.m[2][2] = t * z * z + c;
    return r;
}
M34 readM(u32 a) {
    M34 r;
    for (int i = 0; i < 12; i++) r.m[i / 4][i % 4] = gu::rdf(a + 4 * i);
    return r;
}
void writeM(u32 a, const M34& r) {
    for (int i = 0; i < 12; i++) gu::wrf(a + 4 * i, r.m[i / 4][i % 4]);
}

// ---- Luigi models -----------------------------------------------------------------------------

struct ModelInfo {
    u32 fp[5]{};
    bool luigi = false;
    bool equipment = false;  // Poltergust body/nozzle, flashlight, Game Boy Horror
    u32 nj = 0;
    int pivot = -1;   // neck joint: root of the head subtree
    int hatRef = -1;  // a joint inside the hat (head axis for the wobble)
    std::vector<u8> head;
};
std::unordered_map<u32, ModelInfo> sModels;

bool isHatTexture(u32 hdr) {
    if (!inGuest(hdr) || rd8(hdr) != 10 || rd16(hdr + 2) != 128 || rd16(hdr + 4) != 128) return false;
    return fnv(static_cast<const u8*>(hostp(hdr + 0x20)), 128 * 128 / 2) == kHatHash;
}

V3 bindPos(u32 invBind) {
    // Joint position in the bind pose: -R^-1 t of the inverse bind matrix [R | t].
    M34 a = readM(invBind);
    const float (&m)[3][4] = a.m;
    const float det = m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1]) - m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0]) +
                      m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0]);
    if (std::fabs(det) < 1e-12f) return {0, 0, 0};
    float inv[3][3];
    inv[0][0] = (m[1][1] * m[2][2] - m[1][2] * m[2][1]) / det;
    inv[0][1] = (m[0][2] * m[2][1] - m[0][1] * m[2][2]) / det;
    inv[0][2] = (m[0][1] * m[1][2] - m[0][2] * m[1][1]) / det;
    inv[1][0] = (m[1][2] * m[2][0] - m[1][0] * m[2][2]) / det;
    inv[1][1] = (m[0][0] * m[2][2] - m[0][2] * m[2][0]) / det;
    inv[1][2] = (m[0][2] * m[1][0] - m[0][0] * m[1][2]) / det;
    inv[2][0] = (m[1][0] * m[2][1] - m[1][1] * m[2][0]) / det;
    inv[2][1] = (m[0][1] * m[2][0] - m[0][0] * m[2][1]) / det;
    inv[2][2] = (m[0][0] * m[1][1] - m[0][1] * m[1][0]) / det;
    V3 r;
    float* o = &r.x;
    for (int i = 0; i < 3; i++) o[i] = -(inv[i][0] * m[0][3] + inv[i][1] * m[1][3] + inv[i][2] * m[2][3]);
    return r;
}

void analyze(u32 mdl, ModelInfo& mi) {
    if (rd32(mdl) != 0x04B40000u || !(rd32(mdl + 0x48) & 0x80000000u)) return;  // relocated .mdl only
    const u32 nj = rd16(mdl + 8), nenv = rd16(mdl + 0xC);
    const u32 nodes = rd32(mdl + 0x30), packets = rd32(mdl + 0x34), invBind = rd32(mdl + 0x38);
    const u32 envIdx = rd32(mdl + 0x40), envCnt = rd32(mdl + 0x44), texTable = rd32(mdl + 0x60);
    const u32 mats = rd32(mdl + 0x68), samps = rd32(mdl + 0x6C), shapes = rd32(mdl + 0x70), draws = rd32(mdl + 0x74);
    if (nj == 0 || nj > 512 || !inGuest(nodes) || !inGuest(invBind) || !inGuest(texTable)) return;
    const bool mario = unused2::isMarioModel(mdl);
    // Luigi's equipment (separate models placed at his back/hands every frame without his scale):
    // recognised by one of their first two textures.
    static constexpr u64 kEquipment[] = {
        0xF72720A9044CFF17ull,  // vbody (Poltergust body, game archive)
        0x234BB0F052809EBCull,  // vbody (E3 model, /model/vbody.szp)
        0xA79FE99763FA2CAAull,  // vhead (nozzle)
        0xCAB78D8155BD4C00ull,  // wplight (flashlight)
        0x2FF2E4CE2C6D3ECAull,  // gameboy (Game Boy Horror)
    };
    for (u32 t = 0, n = std::min<u32>(2, rd16(mdl + 0x20)); t < n && !mario; t++) {
        const u32 hdr = rd32(texTable + 4 * t);
        if (!inGuest(hdr)) continue;
        const u32 bytes = texBytes(rd8(hdr), rd16(hdr + 2), rd16(hdr + 4));
        if (!bytes || bytes > 0x4000) continue;
        const u64 h = fnv(static_cast<const u8*>(hostp(hdr + 0x20)), bytes);
        for (u64 e : kEquipment)
            if (h == e) {
                mi.equipment = true;
                mi.nj = nj;
                return;
            }
    }
    int hatTex = -1;
    for (u32 t = 0, n = rd16(mdl + 0x20); t < n && hatTex < 0; t++)
        if (isHatTexture(rd32(texTable + 4 * t))) hatTex = int(t);
    if (hatTex < 0 && !mario) return;
    mi.luigi = true;
    mi.nj = nj;

    // Joint hierarchy from the node graph: +2 first child, +4 next sibling (relative node counts).
    std::vector<int> parent(nj, -1);
    {
        std::vector<std::pair<int, int>> stack{{0, -1}};
        u32 guard = 0;
        while (!stack.empty() && guard++ < nj * 4) {
            auto [i, par] = stack.back();
            stack.pop_back();
            if (i < 0 || u32(i) >= nj) continue;
            const u32 n = nodes + 0x10 * u32(i);
            const u32 j = rd16(n);
            if (j >= nj) continue;
            parent[j] = par;
            const s16 child = s16(rd16(n + 2)), next = s16(rd16(n + 4));
            if (next) stack.push_back({i + next, par});
            if (child) stack.push_back({i + child, int(j)});
        }
    }

    // Joints of the hat: the vertices of the draws whose material samples the hat texture.
    std::vector<int> hat;
    if (hatTex < 0) {
        if (nj == 65) hat = {56, 57, 58};  // Play-as-Mario copy: Luigi's skeleton
    } else if (inGuest(mats) && inGuest(samps) && inGuest(shapes) && inGuest(draws) && inGuest(packets)) {
        std::vector<u32> envFirst(nenv + 1, 0);
        for (u32 e = 0; e < nenv; e++) envFirst[e + 1] = envFirst[e] + rd8(envCnt + e);
        std::vector<u8> mark(nj, 0);
        for (u32 d = 0, nd = rd16(mdl + 0x2A); d < nd; d++) {
            const u32 mat = rd16(draws + 4 * d), shape = rd16(draws + 4 * d + 2);
            const u32 ma = mats + 0x120 * mat;
            if (rd8(ma + 7) == 0 || rd16(samps + 8 * rd16(ma + 0x22)) != u32(hatTex)) continue;
            const u32 sa = shapes + 8 * shape;
            for (u32 p = 0, np = rd16(sa + 4); p < np; p++) {
                const u32 pa = packets + 0x20 * (rd16(sa + 6) + p);
                for (u32 k = 0, nm = std::min<u32>(10, rd16(pa + 0xA)); k < nm; k++) {
                    const u32 v = rd16(pa + 0xC + 2 * k);
                    if (v < nj) mark[v] = 1;
                    else if (v - nj < nenv)
                        for (u32 q = envFirst[v - nj]; q < envFirst[v - nj + 1]; q++) {
                            const u32 j = rd16(envIdx + 2 * q);
                            if (j < nj) mark[j] = 1;
                        }
                }
            }
        }
        for (u32 j = 0; j < nj; j++)
            if (mark[j]) hat.push_back(int(j));
    }
    if (hat.empty()) return;
    // Lowest common ancestor of the hat joints.
    auto chain = [&](int j) {
        std::vector<int> c;
        for (int g = 0; j >= 0 && g < int(nj); g++, j = parent[j]) c.push_back(j);
        return c;
    };
    std::vector<int> lcaChain = chain(hat[0]);
    for (int h : hat) {
        const std::vector<int> c = chain(h);
        while (!lcaChain.empty() && std::find(c.begin(), c.end(), lcaChain.front()) == c.end()) lcaChain.erase(lcaChain.begin());
    }
    if (lcaChain.empty()) return;
    const int lca = lcaChain.front();
    mi.hatRef = lca;
    // Neck: walking up from the hat, the first joint clearly above its parent and well below the hat
    // (luige: 51 at y 148 above 50 at 135.8, the shoulders' height; the opening's 122-joint rig: 88;
    // the file-select scene's smaller, seated 124-joint Luigi: 92). Relative to his height in the
    // bind pose, as the models differ in size.
    // (The file-select Luigi's cap is bound to the neck joint itself, so "below the hat" is measured
    // from the cap's highest joint.)
    float lcaY = -1e30f;
    for (int h : hat) lcaY = std::max(lcaY, bindPos(invBind + 0x30 * u32(h)).y);
    const float height = std::max(lcaY - bindPos(invBind).y, 1.0f);
    for (int j = lca; j >= 0 && parent[j] >= 0; j = parent[j]) {
        const float y = bindPos(invBind + 0x30 * u32(j)).y, py = bindPos(invBind + 0x30 * u32(parent[j])).y;
        if (y - py > 0.025f * height && y < lcaY - 0.12f * height) {
            mi.pivot = j;
            break;
        }
    }
    if (mi.pivot < 0) return;
    mi.head.assign(nj, 0);
    for (u32 j = 0; j < nj; j++)
        for (int a = int(j), g = 0; a >= 0 && g < int(nj); a = parent[a], g++)
            if (a == mi.pivot) {
                mi.head[j] = 1;
                break;
            }
}

const ModelInfo& modelInfo(u32 mdl) {
    const u32 fp[5] = {rd32(mdl), rd32(mdl + 8), rd32(mdl + 0x30), rd32(mdl + 0x60), unused2::isMarioModel(mdl)};
    auto it = sModels.find(mdl);
    if (it != sModels.end() && !std::memcmp(it->second.fp, fp, sizeof fp)) return it->second;
    if (sModels.size() > 512) sModels.clear();
    ModelInfo& mi = sModels[mdl];
    mi = ModelInfo{};
    std::memcpy(mi.fp, fp, sizeof fp);
    analyze(mdl, mi);
    if (mi.luigi) {
        static int sLogged = 0;
        if (sLogged++ < 24) log("[silly] Luigi model %08X: %u joints, neck joint %d, %zu head joints", mdl, mi.nj, mi.pivot,
                                size_t(std::count(mi.head.begin(), mi.head.end(), u8(1))));
    }
    return mi;
}

// Per Luigi model instance (S3DModel): the wobble spring.
struct ObjState {
    u32 mdl = 0;
    u32 frame = ~0u;
    bool init = false;
    V3 prev{}, prev2{}, off{}, vel{};
};
std::unordered_map<u32, ObjState> sObjs;

V3 sFacing{0, 0, 1};  // the gameplay Luigi's facing axis (last frame)

// Body size and paper flattening (along `facing`, unit) as a linear map.
M34 bodyLinear(const V3* facing) {
    const auto& s = cfg();
    M34 lin = ident();
    if (s.paper && facing) lin = scaleAlong(*facing, 0.06f);
    if (s.bodyScale != 1.0f)
        for (int i = 0; i < 3; i++)
            for (int j = 0; j < 3; j++) lin.m[i][j] *= s.bodyScale;
    return lin;
}

// Equipment follows Luigi's joints by place and turn but keeps its own size: scaled (and flattened)
// about its own root, the point where it is held.
void scaleEquipment(u32 joints, u32 nj) {
    const auto& s = cfg();
    if (s.bodyScale == 1.0f && !s.paper) return;
    const M34 b = about(bodyLinear(&sFacing), trans(readM(joints)));
    for (u32 j = 0; j < nj; j++) writeM(joints + 0x30 * j, mul(b, readM(joints + 0x30 * j)));
}

bool jointModsActive() {
    const auto& s = cfg();
    return s.headScale != 1.0f || s.bodyScale != 1.0f || s.wobble || s.paper;
}

void modifyJoints(u32 obj) {
    const u32 mdl = rd32(obj + 4), joints = rd32(obj + 0x14);
    if (!inGuest(mdl) || !inGuest(joints)) return;
    const ModelInfo& mi = modelInfo(mdl);
    if (mi.equipment) {
        scaleEquipment(joints, mi.nj);
        return;
    }
    if (!mi.luigi) return;
    const auto& s = cfg();
    if (sObjs.size() > 256) sObjs.clear();
    ObjState& st = sObjs[obj];
    if (st.mdl != mdl) st = ObjState{}, st.mdl = mdl;
    const u32 nj = mi.nj;
    const int pivot = mi.pivot;
    std::vector<M34> m(nj);
    for (u32 j = 0; j < nj; j++) m[j] = readM(joints + 0x30 * j);

    // Head: size, wobble.
    M34 head = ident();
    bool headOn = false;
    if (pivot >= 0) {
        const V3 p = trans(m[pivot]);
        V3 axis = trans(m[mi.hatRef]) - p;
        const float axisLen = len(axis);
        axis = axisLen > 1e-3f ? axis * (1.0f / axisLen) : V3{0, 1, 0};
        M34 lin = ident();
        if (s.headScale != 1.0f) {
            for (int i = 0; i < 3; i++) lin.m[i][i] = s.headScale;
            headOn = true;
        }
        if (s.wobble) {
            const u32 frame = frame::guestFrames();
            if (!st.init || len(p - st.prev) > 120.0f) {
                st.init = true;
                st.prev = st.prev2 = p;
                st.off = st.vel = {0, 0, 0};
                st.frame = frame;
            } else if (frame != st.frame) {
                // One spring step per game frame (frames skipped while paused or fast-forwarded
                // count as one): lag behind the neck's acceleration, pulled back and damped.
                st.frame = frame;
                const V3 acc = p - st.prev * 2.0f + st.prev2;
                st.prev2 = st.prev;
                st.prev = p;
                const float k = 0.16f, damp = 0.14f;
                st.vel = st.vel - st.off * k - st.vel * damp - acc * 1.6f;
                st.off = st.off + st.vel;
                const float l = len(st.off);
                if (l > 80.0f) st.off = st.off * (80.0f / l);
            }
            const float str = s.wobbleStrength;
            const V3 side = st.off - axis * dot(st.off, axis);
            const float sideLen = len(side);
            M34 w = ident();
            const float stretch = 1.0f + std::clamp(dot(st.off, axis) * 0.012f * str, -0.3f, 0.3f);
            w = scaleAlong(axis, stretch);
            if (sideLen > 1e-3f) {
                const float angle = std::min(sideLen * 0.022f * str, 0.75f);
                V3 rotAxis = cross(axis, side * (1.0f / sideLen));
                const float rl = len(rotAxis);
                if (rl > 1e-4f) w = mul(rotation(rotAxis * (1.0f / rl), angle), w);
            }
            lin = mul(w, lin);
            headOn = true;
        }
        head = about(lin, p);
    }

    // Body: size about joint 0 (his feet), paper flattening along his facing axis.
    M34 body = ident();
    bool bodyOn = false;
    if (s.bodyScale != 1.0f || s.paper) {
        V3 f{m[0].m[0][2], m[0].m[1][2], m[0].m[2][2]};
        const float l = len(f);
        if (l > 1e-4f) {
            f = f * (1.0f / l);
            if (nj == 65) sFacing = f;  // the gameplay Luigi: his equipment flattens along it
        }
        body = about(bodyLinear(l > 1e-4f ? &f : nullptr), trans(m[0]));
        bodyOn = true;
    }
    if (!headOn && !bodyOn) return;
    const M34 bodyHead = mul(body, head);
    for (u32 j = 0; j < nj; j++) {
        const bool inHead = headOn && pivot >= 0 && mi.head[j];
        if (!inHead && !bodyOn) continue;
        writeM(joints + 0x30 * j, mul(inHead ? bodyHead : body, m[j]));
    }
}

// ---- recolouring -------------------------------------------------------------------------------

struct Rgb {
    float r, g, b;
};
float luma(Rgb c) { return 0.299f * c.r + 0.587f * c.g + 0.114f * c.b; }
Rgb from565(u16 v) {
    const int r = (v >> 11) & 31, g = (v >> 5) & 63, b = v & 31;
    return {float((r << 3) | (r >> 2)), float((g << 2) | (g >> 4)), float((b << 3) | (b >> 2))};
}
u16 to565(Rgb c) {
    auto q = [](float v, int bits) {
        const int maxv = (1 << bits) - 1;
        return std::clamp(int(std::lround(v / 255.0f * float(maxv))), 0, maxv);
    };
    return u16(q(c.r, 5) << 11 | q(c.g, 6) << 5 | q(c.b, 5));
}

// Which piece a colour belongs to in a texture of `role`, or -1.
int classify(Rgb c, Role role) {
    const float mx = std::max({c.r, c.g, c.b}), mn = std::min({c.r, c.g, c.b});
    if (mx < 12.0f) return -1;
    const float sat = (mx - mn) / mx;
    float hue = 0.0f;
    if (mx > mn) {
        if (mx == c.r) hue = 60.0f * std::fmod((c.g - c.b) / (mx - mn), 6.0f);
        else if (mx == c.g) hue = 60.0f * ((c.b - c.r) / (mx - mn) + 2.0f);
        else hue = 60.0f * ((c.r - c.g) / (mx - mn) + 4.0f);
        if (hue < 0) hue += 360.0f;
    }
    const bool green = hue >= 55.0f && hue <= 170.0f && sat > 0.3f;
    switch (role) {
    case Role::Hat: return green ? port::kSillyHat : -1;
    case Role::ShirtOveralls:
        if (green) return port::kSillyShirt;
        if (hue >= 180.0f && hue <= 275.0f && sat > 0.12f) return port::kSillyOveralls;
        return -1;
    case Role::Shoes: return (hue >= 340.0f || hue <= 58.0f) && sat > 0.25f ? port::kSillyShoes : -1;
    case Role::Poltergust: return (hue >= 330.0f || hue <= 28.0f) && sat > 0.4f ? port::kSillyPoltergust : -1;
    }
    return -1;
}

// `c` re-tinted to `target` at its brightness relative to the piece's mean brightness `ref`.
Rgb recolor(Rgb c, unsigned target, float ref) {
    Rgb t{float((target >> 16) & 0xFF), float((target >> 8) & 0xFF), float(target & 0xFF)};
    const float tl = luma(t);
    if (tl < 28.0f) t = {t.r + 28.0f - tl, t.g + 28.0f - tl, t.b + 28.0f - tl};  // black still shows shading
    const float f = luma(c) / std::max(ref, 1.0f);
    Rgb n{t.r * f, t.g * f, t.b * f};
    const float mx = std::max({n.r, n.g, n.b});
    if (mx > 255.0f) {
        // Too bright for the hue: towards white at the same brightness.
        const float y = std::min(luma(n), 255.0f);
        const float k = (mx - 255.0f) / std::max(mx - y, 1e-3f);
        n = {n.r + (y - n.r) * k, n.g + (y - n.g) * k, n.b + (y - n.b) * k};
    }
    return {std::clamp(n.r, 0.0f, 255.0f), std::clamp(n.g, 0.0f, 255.0f), std::clamp(n.b, 0.0f, 255.0f)};
}

std::vector<u8> recolorTexture(const u8* src, const KnownTex& k, const port::SillyLuigiSettings& s) {
    const u32 n = texBytes(k.mdlFmt, k.w, k.h);
    std::vector<u8> out(src, src + n);
    auto be16 = [](const u8* p) { return u16(p[0] << 8 | p[1]); };
    auto put16 = [](u8* p, u16 v) { p[0] = u8(v >> 8), p[1] = u8(v); };
    // Mean brightness of each piece's colours (CMPR: block endpoints).
    float sum[port::kSillyPieceCount] = {}, cnt[port::kSillyPieceCount] = {};
    const u32 step = k.mdlFmt == 10 ? 8 : 2;
    for (u32 o = 0; o < n; o += step)
        for (u32 e = 0; e < (k.mdlFmt == 10 ? 2u : 1u); e++) {
            const Rgb c = from565(be16(src + o + 2 * e));
            const int p = classify(c, k.role);
            if (p >= 0) sum[p] += luma(c), cnt[p] += 1.0f;
        }
    float ref[port::kSillyPieceCount];
    for (int p = 0; p < port::kSillyPieceCount; p++) ref[p] = cnt[p] > 0 ? sum[p] / cnt[p] : 128.0f;
    auto apply = [&](u16 v) -> u16 {
        const Rgb c = from565(v);
        const int p = classify(c, k.role);
        if (p < 0 || !s.colorOn[p]) return v;
        return to565(recolor(c, s.colorRgb[p], ref[p]));
    };
    if (k.mdlFmt == 7) {
        for (u32 o = 0; o < n; o += 2) put16(&out[o], apply(be16(src + o)));
        return out;
    }
    for (u32 o = 0; o < n; o += 8) {
        const u16 c0 = be16(src + o), c1 = be16(src + o + 2);
        u16 n0 = apply(c0), n1 = apply(c1);
        u32 idx = u32(src[o + 4]) << 24 | u32(src[o + 5]) << 16 | u32(src[o + 6]) << 8 | src[o + 7];
        if (c0 > c1) {
            // Four-colour block: keep c0 > c1 (swapping the endpoints swaps index pairs 0/1, 2/3).
            if (n0 < n1) std::swap(n0, n1), idx ^= 0x55555555u;
            if (n0 == n1) {
                if (n0 < 0xFFFF) n0++;
                else n1--;
            }
        } else if (n0 > n1) {
            // Three colours + transparent: keep c0 <= c1 (indices 0 and 1 swap, 2 and 3 stay).
            std::swap(n0, n1);
            u32 r = 0;
            for (int i = 0; i < 16; i++) {
                u32 v = (idx >> (2 * i)) & 3;
                if (v < 2) v ^= 1;
                r |= v << (2 * i);
            }
            idx = r;
        }
        put16(&out[o], n0);
        put16(&out[o + 2], n1);
        out[o + 4] = u8(idx >> 24), out[o + 5] = u8(idx >> 16), out[o + 6] = u8(idx >> 8), out[o + 7] = u8(idx);
    }
    return out;
}

struct TexEntry {
    u32 sig[4];
    int known;  // index in kKnown, -1 = not one of them
};
std::unordered_map<u32, TexEntry> sTexAt;
struct Recolored {
    std::vector<u8> data;
    u32 lastFrame;
};
std::map<std::pair<int, u64>, Recolored> sRecolored;

bool colorsActive() {
    const auto& s = cfg();
    for (bool on : s.colorOn)
        if (on) return true;
    return false;
}

}  // namespace

// Joints of Luigi's models, before the skinning matrices are built from them (see the top).
namespace {
// Registered at static init (any order: function-local), read on the guest thread only.
std::vector<JointScaler>& scalers() {
    static std::vector<JointScaler> sList;
    return sList;
}

// Per model instance: a hash of all its joint matrices as we left them. Equal at the next
// fn_80058850 = the joints were not recomputed since (a second skinning pass without new
// animation, e.g. the model viewer's bind pose): changing them again would apply every change
// twice. (All joints: a head-only change leaves joints 0 and the last one as they were.)
struct Stamp {
    u32 mdl = 0;
    u64 hash = 0;
};
std::unordered_map<u32, Stamp> sStamps;
u64 jointHash(u32 joints, u32 nj) {
    const u8* p = static_cast<const u8*>(hostp(joints));
    u64 h = 0xCBF29CE484222325ull;
    for (u32 i = 0; i < 0x30 * nj; i += 8) {
        u64 w;
        std::memcpy(&w, p + i, 8);
        h = (h ^ w) * 0x100000001B3ull;
        h ^= h >> 29;
    }
    return h;
}
}  // namespace

void addJointScaler(JointScaler fn) {
    if (fn) scalers().push_back(fn);
}

HLE(fn_80058850) {
    const bool luigiMods = jointModsActive();
    if ((luigiMods || !scalers().empty()) && !viewer::isOpen()) {
        const u32 obj = c.r[3];
        const u32 mdl = inGuest(obj) ? rd32(obj + 4) : 0, joints = inGuest(obj) ? rd32(obj + 0x14) : 0;
        const u32 nj = inGuest(mdl) ? rd16(mdl + 8) : 0;
        if (inGuest(mdl) && inGuest(joints) && nj > 0 && nj <= 512) {
            if (sStamps.size() > 1024) sStamps.clear();
            Stamp& st = sStamps[obj];
            if (st.mdl != mdl || jointHash(joints, nj) != st.hash) {
                if (luigiMods) modifyJoints(obj);
                for (JointScaler fn : scalers()) fn(c, obj, mdl, joints, nj);
                st.mdl = mdl;
                st.hash = jointHash(joints, nj);
            }
        }
    }
    callOriginal(c, "fn_80058850");
}

const void* texOverride(u32 image, u32 width, u32 height, u32 gxFormat) {
    if (!colorsActive() || viewer::isOpen()) return nullptr;
    u8 mdlFmt;
    if (gxFormat == 0xE) mdlFmt = 10;       // GX_TF_CMPR
    else if (gxFormat == 0x4) mdlFmt = 7;   // GX_TF_RGB565
    else return nullptr;
    bool sizeMatch = false;
    for (const KnownTex& k : kKnown) sizeMatch |= k.mdlFmt == mdlFmt && k.w == width && k.h == height;
    if (!sizeMatch || !inGuest(image)) return nullptr;
    const u32 bytes = texBytes(mdlFmt, width, height);
    const u8* src = static_cast<const u8*>(hostp(image));
    u32 sig[4];
    for (int i = 0; i < 4; i++) std::memcpy(&sig[i], src + (bytes / 4) * u32(i) + (bytes / 8), 4);
    auto it = sTexAt.find(image);
    if (it == sTexAt.end() || std::memcmp(it->second.sig, sig, sizeof sig)) {
        if (sTexAt.size() > 4096) sTexAt.clear();
        const u64 h = fnv(src, bytes);
        int known = -1;
        for (int i = 0; i < int(std::size(kKnown)); i++)
            if (kKnown[i].hash == h && kKnown[i].mdlFmt == mdlFmt && kKnown[i].w == width && kKnown[i].h == height) known = i;
        TexEntry e;
        std::memcpy(e.sig, sig, sizeof sig);
        e.known = known;
        it = sTexAt.insert_or_assign(image, e).first;
    }
    const int known = it->second.known;
    if (known < 0) return nullptr;
    const auto& s = cfg();
    // Key: the colours of the pieces this texture can hold.
    u64 key = 0;
    for (int p = 0; p < port::kSillyPieceCount; p++) {
        const bool on = s.colorOn[p];
        key = key * 0x1000193ull ^ (on ? (u64(s.colorRgb[p]) | 1ull << 24) : 0ull) ^ u64(p) << 40;
    }
    const u32 frame = frame::guestFrames();
    auto r = sRecolored.find({known, key});
    if (r == sRecolored.end()) {
        // Drop copies unused for a while (Aurora has long finished with them: it hashes the texels
        // when the frame is recorded, and a replayed frame is at most a few frames old).
        if (sRecolored.size() > 48)
            for (auto i = sRecolored.begin(); i != sRecolored.end();)
                i = frame - i->second.lastFrame > 600 ? sRecolored.erase(i) : std::next(i);
        r = sRecolored.emplace(std::make_pair(known, key), Recolored{recolorTexture(src, kKnown[known], s), frame}).first;
        static int sLogged = 0;
        if (sLogged++ < 16) log("[silly] recoloured texture %08X (%ux%u, known %d)", image, width, height, known);
    }
    r->second.lastFrame = frame;
    return r->second.data.data();
}

// ---- menu --------------------------------------------------------------------------------------

namespace {
void tip(const char* t) { recomp::menu::ItemTooltip("%s", t); }

struct Preset {
    const char* name;
    unsigned rgb;
};
constexpr Preset kClothPresets[] = {
    {"Luigi green", 0x3CA03C}, {"Mario red", 0xE01818}, {"Wario yellow", 0xF0D020}, {"Waluigi purple", 0x6A2C9A},
    {"Overalls blue", 0x2848C8}, {"White", 0xF4F4F4}, {"Black", 0x202020}, {"Pink", 0xF070B0},
    {"Orange", 0xF08020}, {"Brown", 0x6A3A1E}, {"Gold", 0xD4AF37},
};
constexpr Preset kPoltergustPresets[] = {
    {"Gold", 0xD4AF37}, {"Chrome", 0xC8CCD4}, {"Luigi green", 0x3CA03C}, {"Blue", 0x2050D0},
    {"Purple", 0x7A3CC0}, {"Black", 0x303030}, {"Pink", 0xF070B0}, {"Mario red", 0xE01818},
};

bool pieceRow(const char* label, int piece, const Preset* presets, int presetCount, const char* tooltip) {
    auto& s = port::settings().sillyLuigi;
    bool save = false;
    ImGui::PushID(label);
    save |= menu::Checkbox(label, &s.colorOn[piece]);
    tip(tooltip);
    ImGui::SameLine(ImGui::GetFontSize() * 9.0f);
    ImGui::BeginDisabled(!s.colorOn[piece]);
    float col[3] = {float((s.colorRgb[piece] >> 16) & 0xFF) / 255.0f, float((s.colorRgb[piece] >> 8) & 0xFF) / 255.0f,
                    float(s.colorRgb[piece] & 0xFF) / 255.0f};
    if (ImGui::ColorEdit3("##color", col, ImGuiColorEditFlags_NoInputs)) {
        s.colorRgb[piece] = unsigned(std::lround(col[0] * 255.0f)) << 16 | unsigned(std::lround(col[1] * 255.0f)) << 8 |
                            unsigned(std::lround(col[2] * 255.0f));
    }
    if (ImGui::IsItemDeactivatedAfterEdit()) save = true;
    ImGui::SameLine();
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 9.0f);
    if (ImGui::BeginCombo("##preset", "Preset")) {
        for (int i = 0; i < presetCount; i++)
            if (ImGui::Selectable(presets[i].name)) {
                s.colorRgb[piece] = presets[i].rgb;
                save = true;
            }
        ImGui::EndCombo();
    }
    ImGui::EndDisabled();
    ImGui::PopID();
    return save;
}
}  // namespace

void drawLuigiOptions() {
    applyEnvOnce();
    auto& s = port::settings().sillyLuigi;
    bool save = false;
    recomp::menu::SeparatorText("Luigi");
    menu::SliderFloat("Head size", &s.headScale, 0.25f, 3.0f, "%.2fx");
    save |= ImGui::IsItemDeactivatedAfterEdit();
    tip("Scales Luigi's head (with his cap, face and mustache) about his neck, in gameplay\n"
        "and cutscenes. His body, the camera and the game are unchanged. 1.00x = original.");
    menu::SliderFloat("Body size", &s.bodyScale, 0.25f, 3.0f, "%.2fx");
    save |= ImGui::IsItemDeactivatedAfterEdit();
    tip("Tiny or giant Luigi: his whole model (and what he carries) scaled about his feet.\n"
        "Visual only: the camera, collision and hit boxes stay the original size.\n"
        "1.00x = original.");
    save |= menu::Checkbox("Wobbly head", &s.wobble);
    tip("Luigi's head sits on a spring: it lags, tilts and bounces when he starts, stops,\n"
        "turns or gets thrown around. Standing still it settles.");
    ImGui::BeginDisabled(!s.wobble);
    ImGui::Indent();
    menu::SliderFloat("Wobble strength", &s.wobbleStrength, 0.25f, 3.0f, "%.2f");
    save |= ImGui::IsItemDeactivatedAfterEdit();
    tip("How far the head swings.");
    ImGui::Unindent();
    ImGui::EndDisabled();
    save |= menu::Checkbox("Paper Luigi", &s.paper);
    tip("Paper Mario style: Luigi is flattened to a sheet along the direction he faces.\n"
        "From the front or back he looks normal; when he turns sideways he is paper thin.");
    if (s.headScale != 1.0f || s.bodyScale != 1.0f) {
        if (menu::SmallButton("Original sizes")) s.headScale = s.bodyScale = 1.0f, save = true;
    }

    recomp::menu::SeparatorText("Luigi's clothes");
    ImGui::TextUnformatted("Outfit:");
    for (const Outfit& o : kOutfits) {
        ImGui::SameLine();
        if (menu::SmallButton(o.name)) {
            for (int i = 0; i < 4; i++) {
                s.colorOn[i] = o.rgb[i] != kOrig;
                if (o.rgb[i] != kOrig) s.colorRgb[i] = o.rgb[i];
            }
            save = true;
        }
    }
    tip("Sets all four pieces at once (Classic = Luigi's own colours).");
    save |= pieceRow("Cap", port::kSillyHat, kClothPresets, int(std::size(kClothPresets)),
                     "Recolours Luigi's green cap. The white \"L\" emblem stays. The shading is kept:\n"
                     "the picked colour is the cap's average, lighter and darker where it was.");
    save |= pieceRow("Shirt", port::kSillyShirt, kClothPresets, int(std::size(kClothPresets)),
                     "Recolours the green shirt and sleeves (shading kept).");
    save |= pieceRow("Overalls", port::kSillyOveralls, kClothPresets, int(std::size(kClothPresets)),
                     "Recolours the blue overalls (shading kept). The yellow buttons stay.");
    save |= pieceRow("Shoes", port::kSillyShoes, kClothPresets, int(std::size(kClothPresets)),
                     "Recolours the brown shoes (shading kept).");
    recomp::menu::SeparatorText("Poltergust 3000");
    save |= pieceRow("Poltergust", port::kSillyPoltergust, kPoltergustPresets, int(std::size(kPoltergustPresets)),
                     "Recolours the red tank of the Poltergust 3000 (shading kept). The nozzle,\n"
                     "hose and metal parts stay.");
    if (save) port::saveSettings();
}

}  // namespace recomp::silly
