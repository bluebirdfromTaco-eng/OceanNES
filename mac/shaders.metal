// oceanNES video filters (Metal Shading Language).
//
// Pass 1 builds a source image from the PPU's raw output (9-bit colour index
// per pixel = 6-bit colour + 3 emphasis bits):
//   fs_palette - straight palette lookup (256 x 240)
//   fs_ntsc    - NTSC composite simulation (1024 x 240): every pixel is turned
//                into the 2C02's actual composite waveform (8 samples per pixel,
//                12 samples per colour-subcarrier cycle, the correct phase for
//                each line and frame) and decoded back the way a TV does, with
//                a luma filter and a wider chroma filter. That produces the
//                real artifacts: colour bleed, dot crawl, rainbow fringes.
// Pass 2 scales the source onto the screen:
//   fs_out     - sharp pixels, smooth, or CRT (beam-shaped scanlines,
//                phosphor mask, glow, curvature, gamma-correct blending)

#include <metal_stdlib>
using namespace metal;

struct VOut {
    float4 pos [[position]];
    float2 uv;
};

// rect = (x0, y0, x1, y1) in normalised device coordinates
vertex VOut vs_quad(uint vid [[vertex_id]], constant float4 &rect [[buffer(0)]]) {
    float2 p[4] = { float2(rect.x, rect.y), float2(rect.z, rect.y), float2(rect.x, rect.w), float2(rect.z, rect.w) };
    float2 t[4] = { float2(0.0f, 1.0f), float2(1.0f, 1.0f), float2(0.0f, 0.0f), float2(1.0f, 0.0f) };
    VOut o;
    o.pos = float4(p[vid], 0.0f, 1.0f);
    o.uv = t[vid];
    return o;
}

// ---------------------------------------------------------------- pass 1

fragment float4 fs_palette(VOut in [[stage_in]],
                           texture2d<ushort> idx [[texture(0)]],
                           texture2d<float> pal [[texture(1)]]) {
    uint2 c = uint2(in.pos.xy);
    uint i = uint(idx.read(c).r) & 511u;
    return float4(pal.read(uint2(i, 0u)).rgb, 1.0f);
}

struct NtscParams {
    float enc_hue;   // carrier reference used to encode custom palettes
    float dec_hue;   // decoder hue (3.8 + user adjustment), in 30-degree units
    float sat;       // chroma gain
    float bri;       // brightness offset
    float con;       // contrast
    int   phase0;    // subcarrier phase (0-11) at the start of line 0
    int   custom;    // 1 = custom palette: re-encode its RGB as composite
    int   pad;
};

constant float kLevels[8] = { 0.350f, 0.518f, 0.962f, 1.550f, 1.094f, 1.506f, 1.962f, 1.962f };

// 2C02 composite output for colour index `pix` at subcarrier phase `phase`,
// normalised so black = 0 and white = 1
static float nes_level(uint pix, int phase) {
    int color = int(pix & 15u);
    int level = int((pix >> 4) & 3u);
    int emph = int(pix >> 6);
    if (color > 13) level = 1;
    float lo = kLevels[level];
    float hi = kLevels[4 + level];
    if (color == 0) lo = hi;
    if (color > 12) hi = lo;
    float s = ((color + phase) % 12) < 6 ? hi : lo;
    bool att = ((emph & 1) != 0 && (phase % 12) < 6) ||
               ((emph & 2) != 0 && ((4 + phase) % 12) < 6) ||
               ((emph & 4) != 0 && ((8 + phase) % 12) < 6);
    if (att) s *= 0.746f;
    return (s - 0.518f) / (1.962f - 0.518f);
}

static int sample_phase(int phase0, int line, int s) {
    // 341 dots x 8 samples per line = 2728 = 4 (mod 12); pixel x is output on dot x+1
    return (phase0 + line * 4 + 8 + s) % 12;
}

static float composite_sample(texture2d<ushort> idx, texture2d<float> pal, constant NtscParams &p, int line, int s) {
    int sc = clamp(s, 0, 2047);
    int x = sc >> 3;
    uint pix = uint(idx.read(uint2(uint(x), uint(line))).r) & 511u;
    int ph = sample_phase(p.phase0, line, sc);
    if (p.custom == 0) return nes_level(pix, ph);
    float3 rgb = pal.read(uint2(pix, 0u)).rgb;
    float Y = dot(rgb, float3(0.299f, 0.587f, 0.114f));
    float I = dot(rgb, float3(0.595716f, -0.274453f, -0.321263f));
    float Q = dot(rgb, float3(0.211456f, -0.522591f, 0.311135f));
    float th = M_PI_F * (float(ph) + p.enc_hue) / 6.0f;
    return Y + I * cos(th) + Q * sin(th);
}

fragment float4 fs_ntsc(VOut in [[stage_in]],
                        texture2d<ushort> idx [[texture(0)]],
                        texture2d<float> pal [[texture(1)]],
                        constant NtscParams &p [[buffer(0)]]) {
    int line = int(in.pos.y);
    int c = int(in.pos.x) * 2 + 1;              // 1024 outputs across 2048 samples
    float smp[24];
    for (int k = 0; k < 24; k++) smp[k] = composite_sample(idx, pal, p, line, c - 12 + k);
    // luma: 12-sample box (one full subcarrier cycle) removes the chroma
    float Y = 0.0f;
    for (int k = 6; k < 18; k++) Y += smp[k];
    Y /= 12.0f;
    // chroma: synchronous demodulation with a triangular (narrower band) filter
    float I = 0.0f, Q = 0.0f, ws = 0.0f;
    for (int k = 0; k < 24; k++) {
        float w = 12.0f - abs(float(k) - 11.5f);
        int ph = sample_phase(p.phase0, line, clamp(c - 12 + k, 0, 2047));
        float th = M_PI_F * (float(ph) + p.dec_hue) / 6.0f;
        I += w * smp[k] * cos(th);
        Q += w * smp[k] * sin(th);
        ws += w;
    }
    I = 2.0f * I / ws * p.sat;
    Q = 2.0f * Q / ws * p.sat;
    float3 rgb = float3(Y + 0.946882f * I + 0.623557f * Q,
                        Y - 0.274788f * I - 0.635691f * Q,
                        Y - 1.108545f * I + 1.709007f * Q);
    rgb = (rgb - 0.5f) * p.con + 0.5f + p.bri;
    return float4(clamp(rgb, float3(0.0f), float3(1.0f)), 1.0f);
}

// ---------------------------------------------------------------- pass 2

struct OutParams {
    float2 src_size;    // source texture size in texels
    float2 out_size;    // size of the image rectangle in drawable pixels
    float  v0, v1;      // vertical range of the source to show (overscan crop)
    int    mode;        // 0 sharp, 1 smooth, 2 CRT
    int    mask;        // 0 none, 1 aperture grille, 2 slot mask
    float  curvature;   // 0 = flat
    float  scan;        // scanline strength 0..1
    float  glow;        // 0..1
    float  pad;
};

static float3 to_lin(float3 c) { return pow(max(c, float3(0.0f)), float3(2.2f)); }
static float3 to_srgb(float3 c) { return pow(clamp(c, float3(0.0f), float3(1.0f)), float3(1.0f / 2.2f)); }

static float3 beam(float d, float3 c, float scan) {
    // brighter lines bloom wider, like a real electron beam
    float lum = dot(c, float3(0.299f, 0.587f, 0.114f));
    float sigma = mix(0.30f, 0.46f, sqrt(clamp(lum, 0.0f, 1.0f)));
    sigma = mix(1.2f, sigma, scan);
    // normalised so each line emits the same total light as the source line:
    // scanlines darken the gaps without changing the picture's brightness
    return float3(exp(-0.5f * (d / sigma) * (d / sigma)) / (sigma * 2.5066283f));
}

fragment float4 fs_out(VOut in [[stage_in]],
                       texture2d<float> src [[texture(0)]],
                       constant OutParams &u [[buffer(0)]]) {
    constexpr sampler lin(filter::linear, address::clamp_to_edge);
    float2 uv = in.uv;
    float rows = (u.v1 - u.v0) * u.src_size.y;

    if (u.mode == 0 || u.mode == 1) {
        float2 tuv = float2(uv.x, u.v0 + uv.y * (u.v1 - u.v0));
        if (u.mode == 0) {
            // sharp bilinear: crisp pixels that stay even at non-integer scales
            float2 vis = float2(u.src_size.x, rows);
            float2 scale = max(floor(u.out_size / vis), float2(1.0f));
            float2 texel = tuv * u.src_size;
            float2 tf = floor(texel);
            float2 cd = fract(texel) - 0.5f;
            float2 rr = float2(0.5f) - 0.5f / scale;
            float2 f = (cd - clamp(cd, -rr, rr)) * scale + 0.5f;
            tuv = (tf + f) / u.src_size;
        }
        return float4(src.sample(lin, tuv).rgb, 1.0f);
    }

    // ---- CRT
    float2 cc = uv * 2.0f - 1.0f;
    if (u.curvature > 0.0f) {
        cc *= 1.0f + u.curvature * float2(cc.y * cc.y, cc.x * cc.x);
        uv = cc * 0.5f + 0.5f;
        if (uv.x < 0.0f || uv.x > 1.0f || uv.y < 0.0f || uv.y > 1.0f) return float4(0.0f, 0.0f, 0.0f, 1.0f);
    }
    float y = uv.y * rows - 0.5f;
    float l0 = floor(y);
    float f = y - l0;
    float base = u.v0 * u.src_size.y;
    float dx = 1.0f / u.src_size.x;
    float y0 = (base + clamp(l0, 0.0f, rows - 1.0f) + 0.5f) / u.src_size.y;
    float y1 = (base + clamp(l0 + 1.0f, 0.0f, rows - 1.0f) + 0.5f) / u.src_size.y;
    float3 c0 = to_lin(src.sample(lin, float2(uv.x, y0)).rgb);
    float3 c1 = to_lin(src.sample(lin, float2(uv.x, y1)).rgb);
    float3 col = c0 * beam(f, c0, u.scan) + c1 * beam(1.0f - f, c1, u.scan);

    if (u.glow > 0.0f) {
        float3 g = float3(0.0f);
        for (int k = -3; k <= 3; k++) {
            float o = float(k) * 2.0f * dx;
            g += to_lin(src.sample(lin, float2(uv.x + o, y0)).rgb) + to_lin(src.sample(lin, float2(uv.x + o, y1)).rgb);
        }
        col += g / 14.0f * 0.10f * u.glow;
    }

    if (u.mask != 0) {
        int px = int(in.pos.x), py = int(in.pos.y);
        float lo = 0.70f;
        float3 m = float3(lo);
        int t = (u.mask == 2) ? (px + ((py / 3) & 1) * 3) / 2 % 3 : px % 3;
        if (t == 0) m.r = 1.0f; else if (t == 1) m.g = 1.0f; else m.b = 1.0f;
        if (u.mask == 2 && (py % 3) == 0) m *= 0.85f;
        col *= m / ((1.0f + 2.0f * lo) / 3.0f);
    }

    if (u.curvature > 0.0f) {
        // soft rounded corners and a gentle vignette
        float2 e = abs(cc);
        float2 d = max(e - float2(1.0f - 0.06f), float2(0.0f)) / 0.06f;
        float corner = 1.0f - smoothstep(0.85f, 1.0f, length(d));
        col *= corner * (1.0f - 0.18f * dot(cc * 0.7f, cc * 0.7f));
    }
    return float4(to_srgb(col), 1.0f);
}
