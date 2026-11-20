# betterfx <span class="pd-tag">mac</span>

Better-sounding mixer effects. Same curves and knob feel as rekordbox otherwise; each part has its own switch.

<ModInfo name="betterfx" platform="mac" group="Browsing and playing" />

## What it changes

| Part | Option | rekordbox | With betterfx |
|---|---|---|---|
| **Color FX Filter** (the FLX4's filter knob) | `betterfx_filter` | The low-pass started at about 12 kHz with a +6 dB resonant bump there, moved in 256 steps, and full resonance peaked at +24 dB mid-sweep. | Sweeps smoothly and reaches its intended 25 kHz top; full resonance stays under +9 dB. Same knob law, dead zone and default sound. |
| **EQ, DJM-900NXS2 type** (rekordbox's default) | `betterfx_eq` | Knob moves smoothed over one audio buffer: with small buffers a fast cut thumped. | Same curves; knob moves smoothed over a fixed 5 ms, and a knob change is never dropped. |
| **Isolator** | `betterfx_eq` | A 3.3 ms ramp: bass kills clicked. | Same crossovers and true kill; about 8 ms gain smoothing. |
| **Beat FX Reverb** | `betterfx_reverb` | A 1960s-style network (8 combs, 2 allpasses): metallic ring and flutter. | An 8-line feedback delay network with diffusion, slow modulation and frequency-dependent decay. Decay time, room size and wet level follow rekordbox's own knobs, so the dry/wet mix behaves as before. |
| **Echo / Pitch Echo** | `betterfx_echo` | Every small delay-time change (a tempo fader move) dipped the echo. | Applied once the tempo has been steady for 250 ms. Beat changes are immediate, as before. |
| **Roll / Reverse Roll / Slip Roll** | `betterfx_roll` | A 6.7 ms fade-out/fade-in dip on every repeat. | Each repeat starts exactly on the beat with a short crossfade. |

The reverse-engineering notes behind it (rekordbox 7.2.19's filter, EQ and Beat FX) are in the repository's
`docs/dsp/`.

## Options

| Option | Default | What it does |
|---|---|---|
| `betterfx` | `1` | `0` switches the mod off (restart). |
| `betterfx_filter` | `1` | The Color FX Filter (the FLX4 filter knob). |
| `betterfx_eq` | `1` | The DJM-900NXS2 EQ and the isolator. |
| `betterfx_reverb` | `1` | The Beat FX Reverb (the new reverb sound). |
| `betterfx_echo` | `1` | Echo and Pitch Echo: no dips while the tempo changes. |
| `betterfx_roll` | `1` | Roll, Reverse Roll and Slip Roll: no dip on every repeat. |

## Windows

macOS only for now: the Windows address tables don't have these effect classes yet.

## Related

[audiomem](./audiomem) · [flx4](./flx4) · [Performance and dropouts](/guides/performance)
