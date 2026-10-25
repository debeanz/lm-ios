#include "recomp/widescreen.h"

#include "mobile/mobile.h"
#include "port/install.h"
#include "port/options.h"
#include "port/settings.h"
#include "recomp/hle.h"
#include "recomp/interp.h"
#include "recomp/version.h"
#include "recomp/coop.h"
#include "recomp/guest_util.h"

#include <dolphin/gx/GXAurora.h>

#include <algorithm>
#include <atomic>

namespace recomp {
u32 copyDispCount();  // hle_gx.cpp
namespace tags {
bool textBoxShown();  // tags.cpp: a message window is on screen
int textMode();       // tags.cpp: 2 text boxes blended as one layout, 1 locked to the game frame, 0 neither
}
}
#include <cmath>
#include <vector>

namespace recomp::ws {
namespace {

constexpr float kBaseAspect = 4.0f / 3.0f;
std::atomic<float> sFactor{1.0f};
std::atomic<float> sFov{1.0f};
std::atomic<bool> sAnchorHud{false};
bool sOriginal = false;  // 4:3 mode: Aurora fits the 4:3 frame into the window
float sContentAspect = 0.0f;  // phone Fit with another fixed aspect (AuroraSetContentAspect)
bool sPolicySet = false;

float factorFor(float aspect) { return aspect > kBaseAspect ? kBaseAspect / aspect : 1.0f; }

}  // namespace

// --aspect on the command line wins; otherwise the Visuals tab's setting (live), or on a phone the
// iPhone tab's Aspect Ratio (src/mobile).
float configuredAspect() {
    if (port::options().aspectRatio > 0.0f) return port::options().aspectRatio;
    if (mobile::enabled()) return mobile::targetAspect();
    switch (port::settings().video.aspect) {
    case port::AspectMode::Original4x3: return kBaseAspect;
    case port::AspectMode::Wide16x9: return 16.0f / 9.0f;
    case port::AspectMode::Wide16x10: return 16.0f / 10.0f;
    case port::AspectMode::Ultra21x9: return 21.0f / 9.0f;
    case port::AspectMode::Ultra32x9: return 32.0f / 9.0f;
    default: return 0.0f;  // match the window
    }
}

void applyPolicy(float aspect) {
    // Phone Screen Scaling (src/mobile): Fit keeps any fixed aspect with bars around it (Aurora 0239),
    // Stretch fills the panel, also with 4:3.
    const bool phone = mobile::enabled();
    const bool phoneFit = phone && aspect > 0.0f && mobile::fitScaling();
    const bool original = aspect > 0.0f && aspect <= kBaseAspect + 1e-3f && (!phone || phoneFit);
    const float content = phoneFit && !original ? aspect : 0.0f;
    if (original != sOriginal || content != sContentAspect || !sPolicySet) {
        sOriginal = original;
        sContentAspect = content;
        sPolicySet = true;
        AuroraSetContentAspect(content);
        AuroraSetViewportPolicy(sOriginal || content > 0.0f ? AURORA_VIEWPORT_FIT : AURORA_VIEWPORT_STRETCH);
        log("[ws] aspect %s%s", sOriginal ? "4:3 (original)" : aspect > 0.0f ? "fixed" : "auto (window)",
            content > 0.0f ? " (fit, bars around the image)" : "");
    }
}

void init() { applyPolicy(configuredAspect()); }

void beginFrame() {
    const float aspect = configuredAspect();
    applyPolicy(aspect);
    float f = 1.0f;
    if (!sOriginal) {
        if (aspect > 0.0f) {
            f = factorFor(aspect);
        } else {
            u32 w = 0, h = 0;
            AuroraGetRenderSize(&w, &h);
            if (w && h) f = factorFor(float(w) / float(h));
        }
    }
    const float old = sFactor.exchange(f);
    if (old != f) log("[ws] widescreen factor %.4f (aspect %.4f)", f, kBaseAspect / f);
    sFov = std::clamp(port::settings().video.fovScale, 0.5f, 2.0f);
    sAnchorHud = !port::settings().video.hudKeep4x3;
}

float fovRatio(float m11) {
    const float k = sFov.load(std::memory_order_relaxed);
    if (k == 1.0f || m11 <= 0.0f) return 1.0f;
    const float half = std::atan(1.0f / m11);
    const float scaled = std::clamp(half * k, 0.01f, 1.5f);
    return std::tan(half) / std::tan(scaled);
}

bool adjusting() { return active() || sFov.load(std::memory_order_relaxed) != 1.0f; }

namespace {
bool sShadowLightSetup = false;
}  // namespace
void setShadowLightSetup(bool on) { sShadowLightSetup = on; }


float factor() { return sFactor.load(std::memory_order_relaxed); }

Background backgroundMode() { return Background(port::options().wsBackgrounds); }

// fn_80008D58(camera, top, bottom, left, right, near, far) builds the camera's
// four side culling planes. All 3D projection setters (fn_80008654,
// fn_800087D0, fn_80008928) end with it, so widening left/right here keeps the
// culling volume in step with the Hor+ projection from the GX HLE.
// The field-of-view setting scales all four sides by the same ratio as the
// projection (from the camera's fovY at +0x90, degrees).
HLE(fn_80008D58) {
    const u32 cam = c.r[3];
    // Viewport x and width (the European camera has 12 more bytes before them; fovY is unmoved).
    const u32 vpX = rd32(cam + VOFF(0x1EC, 0x1F8)), vpW = rd32(cam + VOFF(0x1F4, 0x200));
    const bool fullScreen = (vpX == 0 && vpW >= 640) || splitHalf(f32(vpX), f32(vpW));
    if (adjusting() && fullScreen && !sShadowLightSetup) {
        const float fovY = rdf32(cam + 0x90) * 3.14159265f / 180.0f;
        const double sides = fovY > 0.0f ? 1.0 / fovRatio(1.0f / std::tan(fovY * 0.5f)) : 1.0;
        const double inv = sides / factor();
        c.setSingle(1, float(c.f[1] * sides));
        c.setSingle(2, float(c.f[2] * sides));
        c.setSingle(3, float(c.f[3] * inv));
        c.setSingle(4, float(c.f[4] * inv));
    }
    callOriginal(c, "fn_80008D58");
}


// HUD anchoring (widescreen.h, AnchorScope). The in-game HUD is drawn once per frame by
// fn_8003E3D0, which calls one function per element; those are hooked here, so whatever
// they draw (J2DScreens, panes, pictures, text, the GBH's screen) moves as one unit.
// Elements not listed (the element meter, the ghost counters) stay centered with the
// dialogue and menus. The guest thread is the only caller.
namespace {
int sAnchorStack[16];
int sAnchorDepth = 0;
int sRttDepth = 0;  // RenderToTextureScope nesting
int sShift[8];
int sShiftDepth = 0;
}  // namespace

ShiftScope::ShiftScope(int dx) {
    if (sShiftDepth < int(std::size(sShift))) sShift[sShiftDepth] = dx;
    sShiftDepth++;
}
ShiftScope::~ShiftScope() { sShiftDepth--; }
int hudShift() { return sShiftDepth > 0 ? sShift[std::min(sShiftDepth, int(std::size(sShift))) - 1] : 0; }

bool splitHalf(float x, float w) {
    return coop::fixes() && std::fabs(w - 320.0f) < 1.0f && (std::fabs(x) < 1.0f || std::fabs(x - 320.0f) < 1.0f);
}

AnchorScope::AnchorScope(int anchor) {
    if (sAnchorDepth < int(std::size(sAnchorStack))) sAnchorStack[sAnchorDepth] = anchor;
    sAnchorDepth++;
}
AnchorScope::~AnchorScope() { sAnchorDepth--; }

int hudAnchor() {
    if (hudShift() != 0) return 0;  // a half of the split screen: centered, then moved
    if (sAnchorDepth <= 0 || !sAnchorHud.load(std::memory_order_relaxed) || !active()) return 0;
    return sAnchorStack[std::min(sAnchorDepth, int(std::size(sAnchorStack))) - 1];
}

bool inHudScope() {
    return sRttDepth == 0 && ((sAnchorDepth > 0 && active()) || hudShift() != 0);
}

RenderToTextureScope::RenderToTextureScope() { sRttDepth++; }
RenderToTextureScope::~RenderToTextureScope() { sRttDepth--; }

// Game Boy Horror (bottom right): frame, LEDs and the J2DScreen of whatever its screen
// shows (money tally, E. Gadd's call, the cut E3 clock).
// Frame interpolation: the GBH is one 2D layout (interp::TextGroupScope, oneLayout; tags.cpp
// fn_80044138, docs/INTERPOLATION.md "Text boxes at 120"): everything it draws, the static noise over
// its screen (fn_8003BE9C) and, during E. Gadd's call, his portrait's viewport and scissor box
// (fn_80114E18) follow its first frame piece, so the picture on its screen never slides against its
// frame (as the call starts it rises into place by 1-3 units per game frame). (LM_TEXTBOX_LOCK=1:
// shown at the game frame while a text box is up instead, the earlier behaviour.)
HLE(fn_8003D634) {
    AnchorScope s(1);
    ShiftScope half(coop::gbhShift());  // 2-player split screen: in player 1's half
    const bool call = interp::capturing() && tags::textBoxShown();
    const int mode = tags::textMode();
    interp::tagLogMark("gbh begin");
    // (Always one layout, not only during the call: entering the call it moved 3 units in the game
    // frame its group started, with no earlier group to blend from.)
    const bool group = interp::capturing() && mode == 2;
    if (call && mode == 1) interp::pushFrameLock();
    if (group) interp::pushTextGroup(0x8003D634u, true);
    callOriginal(c, "fn_8003D634");
    if (group) interp::popTextGroup();
    if (call && mode == 1) interp::popFrameLock();
    interp::tagLogMark("gbh end");
}
// The static noise drawn over the GBH screen (gbnoise01..03), from the HUD and from the
// GBH view's own pass (fn_8003E7B0(1)).
HLE(fn_8003BE9C) {
    AnchorScope s(1);
    ShiftScope half(coop::gbhShift());
    // The noise as E. Gadd's call connects belongs to his call window (fn_8003D634 above).
    const bool call = interp::capturing() && tags::textBoxShown();
    const int mode = tags::textMode();
    interp::tagLogMark("gbhnoise begin");
    const bool group = interp::capturing() && mode == 2;
    if (call && mode == 1) interp::pushFrameLock();
    if (group) interp::pushTextGroupJoin(0x8003D634u);
    callOriginal(c, "fn_8003BE9C");
    if (group) interp::popTextGroup();
    if (call && mode == 1) interp::popFrameLock();
    interp::tagLogMark("gbhnoise end");
}
// Sets up view n of the frame loop: its viewport/scissor (and for view 1 the depth-clear
// quad, fn_8000B228) are set here, and the view's draws that follow use them. View 1 is the
// GBH screen's live 3D view (unused camera mode). View 0 is the main view; it is smaller
// than the screen while the GBH frame surrounds it (r13+0x200, fn_80010F20), and then
// follows the centered frame. In the unused 2-player mode (coop.cpp) view 1 is player 2's
// half of the split screen, which spans the full width like view 0; with the co-op
// improvements the views are the side-by-side halves, drawn Hor+ in place (ws::splitHalf), so
// they are not in a HUD scope at all.
namespace {
bool sMainViewSetup = false;
}
bool inMainViewSetup() { return sMainViewSetup; }
HLE(fn_800115F8) {
    if (coop::fixes() && !coop::gbhView()) return callOriginal(c, "fn_800115F8");
    AnchorScope s(c.r[3] == 1 && !coop::active() ? 1 : 0);
    sMainViewSetup = c.r[3] == 0 && !coop::active();
    callOriginal(c, "fn_800115F8");
    sMainViewSetup = false;
}
// Event portrait renderer (<PDEMO>): 3D drawn into its own viewport. During E. Gadd's call
// (GBH screen mode lbl_804D846C = 3) that viewport is the GBH screen.
HLE(fn_80114E18) {
    // Frame interpolation: event scenes have their own camera (interp.h TagCat::SubView).
    interp::TagScope subView(interp::TagCat::SubView, VA(0x80114E18), 0, c.r[3]);
    // The call portrait keeps the GBH's anchoring while it closes: the GBH leaves call mode (3)
    // before the portrait's last frames are drawn, and centering those jumped the portrait to the
    // left at 16:9. The anchoring is kept while the portrait is drawn on consecutive game frames
    // after the call (a door close-up later starts centered).

    static u32 sLastFrame = ~0u;
    static bool sCallAnchored = false;
    const u32 frame = ::recomp::copyDispCount();
    const bool inCall = rd32(VA(0x804D846C)) == 3;
    if (inCall) sCallAnchored = true;
    else if (sLastFrame == ~0u || (frame != sLastFrame && frame != sLastFrame + 1)) sCallAnchored = false;
    sLastFrame = frame;
    AnchorScope s(sCallAnchored ? 1 : 0);
    // A portrait shown with a message window (E. Gadd's on the GBH during his call) belongs to the
    // text box: its own 3D scene interpolates (SubView), while its viewport and scissor box follow
    // the GBH's layout (fn_8003D634 above), so it moves with the bezel around it.
    // (LM_TEXTBOX_LOCK=1: shown at the game frame instead.)
    const bool text = interp::capturing() && tags::textBoxShown();
    const bool lock = text && tags::textMode() == 1;
    const bool join = text && tags::textMode() == 2 && sCallAnchored;
    interp::tagLogMark("portrait begin");
    if (lock) interp::pushFrameLock();
    if (join) interp::pushTextViewJoin(0x8003D634u);
    callOriginal(c, "fn_80114E18");
    if (join) interp::popTextViewJoin();
    if (lock) interp::popFrameLock();
    interp::tagLogMark("portrait end");
}
// The event scene's render-to-texture pass, called by fn_80114E18 between the scene and the
// portrait draws: when the scene defines a texture target (door scenes: the reflection of
// Luigi's hand on the brass knob) it sets the camera viewport to (0,0,w,h) (fn_8011548C), clears
// that rectangle with a copy, draws the scene's models and copies (0,0,w,h) into the target
// (fn_801168A8). Inside fn_80114E18's scope that viewport used to follow the centered HUD
// (moved right and narrowed in widescreen) while the copies kept the game's rectangle: the
// knob sampled a cleared texture, and the pass stayed on screen above-left of the knob, its
// depth hiding the door there.
HLE(fn_801142C4) {
    RenderToTextureScope rtt;
    callOriginal(c, "fn_801142C4");
}
// Game Boy Horror map screen (the frame loop's scene for it): the map is drawn into a viewport
// over the GBH's screen (112,84 416x312, fn_8002AC8C), which the bezel overlaps in 4:3. Left
// plain, that viewport is stretched across the wide frame and the map shows beside the
// centered bezel; inside a (centered) scope it is placed like the picture-in-picture views.
HLE(fn_8000C598) {
    AnchorScope s(0);
    callOriginal(c, "fn_8000C598");
}
// Room name when entering a room: the floor (top right) and the room's name (top left).
// Frame interpolation: text, shown at the game frame like every text box (tags.cpp fn_80044138).
HLE(fn_80045BDC) {
    AnchorScope s(kAnchorBySide);
    const int mode = tags::textMode();
    interp::tagLogMark("roomname begin");
    if (mode == 1) interp::pushFrameLock();
    if (mode == 2) interp::pushTextGroup(0x80045BDCu);
    callOriginal(c, "fn_80045BDC");
    if (mode == 2) interp::popTextGroup();
    if (mode == 1) interp::popFrameLock();
    interp::tagLogMark("roomname end");
}
// ---- widened 2D masks (widescreen.h) ----
namespace {
int sWipeDepth = 0;
int sAccountDepth = 0;  // inside fn_80050524 (the area-clear / result screens)
int sEdgeDepth = 0;     // drawing the area-clear mask screen: pictures reach the display's edges

// Logical pixels between the 4:3 frame's edge and the display's edge (plus a little overlap).
s32 sideExtent() { return s32(std::ceil(320.0f / factor() - 320.0f)) + 2; }
}  // namespace
WipeScope::WipeScope() { sWipeDepth++; }
WipeScope::~WipeScope() { sWipeDepth--; }
bool inWipe() { return sWipeDepth > 0 && active(); }

// Result screens after the Portrificationizer (event 02's <ACCOUNT>: E. Gadd's lab, map 4):
// "Now you've caught: (Area n)" with Luigi's flashlight on a yellow wall, "You've caught a total
// of ...", "You've found:" with the money list. Drawn each frame by this function as 2D in the
// 640-wide frame, with Luigi as 3D on top (account object r3, state at +1):
//   - state 0: the iris wipe from the lab, four J2DFillBox boxes around the /timg/ad_wipe_0
//     circle picture, as in fn_80055C54 (WipeScope);
//   - states 3..7: a black J2DFillBox, then three J2DScreens from lbl_803CFA50: +8 the wall
//     (a 1280-wide tiled picture scrolled by the game, so it runs past the 4:3 frame's right
//     edge), +0 the black mask (a tiled picture over x 0..240 and the round spotlight texture in
//     two halves over 240..642) with the text, +4 the money list (behind a fading fill).
// Hor+ showed the wall beside the mask, and in the wipe the lab beside the circle: the mask only
// covered the 4:3 frame. Inside the mask screen the pictures touching the 4:3 frame's edges are
// extended to the display's edges (J2DPicture::drawTexCoord below), so the black surround covers
// any width while the spotlight and the text stay where they are.
HLE(fn_80050524) {
    const bool wipe = rd8(c.r[3] + 1) == 0;
    // Frame interpolation: all of this 2D is shown at its game frame (as the text boxes, tags.cpp).
    // Blended separately, the wipe's circle and its four boxes did not meet in the in-between
    // frames: a line of the lab showed under the circle while it closed.
    interp::FrameLockScope frameLock;
    sAccountDepth++;
    if (wipe) sWipeDepth++;
    callOriginal(c, "fn_80050524");
    if (wipe) sWipeDepth--;
    sAccountDepth--;
}

// Luigi's eyes on the result screens. The 3D Luigi there is a second Luigi S3DModel (not the player's),
// drawn by the account frame loop (fn_8000F1F8: the actors drawn after the 2D, fn_80059C54
// called from 0x8000F370). Nothing advances his texture pattern (usual_bk.txp, the blink: it
// writes the eye sampler of materials 4 and 6) while these screens run, so his eyes keep the
// pattern frame they had when the screens began. Frames 61..66 of the blink's 96 are the
// half-closed (sampler 15) and closed (16, plain skin: no eyes at all) lids; when the event ended
// there, Luigi had no eyes for the whole sequence (it depends on when the last dialogue was
// advanced; presumably the same on the console). Before his draw the pattern is advanced once per
// game frame with the game's own update (fn_8005C9E4, what the actor draw pass does in gameplay),
// so he keeps blinking and opens his eyes again.
// Test aids: LM_ACCT_BLINK=0 (frozen, as the game does), LM_ACCT_EYEFRAME=<f> (set the pattern
// frame when the screens start, e.g. 63 for the closed lids).
HLE(fn_80059C54) {
    if (c.lr == VA(0x8000F374)) {
        static const bool sBlink = [] { const char* e = std::getenv("LM_ACCT_BLINK"); return !(e && e[0] == '0'); }();
        static const char* const sStart = std::getenv("LM_ACCT_EYEFRAME");
        static u32 sModel = 0, sFrame = ~0u;
        const u32 model = c.r[3], frame = ::recomp::copyDispCount();
        if (rd32(model + 0x2C) != 0 && !(model == sModel && frame == sFrame)) {
            if (sStart && sModel != model) {
                wrf32(model + 0x30, float(std::atof(sStart)));
                gu::call(c, VA(0x8005CBBC), {model});  // apply without advancing
            }
            if (sBlink) gu::call(c, VA(0x8005C9E4), {model});
            sModel = model;
            sFrame = frame;
        }
    }
    callOriginal(c, "fn_80059C54");
}

// J2DScreen::draw(screen, x, y, graf): marks the area-clear mask screen (above), and anchors
// the Hidden Mansion star.
// The star (/kawano/base/blo/star_1.blo, picture "star" at 609,421 24x24: just right of the GBH
// frame, bottom right) is its own J2DScreen (lbl_803CE3B0+0xC), shown in the Hidden Mansion
// (fn_8003DF88 makes its ROOT pane visible) and drawn by fn_8003E3D0 itself (call at 0x8003E774,
// PAL 0x8003FDF4), not by the GBH element function, so it stayed centered while the GBH moved to
// the right edge: at 16:9 it sat on the GBH screen over the gold bar. It gets the GBH's anchor
// (and its split-screen half), so it keeps its place beside the GBH at every aspect; 4:3 and
// "Keep HUD at 4:3" are unchanged (no anchoring there, like the GBH).
HLE(draw__9J2DScreenFiiPC14J2DGrafContext) {
    const bool mask = sAccountDepth > 0 && active() && c.r[3] == rd32(VA(0x803CFA50));
    const bool hmStar = c.lr == VA(0x8003E778);
    if (mask) sEdgeDepth++;
    if (hmStar) {
        AnchorScope s(1);
        ShiftScope half(coop::gbhShift());
        callOriginal(c, "draw__9J2DScreenFiiPC14J2DGrafContext");
    } else {
        callOriginal(c, "draw__9J2DScreenFiiPC14J2DGrafContext");
    }
    if (mask) sEdgeDepth--;
}

// J2DPicture::drawTexCoord(this, x, y, w, h, s0, t0, s1, t1, s3, t3, s2, t2, mtx): one quad,
// corners (x,y) (x+w,y) (x+w,y+h) (x,y+h) with texcoords f1/f2, f3/f4, f7/f8, f5/f6, placed by
// mtx * the pane's matrix (+0x84). In the mask screen, a quad whose screen-space side lies on
// the 4:3 frame's left or right edge (or beyond it) is lengthened to the display's edge, its
// texture coordinates extrapolated along the quad: a tiled (repeat) texture keeps tiling and a
// clamped one repeats its edge texels, i.e. the black graphic itself continues to the edge.
HLE(drawTexCoord__10J2DPictureFiiiiffffffffPA3_A4_f) {
    if (sEdgeDepth > 0 && active()) {
        const u32 pane = c.r[3], mtx = c.r[8];
        const s32 x = s32(s16(c.r[4])), w = s32(s16(c.r[6]));
        // x row of mtx * pane matrix
        f32 a[4], b[3][4];
        for (int j = 0; j < 4; j++) a[j] = rdf32(mtx + 4 * j);
        for (int i = 0; i < 3; i++)
            for (int j = 0; j < 4; j++) b[i][j] = rdf32(pane + 0x84 + 16 * i + 4 * j);
        const f32 m00 = a[0] * b[0][0] + a[1] * b[1][0] + a[2] * b[2][0];
        const f32 m01 = a[0] * b[0][1] + a[1] * b[1][1] + a[2] * b[2][1];
        const f32 m03 = a[0] * b[0][3] + a[1] * b[1][3] + a[2] * b[2][3] + a[3];
        if (w > 0 && m00 > 1e-3f && std::fabs(m01) < 1e-4f) {
            const f32 left = m00 * f32(x) + m03, right = m00 * f32(x + w) + m03;
            const f32 ext = f32(sideExtent());
            s32 dl = 0, dr = 0;  // local units added on each side
            if (left <= 0.5f) dl = s32(std::ceil((left + ext) / m00));
            if (right >= 640.0f - 0.5f) dr = s32(std::ceil((640.0f + ext - right) / m00));
            if (dl > 0 || dr > 0) {
                f32 st[8];
                for (int i = 0; i < 8; i++) st[i] = f32(c.f[1 + i]);
                // per local unit along the top (v0 -> v1) and bottom (v3 -> v2) edges
                const f32 topS = (st[2] - st[0]) / f32(w), topT = (st[3] - st[1]) / f32(w);
                const f32 botS = (st[6] - st[4]) / f32(w), botT = (st[7] - st[5]) / f32(w);
                st[0] -= topS * f32(dl), st[1] -= topT * f32(dl);
                st[4] -= botS * f32(dl), st[5] -= botT * f32(dl);
                st[2] += topS * f32(dr), st[3] += topT * f32(dr);
                st[6] += botS * f32(dr), st[7] += botT * f32(dr);
                for (int i = 0; i < 8; i++) c.setSingle(1 + i, st[i]);
                c.r[4] = u32(x - dl);
                c.r[6] = u32(w + dl + dr);
            }
        }
    }
    callOriginal(c, "drawTexCoord__10J2DPictureFiiiiffffffffPA3_A4_f");
}

// J2DFillBox(x, y, w, h, color*): inside the iris wipe, a box touching the 4:3 frame's left or
// right edge (or lying beyond it: the side boxes get a negative width once the circle is wider
// than the screen) is extended to the display's edge. Widths stay as the game computed them
// relative to the edge it does not touch, so the hole is unchanged.
HLE(J2DFillBox__FiiiiQ28JUtility6TColor) {
    if (inWipe()) {
        const s32 ext = sideExtent();
        s32 x = s32(c.r[3]), w = s32(c.r[5]);
        const s32 right = x + w;
        if (x <= 0) {
            w = right + ext;
            x = -ext;
        }
        if (right >= 640) w = 640 + ext - x;
        c.r[3] = u32(x);
        c.r[5] = u32(w);
    }
    callOriginal(c, "J2DFillBox__FiiiiQ28JUtility6TColor");
}

// Health meter (bottom left). In the training room it shows over Luigi instead
// (fn_80041214, a projected position): that call opens a centered scope.
HLE(fn_800417F8) {
    AnchorScope s(-1);
    callOriginal(c, "fn_800417F8");
}

}  // namespace recomp::ws
