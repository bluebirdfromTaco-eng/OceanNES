/*
 * Dot-accurate 2C02 PPU.
 *
 * Models the PPU's internals at the level the hardware actually works:
 *  - every PPU memory access takes two dots: an address-latch (ALE) dot that
 *    puts the address on the bus and stores its low byte in the external octal
 *    latch, and a read dot that reads from (current high address | latch);
 *  - the rendering fetch cadence (NT/AT/pattern, sprite-phase NT/NT/pattern,
 *    the dummy NT fetches at 337-340 and the pattern ALE on dot 0);
 *  - the $2007 read/write state machines, which run a few dots after the CPU
 *    access and collide with rendering fetches exactly like the real chip;
 *  - background shift registers with their serial inputs (pattern high plane
 *    shifts in 1s, attribute shifters are fed from the attribute latch);
 *  - sprite evaluation and fetch through the secondary-OAM address counter and
 *    its "overflowed" flag, the OAM buffer seen through $2004, the
 *    sprite-overflow bug and misaligned OAM;
 *  - per-slot sprite X counters with "counting"/"halted" modes (counters keep
 *    running while rendering is disabled, shifters do not);
 *  - delayed $2001 effects, OAM row corruption when rendering is switched off
 *    mid-frame, VBL/NMI races, odd-frame dot skip and open-bus decay.
 */
#include "nes.h"
#include <string.h>

#define P (&n->ppu)
#define DECAY_FRAMES 36      /* ~600ms */

/* Delays measured in PPU dots from the CPU access (tuned to hardware tests) */
#ifndef MASK_DELAY
#define MASK_DELAY 3         /* $2001 write -> effect */
#endif
#ifndef SKIP_DELAY
#define SKIP_DELAY 2
#endif
#ifndef MASK_DELAY_ON
#define MASK_DELAY_ON MASK_DELAY
#endif
#ifndef SM_R_ALE
#define SM_R_ALE 3           /* $2007 read  -> state machine address latch */
#endif
#ifndef SM_W_ALE
#define SM_W_ALE 3           /* $2007 write -> state machine address latch */
#endif

enum { CAD_NONE, CAD_ALE, CAD_RD };

static inline uint8_t pal_read(Ppu *p, uint16_t a) {
    a &= 0x1F;
    if ((a & 0x13) == 0x10) a &= ~0x10;
    return p->pal[a];
}
static inline void pal_write(Ppu *p, uint16_t a, uint8_t v) {
    a &= 0x1F;
    if ((a & 0x13) == 0x10) a &= ~0x10;
    p->pal[a] = v & 0x3F;
}

static void update_nmi(Nes *n) {
    n->cpu.nmi_flag = (P->ctrl & 0x80) && (P->status & 0x80);
}

/* ------------------------------------------------------------ open bus */
static void io_refresh(Ppu *p, uint8_t v, uint8_t mask) {
    p->io = (uint8_t)((p->io & ~mask) | (v & mask));
    for (int i = 0; i < 8; i++) if (mask & (1 << i)) p->io_stamp[i] = (uint32_t)p->frame;
}
static uint8_t io_get(Ppu *p) {
    for (int i = 0; i < 8; i++)
        if ((p->io & (1 << i)) && (uint32_t)p->frame - p->io_stamp[i] > DECAY_FRAMES) p->io &= ~(1 << i);
    return p->io;
}

/* ------------------------------------------------------------ v helpers */
static inline void inc_hori(Ppu *p) {
    if ((p->v & 0x1F) == 31) { p->v &= ~0x1F; p->v ^= 0x0400; }
    else p->v++;
}
static inline void inc_vert(Ppu *p) {
    if ((p->v & 0x7000) != 0x7000) { p->v += 0x1000; return; }
    p->v &= ~0x7000;
    int y = (p->v & 0x03E0) >> 5;
    if (y == 29) { y = 0; p->v ^= 0x0800; }
    else if (y == 31) y = 0;
    else y++;
    p->v = (uint16_t)((p->v & ~0x03E0) | (y << 5));
}

static inline bool render_line(Ppu *p) { return p->scanline < 240 || p->scanline == 261; }

/* ------------------------------------------------------------ PPU bus */
static inline void bus_ale(Nes *n, uint16_t addr) {
    Ppu *p = P;
    addr &= 0x3FFF;
    p->bus_addr = addr;
    p->octal = (uint8_t)addr;
    p->adbus = (uint8_t)addr;
    cart_ppu_addr(n, addr);
}
static inline uint8_t bus_rd(Nes *n, uint16_t hi) {
    Ppu *p = P;
    uint16_t addr = (uint16_t)(((hi & 0x3F00) | p->octal) & 0x3FFF);
    if (addr != p->bus_addr) { p->bus_addr = addr; cart_ppu_addr(n, addr); }
    uint8_t v = cart_ppu_read(n, addr);
    p->adbus = v;
    return v;
}

/* $2007 increment (with the rendering glitch) */
static void v_increment(Nes *n) {
    Ppu *p = P;
    if (p->rend && render_line(p)) { inc_hori(p); inc_vert(p); }
    else {
        p->v = (uint16_t)((p->v + ((p->ctrl & 4) ? 32 : 1)) & 0x7FFF);
        if (!(p->rend && render_line(p))) { p->bus_addr = p->v & 0x3FFF; cart_ppu_addr(n, p->v & 0x3FFF); }
    }
}

/* ------------------------------------------------------------ registers */
uint8_t ppu_reg_peek(Nes *n, uint16_t a) {
    Ppu *p = P;
    switch (a & 7) {
    case 2: return (uint8_t)((p->status & 0xE0) | (p->io & 0x1F));
    case 4: return (p->rend && render_line(p)) ? p->oam_copy : p->oam[p->oam_addr];
    default: return p->io;
    }
}

uint8_t ppu_reg_read(Nes *n, uint16_t a) {
    Ppu *p = P;
    uint8_t r;
    switch (a & 7) {
    case 2:
        r = (uint8_t)((p->status & 0xE0) | (io_get(p) & 0x1F));
        p->status &= 0x7F;
        p->w = false;
        if (p->scanline == 241 && p->dot == 1) p->prevent_vbl = true;
        update_nmi(n);
        io_refresh(p, r, 0xE0);
        return r;
    case 4:
        r = (p->rend && render_line(p)) ? p->oam_copy : p->oam[p->oam_addr];
        io_refresh(p, r, 0xFF);
        return r;
    case 7: {
        if (p->ignore_2007) return p->io;  /* back-to-back reads: the second one is ignored */
        p->ignore_2007 = 6;
        uint16_t v = p->v & 0x3FFF;
        if (v >= 0x3F00) {
            r = (uint8_t)((pal_read(p, v) & 0x3F) | (io_get(p) & 0xC0));
            if (p->mask & 1) r &= 0x30 | (r & 0xC0);
            io_refresh(p, r, 0x3F);
        } else {
            r = p->buf;
            io_refresh(p, r, 0xFF);
        }
        /* the read buffer is refilled by the PPU DATA state machine */
        p->sm_r_ale = SM_R_ALE; p->sm_r_rd = SM_R_ALE + 2;
        return r;
    }
    default:
        return io_get(p);
    }
}

void ppu_reg_write(Nes *n, uint16_t a, uint8_t v) {
    Ppu *p = P;
    io_refresh(p, v, 0xFF);
    switch (a & 7) {
    case 0:
        p->ctrl = v;
        p->t = (uint16_t)((p->t & ~0x0C00) | ((v & 3) << 10));
        update_nmi(n);
        break;
    case 1:
        p->mask_prev = p->mask_pending; p->mask_write_dot = p->dots;
        p->mask_pending = v;
        p->mask_delay = (!(p->mask & 0x18) && (v & 0x18)) ? MASK_DELAY_ON : MASK_DELAY;
        break;
    case 2: break;
    case 3: p->oam_addr = v; break;
    case 4:
        if (p->rend && render_line(p)) {
            p->oam_addr = (uint8_t)((p->oam_addr + 4) & 0xFC);
        } else {
            if ((p->oam_addr & 3) == 2) v &= 0xE3;
            p->oam[p->oam_addr++] = v;
        }
        break;
    case 5:
        if (!p->w) { p->x = v & 7; p->t = (uint16_t)((p->t & ~0x001F) | (v >> 3)); }
        else p->t = (uint16_t)((p->t & ~0x73E0) | ((v & 7) << 12) | ((v & 0xF8) << 2));
        p->w = !p->w;
        break;
    case 6:
        if (!p->w) p->t = (uint16_t)((p->t & 0x00FF) | ((v & 0x3F) << 8));
        else { p->t = (uint16_t)((p->t & 0x7F00) | v); p->pending_v = p->t; p->v_delay = 3; }
        p->w = !p->w;
        break;
    case 7:
        p->sm_wdata = v;
        p->sm_w_ale = SM_W_ALE; p->sm_w_wr = SM_W_ALE + 1;
        break;
    }
}

/* ------------------------------------------------------------ OAM2 address */
static inline void oam2_inc(Ppu *p) {
    if (p->oam2_ovf) return;          /* the overflowed flag freezes the address */
    p->oam2_addr = (p->oam2_addr + 1) & 31;
    if (p->oam2_addr == 0) p->oam2_ovf = true;
}

/* ------------------------------------------------------------ sprite evaluation */
static void sprite_eval(Nes *n, int d) {
    Ppu *p = P;
    if (d <= 64) {
        /* secondary OAM clear: OAM2 address = (dot-1)/2 */
        if (d & 1) p->oam_copy = 0xFF;
        else {
            p->oam2[p->oam2_addr] = p->oam_copy;
            if (d < 64) oam2_inc(p); else p->oam2_addr = 0;
        }
        if (d == 63) p->oam2_ovf = false;
        return;
    }
    if (d == 65) {
        p->spr0_added = false; p->spr_l = 0;
        p->overflow_cnt = 0; p->copy_done = false; p->in_range = true; /* in_range = "first object" */
    }
    if (d & 1) {
        p->oam_copy = p->oam[p->oam_addr];
    } else {
        int h = (p->ctrl & 0x20) ? 16 : 8;
        int sl = p->scanline;
        unsigned addr = p->oam_addr;
        bool inr = sl >= p->oam_copy && sl < p->oam_copy + h;
        bool first = p->in_range; p->in_range = false;
        if (p->copy_done) {
            addr += 4;
            p->oam_copy = p->oam2[p->oam2_addr];
        } else if (!p->oam2_ovf) {
            p->oam2[p->oam2_addr] = p->oam_copy;
            if (p->spr_l == 0) {
                if (inr) {
                    if (first) p->spr0_added = true;
                    oam2_inc(p); p->spr_l = 1; addr += 1;
                } else {
                    addr = (addr + 4) & 0x1FC;
                }
            } else {
                oam2_inc(p);
                if (p->spr_l == 3) { addr = inr ? addr + 1 : ((addr + 1) & 0x1FC); p->spr_l = 0; }
                else { addr += 1; p->spr_l++; }
            }
            if (addr > 0xFF) p->copy_done = true;
        } else {
            /* secondary OAM full: writes become reads; overflow search (with the bug) */
            p->oam_copy = p->oam2[p->oam2_addr];
            if (p->spr_l == 0 && !inr && p->overflow_cnt == 0) {
                addr = ((((addr >> 2) + 1) << 2) | ((addr + 1) & 3));  /* n and m both increment */
            } else {
                if (p->overflow_cnt == 0) { p->ovf_pending = true; p->overflow_cnt = 3; addr += 1; }
                else {
                    addr += 1;
                    if (--p->overflow_cnt == 0) { p->copy_done = true; addr &= 0x1FC; }
                }
            }
            if (addr > 0xFF) p->copy_done = true;
        }
        p->oam_addr = (uint8_t)addr;
    }
    if (d == 255) p->oam2_ovf = false;
}

static inline uint8_t rev8(uint8_t b) {
    b = (uint8_t)((b & 0xF0) >> 4 | (b & 0x0F) << 4);
    b = (uint8_t)((b & 0xCC) >> 2 | (b & 0x33) << 2);
    b = (uint8_t)((b & 0xAA) >> 1 | (b & 0x55) << 1);
    return b;
}

/* pattern address register for the sprite currently being fetched */
static uint16_t sprite_par(Ppu *p, bool *in_range) {
    int h = (p->ctrl & 0x20) ? 16 : 8;
    int line = p->scanline & 0xFF;            /* the pre-render line compares as line 5 */
    int row = line - p->f_y;
    *in_range = row >= 0 && row < h;
    uint8_t tile = p->f_tile, attr = p->f_attr;
    row &= h - 1;
    if (attr & 0x80) row = h - 1 - row;
    if (h == 16)
        return (uint16_t)(((tile & 1) << 12) | ((tile & 0xFE) << 4) | ((row & 8) << 1) | (row & 7));
    return (uint16_t)(((p->ctrl & 0x08) << 9) | (tile << 4) | row);
}

/* ------------------------------------------------------------ main step */
static void ppu_step(Nes *n) {
    Ppu *p = P;
    int sl = p->scanline, d = p->dot;

    if (p->s0_pending) { p->status |= 0x40; p->s0_pending = false; }
    if (p->ovf_pending) { p->status |= 0x20; p->ovf_pending = false; }
    if (p->ignore_2007) p->ignore_2007--;
    if (p->mask_delay && --p->mask_delay == 0) p->mask = p->mask_pending;
    if (p->v_delay) {
        if (--p->v_delay == 0) {
            p->v = p->pending_v;
            if (!(p->rend && render_line(p))) { p->bus_addr = p->v & 0x3FFF; cart_ppu_addr(n, p->v & 0x3FFF); }
        }
    }

    bool was_rend = p->rend;
    bool rend = (p->mask & 0x18) != 0;
    p->rend = rend;
    bool rl = render_line(p);

    /* OAM row corruption: seeded by the OAM2 address when rendering is
     * switched off mid-frame, applied on the next rendering dot */
    if (rl && was_rend && !rend) {
        int seed = p->oam2_addr;
        if (d >= 65 && d <= 256) seed = ((seed + 3) & ~3) & 31;
        p->corrupt_seed = (uint8_t)seed; p->corrupt_pending = true;
    }
    if (rl && rend && p->corrupt_pending) {
        p->corrupt_pending = false;
        int s = p->corrupt_seed;
        if (s) { memcpy(&p->oam[s * 8], &p->oam[0], 8); p->oam2[s] = p->oam2[0]; }
    }

    /* PPU DATA state machine events this dot */
    bool sm_ale = p->sm_r_ale && --p->sm_r_ale == 0;
    bool sm_rd = p->sm_r_rd && --p->sm_r_rd == 0;
    bool sm_wale = p->sm_w_ale && --p->sm_w_ale == 0;
    bool sm_wr = p->sm_w_wr && --p->sm_w_wr == 0;

    int cad = CAD_NONE;
    uint8_t cad_val = 0;
    uint8_t ad_prev = p->adbus;

    if (rl) {
        bool pre = sl == 261;
        bool fetch_bg = (d >= 1 && d <= 256) || (d >= 321 && d <= 336);
        bool fetch_spr = d >= 257 && d <= 320;

        /* background shift registers: the pixel uses the state before this
         * dot's shift; shifts on dots 1-256 and 321-336; new tile data is
         * loaded on the dot its high pattern byte is read (dot % 8 == 0) */
        uint16_t bg_lo = p->sh_lo, bg_hi = p->sh_hi;
        uint8_t bg_alo = p->ash_lo, bg_ahi = p->ash_hi;
        if (rend && ((d >= 1 && d <= 256) || (d >= 321 && d <= 336))) {
            p->sh_lo = (uint16_t)(p->sh_lo << 1);
            p->sh_hi = (uint16_t)(p->sh_hi << 1 | 1);
            p->ash_lo = (uint8_t)(p->ash_lo << 1 | (p->at_latch & 1));
            p->ash_hi = (uint8_t)(p->ash_hi << 1 | ((p->at_latch >> 1) & 1));
        }

        /* --- pixel output and sprite counters */
        if (d >= 1 && d <= 256) {
            int x = d - 1;
            uint8_t mask = p->mask;
            uint8_t bgp = 0, spp = 0, spattr = 0; bool sp0 = false;
            if (rend && (mask & 0x08) && (x >= 8 || (mask & 0x02))) {
                int bit = 15 - p->x;
                uint8_t pix = (uint8_t)(((bg_lo >> bit) & 1) | (((bg_hi >> bit) & 1) << 1));
                if (pix) {
                    int ab = 7 - p->x;
                    uint8_t at = (uint8_t)(((bg_alo >> ab) & 1) | (((bg_ahi >> ab) & 1) << 1));
                    bgp = (uint8_t)(at << 2 | pix);
                }
            }
            bool show_spr = rend && (mask & 0x10) && (x >= 8 || (mask & 0x04));
            /* 2C02 quirk: after the pre-render line skips its last dot, every
             * sprite's first pixel is drawn at x=0 on scanline 0 and the rest
             * of it lands one pixel to the left */
            bool quirk = p->skip_quirk && sl == 0 && x == 0;
            if (x == 0) p->skip_quirk = false;
            for (int i = 0; i < 8; i++) {
                if (!p->s_active[i]) {
                    if (p->s_cnt[i] == 0) p->s_active[i] = true;
                    else {
                        p->s_cnt[i]--;
                        if (!quirk || !rend) continue;
                        uint8_t qp = (uint8_t)((p->s_lo[i] >> 7) | ((p->s_hi[i] >> 7) << 1));
                        p->s_lo[i] <<= 1; p->s_hi[i] <<= 1;
                        if (qp && show_spr && !(spp & 3)) {
                            spp = (uint8_t)(0x10 | (p->s_attr[i] & 3) << 2 | qp);
                            spattr = p->s_attr[i];
                            sp0 = (i == 0) && p->spr0_line;
                        }
                        continue;
                    }
                }
                if (!rend) continue;
                uint8_t pix = (uint8_t)((p->s_lo[i] >> 7) | ((p->s_hi[i] >> 7) << 1));
                p->s_lo[i] <<= 1; p->s_hi[i] <<= 1;
                if (pix && show_spr && !(spp & 3)) {
                    spp = (uint8_t)(0x10 | (p->s_attr[i] & 3) << 2 | pix);
                    spattr = p->s_attr[i];
                    sp0 = (i == 0) && p->spr0_line;
                }
            }
            if (!pre) {
                if (sp0 && (bgp & 3) && x != 255 && !(p->status & 0x40)) p->s0_pending = true;
                uint8_t idx;
                if ((spp & 3) && (!(bgp & 3) || !(spattr & 0x20))) idx = spp;
                else idx = (bgp & 3) ? bgp : 0;
                uint8_t col;
                if (!rend && (p->v & 0x3F00) == 0x3F00) col = pal_read(p, p->v);
                else col = pal_read(p, idx);
                if (mask & 0x01) col &= 0x30;
                p->fb[sl * 256 + x] = (uint16_t)(col | ((mask & 0xE0) << 1));
            }
        }

        if (rend) {
            /* --- fetch cadence: ALE on odd dots, read on even dots */
            if (fetch_bg) {
                int r = (d - 1) & 7;
                uint16_t nt_addr = (uint16_t)(0x2000 | (p->v & 0x0FFF));
                uint16_t at_addr = (uint16_t)(0x23C0 | (p->v & 0x0C00) | ((p->v >> 4) & 0x38) | ((p->v >> 2) & 0x07));
                uint16_t par = (uint16_t)(((p->ctrl & 0x10) << 8) | (p->nt << 4) | ((p->v >> 12) & 7));
                switch (r) {
                case 0: bus_ale(n, nt_addr); cad = CAD_ALE; break;
                case 1: cad_val = p->nt = bus_rd(n, nt_addr); cad = CAD_RD; break;
                case 2: bus_ale(n, at_addr); cad = CAD_ALE; break;
                case 3: {
                    cad_val = bus_rd(n, at_addr); cad = CAD_RD;
                    int sh = ((p->v >> 4) & 4) | (p->v & 2);
                    p->at = (cad_val >> sh) & 3;
                    break;
                }
                case 4: bus_ale(n, par); cad = CAD_ALE; break;
                case 5: cad_val = p->plo = bus_rd(n, par); cad = CAD_RD; break;
                case 6: bus_ale(n, (uint16_t)(par | 8)); cad = CAD_ALE; break;
                case 7:
                    cad_val = p->phi = bus_rd(n, (uint16_t)(par | 8)); cad = CAD_RD; inc_hori(p);
                    p->sh_lo = (uint16_t)((p->sh_lo & 0xFF00) | p->plo);
                    p->sh_hi = (uint16_t)((p->sh_hi & 0xFF00) | p->phi);
                    p->at_latch = p->at;
                    break;
                }
                if (d == 256 && was_rend) inc_vert(p);   /* not when rendering was just enabled */
            } else if (fetch_spr) {
                int rel = d - 257, slot = rel >> 3, r = rel & 7;
                if (d == 257) { p->oam2_addr = 0; p->spr0_line = p->spr0_added; }
                p->oam_addr = 0;
                /* secondary OAM read through the OAM2 address counter */
                p->oam_copy = p->oam2[p->oam2_addr];
                switch (r) {
                case 0: p->f_y = p->oam_copy; break;
                case 1: p->f_tile = p->oam_copy; break;
                case 2: p->f_attr = p->oam_copy; p->s_attr[slot] = p->oam_copy; break;
                default: p->f_x = p->oam_copy; p->s_cnt[slot] = p->oam_copy; break;
                }
                if (r <= 2 || r == 7) oam2_inc(p);
                uint16_t nt_addr = (uint16_t)(0x2000 | (p->v & 0x0FFF));
                bool inr;
                switch (r) {
                case 0: bus_ale(n, nt_addr); cad = CAD_ALE;
                        if (d == 257) p->v = (uint16_t)((p->v & ~0x041F) | (p->t & 0x041F));
                        break;
                case 1: cad_val = bus_rd(n, nt_addr); cad = CAD_RD; break;
                case 2: bus_ale(n, nt_addr); cad = CAD_ALE; break;
                case 3: cad_val = bus_rd(n, nt_addr); cad = CAD_RD; break;
                case 4: bus_ale(n, sprite_par(p, &inr)); cad = CAD_ALE; break;
                case 5: {
                    uint16_t par = sprite_par(p, &inr);
                    cad_val = bus_rd(n, par); cad = CAD_RD;
                    uint8_t b = inr ? cad_val : 0;
                    p->s_lo[slot] = (p->f_attr & 0x40) ? rev8(b) : b;
                    break;
                }
                case 6: bus_ale(n, (uint16_t)(sprite_par(p, &inr) + 8)); cad = CAD_ALE; break;
                case 7: {
                    uint16_t par = (uint16_t)(sprite_par(p, &inr) + 8);
                    cad_val = bus_rd(n, par); cad = CAD_RD;
                    uint8_t b = inr ? cad_val : 0;
                    p->s_hi[slot] = (p->f_attr & 0x40) ? rev8(b) : b;
                    break;
                }
                }
                if (pre && d >= 280 && d <= 304) p->v = (uint16_t)((p->v & ~0x7BE0) | (p->t & 0x7BE0));
            } else if (d >= 337) {
                uint16_t nt_addr = (uint16_t)(0x2000 | (p->v & 0x0FFF));
                if (d & 1) { bus_ale(n, nt_addr); cad = CAD_ALE; }
                else { cad_val = bus_rd(n, nt_addr); cad = CAD_RD; }
            } else if (d == 0 && !pre) {
                /* continues the previous line's fetch sequence (not after vblank) */
                bus_ale(n, (uint16_t)(((p->ctrl & 0x10) << 8) | (p->nt << 4) | ((p->v >> 12) & 7)));
                cad = CAD_ALE;
            }

            /* --- OAM side */
            if (!pre && d >= 1 && d <= 256) sprite_eval(n, d);
            else if (pre && (d == 63 || d == 255)) p->oam2_ovf = false;  /* no evaluation, but the flag still clears */
            if (d >= 321 || d == 0) p->oam_copy = p->oam2[p->oam2_addr];
            if (d == 339) {
                p->oam2_addr = 0; p->oam2_ovf = false;
                for (int i = 0; i < 8; i++) p->s_active[i] = false;   /* counters start counting */
            }
        }
        if (pre && d == 1) {
            p->status &= 0x1F; p->s0_pending = false; p->ovf_pending = false;
            update_nmi(n);
        }
    } else if (sl == 241 && d == 1) {
        if (!p->prevent_vbl) {
            p->status |= 0x80;
            update_nmi(n);
        }
        p->prevent_vbl = false;
        p->frame_done = true;
        p->frame_phase = p->line0_phase;
    }

    /* --- PPU DATA state machine (collides with the rendering cadence) */
    if (sm_ale || sm_wale) {
        if (cad == CAD_ALE) {
            /* rendering takes priority: address and latch come from the fetch */
        } else if (cad == CAD_RD) {
            p->octal = cad_val;                   /* ALE during a read: latch follows the data */
        } else {
            bus_ale(n, p->v);
        }
        p->sm_hi = (uint16_t)(p->v & 0x3F00);
        p->sm_addr = (uint16_t)((p->v & 0x3F00) | p->octal);
        if (sm_wale) p->sm_waddr = p->v & 0x3FFF;
    }
    if (sm_rd) {
        uint8_t val;
        if (cad == CAD_RD) val = cad_val;         /* synchronised with a rendering read */
        else if (cad == CAD_ALE) {
            /* read while the latch is open: the latch keeps the previous data */
            p->octal = ad_prev;
            val = bus_rd(n, p->bus_addr);
        } else val = bus_rd(n, p->sm_hi);
        p->buf = val;
        v_increment(n);
    }
    if (sm_wr) {
        uint16_t addr = p->sm_waddr;
        if (addr >= 0x3F00) pal_write(p, addr, p->sm_wdata);
        else { p->bus_addr = addr; cart_ppu_addr(n, addr); cart_ppu_write(n, addr, p->sm_wdata); }
        v_increment(n);
    }

    /* advance */
    p->dots++;
    /* the odd-frame skip samples $2001 through a shorter path than rendering */
    bool skip_rend = ((p->dots - p->mask_write_dot >= SKIP_DELAY) ? p->mask_pending : p->mask_prev) & 0x18;
    if (sl == 261 && d == 339 && (p->frame & 1) && skip_rend) {
        p->dot = 0; p->scanline = 0; p->frame++;
        p->skip_quirk = true;
        p->line0_phase = (uint8_t)((p->dots * 8) % 12);
        return;
    }
    if (++p->dot > 340) {
        p->dot = 0;
        /* overclock: repeat the idle post-render line before VBlank. The PPU
         * keeps working (register writes, $2007 accesses), it just draws nothing */
        if (p->scanline == 239) n->oc_left = (uint32_t)(n->oc_lines > 0 ? n->oc_lines : 0);
        if (p->scanline == 240 && n->oc_left) { n->oc_left--; n->oc_active = true; return; }
        n->oc_active = false;
        if (++p->scanline > 261) { p->scanline = 0; p->frame++; p->line0_phase = (uint8_t)((p->dots * 8) % 12); }
    }
}

void ppu_run(Nes *n, uint64_t to) {
    Ppu *p = P;
    do {
        ppu_step(n);
        p->master += 4;
    } while (p->master + 4 <= to);
}

void ppu_power(Nes *n) {
    Ppu *p = P;
    memset(p->oam, 0, sizeof p->oam);
    memset(p->oam2, 0xFF, sizeof p->oam2);
    static const uint8_t pal_init[32] = {
        0x09,0x01,0x00,0x01,0x00,0x02,0x02,0x0D,0x08,0x10,0x08,0x24,0x00,0x00,0x04,0x2C,
        0x09,0x01,0x34,0x03,0x00,0x04,0x00,0x14,0x08,0x3A,0x00,0x02,0x00,0x20,0x2C,0x08 };
    memcpy(p->pal, pal_init, 32);
    memset(p->ciram, 0, sizeof p->ciram);
    p->status = 0; p->oam_addr = 0; p->v = p->t = 0; p->x = 0; p->w = false;
    p->io = 0; memset(p->io_stamp, 0, sizeof p->io_stamp);
    p->frame = 0; p->dots = 0; p->master = 0;
    p->oam2_addr = 0; p->oam2_ovf = false; p->corrupt_pending = false;
    for (int i = 0; i < 8; i++) { p->s_cnt[i] = 0xFF; p->s_active[i] = false; p->s_lo[i] = p->s_hi[i] = 0; p->s_attr[i] = 0; }
    ppu_reset(n);
}

void ppu_reset(Nes *n) {
    Ppu *p = P;
    p->ctrl = 0; p->mask = 0; p->mask_pending = 0; p->mask_delay = 0;
    p->w = false; p->buf = 0; p->t = 0; p->x = 0;
    p->scanline = 0; p->dot = 0;
    p->rend = p->rend_prev = false;
    p->prevent_vbl = false; p->v_delay = 0;
    p->sm_r_ale = p->sm_r_rd = p->sm_w_ale = p->sm_w_wr = 0;
    p->spr0_visible = p->spr0_line = false;
    p->sh_lo = p->sh_hi = 0; p->ash_lo = p->ash_hi = 0;
    p->frame_done = false;
    p->master = 0;
    n->cpu.nmi_flag = false;
}
