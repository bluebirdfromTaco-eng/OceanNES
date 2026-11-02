#define _GNU_SOURCE
/* Headless test-ROM runner: understands the blargg $6000 status protocol,
 * and can dump a screenshot (PPM) for visual checks. */
#include "../src/nes.h"
#include <stdio.h>
#include <stdlib.h>
#define _GNU_SOURCE
#include <string.h>

static Nes nes;

static void save_ppm(const char *path) {
    static uint32_t px[256 * 240];
    nes_render_rgb(&nes, px);
    FILE *f = fopen(path, "wb"); if (!f) return;
    fprintf(f, "P6\n256 240\n255\n");
    for (int i = 0; i < 256 * 240; i++) { uint8_t c[3] = { px[i] >> 16, px[i] >> 8, px[i] }; fwrite(c, 1, 3, f); }
    fclose(f);
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: runtest rom [frames] [shot.ppm]\n"); return 2; }
    FILE *f = fopen(argv[1], "rb"); if (!f) { perror(argv[1]); return 2; }
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *d = malloc(sz); if (fread(d, 1, sz, f) != (size_t)sz) return 2; fclose(f);
    char err[256];
    nes_set_sample_rate(&nes, 44100);
    if (!nes_load(&nes, d, sz, err, sizeof err)) { printf("LOAD FAIL: %s\n", err); return 2; }
    int max = argc > 2 ? atoi(argv[2]) : 3000;
    const char *shot = argc > 3 ? argv[3] : NULL;
    int reset_at = -1; bool protocol = false; int status = -1; bool armed = true;
    float tmp[4096];
    const char *script = argc > 4 ? argv[4] : "";
    for (int fr = 0; fr < max; fr++) {
        nes.pad_state[0] = 0;
        { const char *q = script; int f0, m, dur, k;
          while (sscanf(q, "%d:%x:%d%n", &f0, &m, &dur, &k) == 3) { if (fr >= f0 && fr < f0 + dur) nes.pad_state[0] |= (uint8_t)m; q += k; if (*q == ',') q++; } }
        nes_run_frame(&nes);
        while (nes_read_audio(&nes, tmp, 4096) > 0) {}
        uint8_t s0 = bus_peek(&nes, 0x6001), s1 = bus_peek(&nes, 0x6002), s2 = bus_peek(&nes, 0x6003);
        if (s0 == 0xDE && s1 == 0xB0 && s2 == 0x61) {
            protocol = true;
            uint8_t st = bus_peek(&nes, 0x6000);
            if (st != 0x81) armed = true;
            if (st == 0x81 && reset_at < 0 && armed) reset_at = fr + 10;
            if (reset_at >= 0 && fr == reset_at) { nes_reset(&nes); reset_at = -1; armed = false; }
            if (st < 0x80) { status = st; break; }
        }
    }
    if (protocol) {
        char text[1024]; int i = 0;
        for (; i < 1023; i++) { uint8_t c = bus_peek(&nes, (uint16_t)(0x6004 + i)); if (!c) break; text[i] = (char)c; }
        text[i] = 0;
        printf("%s\n", text);
        printf("RESULT: %s (%d)\n", status == 0 ? "PASS" : status < 0 ? "TIMEOUT" : "FAIL", status);
    } else {
        if (memmem(nes.ppu.ciram, 2048, "$01", 3)) { printf("RESULT: PASS (screen $01)\n"); status = 0; }
        else { uint8_t r = bus_peek(&nes, 0xF8); printf("RESULT: %s ($F8=%02X)\n", r == 1 ? "PASS" : "CHECK", r); status = (r == 1) ? 0 : 1; }
    }
    if (shot) save_ppm(shot);
    return status == 0 ? 0 : 1;
}
