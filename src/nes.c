/*
 * System glue: CPU bus decoding, controllers, power/reset, frame loop,
 * audio ring buffer and save states.
 */
#include "nes.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

static uint8_t pad_read(Nes *n, int port) {
    uint8_t bit;
    if (n->strobe) bit = n->pad_state[port] & 1;
    else { bit = n->pad_shift[port] & 1; n->pad_shift[port] = (uint8_t)(n->pad_shift[port] >> 1 | 0x80); }
    return bit;
}

uint8_t bus_read(Nes *n, uint16_t a) {
    uint8_t v;
    if (a < 0x2000) v = n->ram[a & 0x7FF];
    else if (a < 0x4000) v = ppu_reg_read(n, a);
    else if (a < 0x4020) {
        if (a == 0x4015) {
            /* internal register: bit 5 comes from the 2A03's internal data
             * bus, and the read does not drive the external data bus */
            v = (uint8_t)(apu_read_status(n) | (n->cpu.int_bus & 0x20));
            n->cpu.int_bus = v;
            return v;
        } else if (a == 0x4016 || a == 0x4017) {
            v = (uint8_t)(pad_read(n, a & 1) | (n->cpu.open_bus & 0xE0));
        } else v = n->cpu.open_bus;
    } else {
        v = cart_cpu_read(n, a, n->cpu.open_bus);
        if (n->num_cheats && a >= 0x8000) {
            /* the Game Genie sits between the cartridge and the console */
            for (int i = 0; i < n->num_cheats; i++) {
                const Cheat *c = &n->cheats[i];
                if (c->addr == a && (!c->has_compare || c->compare == v)) { v = c->value; break; }
            }
        }
    }
    n->cpu.open_bus = v;
    n->cpu.int_bus = v;
    return v;
}

void input_latch_clock(Nes *n) {
    bool s = n->out_latch_pending & 1;
    n->strobe = s;
    if (s) { n->pad_shift[0] = n->pad_state[0]; n->pad_shift[1] = n->pad_state[1]; }
}

uint8_t bus_peek(Nes *n, uint16_t a) {
    if (a < 0x2000) return n->ram[a & 0x7FF];
    if (a < 0x4000) return ppu_reg_peek(n, a);
    if (a < 0x4020) return n->cpu.open_bus;
    return cart_cpu_peek(n, a, n->cpu.open_bus);
}

void bus_write(Nes *n, uint16_t a, uint8_t v) {
    n->cpu.open_bus = v;
    n->cpu.int_bus = v;
    if (a < 0x2000) n->ram[a & 0x7FF] = v;
    else if (a < 0x4000) ppu_reg_write(n, a, v);
    else if (a < 0x4020) {
        if (a == 0x4014) {
            n->cpu.sprite_dma = true; n->cpu.sprite_page = v; n->cpu.need_halt = true;
        } else if (a == 0x4016) {
            /* OUT0 is latched by the 2A03 on the next get->put transition */
            n->out_latch_pending = v & 1;
        } else apu_write(n, a, v);
    } else cart_cpu_write(n, a, v);
}

bool nes_load(Nes *n, const uint8_t *data, size_t size, char *err, size_t errlen) {
    double sr = n->apu.sample_rate;
    nes_unload(n);
    if (!cart_load(n, data, size, err, errlen)) return false;
    n->apu.sample_rate = sr;
    palette_generate(n->palette_rgb);
    n->loaded = true;
    nes_power(n);
    return true;
}

void nes_unload(Nes *n) {
    if (n->loaded) cart_free(n);
    n->loaded = false;
}

void nes_power(Nes *n) {
    /* RAM power-up state varies on hardware; most consoles power up with a
     * pattern close to this, and it is what most test ROMs expect */
    for (int i = 0; i < 2048; i++) n->ram[i] = (i & 4) ? 0xFF : 0x00;
    n->strobe = false; n->out_latch_pending = 0; n->pad_shift[0] = n->pad_shift[1] = 0;
    n->oc_left = 0; n->oc_active = false;
    ppu_power(n);
    apu_power(n);
    cart_power(n);
    cpu_power(n);
}

void nes_reset(Nes *n) {
    n->oc_left = 0; n->oc_active = false;
    ppu_reset(n);
    apu_reset(n);
    cart_reset(n);
    n->ppu.master = 0;
    n->cpu.master = 0;
    cpu_reset(n);
}

void nes_run_frame(Nes *n) {
    if (!n->loaded) return;
    n->ppu.frame_done = false;
    while (!n->ppu.frame_done) cpu_step(n);
}

void nes_set_sample_rate(Nes *n, double rate) {
    n->apu.sample_rate = rate;
    n->apu.cycles_per_sample = 1789773.0 / rate;
}

int nes_audio_available(Nes *n) {
    uint32_t w = __atomic_load_n(&n->apu.ring_w, __ATOMIC_ACQUIRE);
    uint32_t r = __atomic_load_n(&n->apu.ring_r, __ATOMIC_ACQUIRE);
    return (int)((w - r) & (AUDIO_RING - 1));
}

int nes_read_audio(Nes *n, float *out, int max) {
    int cnt = 0;
    uint32_t r = __atomic_load_n(&n->apu.ring_r, __ATOMIC_RELAXED);
    uint32_t w = __atomic_load_n(&n->apu.ring_w, __ATOMIC_ACQUIRE);
    while (cnt < max && r != w) { out[cnt++] = n->apu.ring[r]; r = (r + 1) & (AUDIO_RING - 1); }
    __atomic_store_n(&n->apu.ring_r, r, __ATOMIC_RELEASE);
    return cnt;
}

void nes_render_rgb(Nes *n, uint32_t *out) {
    for (int i = 0; i < 256 * 240; i++) out[i] = n->palette_rgb[n->ppu.fb[i] & 0x1FF];
}

/* ------------------------------------------------------------ save states */
#define STATE_MAGIC 0x4E455353u  /* "NESS" */
#define STATE_VER 3

typedef struct {
    uint32_t magic, version, crc, pad;
    Cpu cpu;
    Ppu ppu;
    Apu apu;   /* ring buffer part is skipped on load */
    Cart cart;
    uint8_t ram[2048];
    uint8_t strobe, pad_shift[2];
} StateHdr;

size_t nes_state_size(void) {
    return sizeof(StateHdr) + 0x80000 + 0x20000;  /* enough for wram + chr ram */
}

bool nes_save_state(Nes *n, uint8_t *buf, size_t len) {
    if (!n->loaded || len < nes_state_size()) return false;
    StateHdr *h = (StateHdr *)buf;
    memset(h, 0, sizeof *h);
    h->magic = STATE_MAGIC; h->version = STATE_VER; h->crc = n->cart.crc;
    h->cpu = n->cpu; h->ppu = n->ppu; h->apu = n->apu; h->cart = n->cart;
    memcpy(h->ram, n->ram, 2048);
    h->strobe = n->strobe; h->pad_shift[0] = n->pad_shift[0]; h->pad_shift[1] = n->pad_shift[1];
    uint8_t *p = buf + sizeof *h;
    if (n->cart.wram_size <= 0x80000) memcpy(p, n->cart.wram, n->cart.wram_size);
    p += 0x80000;
    if (n->cart.chr_is_ram && n->cart.chr_size <= 0x20000) memcpy(p, n->cart.chr, n->cart.chr_size);
    return true;
}

bool nes_load_state(Nes *n, const uint8_t *buf, size_t len) {
    if (!n->loaded || len < nes_state_size()) return false;
    const StateHdr *h = (const StateHdr *)buf;
    if (h->magic != STATE_MAGIC || h->version != STATE_VER || h->crc != n->cart.crc) return false;
    Cart keep = n->cart;
    n->cpu = h->cpu;
    /* keep frame buffer as-is to avoid a flash */
    uint16_t *fb = malloc(sizeof n->ppu.fb);
    memcpy(fb, n->ppu.fb, sizeof n->ppu.fb);
    n->ppu = h->ppu;
    memcpy(n->ppu.fb, fb, sizeof n->ppu.fb); free(fb);
    {
        uint32_t rw = n->apu.ring_w, rr = n->apu.ring_r; double sr = n->apu.sample_rate, cps = n->apu.cycles_per_sample;
        float ring_keep0 = n->apu.ring[0];
        memcpy(&n->apu, &h->apu, offsetof(Apu, ring));
        n->apu.ring_w = rw; n->apu.ring_r = rr; n->apu.sample_rate = sr; n->apu.cycles_per_sample = cps;
        n->apu.ring[0] = ring_keep0;
        n->apu.exp_audio = h->apu.exp_audio;
    }
    n->cart = h->cart;
    n->cart.prg = keep.prg; n->cart.chr = keep.chr; n->cart.wram = keep.wram;
    memcpy(n->ram, h->ram, 2048);
    n->strobe = h->strobe; n->pad_shift[0] = h->pad_shift[0]; n->pad_shift[1] = h->pad_shift[1];
    const uint8_t *p = buf + sizeof *h;
    if (n->cart.wram_size <= 0x80000) memcpy(n->cart.wram, p, n->cart.wram_size);
    p += 0x80000;
    if (n->cart.chr_is_ram && n->cart.chr_size <= 0x20000) memcpy(n->cart.chr, p, n->cart.chr_size);
    cart_sync(n);
    return true;
}

/* ------------------------------------------------------------ Game Genie */
bool gg_decode(const char *code, Cheat *out) {
    static const char letters[] = "APZLGITYEOXUKSVN";
    int n[8], len = 0;
    for (const char *p = code; *p; p++) {
        char ch = *p;
        if (ch == ' ' || ch == '-') continue;
        if (ch >= 'a' && ch <= 'z') ch = (char)(ch - 32);
        const char *f = strchr(letters, ch);
        if (!f || len == 8) return false;
        n[len++] = (int)(f - letters);
    }
    if (len != 6 && len != 8) return false;
    out->addr = (uint16_t)(0x8000 | ((n[3] & 7) << 12) | ((n[5] & 7) << 8) | ((n[4] & 8) << 8) |
                           ((n[2] & 7) << 4) | ((n[1] & 8) << 4) | (n[4] & 7) | (n[3] & 8));
    if (len == 6) {
        out->value = (uint8_t)(((n[1] & 7) << 4) | ((n[0] & 8) << 4) | (n[0] & 7) | (n[5] & 8));
        out->has_compare = false; out->compare = 0;
    } else {
        out->value = (uint8_t)(((n[1] & 7) << 4) | ((n[0] & 8) << 4) | (n[0] & 7) | (n[7] & 8));
        out->compare = (uint8_t)(((n[7] & 7) << 4) | ((n[6] & 8) << 4) | (n[6] & 7) | (n[5] & 8));
        out->has_compare = true;
    }
    return true;
}
