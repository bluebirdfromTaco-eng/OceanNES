/* Run AccuracyCoin page and print nametable text rows */
#include "../src/nes.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static Nes nes;
static char map[256];
int main(int argc, char **argv) {
    for (int i = 0; i < 256; i++) { int k = i & 0x7F; map[i] = k < 10 ? (char)('0' + k) : k < 0x24 ? (char)('A' + k - 10) : k == 0x24 ? ' ' : k == 0x31 ? '-' : k == 0x25 ? '$' : k == 0x26 ? ',' : '?'; }
    { const char *m = getenv("MAP"); if (m) { FILE *mf = fopen(m, "rb"); if (mf) { if (fread(map, 1, 256, mf)) {} fclose(mf); } } }
    FILE *f = fopen(argv[1], "rb"); fseek(f, 0, 2); long sz = ftell(f); fseek(f, 0, 0);
    uint8_t *d = malloc(sz); if (fread(d, 1, sz, f) != (size_t)sz) return 1; fclose(f);
    char e[100]; nes_load(&nes, d, sz, e, 100);
    int page = atoi(argv[2]), frames = argc > 3 ? atoi(argv[3]) : 2400, test = argc > 4 ? atoi(argv[4]) : -1;
    /* script: Right (page-1) times, then (Down test times) then A */
    int t = 60; int ev[64][2]; int ne = 0;
    for (int i = 1; i < page; i++) { ev[ne][0] = t; ev[ne][1] = 0x80; ne++; t += 10; }
    if (test >= 0) for (int i = 0; i <= test; i++) { ev[ne][0] = t; ev[ne][1] = 0x20; ne++; t += 10; }
    t += 10; ev[ne][0] = t; ev[ne][1] = 0x01; ne++;
    for (int fr = 0; fr < frames; fr++) {
        nes.pad_state[0] = 0;
        for (int i = 0; i < ne; i++) if (fr >= ev[i][0] && fr < ev[i][0] + 3) nes.pad_state[0] = (uint8_t)ev[i][1];
        nes_run_frame(&nes);
        if (getenv("SHOTFRAME") && fr == atoi(getenv("SHOTFRAME"))) {
            static uint32_t px[256*240]; nes_render_rgb(&nes, px);
            FILE *sf = fopen(getenv("SHOTFILE"), "wb"); fprintf(sf, "P6\n256 240\n255\n");
            for (int i = 0; i < 256*240; i++) { uint8_t c[3] = { px[i] >> 16, px[i] >> 8, px[i] }; fwrite(c, 1, 3, sf); } fclose(sf);
        }
        float tmp[4096]; while (nes_read_audio(&nes, tmp, 4096) > 0) {}
    }
    for (int row = 0; row < 30; row++) {
        char line[33]; int any = 0;
        for (int c = 0; c < 32; c++) { uint8_t b = nes.ppu.ciram[row * 32 + c]; char ch = map[b]; if (ch != ' ') any = 1; line[c] = ch; }
        line[32] = 0; if (any) printf("%s\n", line);
        if (getenv("HEX")) { for (int c = 0; c < 32; c++) printf("%02X ", nes.ppu.ciram[row * 32 + c]); printf("\n"); }
    }
    if (getenv("DUMP")) { int base = (int)strtol(getenv("DUMP"), 0, 16); for (int i = 0; i < 64; i++) { printf("%02X ", nes.ram[(base + i) & 0x7FF]); if (i % 16 == 15) printf("\n"); } }
    return 0;
}
