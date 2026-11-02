/*
 * Cycle-accurate Ricoh 2A03 (6502) core.
 * Every bus access is one CPU cycle; the PPU/APU/mapper are advanced inside
 * each access so that dummy reads/writes, interrupt polling and DMA halts
 * land on the exact cycle they do on hardware.
 */
#include "nes.h"
#include <stdio.h>

#define FC 0x01
#define FZ 0x02
#define FI 0x04
#define FD 0x08
#define FB 0x10
#define FU 0x20
#define FV 0x40
#define FN 0x80

/* NTSC: CPU cycle = 12 master clocks, split around the bus access */
#define START_CLK 6
#define END_CLK 6
#define PPU_OFFSET 1

static inline void start_cycle(Nes *n, bool rd) {
    Cpu *c = &n->cpu;
    c->master += rd ? (START_CLK - 1) : (START_CLK + 1);
    c->cycles++;
    ppu_run(n, c->master - PPU_OFFSET);
    if (n->oc_active) return;              /* overclock lines: APU and mapper timers wait */
    cart_cpu_clock(n);
    apu_clock(n);
}

static inline void end_cycle(Nes *n, bool rd) {
    Cpu *c = &n->cpu;
    c->master += rd ? (END_CLK + 1) : (END_CLK - 1);
    ppu_run(n, c->master - PPU_OFFSET);
    c->prev_need_nmi = c->need_nmi;
    if (!c->prev_nmi_flag && c->nmi_flag) c->need_nmi = true;
    c->prev_nmi_flag = c->nmi_flag;
    c->prev_run_irq = c->run_irq;
    c->run_irq = c->irq_src && !(c->p & FI);
}

static void process_dma(Nes *n, uint16_t read_addr);

static inline uint8_t rd(Nes *n, uint16_t a) {
    if (n->cpu.need_halt) process_dma(n, a);
    start_cycle(n, true);
    uint8_t v = bus_read(n, a);
    end_cycle(n, true);
    if ((a & 0xE007) == 0x2002) {
        /* VBL is latched when M2 rises, but the sprite flags are sampled
         * when M2 falls, ~2 PPU dots later */
        v = (uint8_t)((v & ~0x60) | (n->ppu.status & 0x60));
        n->cpu.open_bus = v;
    }
    return v;
}

static inline void wr(Nes *n, uint16_t a, uint8_t v) {
    Cpu *c = &n->cpu;
    if (c->abort_dmc && c->need_halt && c->dmc_running && !c->sprite_dma) {
        c->dmc_running = false; c->abort_dmc = false; c->need_halt = false; c->need_dummy = false;
    }
    start_cycle(n, false);
    bus_write(n, a, v);
    end_cycle(n, false);
}

/* ---------------------------------------------------------------- DMA */
static uint16_t dma_prev_addr;
static uint8_t dma_read(Nes *n, uint16_t a, bool internal) {
    /* The 2A03's internal registers ($4015-$4017) only respond when the
     * halted CPU address is in $4000-$401F. In that case they conflict with
     * whatever the DMA unit reads, regardless of the DMA address. */
    uint8_t v;
    if (!internal) {
        if (a >= 0x4000 && a <= 0x401F) v = n->cpu.open_bus;
        else v = bus_read(n, a);
        dma_prev_addr = a;
        return v;
    }
    uint16_t ia = (uint16_t)(0x4000 | (a & 0x1F));
    bool same = ia == a;
    uint8_t ext_bus;
    switch (ia) {
    case 0x4015: {
        /* $4015 drives only the 2A03's internal bus (bit 5 floats): the DMA
         * unit sees it, while the external bus keeps the external value */
        uint8_t st = bus_read(n, ia);
        if (!same) {
            uint8_t ext = bus_read(n, a);
            v = (uint8_t)((st & ~0x20) | (ext & 0x20));
            ext_bus = ext;
        } else { v = st; ext_bus = n->cpu.open_bus; }
        break;
    }
    case 0x4016: case 0x4017:
        if (dma_prev_addr == ia) v = n->cpu.open_bus;
        else {
            v = bus_read(n, ia);
            if (!same) {
                uint8_t ob = 0xE0;
                uint8_t ext = bus_read(n, a);
                v = (uint8_t)((ext & ob) | (v & ~ob));
            }
        }
        ext_bus = v;   /* controllers are external: the merge is on the bus */
        break;
    default:
        v = bus_read(n, a);
        ext_bus = v;
        break;
    }
    dma_prev_addr = ia;
    n->cpu.open_bus = ext_bus;
    return v;
}

static void process_dma(Nes *n, uint16_t read_addr) {
    Cpu *c = &n->cpu;
    if (!c->need_halt) return;
    c->in_dma = true;
    c->dma_hit = true;
#ifdef DMADEBUG
    uint64_t dbg_start = c->cycles + 1; bool dbg_dmc = c->dmc_running;
#endif
    /* halt cycle: the pending read happens and is discarded */
    start_cycle(n, true);
    bus_read(n, read_addr);
    end_cycle(n, true);
    c->need_halt = false;

    uint16_t spr_cnt = 0; uint8_t spr_addr = 0; uint8_t val = 0;
    bool skip_dummy = (read_addr == 0x4016 || read_addr == 0x4017);
    bool internal = (read_addr & 0xFFE0) == 0x4000;
    dma_prev_addr = read_addr;

    c->dma_past_halt = true;
    /* a re-queued DMC request re-checks its conditions after halting */
    if (c->dmc_recheck && c->dmc_running) {
        c->dmc_recheck = false;
        if (!apu_dmc_wants_dma(n)) c->abort_dmc = true;
    }
    /* a DMC DMA can only be aborted during its halt cycle */
    if (c->abort_dmc && c->dmc_running) {
        c->dmc_running = false; c->need_dummy = false;
    }
    c->abort_dmc = false;
    while (c->dmc_running || c->sprite_dma) {
        bool get = (c->cycles & 1) == 0;
        if (get) {
            if (c->dmc_running && !c->need_halt && !c->need_dummy) {
                if (c->need_halt) c->need_halt = false; else if (c->need_dummy) c->need_dummy = false;
                start_cycle(n, true);
                uint8_t ib = c->int_bus;   /* DMC DMA only drives the external bus */
                val = dma_read(n, apu_dmc_addr(n), internal);
                c->int_bus = ib;
                end_cycle(n, true);
                c->dmc_running = false; c->abort_dmc = false;
                apu_dmc_dma_done(n, val);
                if (c->dmc_requeue) {
                    /* another request came in while this DMA was running */
                    c->dmc_requeue = false;
                    c->dmc_running = true; c->need_halt = true; c->need_dummy = true;
                    /* nothing left to fetch: it will only cost its halt cycle
                     * (and vanishes entirely if that lands on a write) */
                    if (!apu_dmc_wants_dma(n)) c->abort_dmc = true;
                    if (!c->sprite_dma) break;
                    if (c->abort_dmc) { c->dmc_running = false; c->need_halt = false; c->need_dummy = false; c->abort_dmc = false; }
                }
            } else if (c->sprite_dma) {
                if (c->need_halt) c->need_halt = false; else if (c->need_dummy) c->need_dummy = false;
                start_cycle(n, true);
                val = dma_read(n, (uint16_t)(c->sprite_page * 0x100 + spr_addr), internal);
                end_cycle(n, true);
                spr_addr++; spr_cnt++;
            } else {
                if (c->need_halt) c->need_halt = false; else if (c->need_dummy) c->need_dummy = false;
                start_cycle(n, true);
                if (!skip_dummy) bus_read(n, read_addr);
                end_cycle(n, true);
            }
        } else {
            if (c->sprite_dma && (spr_cnt & 1)) {
                if (c->need_halt) c->need_halt = false; else if (c->need_dummy) c->need_dummy = false;
                start_cycle(n, true);
                bus_write(n, 0x2004, val);
                end_cycle(n, true);
                spr_cnt++;
                if (spr_cnt == 0x200) c->sprite_dma = false;
            } else {
                if (c->need_halt) c->need_halt = false; else if (c->need_dummy) c->need_dummy = false;
                start_cycle(n, true);
                if (!skip_dummy) bus_read(n, read_addr);
                end_cycle(n, true);
            }
        }
    }
    c->in_dma = false;
    c->dma_past_halt = false;
#ifdef DMADEBUG
    if (dbg_dmc) fprintf(stderr, "DMA halt@%llu (%s) len=%llu addr=%04X\n", (unsigned long long)dbg_start, (dbg_start & 1) ? "get" : "put", (unsigned long long)(c->cycles + 1 - dbg_start), read_addr);
#endif
}

void cpu_start_dmc_dma(Nes *n) {
    Cpu *c = &n->cpu;
    c->dmc_running = true;
    c->need_dummy = true;
    c->need_halt = true;
}

void cpu_stop_dmc_dma(Nes *n) {
    Cpu *c = &n->cpu;
    if (!c->dmc_running) return;
    /* the DMA is aborted after its halt cycle; if it hasn't halted the CPU
     * yet it becomes a 1-cycle DMA (which vanishes if it lands on a write) */
    if (c->dma_past_halt) return;   /* already past the halt cycle */
    c->abort_dmc = true;
}

void cpu_set_irq(Nes *n, uint8_t s) { n->cpu.irq_src |= s; }
void cpu_clear_irq(Nes *n, uint8_t s) { n->cpu.irq_src &= (uint8_t)~s; }

/* ---------------------------------------------------------------- helpers */
#define C (&n->cpu)
static inline void setnz_(Cpu *c, uint8_t v) { c->p = (uint8_t)((c->p & ~(FZ|FN)) | (v ? 0 : FZ) | (v & 0x80)); }
#define SETNZ(v) setnz_(C, (uint8_t)(v))

static inline void push(Nes *n, uint8_t v) { wr(n, 0x100 | C->sp, v); C->sp--; }
static inline uint8_t pop(Nes *n) { C->sp++; return rd(n, 0x100 | C->sp); }
static inline uint8_t fetch(Nes *n) { return rd(n, C->pc++); }
static inline uint16_t fetch16(Nes *n) { uint8_t lo = fetch(n); uint8_t hi = fetch(n); return (uint16_t)(lo | hi << 8); }
static inline void dummy_pc(Nes *n) { rd(n, C->pc); }

static inline uint16_t am_zp(Nes *n) { return fetch(n); }
static inline uint16_t am_zpi(Nes *n, uint8_t i) { uint8_t b = fetch(n); rd(n, b); return (uint8_t)(b + i); }
static inline uint16_t am_abs(Nes *n) { return fetch16(n); }
static inline uint16_t am_absi_r(Nes *n, uint8_t i) {
    uint16_t base = fetch16(n); uint16_t a = (uint16_t)(base + i);
    if ((base ^ a) & 0xFF00) rd(n, (uint16_t)((base & 0xFF00) | (a & 0xFF)));
    return a;
}
static inline uint16_t am_absi_w(Nes *n, uint8_t i) {
    uint16_t base = fetch16(n); uint16_t a = (uint16_t)(base + i);
    rd(n, (uint16_t)((base & 0xFF00) | (a & 0xFF)));
    return a;
}
static inline uint16_t am_indx(Nes *n) {
    uint8_t p = fetch(n); rd(n, p); p = (uint8_t)(p + C->x);
    uint8_t lo = rd(n, p); uint8_t hi = rd(n, (uint8_t)(p + 1));
    return (uint16_t)(lo | hi << 8);
}
static inline uint16_t am_indy_base(Nes *n) {
    uint8_t p = fetch(n);
    uint8_t lo = rd(n, p); uint8_t hi = rd(n, (uint8_t)(p + 1));
    return (uint16_t)(lo | hi << 8);
}
static inline uint16_t am_indy_r(Nes *n) {
    uint16_t base = am_indy_base(n); uint16_t a = (uint16_t)(base + C->y);
    if ((base ^ a) & 0xFF00) rd(n, (uint16_t)((base & 0xFF00) | (a & 0xFF)));
    return a;
}
static inline uint16_t am_indy_w(Nes *n) {
    uint16_t base = am_indy_base(n); uint16_t a = (uint16_t)(base + C->y);
    rd(n, (uint16_t)((base & 0xFF00) | (a & 0xFF)));
    return a;
}

/* ALU */
static inline void op_adc(Nes *n, uint8_t v) {
    unsigned s = C->a + v + (C->p & FC);
    C->p = (uint8_t)(C->p & ~(FC | FV));
    if (s > 0xFF) C->p |= FC;
    if (~(C->a ^ v) & (C->a ^ s) & 0x80) C->p |= FV;
    C->a = (uint8_t)s; SETNZ(C->a);
}
static inline void op_sbc(Nes *n, uint8_t v) { op_adc(n, (uint8_t)~v); }
static inline void op_cmp(Nes *n, uint8_t r, uint8_t v) {
    unsigned d = (unsigned)r - v;
    C->p = (uint8_t)((C->p & ~FC) | (r >= v ? FC : 0)); SETNZ((uint8_t)d);
}
static inline uint8_t op_asl(Nes *n, uint8_t v) { C->p = (uint8_t)((C->p & ~FC) | (v >> 7)); v <<= 1; SETNZ(v); return v; }
static inline uint8_t op_lsr(Nes *n, uint8_t v) { C->p = (uint8_t)((C->p & ~FC) | (v & 1)); v >>= 1; SETNZ(v); return v; }
static inline uint8_t op_rol(Nes *n, uint8_t v) { uint8_t c = C->p & FC; C->p = (uint8_t)((C->p & ~FC) | (v >> 7)); v = (uint8_t)(v << 1 | c); SETNZ(v); return v; }
static inline uint8_t op_ror(Nes *n, uint8_t v) { uint8_t c = C->p & FC; C->p = (uint8_t)((C->p & ~FC) | (v & 1)); v = (uint8_t)(v >> 1 | c << 7); SETNZ(v); return v; }
static inline void op_bit(Nes *n, uint8_t v) {
    C->p = (uint8_t)((C->p & ~(FZ | FV | FN)) | (v & (FV | FN)) | ((C->a & v) ? 0 : FZ));
}

static inline void branch(Nes *n, bool cond) {
    int8_t off = (int8_t)fetch(n);
    if (cond) {
        /* interrupts are polled before cycle 2; a taken branch does not poll
         * again before cycle 3, but does before cycle 4 if a page is crossed */
        bool irq1 = C->prev_run_irq, nmi1 = C->prev_need_nmi;
        dummy_pc(n);
        uint16_t t = (uint16_t)(C->pc + off);
        if ((t ^ C->pc) & 0xFF00) {
            rd(n, (uint16_t)((C->pc & 0xFF00) | (t & 0xFF)));
            C->prev_run_irq |= irq1;
        } else {
            C->prev_run_irq = irq1;
            C->prev_need_nmi = nmi1;
        }
        C->pc = t;
    }
}

#define RMW(addr_expr, body) do { uint16_t ea = (addr_expr); uint8_t v = rd(n, ea); wr(n, ea, v); body; wr(n, ea, v); } while (0)

static void sh_store(Nes *n, uint16_t base, uint8_t idx, uint8_t reg) {
    /* SHX/SHY/SHA/TAS: value = reg & (H+1); on page cross the high byte of
     * the address is corrupted to that value. If a DMA halts the CPU on the
     * cycle before the write, the AND with H+1 does not happen. */
    uint16_t a = (uint16_t)(base + idx);
    C->dma_hit = false;
    rd(n, (uint16_t)((base & 0xFF00) | (a & 0xFF)));
    uint8_t val = C->dma_hit ? reg : (uint8_t)(reg & ((base >> 8) + 1));
    if ((base ^ a) & 0xFF00) a = (uint16_t)((val << 8) | (a & 0xFF));
    wr(n, a, val);
}

static void interrupt(Nes *n) {
    dummy_pc(n);
    dummy_pc(n);
    push(n, (uint8_t)(C->pc >> 8));
    push(n, (uint8_t)C->pc);
    uint16_t vec;
    if (C->need_nmi) { C->need_nmi = false; vec = 0xFFFA; }
    else vec = 0xFFFE;
    push(n, (uint8_t)(C->p | FU));
    C->p |= FI;
    uint8_t lo = rd(n, vec); uint8_t hi = rd(n, (uint16_t)(vec + 1));
    C->pc = (uint16_t)(lo | hi << 8);
}

void cpu_power(Nes *n) {
    Cpu *c = &n->cpu;
    c->a = c->x = c->y = 0; c->sp = 0xFD; c->p = FI | FU;
    c->master = 0; c->cycles = 0;
    c->nmi_flag = c->prev_nmi_flag = c->need_nmi = c->prev_need_nmi = false;
    c->irq_src = 0; c->run_irq = c->prev_run_irq = false;
    c->jammed = false;
    c->need_halt = c->need_dummy = c->dmc_running = c->abort_dmc = c->sprite_dma = false;
    c->open_bus = 0;
    cpu_reset(n);
    c->sp = 0xFD;
}

void cpu_reset(Nes *n) {
    Cpu *c = &n->cpu;
    c->jammed = false;
    c->need_halt = c->need_dummy = c->dmc_running = c->abort_dmc = c->sprite_dma = false;
    c->need_nmi = c->prev_need_nmi = false;
    c->irq_src = 0;
    c->p |= FI;
    c->sp = (uint8_t)(c->sp - 3);
    /* reset sequence: 7 cycles before the first opcode fetch
     * (the vector fetch is 2 of them) */
    for (int i = 0; i < 6; i++) { start_cycle(n, true); end_cycle(n, true); }
    uint8_t lo = rd(n, 0xFFFC); uint8_t hi = rd(n, 0xFFFD);
    c->pc = (uint16_t)(lo | hi << 8);
}

void cpu_step(Nes *n) {
    Cpu *c = &n->cpu;
    if (c->jammed) { rd(n, 0xFFFF); return; }
    uint8_t op = fetch(n);
    uint16_t a; uint8_t v;
    switch (op) {
    /* ---- loads */
    case 0xA9: C->a = fetch(n); SETNZ(C->a); break;
    case 0xA5: C->a = rd(n, am_zp(n)); SETNZ(C->a); break;
    case 0xB5: C->a = rd(n, am_zpi(n, C->x)); SETNZ(C->a); break;
    case 0xAD: C->a = rd(n, am_abs(n)); SETNZ(C->a); break;
    case 0xBD: C->a = rd(n, am_absi_r(n, C->x)); SETNZ(C->a); break;
    case 0xB9: C->a = rd(n, am_absi_r(n, C->y)); SETNZ(C->a); break;
    case 0xA1: C->a = rd(n, am_indx(n)); SETNZ(C->a); break;
    case 0xB1: C->a = rd(n, am_indy_r(n)); SETNZ(C->a); break;
    case 0xA2: C->x = fetch(n); SETNZ(C->x); break;
    case 0xA6: C->x = rd(n, am_zp(n)); SETNZ(C->x); break;
    case 0xB6: C->x = rd(n, am_zpi(n, C->y)); SETNZ(C->x); break;
    case 0xAE: C->x = rd(n, am_abs(n)); SETNZ(C->x); break;
    case 0xBE: C->x = rd(n, am_absi_r(n, C->y)); SETNZ(C->x); break;
    case 0xA0: C->y = fetch(n); SETNZ(C->y); break;
    case 0xA4: C->y = rd(n, am_zp(n)); SETNZ(C->y); break;
    case 0xB4: C->y = rd(n, am_zpi(n, C->x)); SETNZ(C->y); break;
    case 0xAC: C->y = rd(n, am_abs(n)); SETNZ(C->y); break;
    case 0xBC: C->y = rd(n, am_absi_r(n, C->x)); SETNZ(C->y); break;
    /* ---- stores */
    case 0x85: wr(n, am_zp(n), C->a); break;
    case 0x95: wr(n, am_zpi(n, C->x), C->a); break;
    case 0x8D: wr(n, am_abs(n), C->a); break;
    case 0x9D: wr(n, am_absi_w(n, C->x), C->a); break;
    case 0x99: wr(n, am_absi_w(n, C->y), C->a); break;
    case 0x81: wr(n, am_indx(n), C->a); break;
    case 0x91: wr(n, am_indy_w(n), C->a); break;
    case 0x86: wr(n, am_zp(n), C->x); break;
    case 0x96: wr(n, am_zpi(n, C->y), C->x); break;
    case 0x8E: wr(n, am_abs(n), C->x); break;
    case 0x84: wr(n, am_zp(n), C->y); break;
    case 0x94: wr(n, am_zpi(n, C->x), C->y); break;
    case 0x8C: wr(n, am_abs(n), C->y); break;
    /* ---- transfers */
    case 0xAA: dummy_pc(n); C->x = C->a; SETNZ(C->x); break;
    case 0xA8: dummy_pc(n); C->y = C->a; SETNZ(C->y); break;
    case 0x8A: dummy_pc(n); C->a = C->x; SETNZ(C->a); break;
    case 0x98: dummy_pc(n); C->a = C->y; SETNZ(C->a); break;
    case 0xBA: dummy_pc(n); C->x = C->sp; SETNZ(C->x); break;
    case 0x9A: dummy_pc(n); C->sp = C->x; break;
    /* ---- stack */
    case 0x48: dummy_pc(n); push(n, C->a); break;
    case 0x08: dummy_pc(n); push(n, (uint8_t)(C->p | FB | FU)); break;
    case 0x68: dummy_pc(n); rd(n, 0x100 | C->sp); C->a = pop(n); SETNZ(C->a); break;
    case 0x28: dummy_pc(n); rd(n, 0x100 | C->sp); C->p = (uint8_t)((pop(n) & ~FB) | FU); break;
    /* ---- logic / arith */
#define READOPS(base_imm, base_zp, base_zpx, base_abs, base_absx, base_absy, base_indx, base_indy, OP) \
    case base_imm: v = fetch(n); OP; break; \
    case base_zp: v = rd(n, am_zp(n)); OP; break; \
    case base_zpx: v = rd(n, am_zpi(n, C->x)); OP; break; \
    case base_abs: v = rd(n, am_abs(n)); OP; break; \
    case base_absx: v = rd(n, am_absi_r(n, C->x)); OP; break; \
    case base_absy: v = rd(n, am_absi_r(n, C->y)); OP; break; \
    case base_indx: v = rd(n, am_indx(n)); OP; break; \
    case base_indy: v = rd(n, am_indy_r(n)); OP; break;
    READOPS(0x29, 0x25, 0x35, 0x2D, 0x3D, 0x39, 0x21, 0x31, (C->a &= v, SETNZ(C->a)))
    READOPS(0x09, 0x05, 0x15, 0x0D, 0x1D, 0x19, 0x01, 0x11, (C->a |= v, SETNZ(C->a)))
    READOPS(0x49, 0x45, 0x55, 0x4D, 0x5D, 0x59, 0x41, 0x51, (C->a ^= v, SETNZ(C->a)))
    READOPS(0x69, 0x65, 0x75, 0x6D, 0x7D, 0x79, 0x61, 0x71, op_adc(n, v))
    READOPS(0xE9, 0xE5, 0xF5, 0xED, 0xFD, 0xF9, 0xE1, 0xF1, op_sbc(n, v))
    READOPS(0xC9, 0xC5, 0xD5, 0xCD, 0xDD, 0xD9, 0xC1, 0xD1, op_cmp(n, C->a, v))
    case 0xEB: v = fetch(n); op_sbc(n, v); break;
    case 0xE0: op_cmp(n, C->x, fetch(n)); break;
    case 0xE4: op_cmp(n, C->x, rd(n, am_zp(n))); break;
    case 0xEC: op_cmp(n, C->x, rd(n, am_abs(n))); break;
    case 0xC0: op_cmp(n, C->y, fetch(n)); break;
    case 0xC4: op_cmp(n, C->y, rd(n, am_zp(n))); break;
    case 0xCC: op_cmp(n, C->y, rd(n, am_abs(n))); break;
    case 0x24: op_bit(n, rd(n, am_zp(n))); break;
    case 0x2C: op_bit(n, rd(n, am_abs(n))); break;
    /* ---- inc/dec */
    case 0xE8: dummy_pc(n); C->x++; SETNZ(C->x); break;
    case 0xC8: dummy_pc(n); C->y++; SETNZ(C->y); break;
    case 0xCA: dummy_pc(n); C->x--; SETNZ(C->x); break;
    case 0x88: dummy_pc(n); C->y--; SETNZ(C->y); break;
#define RMWOPS(bzp, bzpx, babs, babsx, BODY) \
    case bzp: RMW(am_zp(n), BODY); break; \
    case bzpx: RMW(am_zpi(n, C->x), BODY); break; \
    case babs: RMW(am_abs(n), BODY); break; \
    case babsx: RMW(am_absi_w(n, C->x), BODY); break;
    RMWOPS(0xE6, 0xF6, 0xEE, 0xFE, (v++, SETNZ(v)))
    RMWOPS(0xC6, 0xD6, 0xCE, 0xDE, (v--, SETNZ(v)))
    RMWOPS(0x06, 0x16, 0x0E, 0x1E, v = op_asl(n, v))
    RMWOPS(0x46, 0x56, 0x4E, 0x5E, v = op_lsr(n, v))
    RMWOPS(0x26, 0x36, 0x2E, 0x3E, v = op_rol(n, v))
    RMWOPS(0x66, 0x76, 0x6E, 0x7E, v = op_ror(n, v))
    case 0x0A: dummy_pc(n); C->a = op_asl(n, C->a); break;
    case 0x4A: dummy_pc(n); C->a = op_lsr(n, C->a); break;
    case 0x2A: dummy_pc(n); C->a = op_rol(n, C->a); break;
    case 0x6A: dummy_pc(n); C->a = op_ror(n, C->a); break;
    /* ---- flags */
    case 0x18: dummy_pc(n); C->p &= ~FC; break;
    case 0x38: dummy_pc(n); C->p |= FC; break;
    case 0x58: dummy_pc(n); C->p &= ~FI; break;
    case 0x78: dummy_pc(n); C->p |= FI; break;
    case 0xB8: dummy_pc(n); C->p &= ~FV; break;
    case 0xD8: dummy_pc(n); C->p &= ~FD; break;
    case 0xF8: dummy_pc(n); C->p |= FD; break;
    /* ---- branches */
    case 0x10: branch(n, !(C->p & FN)); break;
    case 0x30: branch(n, (C->p & FN)); break;
    case 0x50: branch(n, !(C->p & FV)); break;
    case 0x70: branch(n, (C->p & FV)); break;
    case 0x90: branch(n, !(C->p & FC)); break;
    case 0xB0: branch(n, (C->p & FC)); break;
    case 0xD0: branch(n, !(C->p & FZ)); break;
    case 0xF0: branch(n, (C->p & FZ)); break;
    /* ---- jumps */
    case 0x4C: C->pc = fetch16(n); break;
    case 0x6C: {
        uint16_t p = fetch16(n);
        uint8_t lo = rd(n, p);
        uint8_t hi = rd(n, (uint16_t)((p & 0xFF00) | ((p + 1) & 0xFF)));
        C->pc = (uint16_t)(lo | hi << 8);
        break;
    }
    case 0x20: {
        uint8_t lo = fetch(n);
        rd(n, 0x100 | C->sp);
        push(n, (uint8_t)(C->pc >> 8));
        push(n, (uint8_t)C->pc);
        uint8_t hi = fetch(n);
        C->pc = (uint16_t)(lo | hi << 8);
        break;
    }
    case 0x60: {
        dummy_pc(n); rd(n, 0x100 | C->sp);
        uint8_t lo = pop(n); uint8_t hi = pop(n);
        C->pc = (uint16_t)(lo | hi << 8);
        fetch(n);
        break;
    }
    case 0x40: {
        dummy_pc(n); rd(n, 0x100 | C->sp);
        C->p = (uint8_t)((pop(n) & ~FB) | FU);
        uint8_t lo = pop(n); uint8_t hi = pop(n);
        C->pc = (uint16_t)(lo | hi << 8);
        break;
    }
    case 0x00: {
        fetch(n);
        push(n, (uint8_t)(C->pc >> 8));
        push(n, (uint8_t)C->pc);
        uint16_t vec;
        if (C->need_nmi) { C->need_nmi = false; vec = 0xFFFA; } else vec = 0xFFFE;
        push(n, (uint8_t)(C->p | FB | FU));
        C->p |= FI;
        uint8_t lo = rd(n, vec); uint8_t hi = rd(n, (uint16_t)(vec + 1));
        C->pc = (uint16_t)(lo | hi << 8);
        C->prev_need_nmi = false;
        break;
    }
    case 0xEA: dummy_pc(n); break;

    /* ================= unofficial ================= */
    /* NOPs */
    case 0x1A: case 0x3A: case 0x5A: case 0x7A: case 0xDA: case 0xFA: dummy_pc(n); break;
    case 0x80: case 0x82: case 0x89: case 0xC2: case 0xE2: fetch(n); break;
    case 0x04: case 0x44: case 0x64: rd(n, am_zp(n)); break;
    case 0x14: case 0x34: case 0x54: case 0x74: case 0xD4: case 0xF4: rd(n, am_zpi(n, C->x)); break;
    case 0x0C: rd(n, am_abs(n)); break;
    case 0x1C: case 0x3C: case 0x5C: case 0x7C: case 0xDC: case 0xFC: rd(n, am_absi_r(n, C->x)); break;
    /* LAX */
    case 0xA7: C->a = C->x = rd(n, am_zp(n)); SETNZ(C->a); break;
    case 0xB7: C->a = C->x = rd(n, am_zpi(n, C->y)); SETNZ(C->a); break;
    case 0xAF: C->a = C->x = rd(n, am_abs(n)); SETNZ(C->a); break;
    case 0xBF: C->a = C->x = rd(n, am_absi_r(n, C->y)); SETNZ(C->a); break;
    case 0xA3: C->a = C->x = rd(n, am_indx(n)); SETNZ(C->a); break;
    case 0xB3: C->a = C->x = rd(n, am_indy_r(n)); SETNZ(C->a); break;
    case 0xAB: C->a = C->x = fetch(n); SETNZ(C->a); break;
    /* SAX */
    case 0x87: wr(n, am_zp(n), C->a & C->x); break;
    case 0x97: wr(n, am_zpi(n, C->y), C->a & C->x); break;
    case 0x8F: wr(n, am_abs(n), C->a & C->x); break;
    case 0x83: wr(n, am_indx(n), C->a & C->x); break;
    /* RMW combos */
#define RMWX(bzp, bzpx, babs, babsx, babsy, bindx, bindy, BODY) \
    case bzp: RMW(am_zp(n), BODY); break; \
    case bzpx: RMW(am_zpi(n, C->x), BODY); break; \
    case babs: RMW(am_abs(n), BODY); break; \
    case babsx: RMW(am_absi_w(n, C->x), BODY); break; \
    case babsy: RMW(am_absi_w(n, C->y), BODY); break; \
    case bindx: RMW(am_indx(n), BODY); break; \
    case bindy: RMW(am_indy_w(n), BODY); break;
    RMWX(0x07, 0x17, 0x0F, 0x1F, 0x1B, 0x03, 0x13, (v = op_asl(n, v), C->a |= v, SETNZ(C->a)))      /* SLO */
    RMWX(0x27, 0x37, 0x2F, 0x3F, 0x3B, 0x23, 0x33, (v = op_rol(n, v), C->a &= v, SETNZ(C->a)))      /* RLA */
    RMWX(0x47, 0x57, 0x4F, 0x5F, 0x5B, 0x43, 0x53, (v = op_lsr(n, v), C->a ^= v, SETNZ(C->a)))      /* SRE */
    RMWX(0x67, 0x77, 0x6F, 0x7F, 0x7B, 0x63, 0x73, (v = op_ror(n, v), op_adc(n, v)))                /* RRA */
    RMWX(0xC7, 0xD7, 0xCF, 0xDF, 0xDB, 0xC3, 0xD3, (v--, op_cmp(n, C->a, v)))                       /* DCP */
    RMWX(0xE7, 0xF7, 0xEF, 0xFF, 0xFB, 0xE3, 0xF3, (v++, op_sbc(n, v)))                             /* ISC */
    /* immediates */
    case 0x0B: case 0x2B: C->a &= fetch(n); SETNZ(C->a); C->p = (uint8_t)((C->p & ~FC) | (C->a >> 7)); break; /* ANC */
    case 0x4B: C->a &= fetch(n); C->a = op_lsr(n, C->a); break;  /* ALR */
    case 0x6B: {                                                  /* ARR */
        C->a &= fetch(n);
        C->a = (uint8_t)(C->a >> 1 | (C->p & FC) << 7);
        SETNZ(C->a);
        C->p = (uint8_t)(C->p & ~(FC | FV));
        if (C->a & 0x40) C->p |= FC;
        if (((C->a >> 6) ^ (C->a >> 5)) & 1) C->p |= FV;
        break;
    }
    case 0xCB: {                                                  /* AXS/SBX */
        v = fetch(n); uint8_t ax = C->a & C->x;
        C->p = (uint8_t)((C->p & ~FC) | (ax >= v ? FC : 0));
        C->x = (uint8_t)(ax - v); SETNZ(C->x);
        break;
    }
    case 0x8B: C->a = (uint8_t)((C->a | 0xEE) & C->x & fetch(n)); SETNZ(C->a); break; /* XAA */
    case 0xBB: v = rd(n, am_absi_r(n, C->y)); C->a = C->x = C->sp = v & C->sp; SETNZ(C->a); break; /* LAS */
    /* unstable stores */
    case 0x9C: a = fetch16(n); sh_store(n, a, C->x, C->y); break;            /* SHY */
    case 0x9E: a = fetch16(n); sh_store(n, a, C->y, C->x); break;            /* SHX */
    case 0x9F: a = fetch16(n); sh_store(n, a, C->y, C->a & C->x); break;     /* SHA abs,y */
    case 0x93: a = am_indy_base(n); sh_store(n, a, C->y, C->a & C->x); break; /* SHA (zp),y */
    case 0x9B: a = fetch16(n); C->sp = C->a & C->x; sh_store(n, a, C->y, C->sp); break; /* TAS */
    /* KIL */
    case 0x02: case 0x12: case 0x22: case 0x32: case 0x42: case 0x52:
    case 0x62: case 0x72: case 0x92: case 0xB2: case 0xD2: case 0xF2:
        C->jammed = true; C->pc--; break;
    default: break;
    }
    if (C->prev_run_irq || C->prev_need_nmi) interrupt(n);
}
