/**
 * Dissolver — Spectral DSP engine header
 * GRM Evolution-style spectral dissolution for Ableton Move
 *
 * Core algorithm: PaulStretch-inspired magnitude + random phase reconstruction
 * with spectral spread (log-freq IIR smoothing), tonal/noise separation,
 * and spectral compression.
 *
 * Signal flow per channel:
 *   Input → FFT analysis (magnitude only, discard phase)
 *         → Stretch smoothing (temporal magnitude averaging)
 *         → Spectral spread (log-freq smearing)
 *         → Tonal/noise separation (transient erasure)
 *         → Spectral compressor (flatten dynamics)
 *         → Freeze blend (hold vs. update)
 *         → Evolution-controlled phase reconstruction + IFFT
 *         → Overlap-add output
 *         → Decay Tail (LP-damped recirculating sustain)
 *         → Tilt EQ
 *
 * GPL-3.0 — spectral algorithms derived from PaulXStretch (Nasca/Xenakios/Chappell)
 */

#ifndef DISSOLVER_SPECTRAL_H
#define DISSOLVER_SPECTRAL_H

#include <stdint.h>
#include <math.h>
#include <string.h>
#include "pffft.h"

/* ── Constants ─────────────────────────────────────────────────────── */

#define DSP_SR              44100
#define SPEC_FFT_SIZE       4096        /* ~93ms window at 44.1kHz */
#define SPEC_HALF_SIZE      (SPEC_FFT_SIZE / 2)  /* magnitude bins */
#define SPEC_HOP_SIZE       (SPEC_FFT_SIZE / 4)  /* 75% overlap — 4 overlapping frames for smooth output */
#define SPEC_OLA_BUF_SIZE   (SPEC_FFT_SIZE * 4)  /* overlap-add output buffer (extra room) */

/* Decay tail */
#define DECAY_BUF_SIZE      (DSP_SR * 8)         /* 8s max decay tail */

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define DENORMAL_GUARD 1e-25f
#define MAX_FEEDBACK   0.95f

/* ── Inline utilities ──────────────────────────────────────────────── */

static inline float clampf(float x, float lo, float hi) {
    return x < lo ? lo : (x > hi ? hi : x);
}

static inline float lerpf(float a, float b, float t) {
    return a + t * (b - a);
}

static inline float soft_clip(float x) {
    if (x > 1.5f) return 1.0f;
    if (x < -1.5f) return -1.0f;
    return x - x * x * x / 6.75f;
}

/* ── Spectral engine ───────────────────────────────────────────────── */

typedef struct {
    /* PFFFT */
    PFFFT_Setup *fft_setup;
    float *fft_in;              /* aligned, FFT_SIZE */
    float *fft_out;             /* aligned, FFT_SIZE */
    float *fft_work;            /* aligned, FFT_SIZE */

    /* Analysis window (Hann) */
    float window[SPEC_FFT_SIZE];

    /* Input accumulator */
    float input_buf[SPEC_FFT_SIZE];
    int   input_pos;            /* write position in input_buf */

    /* Magnitude spectra */
    float freq_cur[SPEC_HALF_SIZE];     /* current analysis frame */
    float freq_prev[SPEC_HALF_SIZE];    /* previous frame */
    float freq_proc[SPEC_HALF_SIZE];    /* after spectral processing */
    float freq_frozen[SPEC_HALF_SIZE];  /* frozen snapshot */
    float freq_smooth[SPEC_HALF_SIZE];  /* stretch-smoothed magnitudes */

    /* Spectral spread work buffers */
    float spread_tmp[SPEC_HALF_SIZE];

    /* Phase accumulator (evolution-controlled) */
    float phase_accum[SPEC_HALF_SIZE];

    /* Output overlap-add */
    float ola_buf[SPEC_OLA_BUF_SIZE];
    int   ola_write;            /* where next synthesis frame starts */
    int   ola_read;             /* where next output read starts */
    int   output_counter;       /* counts samples between synthesis frames */

    /* RNG for random phases */
    uint32_t rng;

    /* State flags */
    int initialized;
    int first_frame_done;       /* need at least 1 analysis frame before output */
} SpectralEngine;

/* ── Decay tail ────────────────────────────────────────────────────── */

typedef struct {
    float buffer[DECAY_BUF_SIZE];
    int   write_pos;
    int   length;
    float feedback;
    float input_gate;
    float lp_state;             /* one-pole LP inside feedback loop */
    float lp_coeff;             /* LP coefficient (~2kHz damping) */
} DecayTail;

/* ── Tilt EQ ───────────────────────────────────────────────────────── */

typedef struct {
    float lp_state;
    float tilt;
    float crossover_hz;
    float coeff;
} TiltFilter;

/* ── Single-channel Dissolver ──────────────────────────────────────── */

typedef struct {
    SpectralEngine spectral;
    DecayTail      decay;
    TiltFilter     tilt;
    float          env_state;   /* noise envelope follower */
} DissolverChannel;

/* ── Parameters passed per-block ───────────────────────────────────── */

typedef struct {
    float smoothing;        /* tonal/noise: 0=pass all, 1=only tonal (erase transients) */
    float freeze;           /* 0=live, 1=frozen (always slow update) */
    float stretch;          /* temporal smoothing: 0=live, 1=max pad blur */
    float spread;           /* spectral spread bandwidth */
    float evolution;        /* phase coherence: 0=frozen texture, 1=max shimmer */
    float brightness;       /* tilt EQ (-1..+1 mapped internally) */
    float decay_amt;        /* decay tail amount */
    float feedback_cap;     /* max decay feedback */
    float compress;         /* spectral compressor strength */
    float spread_depth;     /* spread iterations / depth */
    float tilt_freq_norm;   /* tilt crossover 0..1 */
    float output_level;     /* output gain */
    float noise_mix;        /* noise amount: 0=off, 1=full */
    float noise_tone;       /* noise color: 0=white, 0.5=pink, 1=brown */
    float noise_envelope;   /* current envelope value (set by caller per-block) */
    float attack_time;      /* envelope attack: 0=1ms, 1=500ms */
    float release_time;     /* envelope release: 0=10ms, 1=2000ms */
} DissolverParams;

/* ── API ───────────────────────────────────────────────────────────── */

int  dissolver_channel_init(DissolverChannel *ch);
void dissolver_channel_destroy(DissolverChannel *ch);
void dissolver_channel_reset(DissolverChannel *ch);

void dissolver_channel_process(
    DissolverChannel *ch,
    const float *input,
    float *output,
    int frames,
    const DissolverParams *p
);

#endif /* DISSOLVER_SPECTRAL_H */
