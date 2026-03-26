# Dissolver

**Spectral pad generator for [Ableton Move](https://www.ableton.com/move/) via [Schwung](https://github.com/charlesvestal/schwung)**

Dissolver turns any incoming audio into an evolving ambient pad. It captures the harmonic essence of the input through FFT analysis, discards phase information, and reconstructs the signal with evolution-controlled random phases through overlap-add synthesis. Feed it drums, vocals, guitars — anything becomes a breathing texture.

Not a reverb. Not a delay. It actively destroys transients and temporal structure, keeping only the spectral soul.

## Signal Flow

```
Input (stereo int16)
  → FFT analysis (4096-pt, magnitude only — phase discarded)
  → Stretch smoothing (temporal magnitude averaging)
  → Spectral gate (Dissolve — remove weak bins)
  → Spectral spread (log-frequency IIR smearing)
  → Spectral compressor (flatten dynamics)
  → Freeze blend (hold vs. live update)
  → Phase vocoder baseline + evolution-controlled random phase reconstruction
  → IFFT → 75% overlap-add (4 Hann-windowed frames)
  → LP-damped decay tail (up to 8s)
  → Tilt EQ
  → Dry/Wet mix
  → Output (stereo int16, in-place)
```

Two independent spectral engines process L and R channels with decorrelated random phase generators for natural stereo imaging.

## Parameters

### Page 1 — Dissolver

| Knob | Name | Description |
|------|------|-------------|
| 1 | Dissolve | Spectral gate — removes weak bins, keeps strong harmonics. Higher = cleaner, more ethereal |
| 2 | Freeze | Spectral hold vs. live update. At 100%, captures and sustains a snapshot (always breathing, never static) |
| 3 | Stretch | Temporal magnitude smoothing. Higher = longer pad blur, slower response to input changes |
| 4 | Spread | Log-frequency spectral smearing. Blurs spectral peaks into neighbors |
| 5 | Evolution | Phase coherence. 0% = frozen texture (phase vocoder). 100% = full random shimmer (PaulStretch) |
| 6 | Decay | LP-damped recirculating sustain tail (up to ~8s). Progressively darker with each recirculation |
| 7 | Tone | Tilt EQ. 0% = dark, 50% = flat, 100% = bright |
| 8 | Dry/Wet | Equal-power crossfade between original and dissolved signal |

### Page 2 — Advanced

| Knob | Name | Description |
|------|------|-------------|
| 1 | Stereo Width | L/R decorrelation. 0% = mono, 50% = natural, 100% = wide M/S boost |
| 2 | Compress | Spectral dynamics flattening. Higher = all frequency bins at similar level |
| 3 | Spread Depth | Number of spectral spread passes (1-4). More passes = thicker smearing |
| 4 | Attack Time | Reserved |
| 5 | Release Time | Reserved |
| 6 | Feedback Cap | Maximum decay recirculation coefficient. Safety cap for the decay tail |
| 7 | Tilt Freq | Tilt EQ crossover frequency (200-4000 Hz) |
| 8 | Output Level | Final output gain compensation |

## Build

Requires Docker for ARM64 cross-compilation.

```bash
./scripts/build.sh      # Docker ARM64 cross-compile → dist/dissolver/
./scripts/install.sh    # Deploy to move.local via SCP
```

## Credits

- Spectral algorithms derived from [PaulXStretch](https://github.com/paulnasca/paulstretch_cpp) (Nasca/Xenakios/Chappell)
- FFT: [PFFFT](https://bitbucket.org/jpommier/pffft/) (Julien Pommier)
- Built with [Schwung](https://github.com/charlesvestal/schwung) for Ableton Move

## License

GPL-3.0 — see [LICENSE](LICENSE)
