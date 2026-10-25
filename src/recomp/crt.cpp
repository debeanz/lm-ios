// CRT post-process filter. An original implementation (not derived from any existing
// shader's code) of the usual CRT-simulation model: the game frame is averaged down
// to the scanline grid of a 480-line NTSC set, each line is drawn as an electron beam
// whose width and profile depend on its brightness, the result is seen through a
// phosphor mask (aperture grille / slot mask / shadow mask) in output pixels, and
// light scattered in the glass (halation) and across the faceplate (diffusion/bloom)
// comes from small blurred copies. Runs through Aurora's postfx hook, so the ImGui
// overlay is drawn afterwards and stays sharp.
//
// Pass chain (texture index in parentheses; 0 is the game frame):
//   crt_linearize (1)  game frame -> lines x (lines*aspect), linear light, RGBA16F
//   crt_hsoft     (2)  horizontal beam softness (gaussian in game pixels)
//   crt_blur_h/v  (3,4) 1/2 res blur   -> halation
//   crt_blur_h/v  (5,6) 1/4 res blur
//   crt_blur_h/v  (7,8) 1/8 res blur   -> diffusion / bloom
//   crt_final          beams + convergence + mask + glow + geometry -> swapchain
// Everything but the final pass works on <= 480-line textures, so the cost at 4K is
// the final pass only (2-4 bilinear taps per channel plus the mask math).
#include "recomp/crt.h"

#include "mobile/mobile.h"
#include "recomp/gx_bridge.h"
#include "recomp/menu.h"

#include "port/settings.h"

#include <aurora/postfx.h>
#include <imgui.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace recomp::crt {
namespace {

constexpr const char* kShader = R"WGSL(
struct Crt {
    beam: vec4f,     // min sigma, max sigma, spot power, horizontal softness (game px)
    shape: vec4f,    // min shape, max shape, shape power, -
    mask: vec4f,     // type, triad size (px), strength, interlaced
    glow: vec4f,     // halation, diffusion, bloom, brightness
    conv_rg: vec4f,  // red x/y, green x/y (game px / lines)
    conv_b: vec4f,   // blue x/y, any convergence, -
    gamma: vec4f,    // gamma in, gamma out, curvature, corner radius
    dcopy: vec4f,    // display copy filter weights: line above, this line, line below; -
    dcopy2: vec4f,   // display copy gamma, EFB lines, -, -
    sig0: vec4f,     // signal (0 RGB, 1 S-Video, 2 composite, 3 RF), crosstalk, saturation, comb line step
    sig1: vec4f,     // field mod 4, separation (0 notch, 1 2-line comb, 2 3-line comb), 480i timing, field mod 65536
    sig2: vec4f,     // noise sigma, ghost gain, ghost delay (samples), line jitter (game px)
    sig3: vec4f,     // hum depth, hum phase, interference amplitude, interference phase
    sig4: vec4f,     // black level, gain, field parity (480i scan), -
    sigr: vec4f,     // kernel radii (taps each side, <= 16): encoder/RF, Y/C separation, chroma, luma
    k_y: array<vec4f, 9>,    // FIR taps -16..16 (index n + 16): encoder (+ RF tuner) luma
    k_ca: array<vec4f, 9>,   // encoder (+ RF tuner) chroma, in phase
    k_cb: array<vec4f, 9>,   // RF tuner chroma, quadrature (tuner response around the subcarrier)
    k_sep: array<vec4f, 9>,  // TV chroma band-pass (Y/C separation)
    k_trap: array<vec4f, 9>, // TV luma subcarrier trap, as the band-pass it removes (notch: narrower)
    k_dc: array<vec4f, 9>,   // TV chroma low-pass after demodulation (causal: colour trails to the right)
    k_dy: array<vec4f, 9>,   // TV luma low-pass + peaking
};
@group(0) @binding(1) var<uniform> crt: Crt;
@group(0) @binding(3) var tex0: texture_2d<f32>;
@group(0) @binding(4) var tex1: texture_2d<f32>;
@group(0) @binding(5) var tex2: texture_2d<f32>;

// ---- signal path (S-Video / composite / RF). The game frame is resampled to the NTSC line
// at 4 x the colour subcarrier (14.318 MHz, 4fsc: the subcarrier phase advances exactly 90 degrees
// per sample), one row per scanline, so every carrier is +-1 or 0 and the modulated chroma of
// any sample is +-I or +-Q. A 640-pixel game line lasts 47.4 us (13.5 MHz pixel clock), 679
// samples; wider (Hor+) frames get proportionally more samples, so bandwidths stay the same per
// screen width at any aspect. Values are gamma-encoded (video signals are), linear filters only.
fn kat(n: i32) -> vec2i {
    let i = n + 16;
    return vec2i(i >> 2, i & 3);
}
fn tap_y(n: i32) -> f32 { let i = kat(n); return crt.k_y[i.x][i.y]; }
fn tap_ca(n: i32) -> f32 { let i = kat(n); return crt.k_ca[i.x][i.y]; }
fn tap_cb(n: i32) -> f32 { let i = kat(n); return crt.k_cb[i.x][i.y]; }
fn tap_sep(n: i32) -> f32 { let i = kat(n); return crt.k_sep[i.x][i.y]; }
fn tap_trap(n: i32) -> f32 { let i = kat(n); return crt.k_trap[i.x][i.y]; }
fn tap_dc(n: i32) -> f32 { let i = kat(n); return crt.k_dc[i.x][i.y]; }
fn tap_dy(n: i32) -> f32 { let i = kat(n); return crt.k_dy[i.x][i.y]; }

fn rgb_to_yiq(c: vec3f) -> vec3f {
    return vec3f(dot(c, vec3f(0.299, 0.587, 0.114)), dot(c, vec3f(0.5959, -0.2746, -0.3213)),
                 dot(c, vec3f(0.2115, -0.5227, 0.3112)));
}
fn yiq_to_rgb(c: vec3f) -> vec3f {
    return vec3f(c.x + 0.9563 * c.y + 0.6210 * c.z, c.x - 0.2721 * c.y - 0.6474 * c.z,
                 c.x - 1.1070 * c.y + 1.7046 * c.z);
}

fn hash3(a: u32, b: u32, c: u32) -> u32 {
    var h = (a * 0x8da6b343u) ^ (b * 0xd8163841u) ^ (c * 0xcb1ab31fu);
    h ^= h >> 16u;
    h *= 0x7feb352du;
    h ^= h >> 15u;
    h *= 0x846ca68bu;
    h ^= h >> 16u;
    return h;
}
fn rnd(h: u32) -> f32 { return f32(h >> 8u) / 16777216.0; }

// (I, Q) of a chroma signal delayed by q samples, seen at the current sample's carrier phase.
fn rot_quarter(iq: vec2f, q: i32) -> vec2f {
    switch (q & 3) {
        case 1: { return vec2f(-iq.y, iq.x); }
        case 2: { return -iq; }
        case 3: { return vec2f(iq.y, -iq.x); }
        default: { return iq; }
    }
}

// Subcarrier phase of a row in quarter cycles. 480i: 227.5 cycles per line (+half a cycle per
// field line) and 262.5 lines per field (+a quarter cycle per field), so the rows of one field
// alternate by 180 degrees, the two fields are interleaved a quarter cycle apart and the pattern
// moves every field (dot crawl), repeating after 4 fields. The rows of the field not being drawn
// keep the phase of their last field. 240p (double strike, 263 lines): +180 degrees per line and
// per field.
fn row_quarter(row: i32) -> i32 {
    let k = i32(crt.sig1.x);
    if (crt.sig1.z > 0.5) {
        let field = select(k - 1, k, (k & 1) == (row & 1));
        return (field + 2 * (row >> 1)) & 3;
    }
    return (2 * row + 2 * k) & 3;
}
fn carrier(x: i32, q: i32) -> vec2f {  // (cos, sin) of the subcarrier at sample x
    switch ((x + q) & 3) {
        case 0: { return vec2f(1.0, 0.0); }
        case 1: { return vec2f(0.0, 1.0); }
        case 2: { return vec2f(-1.0, 0.0); }
        default: { return vec2f(0.0, -1.0); }
    }
}
// Past the line ends, read the nearest sample with the same carrier phase (no colour fringe).
fn fold4(x: i32, w: i32) -> i32 {
    if (x < 0) {
        return x + 4 * ((3 - x) / 4);
    }
    if (x >= w) {
        return x - 4 * ((x - w + 4) / 4);
    }
    return x;
}

// ---- pass: the console's encoder input: game frame -> Y'IQ at 4fsc, one row per line
@fragment
fn ntsc_encode(in: AuroraPostFxVertex) -> @location(0) vec4f {
    let src = vec2f(textureDimensions(tex0));
    let dst = aurora_postfx.output_size;
    let n = vec2i(clamp(ceil(src / dst * 0.5), vec2f(1.0), vec2f(4.0, 8.0)));
    let base = floor(in.position.xy) / dst;
    var acc = vec3f(0.0);
    for (var j = 0; j < n.y; j++) {
        for (var i = 0; i < n.x; i++) {
            let o = (vec2f(f32(i), f32(j)) + 0.5) / vec2f(n);
            acc += textureSampleLevel(tex0, aurora_linear, base + o / dst, 0.0).rgb;
        }
    }
    return vec4f(rgb_to_yiq(clamp(acc / f32(n.x * n.y), vec3f(0.0), vec3f(1.0))), 1.0);
}

// ---- pass: encoder band limits, then (RF) the tuner response, multipath ghost, hum, interference
// and noise. The tuner filters the modulated signal; on the I/Q pair that is a complex kernel
// (in-phase k_ca, quadrature k_cb), exact at 4fsc.
fn enc_filter(p: vec2i, w: i32) -> vec3f {
    let r = i32(crt.sigr.x);
    var y = 0.0;
    var iq = vec2f(0.0);
    for (var k = -r; k <= r; k++) {
        let t = textureLoad(tex0, vec2i(clamp(p.x - k, 0, w - 1), p.y), 0).xyz;
        y += tap_y(k) * t.x;
        iq += tap_ca(k) * t.yz + tap_cb(k) * vec2f(-t.z, t.y);
    }
    return vec3f(y, iq);
}

@fragment
fn ntsc_filter(in: AuroraPostFxVertex) -> @location(0) vec4f {
    let p = vec2i(in.position.xy);
    let dims = vec2i(textureDimensions(tex0));
    var v = enc_filter(p, dims.x);
    if (crt.sig2.y > 0.0) {  // echo arriving later: to the right, its chroma at another carrier phase
        let d = i32(crt.sig2.z);
        let e = enc_filter(vec2i(p.x - d, p.y), dims.x);
        v += crt.sig2.y * vec3f(e.x, rot_quarter(e.yz, d));
    }
    // Hum: mains ripple (60 Hz) on the gain beats against the 59.94 Hz fields, so one dark band
    // per field rolls slowly up the screen.
    let rowf = f32(p.y) / f32(dims.y);
    v *= 1.0 - crt.sig3.x * (0.5 + 0.5 * sin(6.2831853 * (rowf + crt.sig3.y)));
    if (crt.sig3.z > 0.0) {  // an interfering carrier ~1.6 MHz off: fine diagonal bars
        v.x += crt.sig3.z * sin(6.2831853 * (f32(p.x) * 0.113 + f32(p.y) * 0.31 + crt.sig3.w));
    }
    if (crt.sig2.x > 0.0) {  // white noise over the channel (snow after the TV's filters), new every field
        let seed = u32(crt.sig1.w);
        let a = rnd(hash3(u32(p.x), u32(p.y), seed));
        let b = rnd(hash3(u32(p.x) + 7919u, u32(p.y), seed ^ 0x5bd1e995u));
        v.x += crt.sig2.x * (a + b - 1.0) * 2.449;
    }
    return vec4f(v, 1.0);
}

// ---- pass: the TV's Y/C separation and chroma demodulation. S-Video keeps Y and C apart;
// composite / RF only have Y + C: a notch (band-pass for chroma, the rest is luma) or a comb
// (difference of lines of the same field, whose chroma is in opposite phase), then band-pass.
// Linear, so Y and C go through separately: the C left in luma (dot crawl) and the Y taken as
// chroma (cross-colour, rainbows) are scaled by the crosstalk setting (1 = the true sum).
fn yc(x: i32, row: i32, w: i32) -> vec2f {
    let xf = fold4(x, w);
    let t = textureLoad(tex0, vec2i(xf, row), 0).xyz;
    return vec2f(t.x, dot(t.yz, carrier(xf, row_quarter(row))));
}
fn comb(x: i32, row: i32, w: i32, h: i32) -> vec2f {
    let sep = i32(crt.sig1.y);
    let here = yc(x, row, w);
    if (sep == 0) {
        return here;
    }
    let s = i32(crt.sig0.w);
    var up = row - s;
    var dn = row + s;
    if (up < 0) {
        up = dn;
    }
    if (dn >= h) {
        dn = up;
    }
    if (sep == 1) {
        return 0.5 * (here - yc(x, up, w));
    }
    return 0.25 * (2.0 * here - yc(x, up, w) - yc(x, dn, w));
}

@fragment
fn ntsc_separate(in: AuroraPostFxVertex) -> @location(0) vec4f {
    let p = vec2i(in.position.xy);
    let dims = vec2i(textureDimensions(tex0));
    let here = yc(p.x, p.y, dims.x);
    var luma = here.x;
    var chroma = here.y;
    if (crt.sig0.x > 1.5) {
        let r = i32(crt.sigr.y);
        var c = vec2f(0.0);  // chroma band-pass of (Y part, C part)
        var t = vec2f(0.0);  // what the luma trap removes
        for (var k = -r; k <= r; k++) {
            let v = comb(p.x - k, p.y, dims.x, dims.y);
            c += tap_sep(k) * v;
            t += tap_trap(k) * v;
        }
        let a = crt.sig0.y;
        luma = (here.x - t.x) + a * (here.y - t.y);
        chroma = c.y + a * c.x;
    }
    // Demodulation: chroma x 2 cos / 2 sin of the burst-locked carrier, low-passed next pass.
    return vec4f(luma, 2.0 * chroma * carrier(p.x, row_quarter(p.y)), 1.0);
}

// ---- pass: TV luma channel (bandwidth, peaking) and chroma low-pass, Y'IQ -> R'G'B'
@fragment
fn ntsc_decode(in: AuroraPostFxVertex) -> @location(0) vec4f {
    let p = vec2i(in.position.xy);
    let w = i32(textureDimensions(tex0).x);
    let ry = i32(crt.sigr.w);
    var y = 0.0;
    for (var k = -ry; k <= ry; k++) {
        y += tap_dy(k) * textureLoad(tex0, vec2i(fold4(p.x - k, w), p.y), 0).x;
    }
    let rc = i32(crt.sigr.z);
    var iq = vec2f(0.0);
    for (var k = -rc; k <= rc; k++) {
        iq += tap_dc(k) * textureLoad(tex0, vec2i(fold4(p.x - k, w), p.y), 0).yz;
    }
    y = crt.sig4.x + crt.sig4.y * y;
    iq *= crt.sig4.y * crt.sig0.z;
    return vec4f(clamp(yiq_to_rgb(vec3f(y, iq)), vec3f(0.0), vec3f(1.0)), 1.0);
}

// ---- pass: the display copy (GXCopyDisp) as the console's pixel engine does it: the
// vertical filter of GXSetCopyFilter over the line above / this line / the line below (in EFB
// lines, so it scales with the render resolution; edge lines repeat, as with the copy clamp),
// then the GXSetDispCopyGamma curve.
// Above native resolution an EFB line is several rows; the taps above and below average their
// whole line, so the filter softens like the console's instead of adding sharp copies of the
// image one EFB line above and below (exactly the hardware's three lines at native resolution).
fn dcopy_line(uv: vec2f, line: f32, dy: f32, rows: i32, rowStep: f32) -> vec3f {
    var sum = vec3f(0.0);
    for (var j = 0; j < rows; j++) {
        let y = uv.y + line * dy + (f32(j) - 0.5 * f32(rows - 1)) * rowStep;
        sum += textureSampleLevel(tex0, aurora_linear, vec2f(uv.x, y), 0.0).rgb;
    }
    return sum / f32(rows);
}

@fragment
fn dcopy_filter(in: AuroraPostFxVertex) -> @location(0) vec4f {
    let dy = 1.0 / max(crt.dcopy2.y, 1.0);
    let texH = f32(textureDimensions(tex0).y);
    let rows = clamp(i32(round(dy * texH)), 1, 8);
    let rowStep = 1.0 / texH;
    let above = dcopy_line(in.uv, -1.0, dy, rows, rowStep);
    // The line itself stays one row: the presented picture keeps the render resolution.
    let here = textureSampleLevel(tex0, aurora_linear, in.uv, 0.0).rgb;
    let below = dcopy_line(in.uv, 1.0, dy, rows, rowStep);
    var col = above * crt.dcopy.x + here * crt.dcopy.y + below * crt.dcopy.z;
    col = pow(clamp(col, vec3f(0.0), vec3f(1.0)), vec3f(1.0 / crt.dcopy2.x));
    return vec4f(col, 1.0);
}

// ---- pass: average the game frame over each scanline band, decode to linear light
@fragment
fn crt_linearize(in: AuroraPostFxVertex) -> @location(0) vec4f {
    let src = vec2f(textureDimensions(tex0));
    let dst = aurora_postfx.output_size;
    let band = src / dst;
    let n = vec2i(clamp(ceil(band * 0.5), vec2f(1.0), vec2f(4.0, 8.0)));
    var base = floor(in.position.xy) / dst;
    if (crt.sig2.w > 0.0) {
        // RF line jitter: the TV's horizontal sync locks a little differently on every line of
        // every field (noise on the sync pulses), plus a slow wobble. In game pixels.
        let row = u32(in.position.y);
        let h = rnd(hash3(row, u32(crt.sig1.w), 0x68e31da4u)) - 0.5;
        let wobble = sin(f32(row) * 0.045 + crt.sig1.w * 0.37);
        let src43 = 480.0 * aurora_postfx.source_size.x / aurora_postfx.source_size.y;
        base.x += crt.sig2.w * (1.2 * h + 0.3 * wobble) / src43;
    }
    var acc = vec3f(0.0);
    for (var j = 0; j < n.y; j++) {
        for (var i = 0; i < n.x; i++) {
            let o = (vec2f(f32(i), f32(j)) + 0.5) / vec2f(n);
            let c = textureSampleLevel(tex0, aurora_linear, base + o / dst, 0.0).rgb;
            acc += pow(max(c, vec3f(1e-6)), vec3f(crt.gamma.x));
        }
    }
    return vec4f(acc / f32(n.x * n.y), 1.0);
}

// ---- pass: horizontal spot size of the beam (limited video bandwidth)
@fragment
fn crt_hsoft(in: AuroraPostFxVertex) -> @location(0) vec4f {
    let p = vec2i(in.position.xy);
    let dims = vec2i(textureDimensions(tex0));
    let sigma = crt.beam.w;
    if (sigma < 0.05) {
        return textureLoad(tex0, p, 0);
    }
    let r = i32(clamp(ceil(sigma * 2.5), 1.0, 6.0));
    var acc = vec3f(0.0);
    var wsum = 0.0;
    for (var k = -r; k <= r; k++) {
        let w = exp(-0.5 * f32(k * k) / (sigma * sigma));
        acc += w * textureLoad(tex0, vec2i(clamp(p.x + k, 0, dims.x - 1), p.y), 0).rgb;
        wsum += w;
    }
    return vec4f(acc / wsum, 1.0);
}

// ---- passes: separable gaussian; steps are output texels, bilinear taps fill the gaps
fn blur(uv: vec2f, step: vec2f) -> vec3f {
    var acc = vec3f(0.0);
    var wsum = 0.0;
    for (var k = -4; k <= 4; k++) {
        let w = exp(-0.5 * f32(k * k) / (1.8 * 1.8));
        acc += w * textureSampleLevel(tex0, aurora_linear, uv + step * f32(k), 0.0).rgb;
        wsum += w;
    }
    return acc / wsum;
}

@fragment
fn crt_blur_h(in: AuroraPostFxVertex) -> @location(0) vec4f {
    return vec4f(blur(in.uv, vec2f(1.0 / aurora_postfx.output_size.x, 0.0)), 1.0);
}

@fragment
fn crt_blur_v(in: AuroraPostFxVertex) -> @location(0) vec4f {
    return vec4f(blur(in.uv, vec2f(0.0, 1.0 / aurora_postfx.output_size.y)), 1.0);
}

// ---- final pass
// Gamma(1 + x) for 0 <= x <= 1 (Abramowitz & Stegun 6.1.36).
fn gamma1p(x: f32) -> f32 {
    return 1.0 + x * (-0.577191652 + x * (0.988205891 + x * (-0.897056937 + x * (0.918206857 +
           x * (-0.756704078 + x * (0.482199394 + x * (-0.193527818 + x * 0.035868343)))))));
}

// Beam intensity at distance d (in line spacings) from a line of color c. The profile
// is exp(-0.5 |d/s|^p): s (width) and p (2 = gaussian, larger = flatter) grow with
// brightness; it is normalized to unit area so average brightness is preserved.
// fp is the output pixel height in line spacings; box-filtering the profile over the
// pixel (variance + fp^2/12) keeps thin scanlines from aliasing at low resolutions.
fn beam_weight(d: f32, c: f32, box: f32) -> f32 {
    let lc = log2(clamp(c, 1e-5, 1.0));
    let s = mix(crt.beam.x, crt.beam.y, exp2(lc * crt.beam.z));
    let p = mix(crt.shape.x, crt.shape.y, exp2(lc * crt.shape.z));
    let s2 = s * s + box;
    let se = sqrt(s2);
    let pe = mix(p, 2.0, box / s2);
    let inv = 1.0 / pe;
    let norm = 2.0 * se * exp2(inv) * gamma1p(inv);
    return exp(-0.5 * exp2(pe * log2(max(abs(d) / se, 1e-6)))) / norm;
}

// The scanline grid around uv: rows to visit and the distance to the first one.
struct Scan {
    x: f32,
    l0: f32,
    d0: f32,
    spacing: f32,
    field: f32,
    lines: f32,
    first: i32,
    last: i32,
};

fn scan_setup(uv: vec2f, off: vec2f) -> Scan {
    let dims = vec2f(textureDimensions(tex0));
    var s: Scan;
    s.lines = dims.y;
    let interlaced = crt.mask.w > 0.5;
    s.spacing = select(1.0, 2.0, interlaced);
    s.field = select(0.0, crt.sig4.z, interlaced);  // 59.94 Hz field clock (real time)
    let yf = (uv.y * s.lines - 0.5 + off.y - s.field) / s.spacing;
    s.l0 = floor(yf);
    s.d0 = yf - s.l0;
    s.x = uv.x + off.x / dims.x;
    // Wide beams reach past the two nearest lines.
    let wide = crt.beam.y > 0.5;
    s.first = select(0, -1, wide);
    s.last = select(1, 2, wide);
    return s;
}

fn scan_row(s: Scan, k: i32) -> vec4f {  // rgb = line color, a = 0 outside the picture
    let row = (s.l0 + f32(k)) * s.spacing + s.field;
    if (row < 0.0 || row >= s.lines) {
        return vec4f(0.0);
    }
    return vec4f(textureSampleLevel(tex0, aurora_linear, vec2f(s.x, (row + 0.5) / s.lines), 0.0).rgb, 1.0);
}

// All three guns at the same spot.
fn scan(uv: vec2f, box: f32) -> vec3f {
    let s = scan_setup(uv, vec2f(0.0));
    var acc = vec3f(0.0);
    for (var k = s.first; k <= s.last; k++) {
        let c = scan_row(s, k).rgb;
        let d = s.d0 - f32(k);
        acc += c * vec3f(beam_weight(d, c.r, box), beam_weight(d, c.g, box), beam_weight(d, c.b, box));
    }
    return acc;
}

// One gun (channel selected by `sel`) displaced by `off` (misconvergence).
fn scan1(uv: vec2f, off: vec2f, box: f32, sel: vec3f) -> f32 {
    let s = scan_setup(uv, off);
    var acc = 0.0;
    for (var k = s.first; k <= s.last; k++) {
        let c = dot(scan_row(s, k).rgb, sel);
        acc += c * beam_weight(s.d0 - f32(k), c, box);
    }
    return acc;
}

// Fraction of [t0, t1] (in mask periods) covered by [a, b) repeating every period.
fn coverage(t0: f32, t1: f32, a: f32, b: f32) -> f32 {
    let f1 = floor(t1) * (b - a) + clamp(fract(t1) - a, 0.0, b - a);
    let f0 = floor(t0) * (b - a) + clamp(fract(t0) - a, 0.0, b - a);
    return (f1 - f0) / (t1 - t0);
}

// RGB phosphor stripes of triad width t, box-filtered over the pixel; mean 1 per channel.
fn stripes(x: f32, t: f32) -> vec3f {
    let t0 = (x - 0.5) / t;
    let t1 = (x + 0.5) / t;
    let g = 0.03;
    let third = 1.0 / 3.0;
    return vec3f(coverage(t0, t1, g, third - g), coverage(t0, t1, third + g, 2.0 * third - g),
                 coverage(t0, t1, 2.0 * third + g, 1.0 - g)) / (third - 2.0 * g);
}

// Lit fraction of rows of pitch h with a dark gap (fraction of h); mean 1.
fn rows(y: f32, h: f32, gap: f32) -> f32 {
    return coverage((y - 0.5) / h, (y + 0.5) / h, gap * 0.5, 1.0 - gap * 0.5) / (1.0 - gap);
}

fn phosphor_mask(px: vec2f) -> vec3f {
    let t = max(crt.mask.y, 1.0);
    let kind = i32(crt.mask.x + 0.5);
    if (kind == 0) {  // aperture grille: continuous vertical stripes
        return stripes(px.x, t);
    }
    if (kind == 1) {  // slot mask: stripes cut into slots, alternate triads offset by half a slot
        let column = floor(px.x / t);
        let y = px.y + select(0.0, 0.5 * t, fract(column * 0.5) > 0.25);
        return stripes(px.x, t) * rows(y, t, 0.3);
    }
    if (kind == 2) {  // shadow mask: dot rows, every other row shifted by half a triad (delta)
        let h = 0.6 * t;
        let row = floor(px.y / h);
        let x = px.x + select(0.0, 0.5 * t, fract(row * 0.5) > 0.25);
        return stripes(x, t) * rows(px.y, h, 0.35);
    }
    return vec3f(1.0);
}

// Largest value phosphor_mask() returns (fully lit phosphor), so mask * color can be kept <= 1.
fn mask_peak() -> f32 {
    let stripe = 1.0 / (1.0 / 3.0 - 2.0 * 0.03);
    let kind = i32(crt.mask.x + 0.5);
    if (kind == 0) {
        return stripe;
    }
    if (kind == 1) {
        return stripe / 0.7;
    }
    if (kind == 2) {
        return stripe / 0.65;
    }
    return 2.0;
}

@fragment
fn crt_final(in: AuroraPostFxVertex) -> @location(0) vec4f {
    let vp = aurora_postfx.viewport;
    let uv0 = (in.position.xy - vp.xy) / vp.zw;
    let aspect = vp.z / vp.w;

    // Geometry: barrel distortion of a spherical-ish tube, scaled so the nearer edge
    // just touches the screen; rounded corners in pixels.
    var p = uv0 * 2.0 - 1.0;
    let k = crt.gamma.z;
    let pa = p * vec2f(aspect, 1.0);
    let r2 = dot(pa, pa) / (aspect * aspect + 1.0);
    let edge = min(aspect * aspect, 1.0) / (aspect * aspect + 1.0);
    p = p * (1.0 + k * r2) / (1.0 + k * edge);
    let uv = p * 0.5 + 0.5;
    let half_size = 0.5 * vp.zw;
    let radius = crt.gamma.w * vp.w;
    let q = abs(p) * half_size - (half_size - vec2f(radius));
    let dist = length(max(q, vec2f(0.0))) + min(max(q.x, q.y), 0.0) - radius;
    let inside = clamp(0.5 - dist, 0.0, 1.0);
    if (inside <= 0.0) {
        return vec4f(0.0, 0.0, 0.0, 1.0);
    }

    // Electron beams (per-channel when the guns are misconverged).
    let lines = f32(textureDimensions(tex0).y);
    let spacing = select(1.0, 2.0, crt.mask.w > 0.5);
    // Output pixel height in line spacings (curvature magnifies towards the edges).
    let fp = lines / (vp.w * spacing) * (1.0 + 3.0 * k * r2) / (1.0 + k * edge);
    let box = fp * fp / 12.0;
    var col: vec3f;
    if (crt.conv_b.z > 0.5) {
        col = vec3f(scan1(uv, crt.conv_rg.xy, box, vec3f(1.0, 0.0, 0.0)),
                    scan1(uv, crt.conv_rg.zw, box, vec3f(0.0, 1.0, 0.0)),
                    scan1(uv, crt.conv_b.xy, box, vec3f(0.0, 0.0, 1.0)));
    } else {
        col = scan(uv, box);
    }

    // Halation: electrons/light scattered inside the glass, before the mask.
    let halo = textureSampleLevel(tex1, aurora_linear, uv, 0.0).rgb;
    col = mix(col, halo, crt.glow.x) * crt.glow.w;

    // Phosphor mask, brightness-compensated (mean 1 per channel). A phosphor cannot
    // emit more than full drive, so for bright channels the mask contrast is reduced
    // until the brightest phosphor just reaches 1: highlights keep their brightness and
    // hue, and the mask fades out in them like a real tube's glow fills the gaps.
    let peak = mask_peak();
    // A third of the strength is kept regardless, trading some highlight clipping for a
    // visible mask on bright content.
    let limit = clamp((1.0 / max(col, vec3f(1e-4)) - 1.0) / (peak - 1.0), vec3f(0.0), vec3f(1.0));
    let strength = max(min(vec3f(crt.mask.z), limit), vec3f(crt.mask.z * 0.35));
    var x = col * mix(vec3f(1.0), phosphor_mask(in.position.xy), strength);

    // Diffusion across the faceplate and bloom around bright areas.
    let wide = textureSampleLevel(tex2, aurora_linear, uv, 0.0).rgb * crt.glow.w;
    x = mix(x, wide, crt.glow.y) + crt.glow.z * wide * wide;

    x = clamp(x, vec3f(0.0), vec3f(1.0)) * inside;
    return vec4f(pow(x, vec3f(1.0 / crt.gamma.y)), 1.0);
}
)WGSL";

struct Uniforms {
    float beam[4];
    float shape[4];
    float mask[4];
    float glow[4];
    float convRg[4];
    float convB[4];
    float gamma[4];
    float dcopy[4];
    float dcopy2[4];
    float sig0[4];
    float sig1[4];
    float sig2[4];
    float sig3[4];
    float sig4[4];
    float sigr[4];
    float kY[36];
    float kCa[36];
    float kCb[36];
    float kSep[36];
    float kTrap[36];
    float kDc[36];
    float kDy[36];
};
static_assert(sizeof(Uniforms) <= AURORA_POSTFX_MAX_UNIFORM_SIZE);

int lineCount(port::CrtScanlines s) { return s == port::CrtScanlines::DoubleStrike240 ? 240 : 480; }

// ---- signal path filters (FIR taps -16..16 at 4fsc), designed here from the bandwidths.
constexpr double kFs = 4.0 * 315.0 / 88.0;  // MHz: 4 x the NTSC colour subcarrier (3.579545 MHz)
constexpr double kPi = 3.14159265358979323846;
constexpr int kR = 16;
using Kernel = std::array<double, 2 * kR + 1>;
// Samples of the line per game pixel: 640 pixels at 13.5 MHz (47.4 us) take 679 samples.
constexpr double kSamplesPerPixel = kFs / 13.5;

Kernel delta() {
    Kernel k{};
    k[kR] = 1.0;
    return k;
}

// Linear-phase low-pass (Hann-windowed sinc), -6 dB at fc, unit DC gain.
Kernel lowpass(double fc) {
    Kernel k{};
    double sum = 0.0;
    for (int n = -kR; n <= kR; n++) {
        const double w = 0.5 * (1.0 + std::cos(kPi * n / (kR + 1)));
        const double x = 2.0 * fc / kFs;
        const double s = n == 0 ? x : std::sin(kPi * x * n) / (kPi * n);
        k[n + kR] = s * w;
        sum += s * w;
    }
    for (double& v : k) v /= sum;
    return k;
}

// Analog-style causal low-pass (three equal real poles, -3 dB at fc) with a zero at fs/2 (removes
// the 2fsc demodulation product), advanced by 2 time constants: the TV's luma delay line cancels
// most of the chroma filter's delay, the tail is left and colour trails to the right of edges.
Kernel causal(double fc) {
    const double tau = 0.5098 / (2.0 * kPi * fc) * kFs;  // samples; sqrt(2^(1/3) - 1) = 0.5098
    std::array<double, 4 * kR> h{};
    for (int n = 0; n < int(h.size()); n++) {
        const double t = n + 0.5;
        h[n] = t * t * std::exp(-t / tau);
    }
    const int shift = int(std::lround(2.0 * tau));
    Kernel k{};
    double sum = 0.0;
    for (int n = -kR; n <= kR; n++) {
        const int i = n + shift;  // tap n of the output = sample i of the causal response
        double v = 0.0;
        if (i >= 0 && i < int(h.size())) v += 0.5 * h[i];
        if (i >= 1 && i - 1 < int(h.size())) v += 0.5 * h[i - 1];
        k[n + kR] = v;
        sum += v;
    }
    for (double& v : k) v /= sum;
    return k;
}

Kernel convolve(const Kernel& a, const Kernel& b) {
    Kernel k{};
    for (int n = -kR; n <= kR; n++) {
        double v = 0.0;
        for (int m = -kR; m <= kR; m++) {
            const int j = n - m;
            if (j >= -kR && j <= kR) v += a[m + kR] * b[j + kR];
        }
        k[n + kR] = v;
    }
    return k;
}

int radius(const Kernel& k, const Kernel* also = nullptr) {
    for (int r = kR; r > 0; r--) {
        const auto big = [r](const Kernel& q) {
            return std::abs(q[kR + r]) > 1e-4 || std::abs(q[kR - r]) > 1e-4;
        };
        if (big(k) || (also && big(*also))) return r;
    }
    return 0;
}

void store(float (&dst)[36], const Kernel& k) {
    std::fill(std::begin(dst), std::end(dst), 0.0f);
    for (int i = 0; i <= 2 * kR; i++) dst[i] = float(k[i]);
}

void fillSignal(Uniforms& u, const port::CrtSettings& s, int lines) {
    using port::CrtSignal;
    // The 59.94 Hz field clock runs in real time, independent of the game frame rate and of the
    // presented frame rate: at 120 fps every field is shown for two presents, as a TV would.
    static const auto t0 = std::chrono::steady_clock::now();
    const double t = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    // Test aid LM_CRT_FIELD=<n>: hold the field clock at field n (identical screenshots).
    static const char* sHold = std::getenv("LM_CRT_FIELD");
    const int64_t field = sHold ? int64_t(std::atoll(sHold)) : int64_t(t * 60000.0 / 1001.0);
    u.sig4[2] = float(field & 1);
    const int mode = s.signal == CrtSignal::Rgb ? 0 : s.signal == CrtSignal::SVideo ? 1 : s.signal == CrtSignal::Composite ? 2 : 3;
    if (mode == 0) return;
    const bool rf = mode == 3;
    const bool interlaced = lines >= 480;
    const int sep = mode == 1 ? 0 : int(s.separation);
    u.sig0[0] = float(mode);
    u.sig0[1] = mode == 1 ? 0.0f : s.artifacts;
    u.sig0[2] = s.saturation;
    u.sig0[3] = interlaced ? 2.0f : 1.0f;  // comb: the previous line of the same field
    u.sig1[0] = float(field & 3);
    u.sig1[1] = float(sep);
    u.sig1[2] = interlaced ? 1.0f : 0.0f;
    u.sig1[3] = float(field & 0xFFFF);
    if (rf) {
        u.sig2[0] = 0.08f * s.noise;
        u.sig2[1] = s.ghost;
        u.sig2[2] = float(std::lround(s.ghostDelay * kFs));
        u.sig2[3] = s.jitter;
        u.sig3[0] = 0.08f * s.hum;
        u.sig3[1] = float(std::fmod(t * (60.0 - 60000.0 / 1001.0), 1.0));
        u.sig3[2] = 0.04f * s.interference;
        u.sig3[3] = float(std::fmod(double(field) * 0.137, 1.0));
    }
    // RF: the demodulated picture comes out a little flat (AGC, black level lifted).
    u.sig4[0] = rf ? 0.035f : 0.0f;
    u.sig4[1] = rf ? 0.93f : 1.0f;

    // Encoder (the console's video DAC/encoder): luma to ~6 MHz, chroma 1.5 MHz. RF: then the
    // modulator + the TV tuner/IF strip limit the whole channel to 4.2 MHz; on the modulated
    // chroma that is the low-pass shifted to the subcarrier (complex on I/Q, exact at 4fsc).
    const Kernel tuner = rf ? lowpass(4.2) : delta();
    const Kernel ey = convolve(lowpass(6.0), tuner);
    const Kernel ec = lowpass(1.5);
    Kernel tc{}, ts{};
    for (int n = -kR; n <= kR; n++) {
        tc[n + kR] = tuner[n + kR] * std::cos(kPi * 0.5 * n);
        ts[n + kR] = tuner[n + kR] * std::sin(kPi * 0.5 * n);
    }
    const Kernel ca = convolve(ec, tc), cb = convolve(ec, ts);
    // TV Y/C separation: a band-pass is a low-pass moved to the subcarrier (cos(pi n / 2) at 4fsc).
    // Notch sets: chroma band-pass +-1.3 MHz, and a separate, narrower luma trap (+-0.8 MHz) so
    // luma keeps more detail (the chroma sidebands outside the trap stay in luma: dot crawl).
    // Comb sets: luma = composite - comb chroma, band-passed +-1.8 MHz (the comb does the work).
    const auto bandpass = [](double halfWidth) {
        const Kernel l = lowpass(halfWidth);
        Kernel b{};
        for (int n = -kR; n <= kR; n++) b[n + kR] = 2.0 * l[n + kR] * std::cos(kPi * 0.5 * n);
        return b;
    };
    const Kernel bp = bandpass(sep == 0 ? 1.3 : 1.8);
    const Kernel trap = sep == 0 ? bandpass(0.8) : bp;
    // TV luma channel: low-pass at the luma bandwidth plus peaking (boosted upper mids).
    const Kernel ly = lowpass(s.lumaBandwidth), lh = lowpass(s.lumaBandwidth * 0.5);
    Kernel dy{};
    for (int i = 0; i <= 2 * kR; i++) dy[i] = ly[i] + s.sharpening * (ly[i] - lh[i]);
    const Kernel dc = causal(s.chromaBandwidth);

    store(u.kY, ey);
    store(u.kCa, ca);
    store(u.kCb, cb);
    store(u.kSep, bp);
    store(u.kTrap, trap);
    store(u.kDc, dc);
    store(u.kDy, dy);
    u.sigr[0] = float(std::max(radius(ey), radius(ca, &cb)));
    u.sigr[1] = float(mode == 1 ? 0 : radius(bp, &trap));
    u.sigr[2] = float(radius(dc));
    u.sigr[3] = float(radius(dy));
}

int sAppliedKey = -1;  // -1 = chain disabled

// lines < 0: no CRT passes. filter: the display copy filter pass runs first (alone when the
// CRT filter is off), and every CRT pass input moves up by one. signal: the S-Video / composite /
// RF passes run between them and the CRT passes.
void setChain(int lines, bool filter, bool signal) {
    const int key = lines < 0 && !filter ? -1 : ((std::max(lines, 0) * 2 + (filter ? 1 : 0)) * 2 + (signal ? 1 : 0));
    if (key == sAppliedKey) return;
    sAppliedKey = key;
    if (key < 0) {
        aurora_postfx_set_chain(nullptr, nullptr, 0);
        return;
    }
    const auto rel = [](AuroraPostFxSizeMode m, float v) { return AuroraPostFxSize{m, v}; };
    const AuroraPostFxSize same = rel(AURORA_POSTFX_SIZE_INPUT, 1.0f), half = rel(AURORA_POSTFX_SIZE_INPUT, 0.5f);
    std::vector<AuroraPostFxPass> passes;
    uint32_t o = filter ? 1 : 0;
    if (filter) passes.push_back({"dcopy_filter", rel(AURORA_POSTFX_SIZE_SOURCE, 1.0f), rel(AURORA_POSTFX_SIZE_SOURCE, 1.0f), false, 1, {0}});
    if (lines >= 0 && signal) {
        // One row per scanline, 679 samples per 4:3 picture width (more for wider frames).
        const AuroraPostFxSize w = rel(AURORA_POSTFX_SIZE_ASPECT, float(kSamplesPerPixel * 480.0 / lines));
        const AuroraPostFxSize h = rel(AURORA_POSTFX_SIZE_ABSOLUTE, float(lines));
        passes.push_back({"ntsc_encode", w, h, true, 1, {o}});
        passes.push_back({"ntsc_filter", same, same, true, 1, {o + 1}});
        passes.push_back({"ntsc_separate", same, same, true, 1, {o + 2}});
        passes.push_back({"ntsc_decode", same, same, true, 1, {o + 3}});
        o += 4;
    }
    if (lines >= 0) {
        passes.push_back({"crt_linearize", rel(AURORA_POSTFX_SIZE_ASPECT, 1.0f), rel(AURORA_POSTFX_SIZE_ABSOLUTE, float(lines)), true, 1, {o}});
        passes.push_back({"crt_hsoft", same, same, true, 1, {o + 1}});
        for (uint32_t i = 0; i < 3; i++) {
            passes.push_back({"crt_blur_h", half, half, true, 1, {o + 2 + 2 * i}});
            passes.push_back({"crt_blur_v", same, same, true, 1, {o + 3 + 2 * i}});
        }
        passes.push_back({"crt_final", {}, {}, false, 3, {o + 2, o + 4, o + 8}});
    }
    aurora_postfx_set_chain(kShader, passes.data(), uint32_t(passes.size()));
}

bool sliderF(const char* label, float& v, float lo, float hi, const char* fmt = "%.2f") {
    return recomp::menu::SliderFloat(label, &v, lo, hi, fmt, ImGuiSliderFlags_AlwaysClamp);
}

template <typename E>
bool comboE(const char* label, E& value, const char* const* names, int count) {
    int i = int(value);
    const bool changed = recomp::menu::Combo(label, &i, names, count);
    if (changed) value = E(i);
    return changed;
}

}  // namespace

void applySignal(port::CrtSettings& s, port::CrtSignal signal) {
    using namespace port;
    s.signal = signal;
    s.separation = CrtSeparation::Notch;
    s.artifacts = 1.0f;
    s.noise = s.ghost = s.jitter = s.hum = s.interference = 0.0f;
    s.ghostDelay = 1.0f;
    switch (signal) {
    case CrtSignal::Rgb:
    case CrtSignal::SVideo:  // separate Y and C: only the band limits
        s.lumaBandwidth = 5.0f;
        s.chromaBandwidth = 1.3f;
        s.sharpening = 0.1f;
        s.saturation = 1.0f;
        break;
    case CrtSignal::Composite:
        s.lumaBandwidth = 4.2f;
        s.chromaBandwidth = 1.0f;
        s.sharpening = 0.25f;
        s.saturation = 0.92f;
        break;
    case CrtSignal::Rf:  // channel 3 through the antenna input
        s.lumaBandwidth = 3.4f;
        s.chromaBandwidth = 0.8f;
        s.sharpening = 0.3f;
        s.saturation = 0.85f;
        s.noise = 0.2f;
        s.ghost = 0.08f;
        s.ghostDelay = 1.2f;
        s.jitter = 0.2f;
        s.hum = 0.25f;
        break;
    }
}

void applyPreset(port::CrtSettings& s, port::CrtPreset preset) {
    using namespace port;
    const CrtSettings old = s;
    s = CrtSettings{};  // defaults = aperture grille
    s.enabled = old.enabled;
    s.scanlines = old.scanlines;
    s.preset = preset;
    // The signal path is its own choice: a tube preset keeps it.
    s.signal = old.signal;
    s.separation = old.separation;
    s.lumaBandwidth = old.lumaBandwidth;
    s.chromaBandwidth = old.chromaBandwidth;
    s.sharpening = old.sharpening;
    s.artifacts = old.artifacts;
    s.saturation = old.saturation;
    s.noise = old.noise;
    s.ghost = old.ghost;
    s.ghostDelay = old.ghostDelay;
    s.jitter = old.jitter;
    s.hum = old.hum;
    s.interference = old.interference;
    switch (preset) {
    case CrtPreset::ApertureGrille:
    case CrtPreset::Custom:
        break;
    case CrtPreset::SlotMask:
        s.beamMinSigma = 0.22f;
        s.beamMaxSigma = 0.40f;
        s.beamMaxShape = 3.5f;
        s.mask = CrtMask::SlotMask;
        s.maskSize = 4.0f;
        s.maskStrength = 0.7f;
        s.halation = 0.05f;
        s.diffusion = 0.07f;
        s.curvature = 0.03f;
        s.cornerRadius = 0.03f;
        s.brightness = 1.3f;
        break;
    case CrtPreset::ShadowMask:
        s.beamMinSigma = 0.26f;
        s.beamMaxSigma = 0.44f;
        s.beamMaxShape = 2.6f;
        s.horizontalSoftness = 0.6f;
        s.mask = CrtMask::ShadowMask;
        s.maskSize = 4.0f;
        s.maskStrength = 0.6f;
        s.halation = 0.06f;
        s.diffusion = 0.08f;
        s.bloom = 0.2f;
        s.curvature = 0.04f;
        s.cornerRadius = 0.04f;
        s.brightness = 1.3f;
        break;
    case CrtPreset::ConsumerTv:
        s.beamMinSigma = 0.30f;
        s.beamMaxSigma = 0.48f;
        s.beamMinShape = 2.0f;
        s.beamMaxShape = 2.4f;
        s.horizontalSoftness = 0.9f;
        s.mask = CrtMask::ShadowMask;
        s.maskSize = 5.0f;
        s.maskStrength = 0.45f;
        s.halation = 0.10f;
        s.diffusion = 0.12f;
        s.bloom = 0.25f;
        s.convergence[0] = 0.35f;
        s.convergence[1] = 0.10f;
        s.convergence[4] = -0.35f;
        s.convergence[5] = -0.05f;
        s.curvature = 0.08f;
        s.cornerRadius = 0.06f;
        s.brightness = 1.25f;
        break;
    }
}

void update() {
    static bool sEnvChecked = false;
    if (!sEnvChecked) {
        // Test override (not saved unless the menu changes something):
        // LM_CRT=<preset 0-3>[,<scanlines 0-2>[,<signal 0 RGB, 1 S-Video, 2 composite, 3 RF>]]
        sEnvChecked = true;
        if (const char* env = std::getenv("LM_CRT")) {
            auto& s = port::settings().crt;
            s.enabled = true;
            applyPreset(s, port::CrtPreset(std::clamp(std::atoi(env), 0, 3)));
            if (const char* comma = std::strchr(env, ',')) {
                s.scanlines = port::CrtScanlines(std::clamp(std::atoi(comma + 1), 0, 2));
                if (const char* c2 = std::strchr(comma + 1, ','))
                    applySignal(s, port::CrtSignal(std::clamp(std::atoi(c2 + 1), 0, 3)));
            }
        }
    }
    // Test aid LM_CRT_CYCLE=<seconds>[,<start s>]: from <start> seconds after launch, the signal
    // steps RGB -> S-Video -> composite -> RF every <seconds> (A/B screenshots; with LM_FREEZE_AT).
    static const char* sCycle = std::getenv("LM_CRT_CYCLE");
    if (sCycle) {
        static const auto t0 = std::chrono::steady_clock::now();
        static int sLast = -1;
        const double period = std::max(std::atof(sCycle), 0.1);
        const char* comma = std::strchr(sCycle, ',');
        const double start = comma ? std::atof(comma + 1) : 0.0;
        const double t = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() - start;
        const int step = t < 0.0 ? 0 : int(t / period) % 4;
        if (step != sLast) {
            sLast = step;
            applySignal(port::settings().crt, port::CrtSignal(step));
            std::printf("[crt] signal %d at %.2f s\n", step, t + start);
            std::fflush(stdout);
        }
    }
    const auto& s = port::settings().crt;
    // Display copy filter (GXSetCopyFilter vertical filter + GXSetDispCopyGamma), skipped
    // when it would not change the picture.
    const auto& dc = recomp::gx::displayCopy();
    const float above = float(dc.vfilter[0] + dc.vfilter[1]) / 64.0f;
    const float here = float(dc.vfilter[2] + dc.vfilter[3] + dc.vfilter[4]) / 64.0f;
    const float below = float(dc.vfilter[5] + dc.vfilter[6]) / 64.0f;
    const float gamma = dc.gamma == 1 ? 1.7f : dc.gamma == 2 ? 2.2f : 1.0f;
    // Phone with MetalFX upscaling on (src/mobile): no display copy filter pass. Aurora upscales
    // with MetalFX only when no post-process chain runs, and this pass alone kept the chain on, so
    // MetalFX never ran (the Upscaling row changed nothing). MetalFX resolves the picture itself.
    const bool filter = port::settings().rendering.displayCopyFilter && !mobile::upscalingOn() &&
                        (above != 0.0f || below != 0.0f || here != 1.0f || gamma != 1.0f);
    const float dcopy[4] = {above, here, below, 0.0f};
    const float dcopy2[4] = {gamma, float(dc.efbHeight), 0.0f, 0.0f};
    if (!s.enabled) {
        if (filter) {
            Uniforms u{};
            std::memcpy(u.dcopy, dcopy, sizeof(dcopy));
            std::memcpy(u.dcopy2, dcopy2, sizeof(dcopy2));
            aurora_postfx_set_uniforms(&u, sizeof(u));
        }
        setChain(-1, filter, false);
        return;
    }
    const bool conv = std::any_of(std::begin(s.convergence), std::end(s.convergence), [](float v) { return v != 0.0f; });
    Uniforms u{
        {s.beamMinSigma, std::max(s.beamMaxSigma, s.beamMinSigma), s.beamSpotPower, s.horizontalSoftness},
        {s.beamMinShape, std::max(s.beamMaxShape, s.beamMinShape), s.beamShapePower, 0.0f},
        {float(int(s.mask)), s.maskSize, s.mask == port::CrtMask::None ? 0.0f : s.maskStrength,
         s.scanlines == port::CrtScanlines::Interlaced480i ? 1.0f : 0.0f},
        {s.halation, s.diffusion, s.bloom, s.brightness},
        {s.convergence[0], s.convergence[1], s.convergence[2], s.convergence[3]},
        {s.convergence[4], s.convergence[5], conv ? 1.0f : 0.0f, 0.0f},
        {s.gammaIn, s.gammaOut, s.curvature, s.cornerRadius},
    };
    std::memcpy(u.dcopy, dcopy, sizeof(dcopy));
    std::memcpy(u.dcopy2, dcopy2, sizeof(dcopy2));
    fillSignal(u, s, lineCount(s.scanlines));
    aurora_postfx_set_uniforms(&u, sizeof(u));
    setChain(lineCount(s.scanlines), filter, s.signal != port::CrtSignal::Rgb);
}

bool drawMenu() {
    using namespace port;
    auto& s = settings().crt;
    bool changed = recomp::menu::Checkbox("CRT filter", &s.enabled);
    recomp::menu::ItemTooltip("Makes the picture look like a CRT television: scanlines, a phosphor mask, glow and\n"
                          "screen curvature. One post-processing pass chain on the finished image.%s",
                          mobile::enabled() ? " MetalFX upscaling (Phone tab) pauses while it is on." : "");
    if (s.enabled && !aurora_postfx_active()) {
        ImGui::SameLine();
        ImGui::TextDisabled("(shader failed to compile, see log)");
    }
    ImGui::BeginDisabled(!s.enabled);
    ImGui::Indent();
    static const char* kPresets[] = {"Aperture grille (Royale-like)", "Slot mask", "Shadow mask", "Consumer TV",
                                     "Custom"};
    int preset = int(s.preset);
    if (recomp::menu::Combo("CRT preset", &preset, kPresets, 5)) {
        if (CrtPreset(preset) != CrtPreset::Custom) applyPreset(s, CrtPreset(preset));
        s.preset = CrtPreset(preset);
        changed = true;
    }
    recomp::menu::ItemTooltip("Kind of tube: aperture grille (sharp vertical stripes, like a Trinitron), slot mask,\n"
                          "shadow mask, or a soft consumer TV. Custom keeps the advanced values.");
    static const char* kLines[] = {"480 lines (progressive)", "240 lines (double strike)", "480i (interlaced fields)"};
    changed |= comboE("Scanlines", s.scanlines, kLines, 3);
    recomp::menu::ItemTooltip("480 lines: the game's progressive output. 240 lines: thick visible scanlines like a\n"
                          "240p signal. 480i: alternating interlaced fields, as a TV shows the 480i signal.");
    static const char* kSignals[] = {"RGB (clean)", "S-Video", "Composite", "RF (antenna / coax)"};
    int signal = int(s.signal);
    if (recomp::menu::Combo("Signal", &signal, kSignals, 4)) {
        applySignal(s, CrtSignal(signal));
        changed = true;
    }
    recomp::menu::ItemTooltip("The cable between the console and the TV, modelled on the game's NTSC signal:\n"
                          "RGB: the clean picture.\n"
                          "S-Video: luma and chroma on separate wires; slightly soft luma, softer colour edges,\n"
                          "no dot crawl.\n"
                          "Composite: luma and chroma share one wire on the 3.58 MHz colour subcarrier; the TV\n"
                          "has to split them again: dot crawl on colour edges, rainbows on fine detail, softer\n"
                          "picture, colour bleeding to the right.\n"
                          "RF: composite through an RF modulator and the TV's tuner: softer still, faint ghost,\n"
                          "snow, line jitter and a weak rolling hum bar.\n"
                          "Works with any tube preset (choosing a preset keeps the signal).");
    bool custom = false;

    recomp::menu::openTreesForCheck();
    if (s.signal != CrtSignal::Rgb && ImGui::TreeNode("Signal advanced")) {
        const bool composite = s.signal == CrtSignal::Composite || s.signal == CrtSignal::Rf;
        changed |= sliderF("Luma bandwidth", s.lumaBandwidth, 1.5f, 6.0f, "%.1f MHz");
        recomp::menu::ItemTooltip("Sharpness: bandwidth of the TV's brightness channel. NTSC broadcast is 4.2 MHz;\n"
                              "lower is softer (an old set on RF ~3 MHz), higher keeps more of the game's detail.");
        changed |= sliderF("Chroma bandwidth", s.chromaBandwidth, 0.3f, 2.0f, "%.2f MHz");
        recomp::menu::ItemTooltip("Bandwidth of the colour signal: lower smears colour edges more (to the right,\n"
                              "as the TV's analog colour filter does). 1.3 MHz is the NTSC I channel.");
        changed |= sliderF("Sharpening", s.sharpening, 0.0f, 1.0f);
        recomp::menu::ItemTooltip("The TV's sharpness control (peaking): boosts edges, with a slight halo");
        changed |= sliderF("Color", s.saturation, 0.0f, 1.5f);
        recomp::menu::ItemTooltip("The TV's colour (saturation) control. Composite and RF sets lose a little colour.");
        if (composite) {
            static const char* kSep[] = {"Notch filter (cheap TV)", "2-line comb filter", "3-line comb filter"};
            changed |= comboE("Y/C separation", s.separation, kSep, 3);
            recomp::menu::ItemTooltip("How the TV splits the composite signal into brightness and colour.\n"
                                  "Notch: removes everything near the subcarrier from luma (softer), strong dot\n"
                                  "crawl along vertical colour edges and rainbows on fine detail.\n"
                                  "Comb: subtracts lines of the same field (their colour is in opposite phase):\n"
                                  "sharper, clean flat colour; dots 'hang' on horizontal colour edges instead.");
            changed |= sliderF("Artifact strength", s.artifacts, 0.0f, 1.0f);
            recomp::menu::ItemTooltip("How much luma/chroma crosstalk gets through: the dot crawl (colour left in the\n"
                                  "brightness) and the rainbows (fine detail decoded as colour). 1 = as the signal\n"
                                  "model gives it; 0 keeps the bandwidth limits only.");
        }
        if (s.signal == CrtSignal::Rf) {
            changed |= sliderF("Noise", s.noise, 0.0f, 1.0f);
            recomp::menu::ItemTooltip("Snow from a weak signal (new grain every field)");
            changed |= sliderF("Ghost", s.ghost, 0.0f, 0.5f);
            recomp::menu::ItemTooltip("Strength of a reflected copy of the signal (multipath echo), seen to the right");
            changed |= sliderF("Ghost delay", s.ghostDelay, 0.2f, 4.0f, "%.2f us");
            recomp::menu::ItemTooltip("How much later the echo arrives (1 us is about 13 game pixels to the right)");
            changed |= sliderF("Jitter", s.jitter, 0.0f, 2.0f, "%.2f px");
            recomp::menu::ItemTooltip("Horizontal instability: every line locks a little off (game pixels)");
            changed |= sliderF("Hum bar", s.hum, 0.0f, 1.0f);
            recomp::menu::ItemTooltip("Mains hum: a soft dark band that rolls slowly up the picture (60 Hz against\n"
                                  "the 59.94 Hz fields: one turn every 17 seconds)");
            changed |= sliderF("Interference", s.interference, 0.0f, 1.0f);
            recomp::menu::ItemTooltip("Beat with another carrier: fine moving diagonal bars");
        }
        if (recomp::menu::Button("Reset signal")) {
            applySignal(s, s.signal);
            changed = true;
        }
        recomp::menu::ItemTooltip("Puts the signal values back to the defaults of the chosen signal");
        ImGui::TreePop();
    }
    if (ImGui::TreeNode("CRT advanced")) {
        recomp::menu::SeparatorText("Beam");
        custom |= sliderF("Min beam width", s.beamMinSigma, 0.05f, 0.6f);
        recomp::menu::ItemTooltip("Scanline beam sigma for dark colors, as a fraction of the line spacing");
        custom |= sliderF("Max beam width", s.beamMaxSigma, 0.05f, 0.8f);
        recomp::menu::ItemTooltip("Beam sigma for full brightness: bright lines bloom wider");
        custom |= sliderF("Beam width response", s.beamSpotPower, 0.1f, 4.0f);
        recomp::menu::ItemTooltip("How quickly the beam widens with brightness (power curve)");
        custom |= sliderF("Min beam shape", s.beamMinShape, 2.0f, 8.0f);
        recomp::menu::ItemTooltip("Beam profile exponent for dark colors: 2 = gaussian, higher = flatter top, sharper edge");
        custom |= sliderF("Max beam shape", s.beamMaxShape, 2.0f, 8.0f);
        recomp::menu::ItemTooltip("Beam profile exponent for full brightness");
        custom |= sliderF("Beam shape response", s.beamShapePower, 0.1f, 4.0f);
        recomp::menu::ItemTooltip("How quickly the beam profile changes with brightness (power curve)");
        custom |= sliderF("Horizontal softness", s.horizontalSoftness, 0.0f, 2.0f, "%.2f px");
        recomp::menu::ItemTooltip("Horizontal spot size (gaussian sigma) in game pixels");

        recomp::menu::SeparatorText("Phosphor mask");
        static const char* kMasks[] = {"Aperture grille", "Slot mask", "Shadow mask", "None"};
        custom |= comboE("Mask type", s.mask, kMasks, 4);
        recomp::menu::ItemTooltip("Pattern of the red, green and blue phosphors");
        custom |= sliderF("Mask size", s.maskSize, 1.0f, 12.0f, "%.1f px");
        recomp::menu::ItemTooltip("Width of one RGB triad in output pixels (3 = one pixel per phosphor)");
        custom |= sliderF("Mask strength", s.maskStrength, 0.0f, 1.0f);
        recomp::menu::ItemTooltip("How visible the phosphor pattern is (0 = none)");

        recomp::menu::SeparatorText("Glow");
        custom |= sliderF("Halation", s.halation, 0.0f, 0.5f, "%.3f");
        recomp::menu::ItemTooltip("Light scattered inside the tube glass (local glow, before the mask)");
        custom |= sliderF("Diffusion", s.diffusion, 0.0f, 0.5f, "%.3f");
        recomp::menu::ItemTooltip("Wide haze from light diffused across the faceplate");
        custom |= sliderF("Bloom", s.bloom, 0.0f, 1.0f);
        recomp::menu::ItemTooltip("Glow around bright parts of the picture");
        custom |= sliderF("Brightness boost", s.brightness, 0.5f, 3.0f, "%.2fx");
        recomp::menu::ItemTooltip("Makes up for the light the scanlines and mask take away");

        recomp::menu::SeparatorText("Convergence (game pixels)");
        custom |= ImGui::SliderFloat2("Red offset", &s.convergence[0], -2.0f, 2.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
        recomp::menu::ItemTooltip("Horizontal and vertical shift of the red beam (misconverged tubes)");
        custom |= ImGui::SliderFloat2("Green offset", &s.convergence[2], -2.0f, 2.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
        recomp::menu::ItemTooltip("Horizontal and vertical shift of the green beam");
        custom |= ImGui::SliderFloat2("Blue offset", &s.convergence[4], -2.0f, 2.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
        recomp::menu::ItemTooltip("Horizontal and vertical shift of the blue beam");

        recomp::menu::SeparatorText("Gamma and geometry");
        custom |= sliderF("CRT gamma", s.gammaIn, 1.8f, 3.0f);
        recomp::menu::ItemTooltip("Gamma of the emulated tube (decodes the game image to linear light)");
        custom |= sliderF("Display gamma", s.gammaOut, 1.8f, 3.0f);
        recomp::menu::ItemTooltip("Gamma of your monitor (encodes the result for it)");
        custom |= sliderF("Curvature", s.curvature, 0.0f, 0.3f, "%.3f");
        recomp::menu::ItemTooltip("Bulge of the screen glass (0 = flat)");
        custom |= sliderF("Corner radius", s.cornerRadius, 0.0f, 0.2f, "%.3f");
        recomp::menu::ItemTooltip("Rounding of the picture's corners (0 = square)");
        if (recomp::menu::Button("Reset to preset")) {
            applyPreset(s, s.preset == CrtPreset::Custom ? CrtPreset::ApertureGrille : s.preset);
            changed = true;
        }
        recomp::menu::ItemTooltip("Puts the advanced values back to the chosen preset (Aperture grille if Custom)");
        ImGui::TreePop();
    }
    ImGui::Unindent();
    ImGui::EndDisabled();
    if (custom) s.preset = CrtPreset::Custom;
    return changed || custom;
}

}  // namespace recomp::crt
