# Dissolver — Claude Code context

## What this is
Spectral smearing pad generator — kills transients, preserves harmonic content, blends temporality into evolving pads. Inspired by GRM Tools Evolution.

Plugin type: `audio_fx`
Module ID: `dissolver`
API: `audio_fx_api_v2_t`
Entry: `move_audio_fx_init_v2`
Language: C

Read `design-spec.md` for full design intent. This file is the compressed version.

---

## Sonic intent
GRM Tools Evolution for Move. Dissolve transients, keep the soul. Not a reverb, not a delay — it actively destroys attack envelopes and extracts the sustained harmonic essence into a living pad texture. Feed it drums, vocals, guitars — anything becomes a pad.

---

## DSP architecture
Stereo int16 input → float conversion → two independent Dissolver instances (L/R) → FFT analysis (4096-pt, magnitude only, discard phase) → Stretch smoothing (temporal magnitude averaging) → Spectral spread (log-freq IIR smearing) → Tonal/noise separation (transient erasure) → Spectral compressor → Freeze blend → Evolution-controlled phase reconstruction + IFFT → OLA output → LP-damped decay tail (up to 8s) → Tilt EQ → Dry/Wet Mix → int16 saturation → stereo interleaved output.

---

## Parameters

### Page 1 — Dissolver (root)
| # | Key | Name | Type | Range | Default |
|---|-----|------|------|-------|---------|
| 1 | `smoothing` | Smoothing | float | 0–1 | 0.5 |
| 2 | `freeze` | Freeze | float | 0–1 | 0.3 |
| 3 | `grain_size` | Grain Size | float | 0–1 | 0.5 |
| 4 | `density` | Density | float | 0–1 | 0.6 |
| 5 | `evolution` | Evolution | float | 0–1 | 0.4 |
| 6 | `brightness` | Brightness | float | 0–1 | 0.5 |
| 7 | `mix` | Dry/Wet | float | 0–1 | 0.7 |
| 8 | `decay` | Decay | float | 0–1 | 0.6 |

### Page 2 — Advanced
| # | Key | Name | Type | Range | Default |
|---|-----|------|------|-------|---------|
| 1 | `stereo_width` | Stereo Width | float | 0–1 | 0.5 |
| 2 | `grain_jitter` | Grain Jitter | float | 0–1 | 0.0 |
| 3 | `evo_depth` | Evolution Depth | float | 0–1 | 0.5 |
| 4 | `attack_time` | Attack Time | float | 0–1 | 0.1 |
| 5 | `release_time` | Release Time | float | 0–1 | 0.5 |
| 6 | `feedback_cap` | Feedback Cap | float | 0–1 | 0.85 |
| 7 | `tilt_freq` | Tilt Freq | float | 0–1 | 0.5 |
| 8 | `output_level` | Output Level | float | 0–1 | 0.8 |

---

## Resolved design decisions
- 3MB total memory (two instances with 8s decay) is acceptable on Move
- Stereo width via grain phase offset (not independent trigger timing)
- Freeze at 100% always allows slow update — never fully static, always breathing

## Open questions (resolve before implementing the relevant section)
- Evolution v2: per-grain pitch micro-shifting vs spectral bin permutation — defer to ear test

---

## Move hardware constraints (never violate)
- Sample rate: 44100 Hz (NOT 48000)
- Block size: 128 frames (~2.9ms)
- Audio: int16 stereo interleaved, in-place processing
- No heap allocation in render path
- No `printf` / logging in render path
- No FTZ on ARM — denormal guard required (add 1e-25f to feedback paths)
- Files on device must be owned by `ableton:users`
- Two Dissolver instances (L/R) — budget ~3MB total

---

## API constraints (audio_fx_api_v2)

- API struct: `audio_fx_api_v2_t`, entry symbol: `move_audio_fx_init_v2`
- `process_block(void *instance, int16_t *audio_inout, int frames)` — in-place stereo interleaved
- `set_param(void *instance, const char *key, const char *val)` — string keys, string values
- `get_param(void *instance, const char *key, char *buf, int buf_len)` — MUST return -1 for unknown keys
- `create_instance(const char *module_dir, const char *config_json)` — heap alloc here only
- `destroy_instance(void *instance)` — free here
- Host provides: `sample_rate=44100`, `frames_per_block=128`, `log()` function (not in render)

---

## Shadow UI integration
- `ui_hierarchy` lives in **module.json ONLY** — do NOT return it from get_param
- DSP implements: `chain_params` (JSON), `knob_N_name`, `knob_N_value`, `knob_N_adjust`
- Enum params: get_param returns name string, not index
- `get_param` returns -1 for unknown keys (breaks Master FX menu otherwise)
- State serialization via `state` key (get_param serializes, set_param deserializes)

---

## Repo map
- `src/dsp/dissolver.c` — Move API wrapper + params + process_block
- `src/dsp/dissolver_spectral.h` — Spectral DSP engine structs, constants, inline utils
- `src/dsp/dissolver_spectral.c` — Spectral DSP engine (FFT analysis, spread, tonal/noise, freeze, evolution phase synthesis, decay, tilt)
- `src/dsp/pffft.c` / `pffft.h` — PFFFT library (FFT implementation)
- `src/module.json` — module metadata + ui_hierarchy (keep in sync with root)
- `module.json` — root copy (for builds + desktop installer)
- `scripts/build.sh` — Docker ARM64 cross-compile
- `scripts/install.sh` — deploy to Move
- `Dockerfile` — build container
- `.github/workflows/release.yml` — CI release
- `release.json` — auto-updated by CI
- `files/` — OBSOLETE Claude.ai scaffold (wrong API, for reference only)

## Build & deploy
```bash
./scripts/build.sh          # Docker ARM64 cross-compile
./scripts/install.sh        # Deploy to move.local
```

## Release
Use `/move-release` when ready. Git tag `vX.Y.Z` must match `version` in module.json.
