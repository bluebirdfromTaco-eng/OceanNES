// Runs the Metal filter shaders on the CPU against real emulator frames.
//  - checks that the NTSC composite decode of flat colours reproduces the
//    emulator's generated palette (built-in and custom-palette paths)
//  - renders each filter to PPM files for visual inspection
#include <cstdio>
#include <cstdlib>
#include <cstring>
extern "C" {
#include "../../src/nes.h"
}
#include "metal_shim.h"
using namespace metal;
#include "shader_cpu.inc"

static texture2d<ushort> idxtex;
static texture2d<float> paltex;

static void set_palette(const uint32_t *p) {
    paltex.w = 512; paltex.h = 1; paltex.d.resize(512);
    for (int i = 0; i < 512; i++) paltex.d[i] = (float4){((p[i] >> 16) & 255) / 255.f, ((p[i] >> 8) & 255) / 255.f, (p[i] & 255) / 255.f, 1};
}

static texture2d<float> run_ntsc(NtscParams prm) {
    texture2d<float> out; out.w = 1024; out.h = 240; out.d.resize(1024 * 240);
    for (int y = 0; y < 240; y++) for (int x = 0; x < 1024; x++) {
        VOut v; v.pos = (float4){x + 0.5f, y + 0.5f, 0, 1}; v.uv = (float2){0, 0};
        out.d[y * 1024 + x] = fs_ntsc(v, idxtex, paltex, prm);
    }
    return out;
}
static texture2d<float> run_palette() {
    texture2d<float> out; out.w = 256; out.h = 240; out.d.resize(256 * 240);
    for (int y = 0; y < 240; y++) for (int x = 0; x < 256; x++) {
        VOut v; v.pos = (float4){x + 0.5f, y + 0.5f, 0, 1};
        out.d[y * 256 + x] = fs_palette(v, idxtex, paltex);
    }
    return out;
}
static void run_out(const texture2d<float> &src, OutParams u, int W, int H, const char *path) {
    FILE *f = fopen(path, "wb"); fprintf(f, "P6\n%d %d\n255\n", W, H);
    int bad = 0; double sum = 0;
    for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) {
        VOut v; v.pos = (float4){x + 0.5f, y + 0.5f, 0, 1}; v.uv = (float2){(x + 0.5f) / W, (y + 0.5f) / H};
        float4 c = fs_out(v, src, u);
        if (!std::isfinite(c.x) || !std::isfinite(c.y) || !std::isfinite(c.z)) bad++;
        unsigned char b[3] = { (unsigned char)(clamp(c.x, 0.f, 1.f) * 255), (unsigned char)(clamp(c.y, 0.f, 1.f) * 255), (unsigned char)(clamp(c.z, 0.f, 1.f) * 255) };
        sum += (b[0] + b[1] + b[2]) / 3.0;
        fwrite(b, 1, 3, f);
    }
    fclose(f);
    printf("  %-34s nonfinite=%d mean=%.1f\n", path, bad, sum / (W * H));
}

int main(int argc, char **argv) {
    static Nes nes;
    uint32_t pal[512];
    palette_generate(pal);
    set_palette(pal);
    idxtex.w = 256; idxtex.h = 240; idxtex.d.resize(256 * 240);

    /* 1. flat colour bars: every colour (incl. emphasis) in 16-pixel-wide bars */
    double worst = 0, worst_c = 0;
    for (int custom = 0; custom < 2; custom++) {
        worst = 0;
        for (int base = 0; base < 512; base += 16 * 15) {
            for (int y = 0; y < 240; y++) for (int x = 0; x < 256; x++) {
                int bar = x / 16; int c = base + bar + (y / 16) * 16; if (c > 511) c = 0x0F;
                idxtex.d[y * 256 + x] = (ushort)c;
            }
            NtscParams prm = { 3.8f, 3.8f, custom ? 1.0f : 0.6f, 0, 1, 0, custom, 0 };
            texture2d<float> o = run_ntsc(prm);
            for (int y = 8; y < 240; y += 16) for (int bar = 1; bar < 15; bar++) {
                int c = base + bar + (y / 16) * 16; if (c > 511) continue;
                float4 v = o.d[y * 1024 + bar * 64 + 32];
                uint32_t r = pal[c];
                double e = std::fabs(v.x * 255 - ((r >> 16) & 255)) + std::fabs(v.y * 255 - ((r >> 8) & 255)) + std::fabs(v.z * 255 - (r & 255));
                if (e > worst) { worst = e; worst_c = c; }
            }
        }
        printf("NTSC %s path: worst flat-colour error vs palette = %.1f (sum of RGB /255 units, colour $%03X)\n",
               custom ? "custom-palette" : "built-in", worst, (unsigned)worst_c);
    }

    /* 2. render a real game frame through every filter */
    if (argc > 1) {
        FILE *f = fopen(argv[1], "rb"); fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
        uint8_t *d = (uint8_t *)malloc(sz); if (fread(d, 1, sz, f) != (size_t)sz) return 1; fclose(f);
        char err[128]; nes_set_sample_rate(&nes, 48000); nes_load(&nes, d, sz, err, sizeof err);
        int frames = argc > 2 ? atoi(argv[2]) : 600;
        const char *script = argc > 3 ? argv[3] : "";
        for (int fr = 0; fr < frames; fr++) {
            nes.pad_state[0] = 0;
            const char *q = script; int f0, m, dur, k;
            while (sscanf(q, "%d:%x:%d%n", &f0, &m, &dur, &k) == 3) { if (fr >= f0 && fr < f0 + dur) nes.pad_state[0] |= (uint8_t)m; q += k; if (*q == ',') q++; }
            nes_run_frame(&nes); float t[4096]; while (nes_read_audio(&nes, t, 4096) > 0) {}
        }
        memcpy(idxtex.d.data(), nes.ppu.fb, sizeof nes.ppu.fb);
        int ph = nes.ppu.frame_phase;
        NtscParams prm = { 3.8f, 3.8f, 0.6f, 0, 1, ph, 0, 0 };
        texture2d<float> nt = run_ntsc(prm);
        texture2d<float> pl = run_palette();
        int W = 896, H = 672;    /* 4:3 at 3x */
        float v0 = 8 / 240.f, v1 = 232 / 240.f;
        OutParams sharp = { (float2){256, 240}, (float2){(float)W, (float)H}, v0, v1, 0, 0, 0, 0, 0, 0 };
        OutParams crt = { (float2){256, 240}, (float2){(float)W, (float)H}, v0, v1, 2, 1, 0.06f, 0.7f, 0.6f, 0 };
        OutParams comp = { (float2){1024, 240}, (float2){(float)W, (float)H}, v0, v1, 1, 0, 0, 0, 0, 0 };
        OutParams compcrt = crt; compcrt.src_size = (float2){1024, 240};
        printf("frame phase %d\n", ph);
        run_out(pl, sharp, W, H, "/tmp/claude-0/f_sharp.ppm");
        run_out(pl, crt, W, H, "/tmp/claude-0/f_crt.ppm");
        run_out(nt, comp, W, H, "/tmp/claude-0/f_composite.ppm");
        run_out(nt, compcrt, W, H, "/tmp/claude-0/f_compcrt.ppm");
    }
    return 0;
}
