/* NES APU -> General MIDI converter. See midi.h. */
#include "midi.h"
#include <math.h>
#include <string.h>
#include <stdlib.h>

#define CPU_HZ 1789773.0
#define DRUM_CH 9

static const uint16_t kNoisePeriod[16] = { 4, 8, 16, 32, 64, 96, 128, 160, 202, 254, 380, 508, 762, 1016, 2034, 4068 };
static const uint16_t kDmcRate[16] = { 428, 380, 340, 320, 286, 254, 226, 214, 190, 160, 142, 128, 106, 84, 72, 54 };

static int put(MidiMsg *out, int k, int max, int st, int d1, int d2) {
    if (k < max) { out[k].status = (uint8_t)st; out[k].d1 = (uint8_t)(d1 & 127); out[k].d2 = (uint8_t)(d2 & 127); return k + 1; }
    return k;
}

void midi_init(MidiState *m, Nes *n) {
    memset(m, 0, sizeof *m);
    for (int c = 0; c < 3; c++) { m->bend[c] = 8192; m->expr[c] = -1; }
    m->drum_held[0] = m->drum_held[1] = -1;
    if (n) memcpy(m->trig, n->apu_trig, sizeof m->trig);
}

int midi_setup(MidiState *m, const uint8_t programs[3], MidiMsg *out, int max) {
    int k = midi_silence(m, out, max);
    for (int c = 0; c < 3; c++) {
        k = put(out, k, max, 0xC0 | c, programs[c], 0);          /* program */
        k = put(out, k, max, 0xB0 | c, 7, 104);                   /* channel volume */
        k = put(out, k, max, 0xB0 | c, 10, c == 0 ? 44 : c == 1 ? 84 : 64);   /* pan: pulses a little apart */
        k = put(out, k, max, 0xB0 | c, 101, 0);                   /* pitch-bend range: +-2 semitones */
        k = put(out, k, max, 0xB0 | c, 100, 0);
        k = put(out, k, max, 0xB0 | c, 6, 2);
        k = put(out, k, max, 0xB0 | c, 38, 0);
        k = put(out, k, max, 0xE0 | c, 0, 64);                    /* bend centre */
        m->bend[c] = 8192; m->expr[c] = -1;
    }
    k = put(out, k, max, 0xB0 | DRUM_CH, 7, 100);
    return k;
}

int midi_silence(MidiState *m, MidiMsg *out, int max) {
    int k = 0;
    for (int c = 0; c < 3; c++) {
        if (m->sounding[c]) k = put(out, k, max, 0x80 | c, m->base[c], 0);
        m->sounding[c] = false;
        k = put(out, k, max, 0xB0 | c, 123, 0);                   /* all notes off */
    }
    for (int d = 0; d < 2; d++) {
        if (m->drum_held[d] >= 0) k = put(out, k, max, 0x80 | DRUM_CH, m->drum_held[d], 0);
        m->drum_held[d] = -1;
    }
    return k;
}

static int bend_value(double semis) {
    int v = 8192 + (int)lround(semis / 2.0 * 8191.0);
    return v < 0 ? 0 : v > 16383 ? 16383 : v;
}

static int tonal(MidiState *m, int c, bool active, double freq, int vol, bool trig, MidiMsg *out, int k, int max) {
    if (!active || freq < 16.0 || freq > 12000.0) {
        if (m->sounding[c]) k = put(out, k, max, 0x80 | c, m->base[c], 0);
        m->sounding[c] = false;
        m->vol_prev[c] = 0;
        return k;
    }
    double fn = 69.0 + 12.0 * log2(freq / 440.0);
    if (fn < 0) fn = 0;
    if (fn > 127) fn = 127;
    bool retrig = trig || !m->sounding[c] || fabs(fn - m->base[c]) > 1.5 || vol > m->vol_prev[c] + 3;
    int expr = (int)lround(127.0 * sqrt(vol / 15.0));
    if (retrig) {
        if (m->sounding[c]) k = put(out, k, max, 0x80 | c, m->base[c], 0);
        m->base[c] = (int)lround(fn);
    }
    int b = bend_value(fn - m->base[c]);
    if (abs(b - m->bend[c]) >= 32 || (retrig && b != m->bend[c])) {
        k = put(out, k, max, 0xE0 | c, b & 127, b >> 7);
        m->bend[c] = b;
    }
    if (expr != m->expr[c]) { k = put(out, k, max, 0xB0 | c, 11, expr); m->expr[c] = expr; }
    if (retrig) { k = put(out, k, max, 0x90 | c, m->base[c], 112); m->sounding[c] = true; }
    m->vol_prev[c] = vol;
    return k;
}

static inline int env_vol(const Envelope *e) { return e->constant ? e->vol : e->decay; }

int midi_frame(Nes *n, MidiState *m, MidiMsg *out, int max) {
    Apu *a = &n->apu;
    int k = 0;
    bool trig[5];
    for (int i = 0; i < 5; i++) { trig[i] = n->apu_trig[i] != m->trig[i]; m->trig[i] = n->apu_trig[i]; }

    /* release last frame's drum hits */
    for (int d = 0; d < 2; d++) if (m->drum_held[d] >= 0) { k = put(out, k, max, 0x80 | DRUM_CH, m->drum_held[d], 0); m->drum_held[d] = -1; }

    /* pulses */
    const Pulse *ps[2] = { &a->p1, &a->p2 };
    for (int c = 0; c < 2; c++) {
        const Pulse *p = ps[c];
        int vol = env_vol(&p->env);
        bool muted = p->period < 8 || (!p->sweep_neg && p->target > 0x7FF);
        bool active = p->len.counter && !muted && vol > 0;
        k = tonal(m, c, active, CPU_HZ / (16.0 * (p->period + 1)), vol, trig[c], out, k, max);
    }
    /* triangle (no volume control: full expression) */
    {
        const Triangle *t = &a->tri;
        bool active = t->len.counter && t->lin_ctr && t->period >= 2;
        k = tonal(m, 2, active, CPU_HZ / (32.0 * (t->period + 1)), 15, trig[2], out, k, max);
    }
    /* noise -> hi-hat / snare / kick, picked by pitch */
    {
        const Noise *no = &a->noise;
        int vol = env_vol(&no->env);
        bool active = no->len.counter && vol > 0;
        if (m->noise_cool) m->noise_cool--;
        bool hit = active && ((trig[3] && m->noise_cool == 0) || vol > m->noise_prev_vol + 3 || !m->noise_was_active);
        if (hit) {
            int idx = 15;
            for (int i = 0; i < 16; i++) if (kNoisePeriod[i] == no->period) { idx = i; break; }
            int note = idx <= 3 ? (no->mode ? 49 : 42)        /* crash (metallic mode) / closed hi-hat */
                     : idx <= 5 ? 46                            /* open hi-hat */
                     : idx <= 9 ? 38                            /* snare */
                     : idx <= 12 ? 40                           /* electric snare */
                     : 36;                                      /* kick / rumble */
            k = put(out, k, max, 0x90 | DRUM_CH, note, 40 + vol * 87 / 15);
            m->drum_held[0] = note;
            m->noise_cool = 3;     /* games that rewrite $400F every frame don't machine-gun */
        }
        m->noise_prev_vol = active ? vol : 0;
        m->noise_was_active = active;
    }
    /* DMC sample start -> kick or snare by playback rate */
    if (trig[4]) {
        int idx = 15;
        for (int i = 0; i < 16; i++) if (kDmcRate[i] == a->dmc.rate) { idx = i; break; }
        int note = idx <= 12 ? 36 : 38;
        k = put(out, k, max, 0x90 | DRUM_CH, note, 110);
        m->drum_held[1] = note;
    }
    return k;
}
