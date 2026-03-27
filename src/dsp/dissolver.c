/**
 * Dissolver — Schwung audio FX module
 * Spectral smearing pad generator — kills transients, preserves harmonic content,
 * blends temporality into evolving pads. Inspired by GRM Tools Evolution.
 *
 * Author: fillioning
 * License: GPL-3.0 (spectral algorithms derived from PaulXStretch)
 *
 * API: audio_fx_api_v2 (in-place stereo, int16 interleaved, 44100Hz, 128 frames/block)
 */

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#include <stdint.h>

#include "dissolver_spectral.h"

/* ── host_api_v1_t ──────────────────────────────────────────────────── */

typedef int (*move_mod_emit_value_fn)(void *ctx,
                                      const char *source_id,
                                      const char *target,
                                      const char *param,
                                      float signal, float depth, float offset,
                                      int bipolar, int enabled);
typedef void (*move_mod_clear_source_fn)(void *ctx, const char *source_id);

typedef struct host_api_v1 {
    uint32_t api_version;
    int sample_rate;
    int frames_per_block;
    uint8_t *mapped_memory;
    int audio_out_offset;
    int audio_in_offset;
    void (*log)(const char *msg);
    int (*midi_send_internal)(const uint8_t *msg, int len);
    int (*midi_send_external)(const uint8_t *msg, int len);
    int (*get_clock_status)(void);
    move_mod_emit_value_fn mod_emit_value;
    move_mod_clear_source_fn mod_clear_source;
    void *mod_host_ctx;
} host_api_v1_t;

/* ── audio_fx_api_v2_t ──────────────────────────────────────────────── */

typedef struct audio_fx_api_v2 {
    uint32_t api_version;
    void* (*create_instance)(const char *module_dir, const char *config_json);
    void  (*destroy_instance)(void *instance);
    void  (*process_block)(void *instance, int16_t *audio_inout, int frames);
    void  (*set_param)(void *instance, const char *key, const char *val);
    int   (*get_param)(void *instance, const char *key, char *buf, int buf_len);
    void  (*on_midi)(void *instance, const uint8_t *msg, int len, int source);
} audio_fx_api_v2_t;

static const host_api_v1_t *g_host = NULL;

/* ── Knob definitions ───────────────────────────────────────────────── */

typedef struct {
    const char *key;
    const char *label;
    float min, max, step;
    int is_enum;
    int page;           /* 0 = root (Dissolver), 1 = Advanced */
} knob_def_t;

/* Page 1: Dissolver — knobs 1-8 */
/* Page 2: Advanced — knobs 1-8 (accessed via jog navigation) */
#define NUM_KNOBS_P1 8
#define NUM_KNOBS_P2 8

static const knob_def_t KNOB_MAP_P1[8] = {
    { "smoothing",   "Dissolve",    0, 1, 0.01f, 0, 0 },  /* tonal/noise separation */
    { "freeze",      "Freeze",      0, 1, 0.01f, 0, 0 },  /* spectral freeze */
    { "grain_size",  "Stretch",     0, 1, 0.01f, 0, 0 },  /* stretch ratio */
    { "density",     "Spread",      0, 1, 0.01f, 0, 0 },  /* spectral smearing bandwidth */
    { "evolution",   "Evolution",   0, 1, 0.01f, 0, 0 },  /* phase drift speed */
    { "decay",       "Decay",       0, 1, 0.01f, 0, 0 },
    { "brightness",  "Tone",        0, 1, 0.01f, 0, 0 },  /* tilt EQ */
    { "mix",         "Dry/Wet",     0, 1, 0.01f, 0, 0 },
};

static const knob_def_t KNOB_MAP_P2[8] = {
    { "noise_mix",     "Noise Mix",      0, 1, 0.01f, 0, 1 },
    { "noise_tone",    "Noise Tone",     0, 1, 0.01f, 0, 1 },
    { "stereo_width",  "Stereo Width",   0, 1, 0.01f, 0, 1 },
    { "grain_jitter",  "Compress",       0, 1, 0.01f, 0, 1 },  /* spectral compressor */
    { "evo_depth",     "Spread Depth",   0, 1, 0.01f, 0, 1 },  /* spread iterations */
    { "feedback_cap",  "Feedback Cap",   0, 1, 0.01f, 0, 1 },
    { "tilt_freq",     "Tilt Freq",      0, 1, 0.01f, 0, 1 },
    { "output_level",  "Output Level",   0, 1, 0.01f, 0, 1 },
};

/* ── Helpers ─────────────────────────────────────────────────────────── */
/* clampf, lerpf are in dissolver_spectral.h */

/* ── Instance state ──────────────────────────────────────────────────── */

typedef struct {
    /* Two independent DSP channels */
    DissolverChannel ch_l;
    DissolverChannel ch_r;

    /* Page tracking for knob overlay */
    int current_page;  /* 0 = Dissolver (root), 1 = Advanced */

    /* Page 1 params */
    float smoothing;
    float freeze;
    float grain_size;
    float density;
    float evolution;
    float brightness;
    float mix;
    float decay;

    /* Page 2 params (knobs) */
    float noise_mix;
    float noise_tone;
    float stereo_width;
    float grain_jitter;
    float evo_depth;
    float feedback_cap;
    float tilt_freq;
    float output_level;

    /* Menu-only params */
    float attack_time;
    float release_time;
} dissolver_instance_t;

/* Lookup float param pointer by key string */
static float *param_ptr_by_key(dissolver_instance_t *inst, const char *key) {
    if (strcmp(key, "smoothing") == 0)      return &inst->smoothing;
    if (strcmp(key, "freeze") == 0)         return &inst->freeze;
    if (strcmp(key, "grain_size") == 0)     return &inst->grain_size;
    if (strcmp(key, "density") == 0)        return &inst->density;
    if (strcmp(key, "evolution") == 0)      return &inst->evolution;
    if (strcmp(key, "brightness") == 0)     return &inst->brightness;
    if (strcmp(key, "mix") == 0)            return &inst->mix;
    if (strcmp(key, "decay") == 0)          return &inst->decay;
    if (strcmp(key, "noise_mix") == 0)      return &inst->noise_mix;
    if (strcmp(key, "noise_tone") == 0)     return &inst->noise_tone;
    if (strcmp(key, "stereo_width") == 0)   return &inst->stereo_width;
    if (strcmp(key, "grain_jitter") == 0)   return &inst->grain_jitter;
    if (strcmp(key, "evo_depth") == 0)      return &inst->evo_depth;
    if (strcmp(key, "feedback_cap") == 0)   return &inst->feedback_cap;
    if (strcmp(key, "tilt_freq") == 0)      return &inst->tilt_freq;
    if (strcmp(key, "output_level") == 0)   return &inst->output_level;
    if (strcmp(key, "attack_time") == 0)    return &inst->attack_time;
    if (strcmp(key, "release_time") == 0)   return &inst->release_time;
    return NULL;
}

/* Find knob def by index (1-indexed) on the given page */
static const knob_def_t *find_knob_by_index(int knob_num, int page) {
    if (knob_num < 1 || knob_num > 8) return NULL;
    return (page == 1) ? &KNOB_MAP_P2[knob_num - 1] : &KNOB_MAP_P1[knob_num - 1];
}

/* Find knob def by key across both pages */
static const knob_def_t *find_knob_by_key(const char *key) {
    for (int i = 0; i < 8; i++) {
        if (KNOB_MAP_P1[i].key && strcmp(key, KNOB_MAP_P1[i].key) == 0) return &KNOB_MAP_P1[i];
        if (KNOB_MAP_P2[i].key && strcmp(key, KNOB_MAP_P2[i].key) == 0) return &KNOB_MAP_P2[i];
    }
    return NULL;
}

/* ── Lifecycle ───────────────────────────────────────────────────────── */

static void *create_instance(const char *module_dir, const char *json_defaults) {
    dissolver_instance_t *inst = calloc(1, sizeof(dissolver_instance_t));
    if (!inst) return NULL;

    if (dissolver_channel_init(&inst->ch_l) != 0 ||
        dissolver_channel_init(&inst->ch_r) != 0) {
        dissolver_channel_destroy(&inst->ch_l);
        dissolver_channel_destroy(&inst->ch_r);
        free(inst);
        return NULL;
    }

    /* Decorrelate R channel: different RNG seed → different random phases → true stereo.
     * Without this, L and R produce identical phase patterns → mono wet signal. */
    inst->ch_r.spectral.rng = 987654321u;
    for (int i = 0; i < SPEC_HALF_SIZE; i++) {
        uint32_t *st = &inst->ch_r.spectral.rng;
        *st ^= *st << 13;
        *st ^= *st >> 17;
        *st ^= *st << 5;
        inst->ch_r.spectral.phase_accum[i] = (float)(*st & 0xFFFF)
            * (2.0f * 3.14159265f / 65536.0f);
    }

    inst->current_page = 0;

    /* Page 1 defaults */
    inst->smoothing   = 0.5f;    /* Dissolve 50% */
    inst->freeze      = 0.0f;    /* Freeze off */
    inst->grain_size  = 0.3f;    /* Stretch 30% */
    inst->density     = 1.0f;    /* Spread 100% */
    inst->evolution   = 0.3f;    /* Evolution 30% */
    inst->brightness  = 0.5f;    /* Tone 50% */
    inst->mix         = 1.0f;    /* Dry/Wet 100% */
    inst->decay       = 1.0f;    /* Decay 100% */

    /* Page 2 defaults */
    inst->noise_mix     = 0.0f;
    inst->noise_tone    = 0.0f;    /* white noise by default */
    inst->stereo_width  = 0.5f;
    inst->grain_jitter  = 0.2f;
    inst->evo_depth     = 1.0f;    /* Spread Depth 100% */
    inst->feedback_cap  = 0.85f;
    inst->tilt_freq     = 0.5f;
    inst->output_level  = 0.8f;

    /* Menu-only defaults */
    inst->attack_time   = 0.1f;
    inst->release_time  = 0.5f;

    if (g_host && g_host->log) g_host->log("[dissolver] instance created");
    return inst;
}

static void destroy_instance(void *instance) {
    dissolver_instance_t *inst = (dissolver_instance_t *)instance;
    if (inst) {
        dissolver_channel_destroy(&inst->ch_l);
        dissolver_channel_destroy(&inst->ch_r);
        free(inst);
    }
    if (g_host && g_host->log) g_host->log("[dissolver] instance destroyed");
}

/* ── Parameters ──────────────────────────────────────────────────────── */

static void set_param(void *instance, const char *key, const char *val) {
    dissolver_instance_t *inst = (dissolver_instance_t *)instance;
    if (!inst || !key || !val) return;

    /* Page navigation: Schwung sends _level when user switches pages */
    if (strcmp(key, "_level") == 0) {
        if (strcmp(val, "Advanced") == 0) {
            inst->current_page = 1;
        } else {
            inst->current_page = 0;  /* root, Dissolver, or any other → page 0 */
        }
        return;
    }

    /* knob_N_adjust: Shadow UI sends when user turns knob N */
    if (strncmp(key, "knob_", 5) == 0 && strstr(key, "_adjust")) {
        int knob_num = atoi(key + 5);
        const knob_def_t *k = find_knob_by_index(knob_num, inst->current_page);
        if (k && k->key) {
            float *p = param_ptr_by_key(inst, k->key);
            if (p) {
                float delta = atof(val);
                *p = clampf(*p + delta * k->step, k->min, k->max);
            }
        }
        return;
    }

    /* Regular param set by key */
    float *p = param_ptr_by_key(inst, key);
    if (p) {
        *p = clampf(atof(val), 0.0f, 1.0f);
        return;
    }

    /* State restore */
    if (strcmp(key, "state") == 0) {
        sscanf(val,
            "sm=%f;fr=%f;gs=%f;dn=%f;ev=%f;br=%f;mx=%f;dc=%f;"
            "sw=%f;gj=%f;ed=%f;at=%f;rt=%f;fc=%f;tf=%f;ol=%f;nm=%f;nt=%f",
            &inst->smoothing, &inst->freeze, &inst->grain_size, &inst->density,
            &inst->evolution, &inst->brightness, &inst->mix, &inst->decay,
            &inst->stereo_width, &inst->grain_jitter, &inst->evo_depth, &inst->attack_time,
            &inst->release_time, &inst->feedback_cap, &inst->tilt_freq, &inst->output_level,
            &inst->noise_mix, &inst->noise_tone);
    }
}

static int get_param(void *instance, const char *key, char *buf, int buf_len) {
    dissolver_instance_t *inst = (dissolver_instance_t *)instance;
    if (!inst || !key || !buf || buf_len < 1) return -1;

    /* Module name */
    if (strcmp(key, "name") == 0)
        return snprintf(buf, buf_len, "Dissolver");

    /* chain_params — Shadow UI uses this to discover parameters */
    if (strcmp(key, "chain_params") == 0) {
        return snprintf(buf, buf_len,
            "["
            "{\"key\":\"smoothing\",\"name\":\"Dissolve\",\"type\":\"float\",\"min\":0,\"max\":1,\"step\":0.01},"
            "{\"key\":\"freeze\",\"name\":\"Freeze\",\"type\":\"float\",\"min\":0,\"max\":1,\"step\":0.01},"
            "{\"key\":\"grain_size\",\"name\":\"Stretch\",\"type\":\"float\",\"min\":0,\"max\":1,\"step\":0.01},"
            "{\"key\":\"density\",\"name\":\"Spread\",\"type\":\"float\",\"min\":0,\"max\":1,\"step\":0.01},"
            "{\"key\":\"evolution\",\"name\":\"Evolution\",\"type\":\"float\",\"min\":0,\"max\":1,\"step\":0.01},"
            "{\"key\":\"decay\",\"name\":\"Decay\",\"type\":\"float\",\"min\":0,\"max\":1,\"step\":0.01},"
            "{\"key\":\"brightness\",\"name\":\"Tone\",\"type\":\"float\",\"min\":0,\"max\":1,\"step\":0.01},"
            "{\"key\":\"mix\",\"name\":\"Dry/Wet\",\"type\":\"float\",\"min\":0,\"max\":1,\"step\":0.01},"
            "{\"key\":\"noise_mix\",\"name\":\"Noise Mix\",\"type\":\"float\",\"min\":0,\"max\":1,\"step\":0.01},"
            "{\"key\":\"noise_tone\",\"name\":\"Noise Tone\",\"type\":\"float\",\"min\":0,\"max\":1,\"step\":0.01},"
            "{\"key\":\"stereo_width\",\"name\":\"Stereo Width\",\"type\":\"float\",\"min\":0,\"max\":1,\"step\":0.01},"
            "{\"key\":\"grain_jitter\",\"name\":\"Compress\",\"type\":\"float\",\"min\":0,\"max\":1,\"step\":0.01},"
            "{\"key\":\"evo_depth\",\"name\":\"Spread Depth\",\"type\":\"float\",\"min\":0,\"max\":1,\"step\":0.01},"
            "{\"key\":\"feedback_cap\",\"name\":\"Feedback Cap\",\"type\":\"float\",\"min\":0,\"max\":1,\"step\":0.01},"
            "{\"key\":\"tilt_freq\",\"name\":\"Tilt Freq\",\"type\":\"float\",\"min\":0,\"max\":1,\"step\":0.01},"
            "{\"key\":\"output_level\",\"name\":\"Output Level\",\"type\":\"float\",\"min\":0,\"max\":1,\"step\":0.01},"
            "{\"key\":\"attack_time\",\"name\":\"Attack Time\",\"type\":\"float\",\"min\":0,\"max\":1,\"step\":0.01},"
            "{\"key\":\"release_time\",\"name\":\"Release Time\",\"type\":\"float\",\"min\":0,\"max\":1,\"step\":0.01}"
            "]");
    }

    /* Regular param values */
    float *p = param_ptr_by_key(inst, key);
    if (p) return snprintf(buf, buf_len, "%.4f", (double)*p);

    /* knob_N_name: Shadow UI popup label */
    if (strncmp(key, "knob_", 5) == 0 && strstr(key, "_name")) {
        int idx = atoi(key + 5) - 1;
        const knob_def_t *map = (inst->current_page == 1) ? KNOB_MAP_P2 : KNOB_MAP_P1;
        if (idx >= 0 && idx < 8 && map[idx].label)
            return snprintf(buf, buf_len, "%s", map[idx].label);
        return -1;
    }

    /* knob_N_value: Shadow UI popup value */
    if (strncmp(key, "knob_", 5) == 0 && strstr(key, "_value")) {
        int idx = atoi(key + 5) - 1;
        const knob_def_t *map = (inst->current_page == 1) ? KNOB_MAP_P2 : KNOB_MAP_P1;
        if (idx >= 0 && idx < 8 && map[idx].key) {
            float *kp = param_ptr_by_key(inst, map[idx].key);
            if (kp) return snprintf(buf, buf_len, "%d%%", (int)(*kp * 100));
        }
        return -1;
    }

    /* State serialization */
    if (strcmp(key, "state") == 0) {
        return snprintf(buf, buf_len,
            "sm=%.6f;fr=%.6f;gs=%.6f;dn=%.6f;ev=%.6f;br=%.6f;mx=%.6f;dc=%.6f;"
            "sw=%.6f;gj=%.6f;ed=%.6f;at=%.6f;rt=%.6f;fc=%.6f;tf=%.6f;ol=%.6f;"
            "nm=%.6f;nt=%.6f",
            inst->smoothing, inst->freeze, inst->grain_size, inst->density,
            inst->evolution, inst->brightness, inst->mix, inst->decay,
            inst->stereo_width, inst->grain_jitter, inst->evo_depth, inst->attack_time,
            inst->release_time, inst->feedback_cap, inst->tilt_freq, inst->output_level,
            inst->noise_mix, inst->noise_tone);
    }

    /* CRITICAL: return -1 for unknown keys */
    return -1;
}

/* ── Audio processing ────────────────────────────────────────────────── */

static void process_block(void *instance, int16_t *buf, int frames) {
    dissolver_instance_t *inst = (dissolver_instance_t *)instance;
    if (!inst || frames <= 0) return;
    if (frames > 128) frames = 128;

    /* Deinterleave to float */
    float in_l[128], in_r[128];
    float out_l[128], out_r[128];

    for (int i = 0; i < frames; i++) {
        in_l[i] = (float)buf[i * 2]     / 32768.0f;
        in_r[i] = (float)buf[i * 2 + 1] / 32768.0f;
    }

    /* Build param struct for DSP engine */
    DissolverParams dp;
    dp.smoothing     = inst->smoothing;       /* → tonal/noise separation */
    dp.freeze        = inst->freeze;          /* → spectral freeze */
    dp.stretch       = inst->grain_size;      /* → stretch ratio (reused key) */
    dp.spread        = inst->density;         /* → spectral spread (reused key) */
    dp.evolution     = inst->evolution;        /* → phase variation */
    dp.brightness    = inst->brightness;      /* → tilt EQ */
    dp.decay_amt     = inst->decay;           /* → decay tail */
    dp.feedback_cap  = inst->feedback_cap;
    dp.compress      = inst->grain_jitter;    /* → spectral compressor (reused key) */
    dp.spread_depth  = inst->evo_depth;       /* → spread iterations (reused key) */
    dp.tilt_freq_norm = inst->tilt_freq;
    dp.output_level  = inst->output_level;
    dp.noise_mix     = inst->noise_mix;
    dp.noise_tone    = inst->noise_tone;
    dp.noise_envelope = 0.0f;                 /* filled per-channel in dissolver_channel_process */
    dp.attack_time   = inst->attack_time;
    dp.release_time  = inst->release_time;

    /* Process L and R independently through spectral DSP pipeline */
    dissolver_channel_process(&inst->ch_l, in_l, out_l, frames, &dp);
    dissolver_channel_process(&inst->ch_r, in_r, out_r, frames, &dp);

    /* Stereo width as crossfeed blend:
     * 0 = mono (L=R=mid), 1 = full stereo (independent L/R engines).
     * L and R already have decorrelated RNG seeds → at width=1 they're
     * fully independent spectral textures. Crossfeed narrows gradually. */
    float width = inst->stereo_width;
    float crossfeed = 1.0f - width;  /* 0 at full stereo, 1 at mono */

    /* Equal-power dry/wet crossfade */
    float mix_angle = inst->mix * 1.5707963f;
    float dry_gain = cosf(mix_angle);
    float wet_gain = sinf(mix_angle);
    float out_gain = inst->output_level;

    for (int i = 0; i < frames; i++) {
        float wet_l = out_l[i];
        float wet_r = out_r[i];

        /* Crossfeed: blend each channel toward mid by crossfeed amount */
        if (crossfeed > 0.001f) {
            float mid = (wet_l + wet_r) * 0.5f;
            wet_l = wet_l + crossfeed * (mid - wet_l);
            wet_r = wet_r + crossfeed * (mid - wet_r);
        }

        float final_l = (dry_gain * in_l[i] + wet_gain * wet_l) * out_gain;
        float final_r = (dry_gain * in_r[i] + wet_gain * wet_r) * out_gain;

        /* Clamp and write back */
        int32_t il = (int32_t)(final_l * 32767.0f);
        int32_t ir = (int32_t)(final_r * 32767.0f);
        if (il >  32767) il =  32767; if (il < -32768) il = -32768;
        if (ir >  32767) ir =  32767; if (ir < -32768) ir = -32768;
        buf[i * 2]     = (int16_t)il;
        buf[i * 2 + 1] = (int16_t)ir;
    }
}

/* ── API v2 export ───────────────────────────────────────────────────── */

static audio_fx_api_v2_t g_api = {
    .api_version      = 2,
    .create_instance  = create_instance,
    .destroy_instance = destroy_instance,
    .process_block    = process_block,
    .set_param        = set_param,
    .get_param        = get_param,
    .on_midi          = NULL,
};

__attribute__((visibility("default")))
audio_fx_api_v2_t* move_audio_fx_init_v2(const host_api_v1_t *host) {
    g_host = host;
    if (host && host->log) host->log("[dissolver] loaded — spectral smearing pad generator");
    return &g_api;
}
