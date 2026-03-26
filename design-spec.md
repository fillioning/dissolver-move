# Dissolver — Design Spec

> **Status:** Pre-implementation
> **Plugin type:** `audio_fx`
> **Module ID:** `dissolver`
> **Last updated:** 2026-03-20

---

## What it is

An audio FX that turns any incoming sound into a pad. Stereo int16 audio enters, transients are detected and suppressed, the surviving harmonic content is captured into a granular overlap-add engine that smears temporal detail, a spectral freeze stage holds and slowly evolves the harmonic skeleton, and a decay tail sustains the pad after input stops. A tilt EQ shapes brightness. Dry/wet mix blends the result with the original signal. Two independent DSP instances process L and R channels separately.

## Sonic intent

**References:** GRM Tools Evolution, spectral freezing, granular time-stretching
**Philosophy:** "Dissolve the transients, keep the soul." Not a reverb — it erases attack envelopes and extracts the sustained harmonic essence, blending temporality into a living, breathing texture.
**Not:** Not a reverb. Not a delay. Not a granular looper. It does not preserve rhythm or transient structure — it actively destroys them.

---

## DSP architecture

**Core algorithm:** Granular overlap-add with Hann windowing, preceded by envelope-follower transient suppression, followed by spectral freeze crossfade and recirculating decay buffer.

**Voice architecture:** N/A (audio FX)

**Signal flow:**
```
Input (int16 stereo interleaved)
  → int16-to-float conversion
  → [L and R processed independently through:]
      → Envelope Follower (transient detection + gate)
      → Granular OLA Engine (temporal smearing)
      → Spectral Freeze Blend (harmonic hold + evolution)
      → Decay Tail (recirculating sustain after input stops)
      → Tilt / Brightness EQ
  → Dry/Wet Mix
  → float-to-int16 with saturation
  → Output (int16 stereo interleaved, in-place)
```

**Known DSP challenges:**
- Memory budget: two full instances with 8s decay buffers (~3MB total). May need to reduce if Move complains.
- Denormal risk on ARM (no FTZ) — every one-pole filter and feedback path needs denormal guards.
- Granular OLA normalization: Hann windows with proper hop size should give unity gain, but overlap count is user-variable — normalization must adapt.
- Evolution is currently a simple LFO buffer offset (±32 samples). May want per-grain pitch micro-shifting or spectral bin permutation for deeper GRM-style morphing in v2.
- Single-sample granular processing is CPU-heavy at high density (up to 16 simultaneous grains with Hann window per sample). May need block-based grain rendering optimization.

---

## Parameters

### Page 1 — Dissolver (root)
| # | Name | Key | Type | Range | Default | Notes |
|---|------|-----|------|-------|---------|-------|
| 1 | Smoothing | `smoothing` | float | 0.0–1.0 | 0.5 | Transient suppression strength. 0=none, 1=full kill |
| 2 | Freeze | `freeze` | float | 0.0–1.0 | 0.3 | Spectral hold vs. live update ratio |
| 3 | Grain Size | `grain_size` | float | 0.0–1.0 | 0.5 | OLA window: ~5ms (0) to ~93ms (1) at 44.1kHz |
| 4 | Density | `density` | float | 0.0–1.0 | 0.6 | Grain overlap: 2 (sparse) to 16 (thick) |
| 5 | Evolution | `evolution` | float | 0.0–1.0 | 0.4 | Frozen spectrum drift rate (0–2 Hz LFO) |
| 6 | Brightness | `brightness` | float | 0.0–1.0 | 0.5 | Tilt EQ: 0=dark, 0.5=neutral, 1=bright |
| 7 | Dry/Wet | `mix` | float | 0.0–1.0 | 0.7 | Blend original and dissolved output |
| 8 | Decay | `decay` | float | 0.0–1.0 | 0.6 | Pad tail sustain length (up to ~8s) |

### Page 2 — Advanced
| # | Name | Key | Type | Range | Default | Notes |
|---|------|-----|------|-------|---------|-------|
| 1 | Stereo Width | `stereo_width` | float | 0.0–1.0 | 0.5 | L/R decorrelation amount (grain phase offset) |
| 2 | Grain Jitter | `grain_jitter` | float | 0.0–1.0 | 0.0 | Random variation in grain start position |
| 3 | Evolution Depth | `evo_depth` | float | 0.0–1.0 | 0.5 | How far the spectral drift wanders |
| 4 | Attack Time | `attack_time` | float | 0.0–1.0 | 0.1 | Envelope follower attack speed |
| 5 | Release Time | `release_time` | float | 0.0–1.0 | 0.5 | Envelope follower release speed |
| 6 | Feedback Cap | `feedback_cap` | float | 0.0–1.0 | 0.85 | Maximum decay recirculation (runaway protection) |
| 7 | Tilt Freq | `tilt_freq` | float | 0.0–1.0 | 0.5 | Tilt EQ crossover frequency (200–4000 Hz) |
| 8 | Output Level | `output_level` | float | 0.0–1.0 | 0.8 | Final output gain (compensate for volume changes) |

---

## Open questions

- [x] Is 3MB total memory acceptable on Move, or should decay be capped at 4s? **→ 3MB is fine, keep 8s.**
- [ ] Evolution v2: per-grain pitch micro-shifting vs spectral bin permutation — defer to ear test
- [x] Stereo width implementation: grain phase offset, or independent grain trigger timing? **→ Grain phase offset.**
- [x] Should Freeze at 100% create a true infinite drone, or always allow very slow update? **→ Always allow slow update (never fully static).**

---

## Hardware constraints (Move)

- Block size: 128 frames at 44100 Hz (~2.9ms)
- Audio format: int16 stereo interleaved (in-place processing)
- No FTZ on ARM — guard against denormals in every filter/feedback path
- No heap allocation in render path
- No `printf` in render path
- Files on device must be owned by `ableton:users`

---

## Design conversation reference

> "I want a plugin that turns any incoming sound into a pad. It's not a reverb. It kills all the transients of the sound and only keeps the harmonic content and blends its temporality. Like a GRM tools Evolution, but for Move Everything."
>
> Name chosen: Dissolver
> Knob mapping agreed upon in conversation. Stereo: two independent instances (A). Decay: 8s max buffer (A). Second Advanced page added.
