/*
 * NES APU -> General MIDI converter, for the "MIDI soundfont" sound filter.
 *
 * Once per emulated frame it looks at what each sound channel is doing and
 * emits MIDI messages: pulse 1 / pulse 2 / triangle become melodic notes on
 * MIDI channels 1-3 (pitch slides and vibrato become pitch bends, volume
 * envelopes become expression), noise and DMC hits become drums on channel 10.
 */
#ifndef MIDI_H
#define MIDI_H
#include "nes.h"

typedef struct { uint8_t status, d1, d2; } MidiMsg;
#define MIDI_MAX_MSGS 96

typedef struct {
    bool sounding[3];
    int base[3];          /* MIDI note currently held */
    int bend[3];          /* last pitch-bend value sent */
    int expr[3];          /* last expression (CC11) sent */
    int vol_prev[3];
    uint32_t trig[5];     /* last seen n->apu_trig values */
    int drum_held[2];     /* noise / DMC drum note to release next frame, -1 none */
    int noise_prev_vol, noise_cool;
    bool noise_was_active;
} MidiState;

/* forget everything (e.g. after the synth was reset) */
void midi_init(MidiState *m, Nes *n);
/* program changes + controller setup for the three melodic channels */
int midi_setup(MidiState *m, const uint8_t programs[3], MidiMsg *out, int max);
/* messages for one emulated frame */
int midi_frame(Nes *n, MidiState *m, MidiMsg *out, int max);
/* release everything that's sounding */
int midi_silence(MidiState *m, MidiMsg *out, int max);

#endif
