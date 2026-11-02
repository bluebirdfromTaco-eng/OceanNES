/*
 * Accurate NES emulator core - public + internal definitions.
 * Portable C99. No platform dependencies.
 */
#ifndef NES_H
#define NES_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

typedef struct Nes Nes;


/* Game Genie: replaces the value the CPU reads from a cartridge address
 * (optionally only when the original value equals `compare`) */
#define MAX_CHEATS 64
typedef struct { uint16_t addr; uint8_t value, compare; bool has_compare; } Cheat;
bool gg_decode(const char *code, Cheat *out);   /* 6 or 8 letter code; false if invalid */

enum { IRQ_FRAME = 1, IRQ_DMC = 2, IRQ_MAPPER = 4, IRQ_MAPPER2 = 8 };

enum { BTN_A = 0x01, BTN_B = 0x02, BTN_SELECT = 0x04, BTN_START = 0x08,
       BTN_UP = 0x10, BTN_DOWN = 0x20, BTN_LEFT = 0x40, BTN_RIGHT = 0x80 };

enum { MIRROR_H = 0, MIRROR_V = 1, MIRROR_4 = 2, MIRROR_1A = 3, MIRROR_1B = 4 };

/* ------------------------------------------------------------------ CPU */
typedef struct {
    uint16_t pc;
    uint8_t a, x, y, sp, p;
    uint64_t cycles;
    uint64_t master;          /* master clock (NTSC: 12 per CPU cycle) */
    bool nmi_flag, prev_nmi_flag, need_nmi, prev_need_nmi;
    uint8_t irq_src;
    bool run_irq, prev_run_irq;
    bool jammed;
    /* DMA */
    bool need_halt, need_dummy, dmc_running, abort_dmc, sprite_dma;
    uint8_t sprite_page;
    uint8_t open_bus;   /* external data bus */
    uint8_t int_bus;    /* 2A03 internal data bus */
    bool in_dma;
    bool dma_hit, dma_past_halt, dmc_requeue, dmc_recheck;
} Cpu;

/* ------------------------------------------------------------------ PPU */
typedef struct {
    int scanline, dot;
    uint64_t frame;
    uint64_t master;          /* PPU master clock */
    uint64_t dots;            /* total dots executed */
    uint8_t ctrl, mask, status;
    uint8_t mask_pending, mask_prev; int mask_delay; uint64_t mask_write_dot;
    uint8_t oam_addr;
    uint8_t oam[256];
    uint8_t oam2[32];
    uint16_t v, t;
    uint8_t x;
    bool w;
    uint8_t buf;
    uint8_t io;               /* PPU I/O latch (open bus) */
    uint32_t io_stamp[8];
    uint8_t pal[32];
    uint8_t ciram[2048];
    /* PPU memory bus */
    uint16_t bus_addr;
    uint8_t octal, adbus;
    /* PPU DATA ($2007) state machines: countdowns in dots */
    int sm_r_ale, sm_r_rd, sm_w_ale, sm_w_wr;
    uint16_t sm_hi, sm_addr, sm_waddr;
    uint8_t sm_wdata;
    /* background pipeline */
    uint8_t nt, at, plo, phi, at_latch;
    uint16_t sh_lo, sh_hi;
    uint8_t ash_lo, ash_hi;
    /* sprite units */
    uint8_t s_lo[8], s_hi[8], s_attr[8], s_cnt[8];
    bool s_active[8];
    bool spr0_line, spr0_visible, skip_quirk;
    /* sprite fetch latches */
    uint8_t f_y, f_tile, f_attr, f_x;
    /* sprite evaluation */
    uint8_t oam_copy;          /* the OAM buffer seen through $2004 */
    int oam2_addr; bool oam2_ovf;
    int spr_l, overflow_cnt;
    bool in_range, copy_done, spr0_added;
    bool corrupt_pending; uint8_t corrupt_seed;
    /* misc */
    bool prevent_vbl;
    bool rend, rend_prev;
    uint16_t pending_v; int v_delay;
    int ignore_2007;
    bool s0_pending, ovf_pending;
    /* output */
    uint16_t fb[256 * 240];   /* 6-bit colour | emphasis << 6 */
    bool frame_done;
    uint8_t line0_phase, frame_phase;  /* NTSC colour-subcarrier phase (0-11) at the start of line 0 */
} Ppu;

/* ------------------------------------------------------------------ APU */
typedef struct {
    uint8_t counter, reload_val, prev_val;
    bool enabled, halt, new_halt, reload_pending;
} LenCtr;

typedef struct {
    bool start, loop, constant;
    uint8_t vol, divider, decay;
} Envelope;

typedef struct {
    LenCtr len; Envelope env;
    uint8_t duty, step;
    uint16_t period, timer;
    bool sweep_en, sweep_neg, sweep_reload;
    uint8_t sweep_period, sweep_shift, sweep_div;
    uint16_t target;
    bool is_p1;
} Pulse;

typedef struct {
    LenCtr len;
    uint16_t period, timer;
    uint8_t step;
    uint8_t lin_reload, lin_ctr;
    bool lin_reload_flag, control;
} Triangle;

typedef struct {
    LenCtr len; Envelope env;
    uint16_t period, timer, shift;
    bool mode;
} Noise;

typedef struct {
    bool irq_en, loop;
    uint16_t rate, timer;
    uint8_t level;
    uint16_t sample_addr, sample_len, addr, remaining;
    uint8_t buffer; bool buf_empty;
    uint8_t shift, bits; bool silence;
    int start_delay;
    int disable_delay;
    uint64_t reload_cycle;
    int linger;
} Dmc;

#define AUDIO_RING 16384
typedef struct {
    Pulse p1, p2; Triangle tri; Noise noise; Dmc dmc;
    /* frame counter */
    int fc_cycle; bool fc_mode; bool fc_inhibit;
    int fc_new; int fc_delay; int fc_block;
    bool fc_clear_pending, fc_flag;
    uint64_t cycle;
    /* mixing */
    double sample_rate, cycles_per_sample, sample_acc_cycles;
    double acc; int acc_n;
    double hp1_prev_in, hp1_prev_out, hp2_prev_in, hp2_prev_out, lp_prev;
    float ring[AUDIO_RING];
    uint32_t ring_w, ring_r;
    float exp_audio;           /* expansion audio output (mapper), 0..~1 */
} Apu;

/* ------------------------------------------------------------------ Cart */
typedef struct {
    uint8_t *prg; uint32_t prg_size;
    uint8_t *chr; uint32_t chr_size; bool chr_is_ram;
    uint8_t *wram; uint32_t wram_size;
    bool battery, nes2;
    uint32_t chr_rom_size;
    int mapper, submapper;
    int mirroring;
    bool has_trainer;
    uint32_t crc;
    /* current mapping */
    uint32_t prg_off[4];      /* offsets of $8000,$A000,$C000,$E000 8KB pages */
    int32_t  wram_off;        /* offset into wram for $6000, -1 = open bus, -2 = prg rom */
    uint32_t wram_prg_off;
    bool wram_en, wram_wp;
    uint32_t chr_off[8];      /* 1KB pages */
    bool chr_ram_writable;
    uint8_t *nt[4];           /* nametable 1KB pages */
    uint8_t exram[4096];      /* four-screen VRAM / extra RAM */
    /* mapper registers (generic) */
    uint8_t reg[64];
    int irq_counter, irq_latch, irq_prescaler;
    bool irq_enable, irq_reload, irq_enable2;
    uint64_t a12_low_since; bool a12_prev;
    uint64_t last_write_cycle;
    uint8_t mmc1_shift, mmc1_count;
    uint8_t latch0, latch1;
    /* expansion audio state */
    struct {
        uint8_t ctl[3][4];
        int timer[3]; int step[3]; int out[3];
        uint8_t saw_acc; int saw_step;
        bool halt;
        uint8_t freq_shift;
    } vrc6;
    struct {
        uint8_t reg[16]; uint8_t addr;
        int timer[3]; int out[3]; int div;
        int noise_timer; uint32_t noise_lfsr;
        int env_timer; int env_step; bool env_hold, env_alt, env_att, env_cont; int env_vol;
    } s5b;
} Cart;

/* ------------------------------------------------------------------ Nes */
struct Nes {
    Cpu cpu;
    Ppu ppu;
    Apu apu;
    Cart cart;
    uint8_t ram[2048];
    uint8_t pad_state[2];     /* live buttons from frontend */
    uint8_t pad_shift[2];
    bool strobe;
    uint8_t out_latch_pending;
    uint32_t palette_rgb[512];
    char sav_path[1024];
    bool loaded;
    /* Game Genie codes (frontend setting, not part of save states) */
    Cheat cheats[MAX_CHEATS];
    int num_cheats;
    /* overclock (frontend setting, not part of save states): the idle line
     * after the picture (240) is repeated oc_lines times before VBlank, with
     * the APU and mapper timers paused, so games get more CPU time per frame
     * without any change to game speed, music or raster effects */
    int oc_lines;
    uint32_t oc_left;         /* extra lines still to run this frame */
    bool oc_active;           /* currently inside an extra line */
    /* note-start counters for pulse 1, pulse 2, triangle, noise, DMC (bumped on
     * $4003/$4007/$400B/$400F writes and DMC starts); read by the MIDI converter */
    uint32_t apu_trig[5];
    /* output sound filter (a frontend setting, not part of save states) */
    int audio_filter;         /* AUDIO_FILTER_* */
    double af_state[4];       /* extra stages used by the TV-speaker filter */
};

enum {
    AUDIO_FILTER_NES = 0,     /* front-loader NES: 90 Hz + 440 Hz high-pass, 14 kHz low-pass */
    AUDIO_FILTER_FAMICOM,     /* Famicom: 37 Hz high-pass, 14 kHz low-pass */
    AUDIO_FILTER_TV,          /* NES chain + a small TV speaker (no deep bass, ~4.5 kHz roll-off) */
    AUDIO_FILTER_RAW          /* unfiltered (DC offset removed only) */
};

/* api */

bool nes_load(Nes *n, const uint8_t *data, size_t size, char *err, size_t errlen);
void nes_unload(Nes *n);
void nes_power(Nes *n);
void nes_reset(Nes *n);
void nes_run_frame(Nes *n);
void nes_set_sample_rate(Nes *n, double rate);
int  nes_read_audio(Nes *n, float *out, int max);
int  nes_audio_available(Nes *n);
void nes_render_rgb(Nes *n, uint32_t *out /* 256x240 0xAARRGGBB */);
size_t nes_state_size(void);
bool nes_save_state(Nes *n, uint8_t *buf, size_t len);
bool nes_load_state(Nes *n, const uint8_t *buf, size_t len);

/* internal */
uint8_t bus_read(Nes *n, uint16_t a);
void bus_write(Nes *n, uint16_t a, uint8_t v);
uint8_t bus_peek(Nes *n, uint16_t a);
void cpu_power(Nes *n);
void cpu_reset(Nes *n);
void cpu_step(Nes *n);
void cpu_start_dmc_dma(Nes *n);
void cpu_stop_dmc_dma(Nes *n);
void cpu_set_irq(Nes *n, uint8_t src);
void cpu_clear_irq(Nes *n, uint8_t src);

void ppu_power(Nes *n);
void ppu_reset(Nes *n);
void ppu_run(Nes *n, uint64_t to);
uint8_t ppu_reg_read(Nes *n, uint16_t a);
void ppu_reg_write(Nes *n, uint16_t a, uint8_t v);
uint8_t ppu_reg_peek(Nes *n, uint16_t a);

void input_latch_clock(Nes *n);
void apu_power(Nes *n);
void apu_reset(Nes *n);
void apu_clock(Nes *n);
uint8_t apu_read_status(Nes *n);
void apu_write(Nes *n, uint16_t a, uint8_t v);
void apu_dmc_dma_done(Nes *n, uint8_t v);
uint16_t apu_dmc_addr(Nes *n);
bool apu_dmc_wants_dma(Nes *n);

bool cart_load(Nes *n, const uint8_t *data, size_t size, char *err, size_t errlen);
void cart_free(Nes *n);
void cart_power(Nes *n);
void cart_reset(Nes *n);
uint8_t cart_cpu_read(Nes *n, uint16_t a, uint8_t open_bus);
uint8_t cart_cpu_peek(Nes *n, uint16_t a, uint8_t open_bus);
void cart_cpu_write(Nes *n, uint16_t a, uint8_t v);
uint8_t cart_ppu_read(Nes *n, uint16_t a);
void cart_ppu_write(Nes *n, uint16_t a, uint8_t v);
void cart_ppu_addr(Nes *n, uint16_t a);   /* address bus change notification */
void cart_cpu_clock(Nes *n);
void cart_sync(Nes *n);
float cart_audio(Nes *n);
bool cart_supported(int mapper);

void palette_generate(uint32_t *out512);
void palette_generate_ex(uint32_t *out512, double hue_deg, double sat, double bri, double con);

#endif
