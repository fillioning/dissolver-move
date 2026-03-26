# Dissolver

**Spectral Smearing Pad Generator for Ableton Move**

Dissolves transients and smears harmonic content into evolving pads. Inspired by GRM Tools Evolution. Feed it anything — drums, vocals, guitars, field recordings — and it extracts the sustained harmonic essence, freezing and blending it into a living, breathing texture.

## Signal Flow

```
Input → Transient Suppression → Granular OLA → Spectral Freeze → Decay Tail → Tilt EQ → Mix → Output
```

## Knob Mapping

| Knob | Parameter   | Range        | Description                                    |
|------|-------------|--------------|------------------------------------------------|
| 1    | Smoothing   | 0–100%       | Transient kill strength                        |
| 2    | Freeze      | 0–100%       | Spectral hold vs. live input                   |
| 3    | Grain Size  | 5ms–85ms     | Temporal resolution of smearing                |
| 4    | Density     | 2–16 grains  | Overlap factor / pad thickness                 |
| 5    | Evolution   | 0–2 Hz       | Spectral drift rate                            |
| 6    | Brightness  | Dark–Bright  | Spectral tilt EQ                               |
| 7    | Dry/Wet     | 0–100%       | Mix blend                                      |
| 8    | Decay       | Short–Long   | Sustain tail after input stops                 |

## Build & Deploy

```bash
chmod +x scripts/build.sh scripts/install.sh
./scripts/build.sh      # Cross-compile for ARM64
./scripts/install.sh    # Deploy to Move via SSH
```

## Architecture

- **Transient Suppression**: Asymmetric envelope follower with derivative-based onset detection. Positive envelope deltas trigger proportional gain reduction.
- **Granular OLA**: Circular capture buffer → Hann-windowed overlap-add with configurable grain size (256–4096 samples) and density (2–16 simultaneous grains).
- **Spectral Freeze**: Blends between live granular output and a slowly-updating frozen buffer, with LFO-driven spectral drift for evolution.
- **Decay Tail**: Circular delay buffer with feedback for sustain after input stops.
- **Tilt EQ**: One-pole lowpass/highpass split with gain crossfade for brightness control.

## CLAUDE.md

- Module ID: `dissolver`
- Component type: `audio_fx`
- Output: `dissolver.so` (ARM64)
- 8 parameters, indices 0–7 matching module.json order
- Stereo processing: identical L/R chains (no decorrelation yet)
- Sample rate: expects 48kHz from Move framework
- All DSP is in `src/dissolver.c`, entry point in `src/plugin.c`
