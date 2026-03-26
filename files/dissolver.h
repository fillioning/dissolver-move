#ifndef DISSOLVER_H
#define DISSOLVER_H

#include <stdint.h>
#include <math.h>
#include <string.h>
#include <stdlib.h>

/* ── Constants ─────────────────────────────────────────────────────── */

#define SAMPLE_RATE       48000
#define MAX_GRAIN_SAMPLES 4096      /* ~85ms at 48kHz */
#define MIN_GRAIN_SAMPLES 256       /* ~5ms  */
#define MAX_GRAINS        16        /* max simultaneous overlap grains */
#define ENVELOPE_BUF_SIZE 512       /* envelope follower history */
#define FADE_BUF_SIZE     (MAX_GRAIN_SAMPLES * 2)  /* circular capture buffer */
#define DECAY_BUF_SIZE    (SAMPLE_RATE * 8)         /* 8s max decay tail */

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* ── Envelope follower (transient detector) ────────────────────────── */

typedef struct {
    float attack_coeff;     /* fast rise to track onsets */
    float release_coeff;    /* slow fall for sustained content */
    float envelope;         /* current envelope value */
    float prev_envelope;    /* previous sample's envelope */
    float transient_gate;   /* 0..1 attenuation for transients */
} EnvelopeFollower;

/* ── Single grain ──────────────────────────────────────────────────── */

typedef struct {
    float buffer[MAX_GRAIN_SAMPLES];
    int   length;           /* grain size in samples */
    int   read_pos;         /* current playback position */
    int   active;           /* is this grain playing? */
    float amplitude;        /* per-grain amplitude (window-shaped) */
    float pan;              /* subtle stereo spread (future use) */
} Grain;

/* ── Granular overlap-add engine ───────────────────────────────────── */

typedef struct {
    Grain grains[MAX_GRAINS];
    float capture_buf[FADE_BUF_SIZE];   /* circular input capture */
    int   capture_write;                /* write head in capture buf */
    int   grain_size;                   /* current grain size in samples */
    int   overlap_count;                /* how many grains overlap */
    int   samples_since_trigger;        /* countdown to next grain spawn */
    int   next_grain_idx;               /* round-robin grain slot */
} GranularEngine;

/* ── Spectral freeze accumulator ───────────────────────────────────── */
/*    Simple approach: maintain a smoothed buffer that blends between
      incoming grains and the frozen state                              */

typedef struct {
    float frozen_buf[MAX_GRAIN_SAMPLES];
    int   frozen_len;
    float freeze_amount;    /* 0 = fully live, 1 = fully frozen */
    float evolution_phase;  /* LFO phase for spectral drift */
    float evolution_rate;   /* speed of spectral morphing */
} SpectralFreeze;

/* ── Decay tail (sustain after input stops) ────────────────────────── */

typedef struct {
    float buffer[DECAY_BUF_SIZE];
    int   write_pos;
    int   length;           /* active decay length in samples */
    float feedback;         /* recirculation amount (from decay param) */
    float level;            /* current output level of decay */
    int   active;           /* is the decay tail sounding? */
    float input_gate;       /* tracks whether input is present */
    float gate_release;     /* slow release to detect "input stopped" */
} DecayTail;

/* ── Brightness filter (one-pole tilt) ─────────────────────────────── */

typedef struct {
    float lp_state;         /* lowpass state */
    float hp_state;         /* highpass state */
    float tilt;             /* -1..+1, mapped from 0..1 knob */
} TiltFilter;

/* ── Top-level plugin state ────────────────────────────────────────── */

typedef struct {
    /* DSP engines */
    EnvelopeFollower env;
    GranularEngine   granular;
    SpectralFreeze   freeze;
    DecayTail        decay;
    TiltFilter       tilt;

    /* Parameters (0..1 normalized) */
    float p_smoothing;
    float p_freeze;
    float p_grain_size;
    float p_density;
    float p_evolution;
    float p_brightness;
    float p_mix;
    float p_decay;

    /* Internal */
    float sample_rate;
    int   initialized;
} Dissolver;

/* ── API ───────────────────────────────────────────────────────────── */

void dissolver_init(Dissolver* d, float sample_rate);
void dissolver_reset(Dissolver* d);
void dissolver_set_parameter(Dissolver* d, int param_index, float value);
void dissolver_process(Dissolver* d, const float* input, float* output, int frames);

#endif /* DISSOLVER_H */
