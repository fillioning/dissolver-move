#include "dissolver.h"
#include <stdio.h>

/* ══════════════════════════════════════════════════════════════════════
   Schwung Module Entry Point

   This is the bridge between the Schwung framework and the
   Dissolver DSP engine. It implements the standard module callbacks
   that the framework expects.
   ══════════════════════════════════════════════════════════════════════ */

/* ── Module instance ───────────────────────────────────────────────── */

static Dissolver g_dissolver;

/* ── Framework callbacks ───────────────────────────────────────────── */

void module_init(float sample_rate) {
    dissolver_init(&g_dissolver, sample_rate);
    fprintf(stderr, "[Dissolver] Initialized at %.0f Hz\n", sample_rate);
}

void module_destroy(void) {
    fprintf(stderr, "[Dissolver] Destroyed\n");
}

void module_reset(void) {
    dissolver_reset(&g_dissolver);
}

void module_set_parameter(int index, float value) {
    dissolver_set_parameter(&g_dissolver, index, value);
}

float module_get_parameter(int index) {
    switch (index) {
        case 0: return g_dissolver.p_smoothing;
        case 1: return g_dissolver.p_freeze;
        case 2: return g_dissolver.p_grain_size;
        case 3: return g_dissolver.p_density;
        case 4: return g_dissolver.p_evolution;
        case 5: return g_dissolver.p_brightness;
        case 6: return g_dissolver.p_mix;
        case 7: return g_dissolver.p_decay;
        default: return 0.0f;
    }
}

void module_process(const float* input_l, const float* input_r,
                    float* output_l, float* output_r, int frames) {
    /* Process left channel through Dissolver */
    dissolver_process(&g_dissolver, input_l, output_l, frames);
    
    /* For stereo: process right channel identically 
       (a future enhancement could add stereo decorrelation) */
    dissolver_process(&g_dissolver, input_r, output_r, frames);
}

/* ── Exported symbol table for Schwung dynamic loading ───────────── */

typedef struct {
    void  (*init)(float sample_rate);
    void  (*destroy)(void);
    void  (*reset)(void);
    void  (*set_parameter)(int index, float value);
    float (*get_parameter)(int index);
    void  (*process)(const float*, const float*, float*, float*, int);
} ModuleVTable;

__attribute__((visibility("default")))
const ModuleVTable module_vtable = {
    .init          = module_init,
    .destroy       = module_destroy,
    .reset         = module_reset,
    .set_parameter = module_set_parameter,
    .get_parameter = module_get_parameter,
    .process       = module_process,
};

/* Some frameworks use a single exported init function instead */
__attribute__((visibility("default")))
void* module_create(void) {
    return (void*)&module_vtable;
}
