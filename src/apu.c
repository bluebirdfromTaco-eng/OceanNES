/*
 * 2A03 APU: two pulses, triangle, noise, DMC, frame counter.
 * Clocked once per CPU cycle with hardware-accurate frame-counter timing,
 * $4017 write delays, length-counter reload/halt ordering, and DMC DMA.
 * Output uses the non-linear hardware mixer followed by the NES's analog
 * filter chain (90Hz + 440Hz high-pass, 14kHz low-pass).
 */
#include "nes.h"
#include <string.h>
#include <math.h>
#include <stdio.h>

#define A (&n->apu)
#define CPU_HZ 1789773.0
#ifndef DMC_LINGER
#define DMC_LINGER 2
#endif
#ifndef DMC_PARITY
#define DMC_PARITY 0
#endif

static const uint8_t len_table[32] = {
    10,254,20,2,40,4,80,6,160,8,60,10,14,12,26,14,12,16,24,18,48,20,96,22,192,24,72,26,16,28,32,30 };
static const uint8_t duty_table[4][8] = {
    {0,0,0,0,0,0,0,1},{0,0,0,0,0,0,1,1},{0,0,0,0,1,1,1,1},{1,1,1,1,1,1,0,0} };
static const uint8_t tri_table[32] = {
    15,14,13,12,11,10,9,8,7,6,5,4,3,2,1,0,0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15 };
static const uint16_t noise_period[16] = {
    4,8,16,32,64,96,128,160,202,254,380,508,762,1016,2034,4068 };
static const uint16_t dmc_rate[16] = {
    428,380,340,320,286,254,226,214,190,160,142,128,106,84,72,54 };

static float pulse_mix[31];
static float tnd_mix[203];
static bool tables_ready;

static void init_tables(void) {
    if (tables_ready) return;
    pulse_mix[0] = 0;
    for (int i = 1; i < 31; i++) pulse_mix[i] = (float)(95.52 / (8128.0 / i + 100.0));
    tnd_mix[0] = 0;
    for (int i = 1; i < 203; i++) tnd_mix[i] = (float)(163.67 / (24329.0 / i + 100.0));
    tables_ready = true;
}

/* ------------------------------------------------------------ units */
static inline void len_clock(LenCtr *l) { if (l->counter && !l->halt) l->counter--; }
static inline void len_load(LenCtr *l, uint8_t idx) {
    if (l->enabled) { l->reload_val = len_table[idx]; l->prev_val = l->counter; l->reload_pending = true; }
}
static inline void len_apply(LenCtr *l) {
    if (l->reload_pending) {
        if (l->counter == l->prev_val) l->counter = l->reload_val;
        l->reload_pending = false;
    }
    l->halt = l->new_halt;
}
static inline void env_clock(Envelope *e) {
    if (e->start) { e->start = false; e->decay = 15; e->divider = e->vol; }
    else if (e->divider == 0) {
        e->divider = e->vol;
        if (e->decay) e->decay--;
        else if (e->loop) e->decay = 15;
    } else e->divider--;
}
static inline uint8_t env_out(Envelope *e) { return e->constant ? e->vol : e->decay; }

static void pulse_update_target(Pulse *p) {
    uint16_t delta = p->period >> p->sweep_shift;
    if (p->sweep_neg) p->target = (uint16_t)(p->period - delta - (p->is_p1 ? 1 : 0));
    else p->target = (uint16_t)(p->period + delta);
}
static inline bool pulse_muted(Pulse *p) {
    return p->period < 8 || (!p->sweep_neg && p->target > 0x7FF);
}
static void pulse_sweep(Pulse *p) {
    pulse_update_target(p);
    if (p->sweep_div == 0 && p->sweep_en && p->sweep_shift && !pulse_muted(p)) {
        p->period = p->target; pulse_update_target(p);
    }
    if (p->sweep_div == 0 || p->sweep_reload) { p->sweep_div = p->sweep_period; p->sweep_reload = false; }
    else p->sweep_div--;
}
static inline uint8_t pulse_out(Pulse *p) {
    if (!p->len.counter || pulse_muted(p) || !duty_table[p->duty][p->step]) return 0;
    return env_out(&p->env);
}

static void quarter(Nes *n) {
    Apu *a = A;
    env_clock(&a->p1.env); env_clock(&a->p2.env); env_clock(&a->noise.env);
    if (a->tri.lin_reload_flag) a->tri.lin_ctr = a->tri.lin_reload;
    else if (a->tri.lin_ctr) a->tri.lin_ctr--;
    if (!a->tri.control) a->tri.lin_reload_flag = false;
}
static void half(Nes *n) {
    Apu *a = A;
    len_clock(&a->p1.len); len_clock(&a->p2.len); len_clock(&a->tri.len); len_clock(&a->noise.len);
    pulse_sweep(&a->p1); pulse_sweep(&a->p2);
}

/* ------------------------------------------------------------ DMC */
static void dmc_start_transfer(Nes *n) {
    Dmc *d = &A->dmc;
    /* a DMA can't start until the $4015 enable has taken effect; the
     * pending request is re-issued when the enable delay expires */
    if (d->start_delay > 0) return;
    if (d->buf_empty && d->remaining > 0) cpu_start_dmc_dma(n);
}
static void dmc_restart(Dmc *d) { d->addr = d->sample_addr; d->remaining = d->sample_len; }

uint16_t apu_dmc_addr(Nes *n) { return A->dmc.addr; }
bool apu_dmc_wants_dma(Nes *n) { return A->dmc.buf_empty && A->dmc.remaining > 0; }

void apu_dmc_dma_done(Nes *n, uint8_t v) {
    Dmc *d = &A->dmc;
    if (d->remaining == 0) return;
    d->buffer = v; d->buf_empty = false;
    d->addr = (uint16_t)(d->addr == 0xFFFF ? 0x8000 : d->addr + 1);
    if (--d->remaining == 0) {
        if (d->loop) dmc_restart(d);
        else {
            d->linger = DMC_LINGER;
            if (d->irq_en) cpu_set_irq(n, IRQ_DMC);
        }
    }
}

static void dmc_clock(Nes *n) {
    Dmc *d = &A->dmc;
    /* the DMC timer is clocked by the APU (every other CPU cycle) */
    if (d->timer > 2) { d->timer -= 2; return; }
    {
        d->timer = d->rate;
        d->reload_cycle = n->cpu.cycles;
        if (!d->silence) {
            if (d->shift & 1) { if (d->level <= 125) d->level += 2; }
            else { if (d->level >= 2) d->level -= 2; }
        }
        d->shift >>= 1;
        if (d->bits) d->bits--;
        if (d->bits == 0) {
            d->bits = 8;
            if (d->buf_empty) d->silence = true;
            else {
                d->silence = false; d->shift = d->buffer; d->buf_empty = true;
                if (d->remaining == 0 && d->linger > 0 && !n->cpu.dmc_running) {
                    /* the channel still looks enabled right after its last
                     * fetch: a reload DMA starts, finds nothing to read and
                     * aborts after its halt cycle (an "implicit abort") */
                    cpu_start_dmc_dma(n); n->cpu.abort_dmc = true;
                } else dmc_start_transfer(n);
            }
        }
    }
}

/* ------------------------------------------------------------ mixing */
/* first-order RC filters */
static double highpass(double *prev_in, double *prev_out, double x, double fc, double dt) {
    double rc = 1.0 / (2 * M_PI * fc), k = rc / (rc + dt);
    double y = k * (*prev_out + x - *prev_in);
    *prev_in = x; *prev_out = y;
    return y;
}
static double lowpass(double *prev, double x, double fc, double dt) {
    double rc = 1.0 / (2 * M_PI * fc), k = dt / (rc + dt);
    *prev += k * (x - *prev);
    return *prev;
}

static void output_sample(Nes *n, float s) {
    Apu *a = A;
    /* single-producer / single-consumer ring (the consumer may be an audio thread) */
    uint32_t w = __atomic_load_n(&a->ring_w, __ATOMIC_RELAXED);
    uint32_t r = __atomic_load_n(&a->ring_r, __ATOMIC_ACQUIRE);
    if (((w + 1) & (AUDIO_RING - 1)) == r) return; /* full: drop */
    a->ring[w] = s;
    __atomic_store_n(&a->ring_w, (w + 1) & (AUDIO_RING - 1), __ATOMIC_RELEASE);
}

static void mix(Nes *n) {
    Apu *a = A;
    uint8_t p1 = pulse_out(&a->p1), p2 = pulse_out(&a->p2);
    uint8_t t = tri_table[a->tri.step];
    uint8_t no = (a->noise.len.counter && !(a->noise.shift & 1)) ? env_out(&a->noise.env) : 0;
    float out = pulse_mix[p1 + p2] + tnd_mix[3 * t + 2 * no + a->dmc.level];
    out += cart_audio(n);
    a->acc += out; a->acc_n++;
    a->sample_acc_cycles += 1.0;
    if (a->sample_acc_cycles >= a->cycles_per_sample) {
        a->sample_acc_cycles -= a->cycles_per_sample;
        double s = a->acc / a->acc_n;
        a->acc = 0; a->acc_n = 0;
        double dt = 1.0 / a->sample_rate;
        int mode = n->audio_filter;
        double y;
        if (mode == AUDIO_FILTER_RAW) {
            y = highpass(&a->hp1_prev_in, &a->hp1_prev_out, s, 10.0, dt);   /* DC blocker only */
        } else {
            y = highpass(&a->hp1_prev_in, &a->hp1_prev_out, s, mode == AUDIO_FILTER_FAMICOM ? 37.0 : 90.0, dt);
            if (mode != AUDIO_FILTER_FAMICOM) y = highpass(&a->hp2_prev_in, &a->hp2_prev_out, y, 440.0, dt);
            y = lowpass(&a->lp_prev, y, 14000.0, dt);
            if (mode == AUDIO_FILTER_TV) {
                y = highpass(&n->af_state[0], &n->af_state[1], y, 150.0, dt);
                y = lowpass(&n->af_state[2], y, 4500.0, dt);
                y = lowpass(&n->af_state[3], y, 4500.0, dt);
            }
        }
        output_sample(n, (float)(y * 1.4));
    }
}

/* ------------------------------------------------------------ clock */
void apu_clock(Nes *n) {
    Apu *a = A;
    a->cycle++;
    bool apu_cycle = (n->cpu.cycles & 1) == 0;

    bool get_cycle = (n->cpu.cycles & 1) == 1;
    if (a->fc_clear_pending && get_cycle) { a->fc_flag = false; cpu_clear_irq(n, IRQ_FRAME); a->fc_clear_pending = false; }
    if (!get_cycle) input_latch_clock(n);

    /* frame counter */
    a->fc_cycle++;
    int c = a->fc_cycle;
    int ev = 0; /* 1 quarter, 2 half */
    if (!a->fc_mode) {
        if (c == 7457) ev = 1;
        else if (c == 14913) ev = 2;
        else if (c == 22371) ev = 1;
        /* the flag is raised for two cycles even when inhibited (visible in
         * $4015), but only drives the IRQ line when not inhibited */
        else if (c == 29828) { a->fc_flag = true; if (!a->fc_inhibit) cpu_set_irq(n, IRQ_FRAME); }
        else if (c == 29829) { ev = 2; a->fc_flag = true; if (!a->fc_inhibit) cpu_set_irq(n, IRQ_FRAME); }
        else if (c == 29830) { if (!a->fc_inhibit) { a->fc_flag = true; cpu_set_irq(n, IRQ_FRAME); } else a->fc_flag = false; a->fc_cycle = 0; }
    } else {
        if (c == 7457) ev = 1;
        else if (c == 14913) ev = 2;
        else if (c == 22371) ev = 1;
        else if (c == 37281) ev = 2;
        else if (c == 37282) a->fc_cycle = 0;
    }
    if (ev && !a->fc_block) {
        quarter(n);
        if (ev == 2) half(n);
        a->fc_block = 2;
    }
    if (a->fc_new >= 0) {
        if (--a->fc_delay == 0) {
            a->fc_mode = (a->fc_new & 0x80) != 0;
            a->fc_cycle = 0;
            a->fc_new = -1;
            if (a->fc_mode && !a->fc_block) { quarter(n); half(n); a->fc_block = 2; }
        }
    }
    if (a->fc_block > 0) a->fc_block--;

    len_apply(&a->p1.len); len_apply(&a->p2.len); len_apply(&a->tri.len); len_apply(&a->noise.len);

    /* channel timers */
    if (apu_cycle) {
        if (a->p1.timer == 0) { a->p1.timer = a->p1.period; a->p1.step = (a->p1.step - 1) & 7; } else a->p1.timer--;
        if (a->p2.timer == 0) { a->p2.timer = a->p2.period; a->p2.step = (a->p2.step - 1) & 7; } else a->p2.timer--;
    }
    if (a->tri.timer == 0) {
        a->tri.timer = a->tri.period;
        if (a->tri.len.counter && a->tri.lin_ctr) a->tri.step = (a->tri.step + 1) & 31;
    } else a->tri.timer--;
    if (a->noise.timer == 0) {
        a->noise.timer = (uint16_t)(a->noise.period - 1);
        uint16_t fb = (uint16_t)((a->noise.shift ^ (a->noise.shift >> (a->noise.mode ? 6 : 1))) & 1);
        a->noise.shift = (uint16_t)((a->noise.shift >> 1) | (fb << 14));
    } else a->noise.timer--;
    if (apu_cycle == DMC_PARITY) dmc_clock(n);
    if (a->dmc.linger > 0) a->dmc.linger--;

    if (a->dmc.start_delay > 0 && --a->dmc.start_delay == 0) {
        dmc_start_transfer(n);
    }
    if (a->dmc.disable_delay > 0 && --a->dmc.disable_delay == 0) { a->dmc.remaining = 0; cpu_stop_dmc_dma(n); }

    mix(n);
}

/* ------------------------------------------------------------ registers */
uint8_t apu_read_status(Nes *n) {
    Apu *a = A;
    uint8_t r = 0;
    if (a->p1.len.counter) r |= 1;
    if (a->p2.len.counter) r |= 2;
    if (a->tri.len.counter) r |= 4;
    if (a->noise.len.counter) r |= 8;
    if (a->dmc.remaining) r |= 0x10;
    if (a->fc_flag) r |= 0x40;
    if (n->cpu.irq_src & IRQ_DMC) r |= 0x80;
    /* the flag is actually cleared on the next put->get APU transition */
    a->fc_clear_pending = true;
    return r;
}

static void pulse_write(Pulse *p, int reg, uint8_t v) {
    switch (reg) {
    case 0:
        p->duty = v >> 6;
        p->len.new_halt = (v & 0x20) != 0; p->env.loop = (v & 0x20) != 0;
        p->env.constant = (v & 0x10) != 0; p->env.vol = v & 15;
        break;
    case 1:
        p->sweep_en = (v & 0x80) != 0; p->sweep_period = (v >> 4) & 7;
        p->sweep_neg = (v & 8) != 0; p->sweep_shift = v & 7; p->sweep_reload = true;
        pulse_update_target(p);
        break;
    case 2:
        p->period = (uint16_t)((p->period & 0x700) | v); pulse_update_target(p); break;
    case 3:
        p->period = (uint16_t)((p->period & 0xFF) | ((v & 7) << 8)); pulse_update_target(p);
        len_load(&p->len, v >> 3);
        p->step = 0; p->env.start = true;
        break;
    }
}

void apu_write(Nes *n, uint16_t addr, uint8_t v) {
    Apu *a = A;
    switch (addr) {
    case 0x4000: case 0x4001: case 0x4002: case 0x4003: pulse_write(&a->p1, addr & 3, v); if (addr == 0x4003) n->apu_trig[0]++; break;
    case 0x4004: case 0x4005: case 0x4006: case 0x4007: pulse_write(&a->p2, addr & 3, v); if (addr == 0x4007) n->apu_trig[1]++; break;
    case 0x4008:
        a->tri.control = (v & 0x80) != 0; a->tri.len.new_halt = a->tri.control;
        a->tri.lin_reload = v & 0x7F; break;
    case 0x400A: a->tri.period = (uint16_t)((a->tri.period & 0x700) | v); break;
    case 0x400B:
        a->tri.period = (uint16_t)((a->tri.period & 0xFF) | ((v & 7) << 8));
        len_load(&a->tri.len, v >> 3); a->tri.lin_reload_flag = true; n->apu_trig[2]++; break;
    case 0x400C:
        a->noise.len.new_halt = (v & 0x20) != 0; a->noise.env.loop = (v & 0x20) != 0;
        a->noise.env.constant = (v & 0x10) != 0; a->noise.env.vol = v & 15; break;
    case 0x400E:
        a->noise.mode = (v & 0x80) != 0; a->noise.period = noise_period[v & 15]; break;
    case 0x400F:
        len_load(&a->noise.len, v >> 3); a->noise.env.start = true; n->apu_trig[3]++; break;
    case 0x4010:
        a->dmc.irq_en = (v & 0x80) != 0; a->dmc.loop = (v & 0x40) != 0;
        a->dmc.rate = dmc_rate[v & 15];
        if (!a->dmc.irq_en) cpu_clear_irq(n, IRQ_DMC);
        if (a->dmc.reload_cycle == n->cpu.cycles) a->dmc.timer = a->dmc.rate;
        break;
    case 0x4011: a->dmc.level = v & 0x7F; break;
    case 0x4012: a->dmc.sample_addr = (uint16_t)(0xC000 | (v << 6)); break;
    case 0x4013: a->dmc.sample_len = (uint16_t)((v << 4) | 1); break;
    case 0x4015:
#ifdef DMADEBUG
        fprintf(stderr, "W4015=%02X @%llu (%s) rem=%d bufE=%d bits=%d timer=%d\n", v, (unsigned long long)n->cpu.cycles, (n->cpu.cycles&1)?"get":"put", a->dmc.remaining, a->dmc.buf_empty, a->dmc.bits, a->dmc.timer);
#endif
        a->p1.len.enabled = v & 1; if (!(v & 1)) a->p1.len.counter = 0;
        a->p2.len.enabled = v & 2; if (!(v & 2)) a->p2.len.counter = 0;
        a->tri.len.enabled = v & 4; if (!(v & 4)) a->tri.len.counter = 0;
        a->noise.len.enabled = v & 8; if (!(v & 8)) a->noise.len.counter = 0;
        cpu_clear_irq(n, IRQ_DMC);
        if (!(v & 0x10)) {
            /* an in-flight DMC DMA is cancelled/aborted a few cycles later */
            a->dmc.disable_delay = (n->cpu.cycles & 1) == 0 ? 2 : 3;
        } else if (a->dmc.remaining == 0) {
            dmc_restart(&a->dmc);
            n->apu_trig[4]++;
            a->dmc.start_delay = (n->cpu.cycles & 1) == 0 ? 2 : 3;
        }
        break;
    case 0x4017:
        a->fc_new = v; a->fc_delay = (n->cpu.cycles & 1) ? 4 : 3;
        a->fc_inhibit = (v & 0x40) != 0;
        if (a->fc_inhibit) { a->fc_flag = false; cpu_clear_irq(n, IRQ_FRAME); }
        break;
    }
}

/* ------------------------------------------------------------ power */
void apu_power(Nes *n) {
    Apu *a = A;
    double sr = a->sample_rate > 0 ? a->sample_rate : 48000;
    uint32_t rw = a->ring_w, rr = a->ring_r;
    memset(a, 0, offsetof(Apu, ring));
    a->ring_w = rw; a->ring_r = rr;
    init_tables();
    a->sample_rate = sr; a->cycles_per_sample = CPU_HZ / sr;
    a->p1.is_p1 = true;
    a->noise.shift = 1; a->noise.period = noise_period[0];
    a->dmc.rate = dmc_rate[0]; a->dmc.timer = a->dmc.rate; a->dmc.buf_empty = true; a->dmc.bits = 8; a->dmc.silence = true;
    a->dmc.sample_addr = 0xC000; a->dmc.sample_len = 1;
    a->fc_new = -1;
    a->fc_mode = false; a->fc_inhibit = false;
    /* power-up behaves as if $4017 was written with 0 a few cycles before code runs */
    a->fc_new = 0; a->fc_delay = 1;
}

void apu_reset(Nes *n) {
    Apu *a = A;
    /* on reset $4015 is cleared and $4017 is rewritten with its last value */
    apu_write(n, 0x4015, 0);
    a->fc_new = (a->fc_mode ? 0x80 : 0) | (a->fc_inhibit ? 0x40 : 0);
    a->fc_delay = 1;
    a->tri.step = 0;
    a->dmc.level &= 1;
    a->fc_flag = false;
    cpu_clear_irq(n, IRQ_FRAME | IRQ_DMC);
}
