/*
 * Cartridge loading (iNES / NES 2.0) and mappers.
 * Each mapper keeps its registers in Cart.reg[] and rebuilds the memory map
 * in cart_sync(), which also makes save-states trivial.
 */
#include "nes.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#define K (&n->cart)

/* ------------------------------------------------------------ CRC32 */
static uint32_t crc32(const uint8_t *d, size_t len) {
    uint32_t c = 0xFFFFFFFF;
    for (size_t i = 0; i < len; i++) {
        c ^= d[i];
        for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320 & (0u - (c & 1)));
    }
    return ~c;
}

/* ------------------------------------------------------------ banking helpers */
static void prg8(Nes *n, int slot, int bank) {
    int cnt = (int)(K->prg_size / 0x2000); if (cnt < 1) cnt = 1;
    if (bank < 0) bank += cnt;
    bank %= cnt; if (bank < 0) bank += cnt;
    K->prg_off[slot] = (uint32_t)bank * 0x2000u;
}
static void prg16(Nes *n, int slot, int bank) {
    int cnt = (int)(K->prg_size / 0x4000); if (cnt < 1) { prg8(n, slot * 2, 0); prg8(n, slot * 2 + 1, 0); return; }
    if (bank < 0) bank += cnt;
    bank %= cnt;
    prg8(n, slot * 2, bank * 2); prg8(n, slot * 2 + 1, bank * 2 + 1);
}
static void prg32(Nes *n, int bank) {
    int cnt = (int)(K->prg_size / 0x8000);
    if (cnt < 1) { prg16(n, 0, 0); prg16(n, 1, 0); return; }
    if (bank < 0) bank += cnt;
    bank %= cnt;
    for (int i = 0; i < 4; i++) prg8(n, i, bank * 4 + i);
}
static void chr1(Nes *n, int slot, int bank) {
    int cnt = (int)(K->chr_size / 0x400); if (cnt < 1) cnt = 1;
    if (bank < 0) bank += cnt;
    bank %= cnt;
    K->chr_off[slot] = (uint32_t)bank * 0x400u;
}
static void chr2(Nes *n, int slot, int bank) { chr1(n, slot * 2, bank * 2); chr1(n, slot * 2 + 1, bank * 2 + 1); }
static void chr4(Nes *n, int slot, int bank) { for (int i = 0; i < 4; i++) chr1(n, slot * 4 + i, bank * 4 + i); }
static void chr8(Nes *n, int bank) { for (int i = 0; i < 8; i++) chr1(n, i, bank * 8 + i); }

static void mirror(Nes *n, int m) {
    uint8_t *a = n->ppu.ciram, *b = n->ppu.ciram + 0x400;
    switch (m) {
    case MIRROR_H: K->nt[0] = a; K->nt[1] = a; K->nt[2] = b; K->nt[3] = b; break;
    case MIRROR_V: K->nt[0] = a; K->nt[1] = b; K->nt[2] = a; K->nt[3] = b; break;
    case MIRROR_1A: K->nt[0] = K->nt[1] = K->nt[2] = K->nt[3] = a; break;
    case MIRROR_1B: K->nt[0] = K->nt[1] = K->nt[2] = K->nt[3] = b; break;
    case MIRROR_4:
        K->nt[0] = a; K->nt[1] = b; K->nt[2] = K->exram; K->nt[3] = K->exram + 0x400; break;
    }
}
static void default_mirror(Nes *n) { mirror(n, K->mirroring); }
static void nt_custom(Nes *n, int i, int page) { K->nt[i] = n->ppu.ciram + (page & 1) * 0x400; }

static void wram_default(Nes *n) {
    K->wram_off = K->wram_size ? 0 : -1;
    K->wram_en = true; K->wram_wp = false;
}

/* ------------------------------------------------------------ supported list */
static const int supported[] = { 0, 1, 2, 3, 4, 5, 7, 9, 10, 11, 13, 21, 22, 23, 24, 25, 26, 28, 33, 34, 48, 64, 66, 69, 70, 71, 78, 79, 87, 89, 93, 94, 118, 119, 140, 152, 180, 184, 185, 206, 232, -1 };
bool cart_supported(int m) { for (int i = 0; supported[i] >= 0; i++) if (supported[i] == m) return true; return false; }

/* ------------------------------------------------------------ MMC1 */
static void mmc1_sync(Nes *n) {
    uint8_t ctl = K->reg[0], c0 = K->reg[1], c1 = K->reg[2], pr = K->reg[3];
    static const int mir[4] = { MIRROR_1A, MIRROR_1B, MIRROR_V, MIRROR_H };
    mirror(n, mir[ctl & 3]);
    int outer = 0;
    if (K->prg_size > 0x40000) outer = (c0 & 0x10);   /* SUROM/SXROM 512K */
    int bank = pr & 0x0F;
    switch ((ctl >> 2) & 3) {
    case 0: case 1:
        prg16(n, 0, ((bank & 0x0E) | outer)); prg16(n, 1, ((bank & 0x0E) | outer) + 1); break;
    case 2: prg16(n, 0, outer); prg16(n, 1, bank | outer); break;
    case 3: prg16(n, 0, bank | outer); prg16(n, 1, 0x0F | outer); break;
    }
    if (ctl & 0x10) { chr4(n, 0, c0); chr4(n, 1, c1); }
    else { chr4(n, 0, c0 & 0x1E); chr4(n, 1, (c0 & 0x1E) | 1); }
    K->wram_en = !(pr & 0x10) || K->submapper == 5;
    if (K->wram_size > 0x2000) {
        int wb = (K->wram_size >= 0x8000) ? ((c0 >> 2) & 3) : ((c0 >> 3) & 1);
        K->wram_off = (int32_t)((wb * 0x2000) % K->wram_size);
    } else K->wram_off = K->wram_size ? 0 : -1;
}
static void mmc1_write(Nes *n, uint16_t a, uint8_t v) {
    if (n->cpu.cycles - K->last_write_cycle < 2 && K->last_write_cycle) { K->last_write_cycle = n->cpu.cycles; return; }
    K->last_write_cycle = n->cpu.cycles;
    if (v & 0x80) { K->mmc1_shift = 0; K->mmc1_count = 0; K->reg[0] |= 0x0C; mmc1_sync(n); return; }
    K->mmc1_shift = (uint8_t)(K->mmc1_shift >> 1 | (v & 1) << 4);
    if (++K->mmc1_count == 5) {
        K->reg[(a >> 13) & 3] = K->mmc1_shift;
        K->mmc1_shift = 0; K->mmc1_count = 0;
        mmc1_sync(n);
    }
}

/* ------------------------------------------------------------ MMC3 */
static void mmc3_sync(Nes *n) {
    uint8_t *r = &K->reg[8];
    uint8_t sel = K->reg[0];
    if (sel & 0x40) { prg8(n, 0, -2); prg8(n, 1, r[7]); prg8(n, 2, r[6]); prg8(n, 3, -1); }
    else { prg8(n, 0, r[6]); prg8(n, 1, r[7]); prg8(n, 2, -2); prg8(n, 3, -1); }
    if (K->mapper == 206) { prg8(n, 0, r[6] & 0x0F); prg8(n, 1, r[7] & 0x0F); prg8(n, 2, -2); prg8(n, 3, -1); }
    int inv = (sel & 0x80) && K->mapper != 206 ? 4 : 0;
    chr1(n, 0 ^ inv, r[0] & 0xFE); chr1(n, 1 ^ inv, r[0] | 1);
    chr1(n, 2 ^ inv, r[1] & 0xFE); chr1(n, 3 ^ inv, r[1] | 1);
    chr1(n, 4 ^ inv, r[2]); chr1(n, 5 ^ inv, r[3]); chr1(n, 6 ^ inv, r[4]); chr1(n, 7 ^ inv, r[5]);
    if (K->mapper == 119) {
        /* TQROM: bank bit 6 selects the 8KB CHR RAM placed after the CHR ROM */
        int b[8] = { r[0] & 0xFE, r[0] | 1, r[1] & 0xFE, r[1] | 1, r[2], r[3], r[4], r[5] };
        for (int i = 0; i < 8; i++) {
            int slot = i ^ inv;
            if (b[i] & 0x40) K->chr_off[slot] = K->chr_rom_size + (uint32_t)(b[i] & 7) * 0x400;
            else K->chr_off[slot] = ((uint32_t)(b[i] & 0x3F) * 0x400) % K->chr_rom_size;
        }
    }
    if (K->mapper == 118) {
        /* TxSROM: CHR bank bit 7 selects CIRAM page */
        if (!(sel & 0x80)) { nt_custom(n, 0, r[0] >> 7); nt_custom(n, 1, r[0] >> 7); nt_custom(n, 2, r[1] >> 7); nt_custom(n, 3, r[1] >> 7); }
        else { nt_custom(n, 0, r[2] >> 7); nt_custom(n, 1, r[3] >> 7); nt_custom(n, 2, r[4] >> 7); nt_custom(n, 3, r[5] >> 7); }
    } else if (K->mirroring == MIRROR_4 || K->mapper == 206) default_mirror(n);
    else mirror(n, (K->reg[1] & 1) ? MIRROR_H : MIRROR_V);
    K->wram_off = K->wram_size ? 0 : -1;
    if (K->nes2 && K->submapper == 0 && K->mapper == 4) {
        K->wram_en = (K->reg[2] & 0x80) != 0;
        K->wram_wp = (K->reg[2] & 0x40) != 0;
    } else {
        /* iNES 1.0 can't tell MMC3 from MMC6 (StarTropics), whose RAM
         * enable bits mean something else, so keep the RAM always on */
        K->wram_en = true; K->wram_wp = false;
    }
}
static void mmc3_write(Nes *n, uint16_t a, uint8_t v) {
    switch (a & 0xE001) {
    case 0x8000: K->reg[0] = v; break;
    case 0x8001: K->reg[8 + (K->reg[0] & 7)] = v; break;
    case 0xA000: K->reg[1] = v; break;
    case 0xA001: K->reg[2] = v; break;
    case 0xC000: K->irq_latch = v; break;
    case 0xC001: K->irq_counter = 0; K->irq_reload = true; break;
    case 0xE000: K->irq_enable = false; cpu_clear_irq(n, IRQ_MAPPER); break;
    case 0xE001: K->irq_enable = true; break;
    }
    mmc3_sync(n);
}
static void mmc3_a12(Nes *n) {
    int before = K->irq_counter;
    if (K->irq_counter == 0 || K->irq_reload) K->irq_counter = K->irq_latch;
    else K->irq_counter--;
    if (K->submapper == 4 || (K->submapper == 1 && K->nes2)) { /* MMC3A / MMC6 behaviour */
        if ((before > 0 || K->irq_reload) && K->irq_counter == 0 && K->irq_enable) cpu_set_irq(n, IRQ_MAPPER);
    } else if (K->irq_counter == 0 && K->irq_enable) cpu_set_irq(n, IRQ_MAPPER);
    K->irq_reload = false;
}

/* ------------------------------------------------------------ RAMBO-1 */
static void rambo_clock(Nes *n) {
    if (K->irq_reload) { K->irq_counter = K->irq_latch + (K->irq_latch ? 1 : 0); K->irq_reload = false; }
    else if (K->irq_counter == 0) K->irq_counter = K->irq_latch;
    else K->irq_counter--;
    if (K->irq_counter == 0 && K->irq_enable) cpu_set_irq(n, IRQ_MAPPER);
}

/* ------------------------------------------------------------ MMC5 */
/* reg map: 0 prg mode,1 chr mode,2/3 ram protect,4 exram mode,5 nt map,6 fill tile,7 fill attr,
 * 8..12 prg regs ($5113-$5117), 16..27 chr regs ($5120-$512B), 28 chr upper, 29 irq scanline,
 * 30 multiplicand, 31 multiplier, 32 split ctl */
typedef struct { uint16_t chr_a[8], chr_b[4]; bool last_b; bool in_frame; int scanline; int nt_reads; uint16_t last_nt; bool irq_pending; int idle; bool sprite8x16; bool fetch_sprites; int tile_x; uint8_t split_tile; } Mmc5Ext;
static Mmc5Ext mmc5;
static uint8_t mmc5_fill_nt[1024];

static void mmc5_sync(Nes *n) {
    uint8_t *r = K->reg;
    /* nametables */
    for (int i = 0; i < 4; i++) {
        int s = (r[5] >> (i * 2)) & 3;
        switch (s) {
        case 0: K->nt[i] = n->ppu.ciram; break;
        case 1: K->nt[i] = n->ppu.ciram + 0x400; break;
        case 2: K->nt[i] = (r[4] <= 1) ? K->exram : mmc5_fill_nt; break;
        case 3: K->nt[i] = mmc5_fill_nt; break;
        }
    }
    memset(mmc5_fill_nt, r[6], 960);
    uint8_t at = (uint8_t)((r[7] & 3) * 0x55);
    memset(mmc5_fill_nt + 960, at, 64);
}
static int mmc5_prg_map(Nes *n, uint16_t a, uint32_t *off) {
    /* returns 1 rom, 0 ram, -1 none */
    uint8_t *r = K->reg;
    int mode = r[0] & 3;
    uint8_t v; int sz;  /* sz in 8K units */
    if (a < 0x8000) { v = r[8] & 0x7F; *off = (uint32_t)(v * 0x2000) % (K->wram_size ? K->wram_size : 1); return K->wram_size ? 0 : -1; }
    switch (mode) {
    case 0: v = r[12] | 0x80; sz = 4; break;
    case 1: v = (a < 0xC000) ? r[10] : (r[12] | 0x80); sz = 2; break;
    case 2: if (a < 0xC000) { v = r[10]; sz = 2; } else if (a < 0xE000) { v = r[11]; sz = 1; } else { v = r[12] | 0x80; sz = 1; } break;
    default: v = r[9 + ((a - 0x8000) >> 13)]; if (a >= 0xE000) v |= 0x80; sz = 1; break;
    }
    int bank = v & 0x7F;
    if (sz == 4) bank &= ~3; else if (sz == 2) bank &= ~1;
    uint32_t within = (uint32_t)(a & (sz * 0x2000 - 1));
    if (v & 0x80) { *off = ((uint32_t)bank * 0x2000 + within) % K->prg_size; return 1; }
    if (!K->wram_size) return -1;
    *off = ((uint32_t)(bank & 7) * 0x2000 + within) % K->wram_size;
    return 0;
}
static uint32_t mmc5_chr_off(Nes *n, uint16_t a, bool use_b) {
    uint8_t mode = K->reg[1] & 3;
    int slot = (a >> 10) & 7;
    int bank;
    if (!use_b) {
        switch (mode) {
        case 0: bank = mmc5.chr_a[7] * 8 + slot; break;
        case 1: bank = mmc5.chr_a[(slot & 4) + 3] * 4 + (slot & 3); break;
        case 2: bank = mmc5.chr_a[(slot & 6) + 1] * 2 + (slot & 1); break;
        default: bank = mmc5.chr_a[slot]; break;
        }
    } else {
        int s4 = slot & 3;
        switch (mode) {
        case 0: bank = mmc5.chr_b[3] * 8 + slot; break;
        case 1: bank = mmc5.chr_b[3] * 4 + s4; break;
        case 2: bank = mmc5.chr_b[(s4 & 2) + 1] * 2 + (s4 & 1); break;
        default: bank = mmc5.chr_b[s4]; break;
        }
    }
    return ((uint32_t)bank * 0x400 + (a & 0x3FF)) % (K->chr_size ? K->chr_size : 0x2000);
}

/* ------------------------------------------------------------ VRC2/4 */
static void vrc_addr(Nes *n, uint16_t a, int *r0, int *r1) {
    int a0, a1;
    switch (K->mapper) {
    case 21: a0 = ((a >> 1) | (a >> 6)) & 1; a1 = ((a >> 2) | (a >> 7)) & 1; break;
    case 22: a0 = (a >> 1) & 1; a1 = a & 1; break;
    case 23: a0 = (a | (a >> 2)) & 1; a1 = ((a >> 1) | (a >> 3)) & 1; break;
    case 25: a0 = ((a >> 1) | (a >> 3)) & 1; a1 = (a | (a >> 2)) & 1; break;
    case 26: a0 = (a >> 1) & 1; a1 = a & 1; break;
    default: a0 = a & 1; a1 = (a >> 1) & 1; break;
    }
    *r0 = a0; *r1 = a1;
}
static void vrc4_sync(Nes *n) {
    uint8_t *r = K->reg;
    bool swap = (r[2] & 2) && K->mapper != 22;
    if (swap) { prg8(n, 0, -2); prg8(n, 2, r[0] & 0x1F); }
    else { prg8(n, 0, r[0] & 0x1F); prg8(n, 2, -2); }
    prg8(n, 1, r[1] & 0x1F); prg8(n, 3, -1);
    for (int i = 0; i < 8; i++) {
        int b = r[16 + i * 2] | (r[17 + i * 2] << 4);
        if (K->mapper == 22) b >>= 1;
        chr1(n, i, b);
    }
    static const int mir[4] = { MIRROR_V, MIRROR_H, MIRROR_1A, MIRROR_1B };
    if (K->mapper == 22 || (K->mapper == 23 && K->submapper == 3) || (K->mapper == 25 && K->submapper == 3))
        mirror(n, (r[3] & 1) ? MIRROR_H : MIRROR_V);
    else mirror(n, mir[r[3] & 3]);
    K->wram_off = K->wram_size ? 0 : -1;
    K->wram_en = true;
}
static void vrc_irq_ctl(Nes *n, uint8_t v) {
    K->irq_enable2 = v & 1; K->irq_enable = (v & 2) != 0; K->reg[40] = v & 4;
    if (K->irq_enable) { K->irq_counter = K->irq_latch; K->irq_prescaler = 341; }
    cpu_clear_irq(n, IRQ_MAPPER);
}
static void vrc_irq_ack(Nes *n) { K->irq_enable = K->irq_enable2; cpu_clear_irq(n, IRQ_MAPPER); }
static void vrc_irq_clock(Nes *n) {
    if (!K->irq_enable) return;
    K->irq_prescaler -= 3;
    if (K->reg[40] || K->irq_prescaler <= 0) {
        if (!K->reg[40]) K->irq_prescaler += 341;
        if (K->irq_counter == 0xFF) { K->irq_counter = K->irq_latch; cpu_set_irq(n, IRQ_MAPPER); }
        else K->irq_counter++;
    }
}
static void vrc4_write(Nes *n, uint16_t a, uint8_t v) {
    int a0, a1; vrc_addr(n, a, &a0, &a1);
    int sub = a0 | a1 << 1;
    uint8_t *r = K->reg;
    switch (a & 0xF000) {
    case 0x8000: r[0] = v; break;
    case 0x9000:
        if (K->mapper == 22) r[3] = v;
        else if (sub == 2) r[2] = v;
        else if (sub == 0) r[3] = v;
        break;
    case 0xA000: r[1] = v; break;
    case 0xB000: case 0xC000: case 0xD000: case 0xE000: {
        int idx = ((a >> 12) - 0xB) * 2 + (sub >> 1);
        if (sub & 1) r[17 + idx * 2] = v & 0x1F; else r[16 + idx * 2] = v & 0x0F;
        break;
    }
    case 0xF000:
        switch (sub) {
        case 0: K->irq_latch = (K->irq_latch & 0xF0) | (v & 0x0F); break;
        case 1: K->irq_latch = (K->irq_latch & 0x0F) | ((v & 0x0F) << 4); break;
        case 2: vrc_irq_ctl(n, v); break;
        case 3: vrc_irq_ack(n); break;
        }
        break;
    }
    vrc4_sync(n);
}

/* ------------------------------------------------------------ VRC6 */
static void vrc6_sync(Nes *n) {
    uint8_t *r = K->reg;
    prg16(n, 0, r[0] & 0x0F);
    prg8(n, 2, r[1] & 0x1F); prg8(n, 3, -1);
    for (int i = 0; i < 8; i++) chr1(n, i, r[16 + i]);
    static const int mir[4] = { MIRROR_V, MIRROR_H, MIRROR_1A, MIRROR_1B };
    mirror(n, mir[(r[2] >> 2) & 3]);
    K->wram_off = K->wram_size ? 0 : -1;
    K->wram_en = (r[2] & 0x80) != 0;
}
static void vrc6_write(Nes *n, uint16_t a, uint8_t v) {
    int sub = (K->mapper == 26) ? (((a >> 1) & 1) | ((a & 1) << 1)) : (a & 3);
    uint8_t *r = K->reg;
    switch (a & 0xF000) {
    case 0x8000: r[0] = v; break;
    case 0x9000: case 0xA000: case 0xB000: {
        int ch = ((a >> 12) & 0xF) - 9;
        if (ch == 2 && sub == 3) { r[2] = v; break; }
        if (ch == 0 && sub == 3) { K->vrc6.halt = v & 1; K->vrc6.freq_shift = (v & 4) ? 8 : (v & 2) ? 4 : 0; break; }
        K->vrc6.ctl[ch][sub] = v;
        if (sub == 2 && !(v & 0x80)) { if (ch < 2) K->vrc6.step[ch] = 0; else { K->vrc6.saw_step = 0; K->vrc6.saw_acc = 0; } }
        break;
    }
    case 0xC000: r[1] = v; break;
    case 0xD000: r[16 + sub] = v; break;
    case 0xE000: r[20 + sub] = v; break;
    case 0xF000:
        switch (sub) {
        case 0: K->irq_latch = v; break;
        case 1: vrc_irq_ctl(n, v); break;
        case 2: vrc_irq_ack(n); break;
        }
        break;
    }
    vrc6_sync(n);
}
static void vrc6_audio_clock(Nes *n) {
    if (K->vrc6.halt) return;
    for (int ch = 0; ch < 2; ch++) {
        uint8_t *c = K->vrc6.ctl[ch];
        if (!(c[2] & 0x80)) continue;
        int period = ((c[2] & 0x0F) << 8 | c[1]) >> K->vrc6.freq_shift;
        if (--K->vrc6.timer[ch] <= 0) { K->vrc6.timer[ch] = period + 1; K->vrc6.step[ch] = (K->vrc6.step[ch] + 1) & 15; }
    }
    uint8_t *s = K->vrc6.ctl[2];
    if (s[2] & 0x80) {
        int period = ((s[2] & 0x0F) << 8 | s[1]) >> K->vrc6.freq_shift;
        if (--K->vrc6.timer[2] <= 0) {
            K->vrc6.timer[2] = period + 1;
            K->vrc6.saw_step++;
            if (K->vrc6.saw_step >= 14) { K->vrc6.saw_step = 0; K->vrc6.saw_acc = 0; }
            else if (!(K->vrc6.saw_step & 1)) K->vrc6.saw_acc = (uint8_t)(K->vrc6.saw_acc + (s[0] & 0x3F));
        }
    }
}
static float vrc6_audio_out(Nes *n) {
    int out = 0;
    for (int ch = 0; ch < 2; ch++) {
        uint8_t *c = K->vrc6.ctl[ch];
        if (!(c[2] & 0x80)) continue;
        int duty = (c[0] >> 4) & 7;
        if ((c[0] & 0x80) || K->vrc6.step[ch] <= duty) out += c[0] & 0x0F;
    }
    if (K->vrc6.ctl[2][2] & 0x80) out += K->vrc6.saw_acc >> 3;
    return out * 0.00755f;  /* roughly matches 2A03 pulse level */
}

/* ------------------------------------------------------------ Sunsoft 5B audio */
static void s5b_clock(Nes *n) {
    /* runs at CPU/2 */
    if (++K->s5b.div < 2) return;
    K->s5b.div = 0;
    uint8_t *r = K->s5b.reg;
    for (int ch = 0; ch < 3; ch++) {
        int period = (r[ch * 2] | ((r[ch * 2 + 1] & 0x0F) << 8));
        if (period == 0) period = 1;
        if (++K->s5b.timer[ch] >= period * 8) { K->s5b.timer[ch] = 0; K->s5b.out[ch] ^= 1; }
    }
    int np = (r[6] & 0x1F); if (!np) np = 1;
    if (++K->s5b.noise_timer >= np * 16) {
        K->s5b.noise_timer = 0;
        uint32_t l = K->s5b.noise_lfsr;
        uint32_t bit = (l ^ (l >> 3)) & 1;
        K->s5b.noise_lfsr = (l >> 1) | (bit << 16);
    }
    int ep = r[11] | (r[12] << 8); if (!ep) ep = 1;
    if (++K->s5b.env_timer >= ep * 16) {
        K->s5b.env_timer = 0;
        if (!K->s5b.env_hold) {
            K->s5b.env_step++;
            if (K->s5b.env_step >= 32) {
                if (!K->s5b.env_cont) { K->s5b.env_hold = true; K->s5b.env_vol = 0; }
                else if (K->s5b.env_hold) {}
                else { K->s5b.env_step = 0; if (K->s5b.env_alt) K->s5b.env_att = !K->s5b.env_att; }
            }
            if (!K->s5b.env_hold) {
                int st = K->s5b.env_step & 31;
                K->s5b.env_vol = K->s5b.env_att ? st : 31 - st;
            }
        }
    }
}
static float s5b_out(Nes *n) {
    static float voltab[32]; static bool init;
    if (!init) { for (int i = 0; i < 32; i++) voltab[i] = (i == 0) ? 0 : (float)(1.0 / __builtin_pow(10.0, (31 - i) * 1.5 / 20.0)); init = true; }
    uint8_t *r = K->s5b.reg;
    float out = 0;
    for (int ch = 0; ch < 3; ch++) {
        bool tone = !(r[7] & (1 << ch)) ? K->s5b.out[ch] : true;
        bool noise = !(r[7] & (8 << ch)) ? (K->s5b.noise_lfsr & 1) : true;
        if (tone && noise) {
            int vol = (r[8 + ch] & 0x10) ? K->s5b.env_vol : ((r[8 + ch] & 0x0F) * 2 + 1);
            out += voltab[vol & 31];
        }
    }
    return out * 0.12f;
}

/* ------------------------------------------------------------ sync dispatcher */
void cart_sync(Nes *n) {
    uint8_t *r = K->reg;
    switch (K->mapper) {
    case 0: prg32(n, 0); if (K->prg_size <= 0x4000) { prg16(n, 0, 0); prg16(n, 1, 0); } chr8(n, 0); default_mirror(n); break;
    case 1: case 155: mmc1_sync(n); break;
    case 2: case 94: case 180:
        if (K->mapper == 180) { prg16(n, 0, 0); prg16(n, 1, r[0]); }
        else { prg16(n, 0, K->mapper == 94 ? (r[0] >> 2) : r[0]); prg16(n, 1, -1); }
        chr8(n, 0); default_mirror(n); break;
    case 3: prg32(n, 0); if (K->prg_size <= 0x4000) { prg16(n, 0, 0); prg16(n, 1, 0); } chr8(n, r[0]); default_mirror(n); break;
    case 4: case 118: case 119: case 206: mmc3_sync(n); break;
    case 5: mmc5_sync(n); break;
    case 7: prg32(n, r[0] & 0x0F); chr8(n, 0); mirror(n, (r[0] & 0x10) ? MIRROR_1B : MIRROR_1A); break;
    case 9: case 10:
        if (K->mapper == 9) { prg8(n, 0, r[0]); prg8(n, 1, -3); prg8(n, 2, -2); prg8(n, 3, -1); }
        else { prg16(n, 0, r[0]); prg16(n, 1, -1); }
        chr4(n, 0, K->latch0 ? r[2] : r[1]);
        chr4(n, 1, K->latch1 ? r[4] : r[3]);
        mirror(n, (r[5] & 1) ? MIRROR_H : MIRROR_V);
        break;
    case 11: prg32(n, r[0] & 3); chr8(n, r[0] >> 4); default_mirror(n); break;
    case 13: prg32(n, 0); chr4(n, 0, 0); chr4(n, 1, r[0] & 3); default_mirror(n); break;
    case 21: case 22: case 23: case 25: vrc4_sync(n); break;
    case 24: case 26: vrc6_sync(n); break;
    case 28: {   /* Action 53: r[0] chr, r[1] inner prg, r[2] mode, r[3] outer prg */
        static const int mir[4] = { MIRROR_1A, MIRROR_1B, MIRROR_V, MIRROR_H };
        mirror(n, mir[r[2] & 3]);
        chr8(n, r[0] & 3);
        int mask = (2 << ((r[2] >> 4) & 3)) - 1;   /* game size in 16K units - 1 */
        int outer = r[3] << 1, inner = r[1] & 0x0F;
        if (!(r[2] & 0x08)) {
            prg16(n, 0, ((inner << 1) & mask) | (outer & ~mask));
            prg16(n, 1, (((inner << 1) | 1) & mask) | (outer & ~mask));
        } else if (r[2] & 0x04) {
            prg16(n, 0, (inner & mask) | (outer & ~mask));
            prg16(n, 1, outer | 1);
        } else {
            prg16(n, 0, outer);
            prg16(n, 1, (inner & mask) | (outer & ~mask));
        }
        break;
    }
    case 33: case 48:
        prg8(n, 0, r[0] & 0x3F); prg8(n, 1, r[1] & 0x3F); prg8(n, 2, -2); prg8(n, 3, -1);
        chr2(n, 0, r[2]); chr2(n, 1, r[3]);
        chr1(n, 4, r[4]); chr1(n, 5, r[5]); chr1(n, 6, r[6]); chr1(n, 7, r[7]);
        mirror(n, (r[8] & 0x40) ? MIRROR_H : MIRROR_V);
        break;
    case 64: {   /* Tengen RAMBO-1 */
        uint8_t sel = r[16]; uint8_t *b = &r[17];   /* b[0..9] = R0..R9, b[15] = RF */
        int inv = (sel & 0x80) ? 4 : 0;
        if (sel & 0x20) {
            chr1(n, 0 ^ inv, b[0]); chr1(n, 1 ^ inv, b[8]); chr1(n, 2 ^ inv, b[1]); chr1(n, 3 ^ inv, b[9]);
        } else {
            chr1(n, 0 ^ inv, b[0] & 0xFE); chr1(n, 1 ^ inv, b[0] | 1); chr1(n, 2 ^ inv, b[1] & 0xFE); chr1(n, 3 ^ inv, b[1] | 1);
        }
        chr1(n, 4 ^ inv, b[2]); chr1(n, 5 ^ inv, b[3]); chr1(n, 6 ^ inv, b[4]); chr1(n, 7 ^ inv, b[5]);
        if (sel & 0x40) { prg8(n, 0, b[15]); prg8(n, 1, b[6]); prg8(n, 2, b[7]); }
        else { prg8(n, 0, b[6]); prg8(n, 1, b[7]); prg8(n, 2, b[15]); }
        prg8(n, 3, -1);
        mirror(n, (r[1] & 1) ? MIRROR_H : MIRROR_V);
        break;
    }
    case 70: case 152:
        prg16(n, 0, (r[0] >> 4) & (K->mapper == 152 ? 7 : 0x0F)); prg16(n, 1, -1); chr8(n, r[0] & 0x0F);
        if (K->mapper == 152) mirror(n, (r[0] & 0x80) ? MIRROR_1B : MIRROR_1A); else default_mirror(n);
        break;
    case 78:
        prg16(n, 0, r[0] & 7); prg16(n, 1, -1); chr8(n, r[0] >> 4);
        if (K->submapper == 3 || K->mirroring == MIRROR_4) mirror(n, (r[0] & 8) ? MIRROR_V : MIRROR_H);
        else mirror(n, (r[0] & 8) ? MIRROR_1B : MIRROR_1A);
        break;
    case 89:
        prg16(n, 0, (r[0] >> 4) & 7); prg16(n, 1, -1); chr8(n, (r[0] & 7) | ((r[0] >> 4) & 8));
        mirror(n, (r[0] & 8) ? MIRROR_1B : MIRROR_1A);
        break;
    case 93: prg16(n, 0, (r[0] >> 4) & 7); prg16(n, 1, -1); chr8(n, 0); default_mirror(n); break;
    case 184: prg32(n, 0); if (K->prg_size <= 0x4000) { prg16(n, 0, 0); prg16(n, 1, 0); } chr4(n, 0, r[0] & 7); chr4(n, 1, ((r[0] >> 4) & 7) | 4); default_mirror(n); break;
    case 185: prg32(n, 0); if (K->prg_size <= 0x4000) { prg16(n, 0, 0); prg16(n, 1, 0); } chr8(n, 0); default_mirror(n); break;
    case 232: {
        int outer = (r[1] >> 3) & 3;
        prg16(n, 0, outer * 4 + (r[0] & 3)); prg16(n, 1, outer * 4 + 3); chr8(n, 0); default_mirror(n);
        break;
    }
    case 34:
        if (K->chr_size > 0x2000 && !K->chr_is_ram) { prg32(n, r[0] & 1); chr4(n, 0, r[1]); chr4(n, 1, r[2]); }
        else { prg32(n, r[0]); chr8(n, 0); }
        default_mirror(n); break;
    case 66: prg32(n, (r[0] >> 4) & 3); chr8(n, r[0] & 3); default_mirror(n); break;
    case 69: {
        for (int i = 0; i < 8; i++) chr1(n, i, r[i]);
        prg8(n, 0, r[9] & 0x3F); prg8(n, 1, r[10] & 0x3F); prg8(n, 2, r[11] & 0x3F); prg8(n, 3, -1);
        static const int mir[4] = { MIRROR_V, MIRROR_H, MIRROR_1A, MIRROR_1B };
        mirror(n, mir[r[12] & 3]);
        if (r[8] & 0x40) { K->wram_off = K->wram_size ? 0 : -1; K->wram_en = (r[8] & 0x80) != 0; }
        else { K->wram_off = -2; K->wram_prg_off = (uint32_t)((r[8] & 0x3F) * 0x2000) % K->prg_size; K->wram_en = true; }
        break;
    }
    case 71:
        prg16(n, 0, r[0]); prg16(n, 1, -1); chr8(n, 0);
        if (r[1] & 0x80) mirror(n, (r[1] & 0x10) ? MIRROR_1B : MIRROR_1A); else default_mirror(n);
        break;
    case 79: prg32(n, (r[0] >> 3) & 1); chr8(n, r[0] & 7); default_mirror(n); break;
    case 87: prg32(n, 0); if (K->prg_size <= 0x4000) { prg16(n, 0, 0); prg16(n, 1, 0); } chr8(n, ((r[0] & 1) << 1) | ((r[0] >> 1) & 1)); default_mirror(n); break;
    case 140: prg32(n, (r[0] >> 4) & 3); chr8(n, r[0] & 0x0F); default_mirror(n); break;
    default: prg32(n, 0); chr8(n, 0); default_mirror(n); break;
    }
}

/* ------------------------------------------------------------ CPU side */
static inline uint8_t prg_read(Nes *n, uint16_t a) {
    return K->prg[K->prg_off[(a >> 13) & 3] + (a & 0x1FFF)];
}

static uint8_t mmc5_read(Nes *n, uint16_t a, uint8_t ob) {
    if (a >= 0x6000) {
        uint32_t off; int t = mmc5_prg_map(n, a, &off);
        if (t == 1) return K->prg[off];
        if (t == 0) return K->wram[off];
        return ob;
    }
    switch (a) {
    case 0x5204: {
        uint8_t v = (uint8_t)((K->irq_reload ? 0x80 : 0) | (mmc5.in_frame ? 0x40 : 0));
        K->irq_reload = false; cpu_clear_irq(n, IRQ_MAPPER);
        return v;
    }
    case 0x5205: return (uint8_t)((K->reg[30] * K->reg[31]) & 0xFF);
    case 0x5206: return (uint8_t)((K->reg[30] * K->reg[31]) >> 8);
    case 0x5015: return 0;
    }
    if (a >= 0x5C00 && a < 0x6000) {
        if (K->reg[4] >= 2) return K->exram[a - 0x5C00];
        return ob;
    }
    return ob;
}

uint8_t cart_cpu_peek(Nes *n, uint16_t a, uint8_t ob) {
    if (K->mapper == 5) {
        if (a == 0x5204) return (uint8_t)((K->irq_reload ? 0x80 : 0) | (mmc5.in_frame ? 0x40 : 0));
        return mmc5_read(n, a, ob);
    }
    return cart_cpu_read(n, a, ob);
}

uint8_t cart_cpu_read(Nes *n, uint16_t a, uint8_t ob) {
    if (K->mapper == 5) return mmc5_read(n, a, ob);
    if (a >= 0x8000) return prg_read(n, a);
    if (a >= 0x6000) {
        if (K->wram_off == -2) return K->prg[K->wram_prg_off + (a & 0x1FFF)];
        if (K->wram_off < 0 || !K->wram_en) return ob;
        return K->wram[(uint32_t)K->wram_off + (a & 0x1FFF)];
    }
    return ob;
}

static void mmc5_write(Nes *n, uint16_t a, uint8_t v) {
    uint8_t *r = K->reg;
    if (a >= 0x6000) {
        if ((r[2] & 3) != 2 || (r[3] & 3) != 1) return;
        uint32_t off; int t = mmc5_prg_map(n, a, &off);
        if (t == 0) K->wram[off] = v;
        return;
    }
    if (a >= 0x5C00 && a < 0x6000) {
        if (r[4] != 3) K->exram[a - 0x5C00] = (r[4] <= 1 && !mmc5.in_frame) ? 0 : v;
        return;
    }
    switch (a) {
    case 0x5100: r[0] = v & 3; break;
    case 0x5101: r[1] = v & 3; break;
    case 0x5102: r[2] = v & 3; break;
    case 0x5103: r[3] = v & 3; break;
    case 0x5104: r[4] = v & 3; break;
    case 0x5105: r[5] = v; break;
    case 0x5106: r[6] = v; break;
    case 0x5107: r[7] = v & 3; break;
    case 0x5113: case 0x5114: case 0x5115: case 0x5116: case 0x5117: r[8 + (a - 0x5113)] = v; break;
    case 0x5120: case 0x5121: case 0x5122: case 0x5123: case 0x5124: case 0x5125: case 0x5126: case 0x5127:
        mmc5.chr_a[a - 0x5120] = (uint16_t)(v | (r[28] << 8)); mmc5.last_b = false; break;
    case 0x5128: case 0x5129: case 0x512A: case 0x512B:
        mmc5.chr_b[a - 0x5128] = (uint16_t)(v | (r[28] << 8)); mmc5.last_b = true; break;
    case 0x5130: r[28] = v & 3; break;
    case 0x5200: r[32] = v; break;
    case 0x5203: r[29] = v; break;
    case 0x5204: K->irq_enable = (v & 0x80) != 0; if (K->irq_enable && K->irq_reload) cpu_set_irq(n, IRQ_MAPPER); else if (!K->irq_enable) cpu_clear_irq(n, IRQ_MAPPER); break;
    case 0x5205: r[30] = v; break;
    case 0x5206: r[31] = v; break;
    }
    mmc5_sync(n);
}

void cart_cpu_write(Nes *n, uint16_t a, uint8_t v) {
    uint8_t *r = K->reg;
    if (K->mapper == 5) { if (a >= 0x5000) mmc5_write(n, a, v); return; }
    if (a < 0x6000) {
        if (K->mapper == 28 && a >= 0x5000) { r[4] = v & 0x81; return; }
        if (K->mapper == 79 && (a & 0x4100) == 0x4100) { r[0] = v; cart_sync(n); }
        return;
    }
    if (a < 0x8000) {
        if (K->mapper == 34 && K->chr_size > 0x2000 && !K->chr_is_ram) {
            if (a == 0x7FFD) r[0] = v; else if (a == 0x7FFE) r[1] = v; else if (a == 0x7FFF) r[2] = v;
            cart_sync(n);
        }
        if (K->mapper == 140) { r[0] = v; cart_sync(n); return; }
        if (K->mapper == 184) { r[0] = v; cart_sync(n); return; }
        if (K->mapper == 87) { r[0] = v; cart_sync(n); return; }
        if (K->wram_off >= 0 && K->wram_en && !K->wram_wp) K->wram[(uint32_t)K->wram_off + (a & 0x1FFF)] = v;
        return;
    }
    /* bus conflicts for discrete boards that have them */
    bool conflicts = false;
    switch (K->mapper) {
    case 3: case 7: case 2: conflicts = K->submapper == 2; break;
    case 11: case 66: case 70: case 152: case 78: case 89: case 93: case 185: conflicts = true; break;
    case 34: conflicts = !(K->chr_size > 0x2000 && !K->chr_is_ram); break;
    }
    if (conflicts) v &= prg_read(n, a);

    switch (K->mapper) {
    case 1: case 155: mmc1_write(n, a, v); return;
    case 2: case 94: case 180: r[0] = v; break;
    case 3: r[0] = v; break;
    case 4: case 118: case 119: case 206: mmc3_write(n, a, v); return;
    case 33: case 48:
        switch (a & 0xE003) {
        case 0x8000: r[0] = v & 0x3F; if (K->mapper == 33) r[8] = v; break;
        case 0x8001: r[1] = v & 0x3F; break;
        case 0x8002: r[2] = v; break;
        case 0x8003: r[3] = v; break;
        case 0xA000: case 0xA001: case 0xA002: case 0xA003: r[4 + (a & 3)] = v; break;
        case 0xC000: K->irq_latch = (uint8_t)(v ^ 0xFF); break;
        case 0xC001: K->irq_counter = 0; K->irq_reload = true; break;
        case 0xC002: K->irq_enable = true; break;
        case 0xC003: K->irq_enable = false; cpu_clear_irq(n, IRQ_MAPPER); break;
        case 0xE000: if (K->mapper == 48) r[8] = v; break;
        }
        break;
    case 64:
        switch (a & 0xE001) {
        case 0x8000: r[16] = v; break;
        case 0x8001: r[17 + (r[16] & 0x0F)] = v; break;
        case 0xA000: r[1] = v; break;
        case 0xC000: K->irq_latch = v; break;
        case 0xC001: K->reg[40] = v & 1; K->irq_reload = true; K->irq_prescaler = 0; break;
        case 0xE000: K->irq_enable = false; cpu_clear_irq(n, IRQ_MAPPER); break;
        case 0xE001: K->irq_enable = true; break;
        }
        break;
    case 70: case 152: case 78: case 89: case 93: r[0] = v; break;
    case 185: r[0] = v; break;
    case 232: if (a < 0xC000) r[1] = v; else r[0] = v; break;
    case 7: r[0] = v; break;
    case 9: case 10:
        switch (a & 0xF000) {
        case 0xA000: r[0] = v & 0x0F; break;
        case 0xB000: r[1] = v & 0x1F; break;
        case 0xC000: r[2] = v & 0x1F; break;
        case 0xD000: r[3] = v & 0x1F; break;
        case 0xE000: r[4] = v & 0x1F; break;
        case 0xF000: r[5] = v; break;
        }
        break;
    case 11: case 13: case 66: case 34: r[0] = v; break;
    case 21: case 22: case 23: case 25: vrc4_write(n, a, v); return;
    case 24: case 26: vrc6_write(n, a, v); return;
    case 69:
        if (a < 0xA000) r[63] = v & 0x0F;
        else if (a < 0xC000) {
            int cmd = r[63];
            if (cmd <= 0x0C) r[cmd] = v;
            else if (cmd == 0x0D) { K->irq_enable = v & 1; K->irq_enable2 = (v & 0x80) != 0; cpu_clear_irq(n, IRQ_MAPPER); }
            else if (cmd == 0x0E) K->irq_counter = (K->irq_counter & 0xFF00) | v;
            else if (cmd == 0x0F) K->irq_counter = (K->irq_counter & 0x00FF) | (v << 8);
        } else if (a < 0xE000) K->s5b.addr = v & 0x0F;
        else {
            K->s5b.reg[K->s5b.addr] = v;
            if (K->s5b.addr == 13) {
                K->s5b.env_cont = v & 8; K->s5b.env_att = (v & 4) != 0; K->s5b.env_alt = (v & 2) != 0;
                K->s5b.env_hold = false; K->s5b.env_step = 0; K->s5b.env_timer = 0;
                K->s5b.env_vol = K->s5b.env_att ? 0 : 31;
                if (v & 1) { /* hold handled at end of cycle */ }
                K->reg[60] = v & 1;
            }
        }
        break;
    case 71:
        if (a < 0xC000) { if (a >= 0x9000) r[1] = (uint8_t)(0x80 | v); }
        else r[0] = v;
        break;
    case 87: case 140: case 79: break;
    case 28: {
        int sel = (r[4] & 1) | ((r[4] >> 6) & 2);
        r[sel] = v;
        /* in one-screen modes, bit 4 of CHR/inner-PRG writes picks the nametable */
        if (sel < 2 && !(r[2] & 2)) r[2] = (uint8_t)((r[2] & ~1) | ((v >> 4) & 1));
        break;
    }
    default: break;
    }
    cart_sync(n);
}

void cart_cpu_clock(Nes *n) {
    switch (K->mapper) {
    case 21: case 22: case 23: case 25: case 24: case 26:
        vrc_irq_clock(n);
        if (K->mapper == 24 || K->mapper == 26) vrc6_audio_clock(n);
        break;
    case 69:
        if (K->irq_enable2) {
            K->irq_counter = (K->irq_counter - 1) & 0xFFFF;
            if (K->irq_counter == 0xFFFF && K->irq_enable) cpu_set_irq(n, IRQ_MAPPER);
        }
        s5b_clock(n);
        break;
    case 64:
        if (K->reg[40] && ++K->irq_prescaler >= 4) { K->irq_prescaler = 0; rambo_clock(n); }
        break;
    case 5:
        /* in-frame detection: PPU stops reading when rendering disabled / vblank */
        if (mmc5.in_frame && ++mmc5.idle > 3) { mmc5.in_frame = false; mmc5.last_nt = 0; }
        break;
    }
}

float cart_audio(Nes *n) {
    switch (K->mapper) {
    case 24: case 26: return vrc6_audio_out(n);
    case 69: return s5b_out(n);
    }
    return 0;
}

/* ------------------------------------------------------------ PPU side */
void cart_ppu_addr(Nes *n, uint16_t a) {
    if (K->mapper == 4 || K->mapper == 118 || K->mapper == 119 || K->mapper == 48 || K->mapper == 64) {
        bool a12 = (a & 0x1000) != 0;
        if (a12 && !K->a12_prev) {
            if (n->ppu.dots - K->a12_low_since > 10) {
                if (K->mapper == 64) { if (!K->reg[40]) rambo_clock(n); }
                else mmc3_a12(n);
            }
        } else if (!a12 && K->a12_prev) {
            K->a12_low_since = n->ppu.dots;
        }
        K->a12_prev = a12;
    }
}

static uint8_t mmc5_ppu_read(Nes *n, uint16_t a) {
    Ppu *p = &n->ppu;
    mmc5.idle = 0;
    bool big = (p->ctrl & 0x20) != 0;
    bool rendering = p->rend && (p->scanline < 240 || p->scanline == 261);
    /* scanline detection: three consecutive PPU reads of the same nametable
     * address (the dummy fetches at dots 337-340 plus the first fetch of the
     * next line). Any other read in between breaks the sequence. */
    if (a >= 0x2000 && a < 0x3000 && a == mmc5.last_nt) {
        if (++mmc5.nt_reads == 2) {
            if (!mmc5.in_frame) { mmc5.in_frame = true; mmc5.scanline = 0; K->irq_reload = false; cpu_clear_irq(n, IRQ_MAPPER); }
            else {
                mmc5.scanline++;
                if (mmc5.scanline == K->reg[29]) { K->irq_reload = true; if (K->irq_enable) cpu_set_irq(n, IRQ_MAPPER); }
            }
        }
    } else mmc5.nt_reads = 0;
    mmc5.last_nt = a;
    if (a >= 0x2000) {
        uint8_t *page = K->nt[(a >> 10) & 3];
        /* extended attribute mode */
        if (K->reg[4] == 1 && rendering && !(p->dot >= 257 && p->dot <= 320)) {
            if ((a & 0x3FF) >= 0x3C0) {
                uint8_t ex = K->exram[p->v & 0x3FF];
                uint8_t pal = ex >> 6;
                return (uint8_t)(pal * 0x55);
            }
        }
        return page[a & 0x3FF];
    }
    /* CHR */
    if (K->reg[4] == 1 && rendering && !(p->dot >= 257 && p->dot <= 320)) {
        uint8_t ex = K->exram[p->v & 0x3FF];
        uint32_t bank = (uint32_t)((ex & 0x3F) | (K->reg[28] << 6));
        return K->chr[(bank * 0x1000 + (a & 0x0FFF)) % K->chr_size];
    }
    bool use_b;
    if (!big) use_b = false;
    else if (rendering) use_b = !(p->dot >= 257 && p->dot <= 320);
    else use_b = mmc5.last_b;
    return K->chr[mmc5_chr_off(n, a, use_b)];
}

uint8_t cart_ppu_read(Nes *n, uint16_t a) {
    if (K->mapper == 5) return mmc5_ppu_read(n, a);
    if (a < 0x2000) {
        if (K->mapper == 185 && (!(K->reg[0] & 0x0F) || K->reg[0] == 0x13)) return 0xFF;  /* CHR disabled (copy protection) */
        uint8_t v = K->chr[K->chr_off[a >> 10] + (a & 0x3FF)];
        if (K->mapper == 9 || K->mapper == 10) {
            uint16_t m = a & 0x1FF8;
            if (K->mapper == 9 ? (a == 0x0FD8) : (m == 0x0FD8)) { K->latch0 = 0; cart_sync(n); }
            else if (K->mapper == 9 ? (a == 0x0FE8) : (m == 0x0FE8)) { K->latch0 = 1; cart_sync(n); }
            else if (m == 0x1FD8) { K->latch1 = 0; cart_sync(n); }
            else if (m == 0x1FE8) { K->latch1 = 1; cart_sync(n); }
        }
        return v;
    }
    return K->nt[(a >> 10) & 3][a & 0x3FF];
}

void cart_ppu_write(Nes *n, uint16_t a, uint8_t v) {
    if (a < 0x2000) {
        if (K->mapper == 119) {
            uint32_t off = K->chr_off[a >> 10] + (a & 0x3FF);
            if (off >= K->chr_rom_size) K->chr[off] = v;
            return;
        }
        if (K->chr_is_ram) {
            if (K->mapper == 5) K->chr[mmc5_chr_off(n, a, false)] = v;
            else K->chr[K->chr_off[a >> 10] + (a & 0x3FF)] = v;
        }
        return;
    }
    uint8_t *page = K->nt[(a >> 10) & 3];
    if (K->mapper == 5 && page == mmc5_fill_nt) return;
    page[a & 0x3FF] = v;
}

/* ------------------------------------------------------------ load */
bool cart_load(Nes *n, const uint8_t *d, size_t size, char *err, size_t errlen) {
    Cart *c = K;
    memset(c, 0, sizeof *c);
    if (size < 16 || memcmp(d, "NES\x1A", 4) != 0) { snprintf(err, errlen, "Not an iNES ROM file"); return false; }
    bool nes2 = (d[7] & 0x0C) == 0x08;
    uint32_t prg_banks = d[4], chr_banks = d[5];
    c->mapper = (d[6] >> 4) | (d[7] & 0xF0);
    if (nes2) {
        c->mapper |= (d[8] & 0x0F) << 8;
        c->submapper = d[8] >> 4;
        prg_banks |= (uint32_t)(d[9] & 0x0F) << 8;
        chr_banks |= (uint32_t)(d[9] & 0xF0) << 4;
    } else if (memcmp(d + 12, "\0\0\0\0", 4) != 0) {
        /* old "DiskDude!" style garbage: ignore upper mapper nibble */
        c->mapper &= 0x0F;
    }
    c->mirroring = (d[6] & 8) ? MIRROR_4 : (d[6] & 1) ? MIRROR_V : MIRROR_H;
    c->battery = (d[6] & 2) != 0;
    c->has_trainer = (d[6] & 4) != 0;
    size_t off = 16 + (c->has_trainer ? 512 : 0);
    c->prg_size = prg_banks * 0x4000;
    c->chr_size = chr_banks * 0x2000;
    if (c->prg_size == 0 || off + c->prg_size > size) { snprintf(err, errlen, "ROM file is truncated or corrupt"); return false; }
    if (off + c->prg_size + c->chr_size > size) c->chr_size = (uint32_t)((size - off - c->prg_size) & ~0x1FFFu);
    if (!cart_supported(c->mapper)) { snprintf(err, errlen, "Mapper %d is not supported", c->mapper); return false; }

    c->prg = malloc(c->prg_size);
    memcpy(c->prg, d + off, c->prg_size);
    c->nes2 = nes2;
    c->chr_rom_size = c->chr_size;
    if (c->chr_size && c->mapper == 119) {
        c->chr = calloc(1, c->chr_size + 0x2000);
        memcpy(c->chr, d + off + c->prg_size, c->chr_size);
    } else if (c->chr_size) {
        c->chr = malloc(c->chr_size);
        memcpy(c->chr, d + off + c->prg_size, c->chr_size);
    } else {
        uint32_t chr_ram = 0x2000;
        if (nes2) { int s = d[11] & 0x0F; if (s) chr_ram = 64u << s; int s2 = d[11] >> 4; if (s2) chr_ram += 64u << s2; }
        if (c->mapper == 13) chr_ram = 0x4000;
        if (c->mapper == 28 && !nes2) chr_ram = 0x8000;
        if (chr_ram < 0x2000) chr_ram = 0x2000;
        c->chr = calloc(1, chr_ram); c->chr_size = chr_ram; c->chr_is_ram = true;
    }
    c->wram_size = 0x2000;
    if (nes2) {
        uint32_t s = 0; int a = d[10] & 0x0F, b = d[10] >> 4;
        if (a) s += 64u << a; if (b) s += 64u << b;
        if (s) c->wram_size = s < 0x2000 ? 0x2000 : s;
    }
    if (c->mapper == 5) c->wram_size = 0x10000;
    if (c->mapper == 1 && c->prg_size > 0x40000 && c->wram_size < 0x8000 && !nes2) c->wram_size = 0x2000;
    c->wram = calloc(1, c->wram_size);
    c->crc = crc32(d + 16, size - 16);
    if (c->has_trainer) memcpy(c->wram + 0x1000, d + 16, 512);
    return true;
}

void cart_free(Nes *n) {
    free(K->prg); free(K->chr); free(K->wram);
    K->prg = K->chr = K->wram = NULL;
}

void cart_power(Nes *n) {
    Cart *c = K;
    memset(c->reg, 0, sizeof c->reg);
    c->irq_counter = c->irq_latch = c->irq_prescaler = 0;
    c->irq_enable = c->irq_reload = c->irq_enable2 = false;
    c->a12_low_since = 0; c->a12_prev = false; c->last_write_cycle = 0;
    c->mmc1_shift = c->mmc1_count = 0; c->latch0 = c->latch1 = 0;
    memset(&c->vrc6, 0, sizeof c->vrc6);
    memset(&c->s5b, 0, sizeof c->s5b); c->s5b.noise_lfsr = 1;
    wram_default(n);
    switch (c->mapper) {
    case 1: case 155: c->reg[0] = 0x0C; break;
    case 4: case 118: case 119: case 206: c->reg[8 + 6] = 0; c->reg[8 + 7] = 1; c->reg[2] = 0x80; break;
    case 64: c->reg[17 + 6] = 0; c->reg[17 + 7] = 1; c->reg[17 + 15] = 0xFE; break;
    case 5: c->reg[0] = 3; c->reg[1] = 3; c->reg[12] = 0xFF; memset(&mmc5, 0, sizeof mmc5); break;
    case 9: case 10: c->latch0 = 1; c->latch1 = 1; break;
    case 24: case 26: c->reg[2] = 0x80; break;
    case 69: c->reg[8] = 0xC0; break;
    case 28: c->reg[3] = 0xFF; break;
    }
    cart_sync(n);
}

void cart_reset(Nes *n) {
    if (K->mapper == 1) { K->mmc1_shift = 0; K->mmc1_count = 0; K->reg[0] |= 0x0C; }
    cart_sync(n);
}
