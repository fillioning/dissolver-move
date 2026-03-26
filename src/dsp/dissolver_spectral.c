/**
 * Dissolver — Spectral DSP engine
 * GRM Evolution-style spectral dissolution for Ableton Move
 *
 * Core: magnitude-only FFT + spectral processing + evolution-controlled phase IFFT
 * Algorithms derived from PaulXStretch (GPL-3.0)
 *
 * v2 fixes:
 *   - Stretch decoupled from input (always real-time analysis, stretch = magnitude smoothing)
 *   - Evolution wired to phase coherence (0=frozen texture, 1=max shimmer)
 *   - Tonal/noise separation buffer corruption fixed
 *   - COLA normalization for correct OLA gain
 *   - Startup silence eliminated (synthesize immediately on first analysis)
 *   - Decay tail LP-damped feedback (diffuse pad sustain, not metallic echo)
 */

#include "dissolver_spectral.h"
#include <stdlib.h>

/* ══════════════════════════════════════════════════════════════════════
   RNG — fast pseudo-random for phase generation (no stdlib in render)
   ══════════════════════════════════════════════════════════════════════ */

static inline float rng_phase(uint32_t *state) {
    *state ^= *state << 13;
    *state ^= *state >> 17;
    *state ^= *state << 5;
    return (float)(*state & 0xFFFF) * (2.0f * (float)M_PI / 65536.0f);
}

/* ══════════════════════════════════════════════════════════════════════
   Spectral processing functions
   (derived from PaulXStretch ProcessedStretch.h, rewritten in C)
   ══════════════════════════════════════════════════════════════════════ */

/**
 * spectrum_spread — log-frequency bidirectional IIR smoothing
 * Core "dissolve" operation: blurs spectral peaks into neighbors.
 *
 * IMPORTANT: modifies `freq` in-place. `tmp` is used as work buffer.
 * After return, `freq` contains the smoothed result.
 */
static void spectrum_spread(float *freq, float *tmp, int nfreq,
                            float bandwidth, float depth_norm) {
    if (bandwidth < 0.001f) return;

    int passes = 1 + (int)(depth_norm * 3.0f);  /* 1-4 passes */
    float bw2 = bandwidth * bandwidth;
    float a_base = 1.0f - powf(2.0f, -bw2 * 10.0f);

    float scale = 8192.0f / (float)nfreq;
    float a = powf(a_base, scale * (float)passes);

    for (int pass = 0; pass < passes; pass++) {
        /* Forward pass */
        tmp[0] = freq[0];
        for (int i = 1; i < nfreq; i++) {
            tmp[i] = tmp[i - 1] * a + freq[i] * (1.0f - a) + DENORMAL_GUARD;
        }
        /* Backward pass */
        freq[nfreq - 1] = tmp[nfreq - 1];
        for (int i = nfreq - 2; i >= 0; i--) {
            freq[i] = freq[i + 1] * a + tmp[i] * (1.0f - a) + DENORMAL_GUARD;
        }
    }
}

/**
 * spectrum_compress — spectral dynamics flattening
 * power: 0=off, 1=full compression (normalize all bins to same level)
 */
static void spectrum_compress(float *freq, int nfreq, float power) {
    if (power < 0.01f) return;

    float sum = 0.0f;
    for (int i = 0; i < nfreq; i++) {
        sum += freq[i] * freq[i];
    }
    float rms = sqrtf(sum / (float)nfreq) * 0.1f;
    if (rms < 1e-10f) return;

    float gain = powf(rms, -power);
    if (gain > 100.0f) gain = 100.0f;

    for (int i = 0; i < nfreq; i++) {
        freq[i] *= gain;
    }
}

/* ══════════════════════════════════════════════════════════════════════
   Spectral Engine — FFT analysis, processing, evolution phase synthesis
   ══════════════════════════════════════════════════════════════════════ */

static int spectral_init(SpectralEngine *s) {
    memset(s, 0, sizeof(SpectralEngine));

    s->fft_setup = pffft_new_setup(SPEC_FFT_SIZE, PFFFT_REAL);
    if (!s->fft_setup) return -1;

    s->fft_in   = (float *)pffft_aligned_calloc(SPEC_FFT_SIZE, sizeof(float));
    s->fft_out  = (float *)pffft_aligned_calloc(SPEC_FFT_SIZE, sizeof(float));
    s->fft_work = (float *)pffft_aligned_calloc(SPEC_FFT_SIZE, sizeof(float));

    if (!s->fft_in || !s->fft_out || !s->fft_work) return -1;

    /* Hann window */
    for (int i = 0; i < SPEC_FFT_SIZE; i++) {
        s->window[i] = 0.5f * (1.0f - cosf(2.0f * (float)M_PI * (float)i / (float)(SPEC_FFT_SIZE - 1)));
    }

    /* Initialize phase accumulators with random phases */
    s->rng = 123456789u;
    for (int i = 0; i < SPEC_HALF_SIZE; i++) {
        s->phase_accum[i] = rng_phase(&s->rng);
    }

    s->input_pos = 0;
    s->ola_write = 0;
    s->ola_read = 0;
    s->output_counter = 0;
    s->initialized = 1;
    s->first_frame_done = 0;

    return 0;
}

static void spectral_destroy(SpectralEngine *s) {
    if (s->fft_in)    pffft_aligned_free(s->fft_in);
    if (s->fft_out)   pffft_aligned_free(s->fft_out);
    if (s->fft_work)  pffft_aligned_free(s->fft_work);
    if (s->fft_setup) pffft_destroy_setup(s->fft_setup);
    s->fft_in = NULL;
    s->fft_out = NULL;
    s->fft_work = NULL;
    s->fft_setup = NULL;
    s->initialized = 0;
}

/**
 * Analyze one FFT frame: window → FFT → extract magnitudes
 */
static void spectral_analyze(SpectralEngine *s) {
    memcpy(s->freq_prev, s->freq_cur, SPEC_HALF_SIZE * sizeof(float));

    /* Apply window to input */
    for (int i = 0; i < SPEC_FFT_SIZE; i++) {
        s->fft_in[i] = s->input_buf[i] * s->window[i];
    }

    /* Forward FFT */
    pffft_transform_ordered(s->fft_setup, s->fft_in, s->fft_out,
                            s->fft_work, PFFFT_FORWARD);

    /* Extract magnitudes (discard phase — core PaulStretch concept) */
    s->freq_cur[0] = fabsf(s->fft_out[0]);  /* DC */
    for (int i = 1; i < SPEC_HALF_SIZE; i++) {
        float re = s->fft_out[i * 2];
        float im = s->fft_out[i * 2 + 1];
        s->freq_cur[i] = sqrtf(re * re + im * im);
    }
}

/**
 * Synthesize one FFT frame: magnitudes → evolution-controlled phases → IFFT → OLA
 *
 * evolution: 0 = reuse accumulated phases (frozen texture)
 *            1 = fully random phase deltas each frame (maximum shimmer)
 */
static void spectral_synthesize(SpectralEngine *s, float evolution) {
    /* DC: no phase */
    s->fft_out[0] = s->freq_proc[0];
    s->fft_out[1] = 0.0f;

    /* Phase advance strategy:
     * - Natural advance: 2*PI*bin*hop/FFT_size per frame (phase vocoder baseline).
     *   Prevents the ~43Hz ringing artifact from OLA-ing identical waveforms.
     * - Random variation: scaled by evolution knob.
     *   evolution=0 → clean phase vocoder (frozen texture, no ringing)
     *   evolution=1 → full random phases (PaulStretch shimmer) */
    float hop_phase_inc = 2.0f * (float)M_PI * (float)SPEC_HOP_SIZE / (float)SPEC_FFT_SIZE;

    for (int i = 1; i < SPEC_HALF_SIZE; i++) {
        float natural = hop_phase_inc * (float)i;
        float random_delta = (rng_phase(&s->rng) - (float)M_PI) * evolution;

        s->phase_accum[i] += natural + random_delta;

        /* Wrap to [0, 2*PI) */
        while (s->phase_accum[i] >= 2.0f * (float)M_PI) s->phase_accum[i] -= 2.0f * (float)M_PI;
        while (s->phase_accum[i] < 0.0f)                 s->phase_accum[i] += 2.0f * (float)M_PI;

        float phase = s->phase_accum[i];
        s->fft_out[i * 2]     = s->freq_proc[i] * cosf(phase);
        s->fft_out[i * 2 + 1] = s->freq_proc[i] * sinf(phase);
    }

    /* Inverse FFT */
    pffft_transform_ordered(s->fft_setup, s->fft_out, s->fft_in,
                            s->fft_work, PFFFT_BACKWARD);

    /* Scale: 1/N for IFFT * COLA compensation for Hann² at 75% overlap
     * Hann² sum at 75% hop = 1.5; empirical boost for random-phase energy scatter */
    float scale = (4.0f / 3.0f) / (float)SPEC_FFT_SIZE;
    for (int i = 0; i < SPEC_FFT_SIZE; i++) {
        s->fft_in[i] *= scale * s->window[i];
    }

    /* Overlap-add into output buffer */
    for (int i = 0; i < SPEC_FFT_SIZE; i++) {
        int idx = (s->ola_write + i) % SPEC_OLA_BUF_SIZE;
        s->ola_buf[idx] += s->fft_in[i];
    }

    /* Advance OLA write by hop */
    s->ola_write = (s->ola_write + SPEC_HOP_SIZE) % SPEC_OLA_BUF_SIZE;
}

/**
 * Process the magnitude spectrum through the dissolution chain.
 * freq_proc is pre-loaded with stretch-smoothed magnitudes.
 */
static void spectral_process_magnitudes(SpectralEngine *s, const DissolverParams *p) {
    /* 1. Dissolve — spectral gate
     * Removes weak spectral bins (noise floor), keeps strong harmonics.
     * At 0: everything passes. At 1: only the strongest harmonics survive.
     * Result: cleaner, more ethereal pad — no added noise. */
    if (p->smoothing > 0.01f) {
        float sum_sq = 0.0f;
        for (int i = 1; i < SPEC_HALF_SIZE; i++) {
            sum_sq += s->freq_proc[i] * s->freq_proc[i];
        }
        float rms = sqrtf(sum_sq / (float)(SPEC_HALF_SIZE - 1));
        if (rms > 1e-10f) {
            float thresh = rms * p->smoothing * 2.0f;
            for (int i = 1; i < SPEC_HALF_SIZE; i++) {
                if (s->freq_proc[i] < thresh) {
                    float ratio = s->freq_proc[i] / thresh;
                    s->freq_proc[i] *= ratio;  /* soft quadratic gate */
                }
            }
        }
    }

    /* 2. Spectral spread — log-frequency smearing
     * Bandwidth capped at 0.4 to prevent full white noise at high values */
    if (p->spread > 0.01f) {
        spectrum_spread(s->freq_proc, s->spread_tmp, SPEC_HALF_SIZE,
                        p->spread * 0.4f, p->spread_depth);
    }

    /* 3. Spectral compressor */
    spectrum_compress(s->freq_proc, SPEC_HALF_SIZE, p->compress);

    /* 4. Freeze blend — hold a magnitude snapshot, always allow slow update */
    if (p->freeze > 0.01f) {
        float update_rate = (1.0f - p->freeze) * 0.1f + 0.001f;
        for (int i = 0; i < SPEC_HALF_SIZE; i++) {
            s->freq_frozen[i] += update_rate * (s->freq_proc[i] - s->freq_frozen[i])
                                 + DENORMAL_GUARD;
            s->freq_proc[i] = lerpf(s->freq_proc[i], s->freq_frozen[i], p->freeze);
        }
    }

    /* 5. Low-frequency body preservation
     * Spectral spread redistributes low-freq energy upward — restore sub/low-mid
     * by blending lowest bins back toward the pre-processing magnitudes (freq_smooth) */
    for (int i = 0; i < 40 && i < SPEC_HALF_SIZE; i++) {
        float blend = (float)(40 - i) / 40.0f * 0.7f;  /* 70% at DC → 0% at ~430Hz */
        s->freq_proc[i] = lerpf(s->freq_proc[i], s->freq_smooth[i], blend);
    }
}

/**
 * Feed input samples and produce output samples.
 *
 * KEY CHANGE from v1: input analysis always runs in real-time.
 * Stretch controls how slowly the working magnitude spectrum (freq_smooth)
 * tracks new analysis data. Synthesis fires at a constant rate (every hop).
 * This eliminates startup silence and input starvation at high stretch.
 */
static void spectral_process(SpectralEngine *s, const float *input, float *output,
                              int frames, const DissolverParams *p) {
    if (!s->initialized) {
        memset(output, 0, frames * sizeof(float));
        return;
    }

    /* Stretch → smoothing alpha for magnitude tracking
     * stretch=0: alpha=1.0 (instant, live response)
     * stretch=0.5: alpha≈0.05 (smooth pad, ~1s time constant)
     * stretch=1.0: alpha≈0.0025 (very slow, long pad blur) */
    float stretch_alpha = expf(-p->stretch * 6.0f);

    for (int i = 0; i < frames; i++) {
        /* ── Stage 1: Accumulate input ── */
        s->input_buf[s->input_pos] = input[i];
        s->input_pos++;

        /* ── Stage 2: Analyze when we have a full FFT frame ── */
        if (s->input_pos >= SPEC_FFT_SIZE) {
            spectral_analyze(s);

            /* Shift input buffer by hop (keep overlap) */
            memmove(s->input_buf, s->input_buf + SPEC_HOP_SIZE,
                    (SPEC_FFT_SIZE - SPEC_HOP_SIZE) * sizeof(float));
            s->input_pos = SPEC_FFT_SIZE - SPEC_HOP_SIZE;

            if (!s->first_frame_done) {
                /* Bootstrap: initialize smooth/frozen/prev from first analysis */
                memcpy(s->freq_prev, s->freq_cur, SPEC_HALF_SIZE * sizeof(float));
                memcpy(s->freq_frozen, s->freq_cur, SPEC_HALF_SIZE * sizeof(float));
                memcpy(s->freq_smooth, s->freq_cur, SPEC_HALF_SIZE * sizeof(float));
                s->first_frame_done = 1;

                /* Synthesize immediately to fill OLA and minimize startup silence */
                memcpy(s->freq_proc, s->freq_smooth, SPEC_HALF_SIZE * sizeof(float));
                spectral_process_magnitudes(s, p);
                spectral_synthesize(s, p->evolution);

                /* Realign read head to start of synthesized data —
                 * without this, ola_read has been advancing through zeros
                 * and sits ahead of the write head → silence gap */
                s->ola_read = 0;
                s->output_counter = 0;
            }
        }

        /* ── Stage 3: Fixed-rate output synthesis ── */
        s->output_counter++;
        if (s->output_counter >= SPEC_HOP_SIZE && s->first_frame_done) {
            s->output_counter = 0;

            /* Update smoothed magnitudes from latest analysis (stretch-controlled) */
            for (int j = 0; j < SPEC_HALF_SIZE; j++) {
                s->freq_smooth[j] += stretch_alpha * (s->freq_cur[j] - s->freq_smooth[j])
                                     + DENORMAL_GUARD;
            }

            /* Process through dissolution chain */
            memcpy(s->freq_proc, s->freq_smooth, SPEC_HALF_SIZE * sizeof(float));
            spectral_process_magnitudes(s, p);
            spectral_synthesize(s, p->evolution);
        }

        /* ── Stage 4: Read from OLA output buffer ── */
        output[i] = s->ola_buf[s->ola_read];
        s->ola_buf[s->ola_read] = 0.0f;  /* clear after reading */
        s->ola_read = (s->ola_read + 1) % SPEC_OLA_BUF_SIZE;
    }
}

/* ══════════════════════════════════════════════════════════════════════
   Decay Tail — LP-damped recirculating sustain
   v2: one-pole lowpass inside feedback loop → diffuse pad tail,
   not metallic comb resonance.
   ══════════════════════════════════════════════════════════════════════ */

static void decay_init(DecayTail *dt) {
    memset(dt->buffer, 0, sizeof(dt->buffer));
    dt->write_pos = 0;
    dt->length = DSP_SR * 2;
    dt->feedback = 0.5f;
    dt->input_gate = 0.0f;
    dt->lp_state = 0.0f;
    /* ~2kHz cutoff — damps highs each recirculation for natural pad decay */
    dt->lp_coeff = 1.0f - expf(-6.2831853f * 2000.0f / (float)DSP_SR);
}

static float decay_process(DecayTail *dt, float input) {
    float abs_in = fabsf(input);

    if (abs_in > 0.001f) {
        dt->input_gate = 1.0f;
    } else {
        dt->input_gate = dt->input_gate * 0.9999f + DENORMAL_GUARD;
    }

    if (dt->write_pos >= dt->length) {
        dt->write_pos = dt->write_pos % dt->length;
    }

    float read_sample = dt->buffer[dt->write_pos];

    /* Soft-clip + lowpass inside feedback loop:
     * - soft_clip prevents runaway at high feedback
     * - LP damps high frequencies each recirculation → progressively darker tail
     *   (this is what real reverb tails do) */
    float fb_raw = soft_clip(read_sample) * dt->feedback;
    dt->lp_state += dt->lp_coeff * (fb_raw - dt->lp_state) + DENORMAL_GUARD;

    dt->buffer[dt->write_pos] = input + dt->lp_state + DENORMAL_GUARD;
    dt->write_pos = (dt->write_pos + 1) % dt->length;

    float tail_output = dt->lp_state;
    float gate = clampf(dt->input_gate, 0.0f, 1.0f);
    float out = input + tail_output * (1.0f - gate * 0.5f);

    return out;
}

/* ══════════════════════════════════════════════════════════════════════
   Tilt EQ
   ══════════════════════════════════════════════════════════════════════ */

static void tilt_init(TiltFilter *t) {
    t->lp_state = 0.0f;
    t->tilt = 0.0f;
    t->crossover_hz = 800.0f;
    t->coeff = 1.0f - expf(-6.2831853f * 800.0f / (float)DSP_SR);
}

static void tilt_update_coeff(TiltFilter *t) {
    t->coeff = 1.0f - expf(-6.2831853f * t->crossover_hz / (float)DSP_SR);
}

static float tilt_process(TiltFilter *t, float input) {
    t->lp_state += t->coeff * (input - t->lp_state) + DENORMAL_GUARD;
    float lp = t->lp_state;
    float hp = input - lp;

    float tilt = t->tilt;
    float lp_gain, hp_gain;

    if (tilt >= 0.0f) {
        lp_gain = 1.0f - tilt * 0.7f;
        hp_gain = 1.0f + tilt * 1.5f;
    } else {
        lp_gain = 1.0f + (-tilt) * 1.5f;
        hp_gain = 1.0f - (-tilt) * 0.7f;
    }

    return lp * lp_gain + hp * hp_gain;
}

/* ══════════════════════════════════════════════════════════════════════
   Public API
   ══════════════════════════════════════════════════════════════════════ */

int dissolver_channel_init(DissolverChannel *ch) {
    int ret = spectral_init(&ch->spectral);
    if (ret != 0) return ret;
    decay_init(&ch->decay);
    tilt_init(&ch->tilt);
    return 0;
}

void dissolver_channel_destroy(DissolverChannel *ch) {
    spectral_destroy(&ch->spectral);
}

void dissolver_channel_reset(DissolverChannel *ch) {
    spectral_destroy(&ch->spectral);
    spectral_init(&ch->spectral);
    decay_init(&ch->decay);
    tilt_init(&ch->tilt);
}

void dissolver_channel_process(
    DissolverChannel *ch,
    const float *input,
    float *output,
    int frames,
    const DissolverParams *p)
{
    /* Spectral dissolution (analysis → stretch smooth → spread → tonal → freeze → synthesis) */
    float spectral_out[128];
    spectral_process(&ch->spectral, input, spectral_out, frames, p);

    /* Update decay params */
    float fb = p->decay_amt * p->feedback_cap;
    if (fb > MAX_FEEDBACK) fb = MAX_FEEDBACK;
    ch->decay.feedback = fb;
    ch->decay.length = (int)(p->decay_amt * (float)(DECAY_BUF_SIZE - 1)) + DSP_SR / 4;
    if (ch->decay.length > DECAY_BUF_SIZE) ch->decay.length = DECAY_BUF_SIZE;

    /* Update tilt params */
    ch->tilt.tilt = (p->brightness - 0.5f) * 2.0f;
    ch->tilt.crossover_hz = 200.0f + p->tilt_freq_norm * 3800.0f;
    tilt_update_coeff(&ch->tilt);

    /* Post-processing: Decay → Tilt */
    for (int i = 0; i < frames; i++) {
        float sample = spectral_out[i];
        sample = decay_process(&ch->decay, sample);
        output[i] = tilt_process(&ch->tilt, sample);
    }
}
