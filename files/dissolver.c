#include "dissolver.h"

/* ══════════════════════════════════════════════════════════════════════
   DISSOLVER — Spectral Smearing Pad Generator
   
   Signal flow:
   
   Input → Envelope Follower → Transient Gate → Capture Buffer
                                                      ↓
                                              Granular OLA Engine
                                                      ↓
                                              Spectral Freeze Blend
                                                      ↓
                                              Decay Tail Sustainer
                                                      ↓
                                              Tilt / Brightness EQ
                                                      ↓
                                              Dry/Wet Mix → Output
   ══════════════════════════════════════════════════════════════════════ */

/* ── Utility ───────────────────────────────────────────────────────── */

static inline float clampf(float x, float lo, float hi) {
    return x < lo ? lo : (x > hi ? hi : x);
}

static inline float lerpf(float a, float b, float t) {
    return a + t * (b - a);
}

/* Hann window for grain envelope */
static inline float hann_window(int pos, int length) {
    if (length <= 1) return 1.0f;
    float phase = (float)pos / (float)(length - 1);
    return 0.5f * (1.0f - cosf(2.0f * (float)M_PI * phase));
}

/* One-pole smoother coefficient from time in seconds */
static inline float one_pole_coeff(float time_sec, float sr) {
    if (time_sec <= 0.0f) return 1.0f;
    return 1.0f - expf(-1.0f / (time_sec * sr));
}

/* ── Envelope Follower ─────────────────────────────────────────────── */

static void env_init(EnvelopeFollower* e, float sr) {
    e->attack_coeff  = one_pole_coeff(0.0005f, sr);  /* 0.5ms attack */
    e->release_coeff = one_pole_coeff(0.050f, sr);    /* 50ms release */
    e->envelope      = 0.0f;
    e->prev_envelope = 0.0f;
    e->transient_gate = 1.0f;
}

static float env_process(EnvelopeFollower* e, float input, float smoothing) {
    float abs_in = fabsf(input);
    
    /* Asymmetric envelope follower */
    float coeff = (abs_in > e->envelope) ? e->attack_coeff : e->release_coeff;
    e->envelope = e->envelope + coeff * (abs_in - e->envelope);
    
    /* Transient detection: compare fast envelope derivative */
    float env_delta = e->envelope - e->prev_envelope;
    e->prev_envelope = e->envelope;
    
    /* Positive delta = onset/transient. Suppress proportionally to smoothing. */
    if (env_delta > 0.0f) {
        /* Scale the transient suppression: smoothing 0=none, 1=full kill */
        float suppress = env_delta * smoothing * 80.0f;  /* sensitivity scaling */
        suppress = clampf(suppress, 0.0f, 1.0f);
        /* Smooth the gate to avoid clicks */
        e->transient_gate = e->transient_gate * 0.95f + (1.0f - suppress) * 0.05f;
    } else {
        /* Release: gate opens back up */
        e->transient_gate = e->transient_gate * 0.99f + 1.0f * 0.01f;
    }
    
    e->transient_gate = clampf(e->transient_gate, 0.0f, 1.0f);
    
    return input * e->transient_gate;
}

/* ── Granular Engine ───────────────────────────────────────────────── */

static void granular_init(GranularEngine* g) {
    memset(g->capture_buf, 0, sizeof(g->capture_buf));
    g->capture_write = 0;
    g->grain_size = 2048;
    g->overlap_count = 4;
    g->samples_since_trigger = 0;
    g->next_grain_idx = 0;
    
    for (int i = 0; i < MAX_GRAINS; i++) {
        g->grains[i].active = 0;
        g->grains[i].read_pos = 0;
        g->grains[i].length = 0;
        g->grains[i].amplitude = 0.0f;
    }
}

static void granular_update_params(GranularEngine* g, float grain_size_norm, float density_norm) {
    /* Map grain size: 0..1 → MIN_GRAIN_SAMPLES..MAX_GRAIN_SAMPLES */
    g->grain_size = MIN_GRAIN_SAMPLES + 
        (int)(grain_size_norm * (float)(MAX_GRAIN_SAMPLES - MIN_GRAIN_SAMPLES));
    
    /* Map density: 0..1 → 2..MAX_GRAINS overlap count */
    g->overlap_count = 2 + (int)(density_norm * (float)(MAX_GRAINS - 2));
}

/* Write a sample into the circular capture buffer */
static void granular_capture(GranularEngine* g, float sample) {
    g->capture_buf[g->capture_write] = sample;
    g->capture_write = (g->capture_write + 1) % FADE_BUF_SIZE;
}

/* Spawn a new grain from the capture buffer */
static void granular_spawn_grain(GranularEngine* g) {
    Grain* grain = &g->grains[g->next_grain_idx];
    
    int len = g->grain_size;
    grain->length   = len;
    grain->read_pos = 0;
    grain->active   = 1;
    grain->amplitude = 1.0f;
    
    /* Copy from capture buffer (read backwards from write head) */
    int read_start = (g->capture_write - len + FADE_BUF_SIZE) % FADE_BUF_SIZE;
    for (int i = 0; i < len; i++) {
        int idx = (read_start + i) % FADE_BUF_SIZE;
        grain->buffer[i] = g->capture_buf[idx];
    }
    
    g->next_grain_idx = (g->next_grain_idx + 1) % MAX_GRAINS;
}

/* Process one sample from all active grains (overlap-add output) */
static float granular_process(GranularEngine* g, float input) {
    /* Capture input */
    granular_capture(g, input);
    
    /* Check if we should spawn a new grain */
    int hop_size = g->grain_size / g->overlap_count;
    if (hop_size < 1) hop_size = 1;
    
    g->samples_since_trigger++;
    if (g->samples_since_trigger >= hop_size) {
        granular_spawn_grain(g);
        g->samples_since_trigger = 0;
    }
    
    /* Sum all active grains with Hann windowing */
    float out = 0.0f;
    int active_count = 0;
    
    for (int i = 0; i < MAX_GRAINS; i++) {
        Grain* grain = &g->grains[i];
        if (!grain->active) continue;
        
        float window = hann_window(grain->read_pos, grain->length);
        out += grain->buffer[grain->read_pos] * window * grain->amplitude;
        
        grain->read_pos++;
        if (grain->read_pos >= grain->length) {
            grain->active = 0;
        }
        active_count++;
    }
    
    /* Normalize by overlap to prevent volume buildup */
    if (active_count > 0) {
        /* Hann OLA with proper hop gives ~1.0 gain, but we normalize gently */
        float norm = 2.0f / (float)(g->overlap_count);
        out *= norm;
    }
    
    return out;
}

/* ── Spectral Freeze ───────────────────────────────────────────────── */

static void freeze_init(SpectralFreeze* f) {
    memset(f->frozen_buf, 0, sizeof(f->frozen_buf));
    f->frozen_len = MAX_GRAIN_SAMPLES;
    f->freeze_amount = 0.0f;
    f->evolution_phase = 0.0f;
    f->evolution_rate = 0.0f;
}

static float freeze_process(SpectralFreeze* f, float input, float grain_phase, int grain_size) {
    /* grain_phase: 0..1 normalized position within current grain cycle */
    int idx = (int)(grain_phase * (float)(grain_size - 1));
    idx = clampf(idx, 0, MAX_GRAIN_SAMPLES - 1);
    
    /* Blend between live input and frozen buffer */
    float frozen_sample = f->frozen_buf[idx];
    float blended = lerpf(input, frozen_sample, f->freeze_amount);
    
    /* Update frozen buffer slowly (inverse of freeze controls update rate) */
    float update_rate = 1.0f - f->freeze_amount;
    update_rate = update_rate * 0.1f;  /* slow down the update */
    f->frozen_buf[idx] = f->frozen_buf[idx] + update_rate * (input - f->frozen_buf[idx]);
    
    /* Evolution: subtle modulation of the frozen buffer readout */
    f->evolution_phase += f->evolution_rate / 48000.0f;
    if (f->evolution_phase > 1.0f) f->evolution_phase -= 1.0f;
    
    float evo_mod = sinf(2.0f * (float)M_PI * f->evolution_phase);
    
    /* Cross-read from a slightly offset position for spectral drift */
    int offset = (int)(evo_mod * 32.0f);  /* ±32 samples drift */
    int drift_idx = (idx + offset + MAX_GRAIN_SAMPLES) % MAX_GRAIN_SAMPLES;
    if (drift_idx < grain_size) {
        float drift_amount = f->evolution_rate * 0.3f;
        blended = lerpf(blended, f->frozen_buf[drift_idx], drift_amount);
    }
    
    return blended;
}

/* ── Decay Tail ────────────────────────────────────────────────────── */

static void decay_init(DecayTail* dt) {
    memset(dt->buffer, 0, sizeof(dt->buffer));
    dt->write_pos = 0;
    dt->length = SAMPLE_RATE * 2;  /* default 2 seconds */
    dt->feedback = 0.5f;
    dt->level = 0.0f;
    dt->active = 0;
    dt->input_gate = 0.0f;
    dt->gate_release = 0.0f;
}

static float decay_process(DecayTail* dt, float input) {
    float abs_in = fabsf(input);
    
    /* Track input presence */
    if (abs_in > 0.001f) {
        dt->input_gate = 1.0f;
    } else {
        dt->input_gate *= 0.9999f;  /* very slow release */
    }
    
    /* Write into decay buffer with feedback */
    float read_sample = dt->buffer[dt->write_pos];
    dt->buffer[dt->write_pos] = input + read_sample * dt->feedback;
    dt->write_pos = (dt->write_pos + 1) % dt->length;
    
    /* When input fades, the recirculating buffer provides the tail */
    float tail_output = read_sample * dt->feedback;
    
    /* Blend: when input is present, pass through; when absent, use tail */
    float gate = clampf(dt->input_gate, 0.0f, 1.0f);
    float out = input + tail_output * (1.0f - gate * 0.5f);
    
    return out;
}

/* ── Tilt Filter ───────────────────────────────────────────────────── */

static void tilt_init(TiltFilter* t) {
    t->lp_state = 0.0f;
    t->hp_state = 0.0f;
    t->tilt = 0.0f;
}

static float tilt_process(TiltFilter* t, float input) {
    /* Simple tilt EQ: one-pole LP + HP crossfade
       tilt < 0 = darker (more LP), tilt > 0 = brighter (more HP) */
    float cutoff_freq = 800.0f;
    float coeff = one_pole_coeff(1.0f / (2.0f * (float)M_PI * cutoff_freq), SAMPLE_RATE);
    
    /* Lowpass */
    t->lp_state += coeff * (input - t->lp_state);
    float lp = t->lp_state;
    
    /* Highpass */
    float hp = input - lp;
    
    /* Tilt blend */
    float tilt = t->tilt;  /* -1..+1 */
    float lp_gain, hp_gain;
    
    if (tilt >= 0.0f) {
        /* Bright: boost highs, cut lows */
        lp_gain = 1.0f - tilt * 0.7f;
        hp_gain = 1.0f + tilt * 1.5f;
    } else {
        /* Dark: boost lows, cut highs */
        lp_gain = 1.0f + (-tilt) * 1.5f;
        hp_gain = 1.0f - (-tilt) * 0.7f;
    }
    
    return lp * lp_gain + hp * hp_gain;
}

/* ══════════════════════════════════════════════════════════════════════
   Public API
   ══════════════════════════════════════════════════════════════════════ */

void dissolver_init(Dissolver* d, float sample_rate) {
    memset(d, 0, sizeof(Dissolver));
    d->sample_rate = sample_rate;
    
    env_init(&d->env, sample_rate);
    granular_init(&d->granular);
    freeze_init(&d->freeze);
    decay_init(&d->decay);
    tilt_init(&d->tilt);
    
    /* Default parameter values */
    d->p_smoothing  = 0.5f;
    d->p_freeze     = 0.3f;
    d->p_grain_size = 0.5f;
    d->p_density    = 0.6f;
    d->p_evolution  = 0.4f;
    d->p_brightness = 0.5f;
    d->p_mix        = 0.7f;
    d->p_decay      = 0.6f;
    
    d->initialized = 1;
}

void dissolver_reset(Dissolver* d) {
    float sr = d->sample_rate;
    dissolver_init(d, sr);
}

void dissolver_set_parameter(Dissolver* d, int param_index, float value) {
    value = clampf(value, 0.0f, 1.0f);
    
    switch (param_index) {
        case 0: d->p_smoothing  = value; break;
        case 1: d->p_freeze     = value; break;
        case 2: d->p_grain_size = value; break;
        case 3: d->p_density    = value; break;
        case 4:
            d->p_evolution = value;
            d->freeze.evolution_rate = value * 2.0f;  /* 0..2 Hz LFO */
            break;
        case 5:
            d->p_brightness = value;
            d->tilt.tilt = (value - 0.5f) * 2.0f;  /* 0..1 → -1..+1 */
            break;
        case 6: d->p_mix = value; break;
        case 7:
            d->p_decay = value;
            d->decay.feedback = value * 0.85f;  /* cap at 0.85 to avoid runaway */
            d->decay.length = (int)(value * (float)(DECAY_BUF_SIZE - 1)) + SAMPLE_RATE / 4;
            if (d->decay.length > DECAY_BUF_SIZE) d->decay.length = DECAY_BUF_SIZE;
            break;
    }
}

void dissolver_process(Dissolver* d, const float* input, float* output, int frames) {
    if (!d->initialized) {
        memset(output, 0, frames * sizeof(float));
        return;
    }
    
    /* Update granular params (grain size & density may have changed) */
    granular_update_params(&d->granular, d->p_grain_size, d->p_density);
    d->freeze.freeze_amount = d->p_freeze;
    
    for (int i = 0; i < frames; i++) {
        float dry = input[i];
        float sample = dry;
        
        /* Stage 1: Transient suppression */
        sample = env_process(&d->env, sample, d->p_smoothing);
        
        /* Stage 2: Granular overlap-add (temporal smearing) */
        float granular_out = granular_process(&d->granular, sample);
        
        /* Stage 3: Spectral freeze blend */
        /* Compute a rough grain phase from the trigger counter */
        int hop = d->granular.grain_size / d->granular.overlap_count;
        if (hop < 1) hop = 1;
        float grain_phase = (float)(d->granular.samples_since_trigger) / (float)hop;
        grain_phase = clampf(grain_phase, 0.0f, 1.0f);
        
        float frozen_out = freeze_process(&d->freeze, granular_out, 
                                           grain_phase, d->granular.grain_size);
        
        /* Stage 4: Decay tail */
        float sustained = decay_process(&d->decay, frozen_out);
        
        /* Stage 5: Brightness / tilt EQ */
        float shaped = tilt_process(&d->tilt, sustained);
        
        /* Stage 6: Dry/Wet mix */
        output[i] = lerpf(dry, shaped, d->p_mix);
    }
}
