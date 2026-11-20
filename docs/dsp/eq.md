# rekordbox 7.2.19 (macOS arm64) channel EQ / isolator DSP: reverse-engineering notes

Addresses are VAs in the thin arm64 binary `rb64`. My helper scripts and disassembly are in
`$SP/eqtools/` (rd.py reads tables, xref.py finds adrp references, blx.py finds `bl` callers, model.py
models the filters from the decoded formulas). Other agents also write to `$SP/dsp/`, so only this file lives there.

## 0. Verdict, ranked by audible impact

The DSP is careful work: double-precision biquads, prewarped bilinear designs, coefficients recomputed for the
actual sample rate, and a DF-I-equivalent state hand-over whenever coefficients change. Nothing is badly wrong.
The issues below are ranked by how much you would hear them.

| # | Issue | Applies to | Impact |
|---|---|---|---|
| 1 | Both EQ types (DJM-900NXS and DJM-900NXS2) cut at most -26 dB. Only ISOLATOR mode really kills (gain 0). This matches the DJM spec (+6/-26 for EQ, +6/-inf for the isolator), so it is a design choice, not a bug. With all three EQ knobs at full cut, 200-300 Hz is only about -12 dB down. | EQ modes | High if you expect kills |
| 2 | The NXS2 type (the default) smooths only within one audio buffer. The knob moves linearly from the old value to the new one across the buffer, and coefficients update every 32 samples (64 above 64 kHz), so smoothing time = buffer length. With small buffers (64-128 samples) a fast cut changes the shelf by up to 26 dB in about 1.5-3 ms, which can click or thump on bass. With large buffers it is smooth. | NXS2 (default) | Medium |
| 3 | The DJM-900NXS type (classic) has three problems. (a) A rate limiter moves the gain at most one table step every round(fs/3000) samples, so centre to full cut takes about 80 ms and kills feel sluggish (0x102ab5edc..). (b) The 13 kHz HI bell's upper skirt is squashed by bilinear warping: a HI kill is only -8.6 dB at 20 kHz at 44.1k (-12.5 at 48k) where the analog prototype gives -21 dB. (c) The LOW band is a bell at 70 Hz, so a LOW kill is only -12 dB at 20 Hz and -16 dB at 30 Hz. (c) may be faithful to the hardware; (b) is a digital artifact. | NXS (non-default) | Medium (only if selected) |
| 4 | Lost-update race in the NXS2 type. The UI thread calls setParameter directly (it writes the value and sets a per-band dirty flag, with no lock or queue). The audio thread reads the target at the start of the buffer and clears the dirty flag near the end. A knob change that lands in that window is dropped until the knob moves again. The window is roughly the EQ's CPU time per buffer, so the odds are around 0.1% per update (guess). It matters for one-shot jumps such as a double-click reset to centre or a MIDI kill button. | NXS2 | Low (rare, but sticky) |
| 5 | The isolator gain ramp is linear and only 3.3 ms long (GainSmoother). A full bass kill or restore in 3.3 ms is close to a hard gate on 50 Hz content, so a slight click is possible. 5-10 ms would be cleaner. | ISOLATOR | Low |
| 6 | The isolator's three bands are LP284 + HP284·LP4752 + HP4752, with no allpass compensation of the low band for the upper crossover (or of the high band for the lower one). The flat sum still stays within 0.04 dB. Phase is an LR allpass even at centre, and the isolator is never bypassed. | ISOLATOR | Inaudible |
| 7 | Minor: the classic Equalizer starts at index 239 (+0.035 dB) instead of 238. Isolator::setParameter does not clamp its index, but the caller clamps 0..255. No denormal handling, but the state is double and Apple Silicon handles subnormals at full speed, so it does not matter. | various | None |

**A better implementation** would keep the shapes (they emulate the DJM hardware) and fix the dynamics:
- Smooth each band's gain in dB (or the knob) with a fixed time constant that does not depend on buffer size (about 5-10 ms, one-pole or linear).
- Recompute coefficients every 8-16 samples (or interpolate them per sample). Keep the DF-I-equivalent state hand-over rekordbox already uses.
- Fix the dirty-flag race: atomically exchange the flag at the start of the buffer, or compare the target at the end.
- For the classic type, design the 13 kHz bell so it is not cramped near Nyquist: a matched / Vicanek-style biquad, or 2x oversampling of the HI band only.
- Optionally offer "full kill at 0" for the EQ types: map the last few percent of knob travel to -inf, or crossfade to an isolator-style band kill.

The isolator needs nothing important. It is already a Linkwitz-Riley 4th-order 3-band split at 284 / 4752 Hz with a true kill.

## 1. How the EQ type is chosen (Preferences > Mixer)

- UI: `ControllerEqualizerMode` (ctor 0x100501538; buttonClicked 0x100502260).
  - "EQ/ISOLATOR" has toggles **EQ** (+0x110, mode 1) and **ISOLATOR** (+0x118, mode 0).
  - "EQ Type" has toggles **DJM-900NXS** (+0x128, type 0) and **DJM-900NXS2** (+0x130, type 1).
  - Strings are at 0x104f856d4 "EQ", 0x104f856d7 "ISOLATOR", 0x104f856e0 "EQ/ISOLATOR", 0x104f856ec "DJM-900NXS", 0x104f79ad4 "DJM-900NXS2", 0x104f856f7 "EQ Type".
- Settings: keys `_kMixerEQMode` 0x1056cdf98 and `_kMixerEQType` 0x1056cdfa0 (juce::String objects). Defaults: `_kMixerEQModeDefaultValue` 0x1037bbc30 = 1 (EQ) and `_kMixerEQTypeDefaultValue` 0x1037bbc34 = 1 (DJM-900NXS2).
- `djplay::SettingIF::setEqualizerMode` 0x1015cf410 and `setEqualizerType` 0x1015cf5b0 store the setting, then call `IDjMixerControl::selectEqIso` (vtable +0x38) with
  `EEq_Type_t = (mode==1) ? (type ? 3 : 1) : (type ? 2 : 0)` (csel/csinc at 0x1015cf4b8..0x1015cf4d8).
  `loadPreferredSettings_Player` (0x1015cfc10) applies the saved setting at startup.
- `DjMixerUnit::selectEqIso` 0x102aa8748 calls `InputChannelGraph::selectEqIso` 0x102a919a0, which stores to `EqIso+0x48`.
- `EqIso::processBlock` 0x102ab4594 switches on mode (+0x4c) through the jump table at 0x1038e17e5 = {0,4,0,8}:
  **0 and 2 go to Isolator (+0x28), 1 goes to Equalizer (+0x20, "DJM-900NXS"), 3 goes to EqualizerNXS2 (+0x30, "DJM-900NXS2")**.
  On a change it fades out to 0 with a 3.3 ms GainSmoother (+0x38), calls the new EQ's initialize() (state cleared), then fades back in.
  The two fades add up to a ~7 ms dip, only when the preference changes.
  `EqIso::setParameter` 0x102ab4758 forwards each knob to all three EQ objects, so they always hold current values.
- **Default = EQ mode + DJM-900NXS2 type, which runs EqualizerNXS2.** In ISOLATOR mode the EQ type setting is ignored (same Isolator object).
- `MainComponent::deviceUpdateIsolatorMode` 0x10097f7f0 is unrelated. It only sends the DDJ hardware knob mode (getDDJConNobControl).

## 2. Knob to parameter path

`EqControlComp::sliderValueChanged` 0x1000d27c4 normalises the slider to 0..1, then calls
`DjEngineIF::setEqualizer(ch, band, v)` 0x100914f84. That makes a direct virtual call from the message
thread (no queue) to `DjMixerUnit::setEqPosition` 0x102aa85b8, then `InputChannelGraph::setEqPosition` 0x102a91818.
There the value is clamped to [0,1] and multiplied by 255.0 (float, not rounded) before `EqIso::setParameter(band, k)`.
Bands: 0 = HI, 1 = MID, 2 = LOW. Each EQ class then maps k in 0..255 differently (below). MIDI devices arrive through
EqControlComp::eventAbsoluteValueChanged and end up in the same path.

## 3. Shared filter primitive: class Filter (biquad, double precision)

- `Filter::initialize(fs, f0, arg)` 0x102aad008 stores: +8 fs, +0x10 T=1/fs, +0x18 T², +0x20 **w0 = 2·fs·tan(π f0/fs) (prewarped)**,
  +0x28 w0², +0x30 arg, +0x38 w0·arg. tan is the libm stub 0x1034d2f1c, and __exp10 is 0x1034d1bc0 (from dyld_info fixups).
- Coefficients are doubles at +0x40.. in the order b0, b1, a1, b2, a2, from a plain bilinear transform s = (2/T)(1-z⁻¹)/(1+z⁻¹).
- `make_eq_peaking(q, dB)` 0x102aad7fc uses A = 10^(dB/20).
  - Boost: H = (s² + A·w0/q·s + w0²)/(s² + w0/q·s + w0²).
  - Cut: H = (s² + w0/q·s + w0²)/(s² + w0/(qA)·s + w0²).
  - The gain at f0 is exactly A.
- `make_eq_high_shelving(dB)` 0x102aad6b4, with d = arg:
  - Boost: H = A(s² + d·w0·s + w0²)/(s² + d√A·w0·s + A·w0²).
  - Cut: H = (A s² + d√A·w0·s + w0²)/(s² + d·w0·s + w0²).
  - The zero pair and pole pair are √A apart in frequency, each with damping ζ = d/2.
- `make_eq_low_shelving(dB)` 0x102aad910:
  - Cut: H = (s² + d√A·w0·s + A·w0²)/(s² + d·w0·s + w0²).
  - Boost: H = (s² + d·w0·s + w0²)/(s² + d·w0/√A·s + w0²/A).
- `make_lpf2_butter` 0x102aad298 and `make_hpf2_butter` 0x102aad58c: prewarped when bool = 1; damping constant 2√2 at 0x102aad348, i.e. Q = 0.7071.
- `filter2` (double 0x102aadd24, float 0x102aadf28, AudioBuffer 0x102aae314):
  - **Transposed Direct Form II with double state.** Float I/O is converted to double.
  - It keeps x[n-1], x[n-2], y[n-1], y[n-2] per channel (+0xa0/+0xb0/+0xc0/+0xd0).
  - When coefficients change (flag +0xf8, set by make_*), it rebuilds the TDF-II state from that history:
    s1 = b1x1 + b2x2 - a1y1 - a2y2 and s2 = b2x1 - a2y1. A coefficient switch therefore behaves like a Direct-Form-I switch, the most benign case for time-varying biquads. Good practice.
  - The AudioBuffer variant uses std::valarray. It only reallocates on a channel-count mismatch, so normally nothing is allocated.

## 4. Equalizer ("DJM-900NXS" EQ type, mode 1)

ctor 0x102ab5ad0, initialize 0x102ab63a0, processBlock(AB) 0x102ab5ce0, processBlock(f*) 0x102ab6024, setParameter 0x102ab64cc.
- Topology: **three cascaded peaking biquads**:
  - HI f0 = 13000 Hz (0x40c9640000000000)
  - MID 1000 Hz
  - LOW 70 Hz
- Per band: index i = EQ_KNOB_TABLE[round(k)] in 0..487.
  - Gain: dB = EQ_HI_GAIN_TABLE[i] (the MID gain table is identical).
  - Q = EQ_{HI,MID,LO}_LOWQ_TABLE[i]. LO = HI, MID = 1.375 × HI.
- Tables:
  - EQ_KNOB_TABLE 0x1038e17f0: 256 × u16. Knob 127..134 all map to index 238, an 8-step flat detent.
  - The gain/Q tables are 488 floats each: EQ_HI_LOWQ 0x1038e19f0, EQ_MID_LOWQ 0x1038e2190, EQ_LO_LOWQ 0x1038e2930, EQ_HI_GAIN 0x1038e30d0, EQ_MID_GAIN 0x1038e3870.
  - Pointer variables: Equalizer::mp_* at 0x1053b2700.., EqualizerNXS2::mp_* at 0x1053b2728...
- Gain law: -26.375 dB at k=0, 0 dB in the detent, +6.003 dB at k=255. Along the way: k=32 is -11.1, k=64 is -4.3, k=96 is -1.6, k=192 is +2.7 dB.
- The Q tables are built so the broad section is constant:
  - For cuts the pole Q = q·A = 0.0802 (HI/LO) or 0.1103 (MID), and only the zero Q narrows (1.67 / 2.30 at kill).
  - For boosts the zero Q = q/A = 0.0802 and the pole Q widens to 0.16.
  - So each "bell" is very broad. The HI band has a pole near 160 kHz, which explains the Nyquist cramping.
- Smoothing (**rate limiter in table steps**):
  - Each band keeps its current index (u16 at +0x2c/+0x2e/+0x30) and moves it ±1 toward the target every round(fs/3000) samples (15 at 44.1k, 16 at 48k). The constant 1/3000 is at 0x102ab5da8, and the remainder counters at +0x50/+0x54/+0x58 carry across buffers.
  - make_eq_peaking runs at every step. The step size is up to ~0.46 dB near kill, so there is no audible zipper.
  - **Centre to kill takes 238 steps, about 80 ms; a full sweep takes 162 ms.**
- Modeled responses at 44.1k (dB):

  | Setting | 20 Hz | 30 Hz | 70 Hz | 100 Hz | 200 Hz | 1 kHz | 5 kHz | 13 kHz | 16 kHz | 18 kHz | 20 kHz |
  |---|---|---|---|---|---|---|---|---|---|---|---|
  | LOW kill | -11.9 | -16.0 | -26.4 | -22.4 | -13.9 | -2.5 | | | | | |
  | MID kill | | | | -2.6 | -6.6 | -26.4 | -6.3 | | | | |
  | HI kill | | | | | | -1.6 | -11.7 | -26.4 | -20.5 | -15.1 | **-8.6** |

  The analog prototype for HI kill gives -24.7 at 16k and -21.3 at 20k; at 48k the 20 kHz value is -12.5, at 96k -20.0. HI +6 dB sags to +4.5 dB at 20k.

## 5. EqualizerNXS2 ("DJM-900NXS2" EQ type, mode 3, **default**)

ctor 0x102ab65c8, initialize 0x102ab66ec, processBlock(AB) 0x102ab6b14, processBlock(f*) 0x102ab7070, setParameter 0x102ab777c.
- Topology:
  - **HI: high shelf at 2200 Hz**, d = 2.0 (ζ = 1, two coincident real pole pairs).
  - **MID: peaking at 1000 Hz**, with the same knob/Q/gain tables as the classic MID.
  - **LOW: low shelf at 250 Hz**, d = 1.5 (ζ = 0.75).
- Gain law for HI and LOW (closed form, 0x102ab6750..0x102ab67f8), with k clamped to 0..255:
  - k < 126: dB = -20·(1.15 - k/126)² + 0.45. That gives -26.0 at 0, -15.6 at k=32, -7.8 at k=64, -2.6 at k=96.
  - 126 ≤ k ≤ 129: 0 dB (detent).
  - k > 129: dB = (k-129)/21. That gives +6.0 at 255.
  - The slope is continuous at 126 (0.0476 dB per step on both sides).
- Smoothing:
  - setParameter stores k and sets a dirty byte (+0x28/+0x34/+0x40).
  - processBlock interpolates k linearly from the previous target (+0x24/+0x30/+0x3c) to the new one **across the current buffer**.
  - Coefficients are redesigned every 32 samples (+0x60 = 32, or 64 if fs > 64000; 0x102ab6704). The last 32..63 samples use the exact target.
  - Smoothing time therefore equals the buffer length. A buffer of 32 samples or fewer jumps straight to the target.
- Processing happens in double: the float input is copied into calloc'd double buffers (+0x68/+0x70) and back.
- **Race:** the target is read at 0x102ab6b60 and the dirty flags are cleared at 0x102ab6ebc / 0x6f30 / 0x6ff4 after nearly the whole buffer has been filtered.
  setParameter comes straight from the UI thread (`DjEngineIF::setEqualizer` 0x100914fe8 makes the virtual call directly), so an update in that window is lost.
- Modeled responses at 44.1k (dB):

  | Setting | 20 Hz | 50 Hz | 100 Hz | 150 Hz | 200 Hz | 300 Hz | 2 kHz | 5 kHz | 10 kHz | 20 kHz |
  |---|---|---|---|---|---|---|---|---|---|---|
  | LOW kill | -25.8 | -23.4 | -15.5 | -9.5 | -5.7 | -2.1 | | | | |
  | HI kill | | | | | | | -4.9 | -14.2 | -21.8 | -25.9 |
  | All kill | -25.9 | -24.2 | | | -12.3 | -12.2 | | -20.5 | | -26 |

  HI kill has no Nyquist problem, because the shelf maps s→∞ to z = -1 exactly. MID is the same as the classic type. In "all kill" there is a hole around 200-300 Hz between the low shelf and the mid bell.

## 6. Isolator (mode 0/2)

ctor 0x102ab8a50, initialize 0x102ab9268, processBlock(AB) 0x102ab93c8, processBlock(f*) 0x102ab96e8,
applyFilter 0x102ab9828, setParameter 0x102ab9958 (rounds k to int, no clamp).
- Topology: **3-band Linkwitz-Riley 4th order (two cascaded Butterworth 2nd-order biquads, prewarped)**.
  The crossovers are **284 Hz** (0x4071c00000000000) and **4752 Hz** (0x40b2900000000000).
  - HI = HP4752², filters +0x30, +0x38.
  - MID = HP284²·LP4752², filters +0x40, +0x48, +0x50, +0x58.
  - LOW = LP284², filters +0x60, +0x68.
  - Each band goes into its own AudioBuffer, gets a gain ramp, and the three are summed (0x102ab97d8).
- No allpass compensation. The modeled flat sum is within **0.04 dB** at 44.1k and 48k (worst near 300-500 Hz), so it does not matter.
- Gain: `ISOLATOR_GAIN_TABLE` 0x1038e402c, 256 linear-amplitude floats, pointer `Isolator::mpCurveAd` 0x1053b2740.
  - **k=0 gives 0.0 (a true kill)**. k=1..9 is quadratic (0.0006 … 0.0486), then the gain rises linearly to 1.0 at k=124.
  - k=124..131 is 1.0 (detent). Above that it rises linearly to 1.995 (+6 dB) at k=253..255.
  - In dB: k=16 is -18.8, k=32 is -12.2, k=64 is -5.9, k=192 is +3.5.
- Smoothing: one GainSmoother per band. Each is a linear ramp over round(fs·3.3 ms) samples (146 at 44.1k), retargeted when the gain changes
  (GainSmoother ctor 0x1028e0400, setTargetGain 0x1028e065c, applyGainRamp 0x1028e1418). The crossover filters never change.
- Modeled kills (dB):

  | Kill | 20 Hz | 50 Hz | 100 Hz | 200 Hz | 284 Hz | 400 Hz | 600 Hz | 1 kHz | 2 kHz | 3 kHz | 4752 Hz | 6 kHz | 8 kHz | 10 kHz | 13 kHz | 20 kHz |
  |---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
  | LOW | -92 | -60 | -36 | -14 | -6 | -2 | | | | | | | | | | |
  | MID | | | -0.1 | -1.9 | -6 | -14 | -26 | -43 | -31 | -18 | -6 | -2.7 | | | | |
  | HI | | | | | | | | | -0.2 | -1.2 | -6 | -11.6 | -21.6 | -31 | -46 | -103 |

## 7. Sample rate, precision, denormals

- Every design uses w0 = 2fs·tan(πf0/fs), and initialize() runs from onSamplingrateChanged and onBufferSizeChanged for all three classes. So coefficients are correct at 44.1, 48 and 96 kHz.
- The only real bilinear artifact is the classic type's very broad 13 kHz bell, whose upper skirt is cramped. The shelves (NXS2) and the LR crossovers are fine.
- State and coefficients are double. There is no FTZ code, which does not matter on Apple Silicon or with double state.
- I did not check the Windows build. It is presumably the same source, but x64 denormals would be the only place it could matter.

## 8. Could a mod fix it? (feasibility)

- EqIso calls the active EQ **virtually**: `blr` at 0x102ab464c, 0x102ab470c and 0x102ab4718, through vtable slot +0x10.
  hooks.def HOOK() only patches `bl`/`b` sites, so the DSP cannot be swapped through that existing mechanism. The options are:
  1. **Vtable slot replacement (cleanest).** Patch `vtable for EqualizerNXS2` at 0x105351c10:
     - slot +0x10 = processBlock(AudioBuffer&,int,int) 0x102ab6b14
     - slot +0x18 = processBlock(float*,float*,int) 0x102ab7070
     - Equalizer's vtable is at 0x105351ba0 (+0x10 = 0x102ab5ce0) and Isolator's at 0x105351cb0 (+0x10 = 0x102ab93c8).
     - The binary is thin arm64 without PAC. The vtables sit in __DATA_CONST, so the dylib would mprotect the page writable, store its pointer, and protect it again. This is a new kind of patch for the framework; resolve the slot by symbol name to stay update-proof.
     - The replacement reads the knob targets from the object: NXS2 floats at +0x20/+0x2c/+0x38 (0..255); Equalizer floats at +0x20/+0x24/+0x28; Isolator ints at +0x20/+0x24/+0x28.
     - It reads fs (double) at +0x8, buffer size at +0x10 and channels at +0x14 (AbstractMixerComponent).
     - It keeps its own per-instance state in a map keyed by `this`: biquad state per channel, smoothed gains, and last fs. Reset the state when fs changes or when EqIso switches mode; a mode switch can be detected because the EqIso fades and calls initialize().
     - That gives time-constant smoothing that does not depend on buffer size and removes the dirty-flag race (just read the float every buffer).
  2. **Cheap gain-law tweaks through existing `bl` HOOKs.** In `EqualizerNXS2::processBlock(AB)` there are 2 `bl Filter::make_eq_high_shelving`, 2 `bl make_eq_low_shelving` and 2 `bl make_eq_peaking`; initialize() has 1 of each. Hooking those lets a mod remap the dB argument, for example "full kill": below k≈8 slide -26 to -80 dB. That does not touch smoothing.
     - For the classic type, the 6 `bl make_eq_peaking` sites in Equalizer::processBlock(AB) plus 3 in initialize() could swap in a better 13 kHz design.
  3. To fix only the race, hook `EqIso::setParameter`. Its vtable slot is +0x20 and it is reached by `blr` from InputChannelGraph::setEqPosition 0x102a91858, so this also needs a vtable patch. Hooking is not strictly necessary, because a vtable-replaced processBlock simply ignores the dirty flags.
- Rough cost: per channel, 3 biquads × 2 channels with coefficients recomputed every 16 samples (tan + exp10 → cache tan(w0) per fs; only exp10/sqrt per update). That is negligible.

## 9. Uncertainties

- Which audio buffer size rekordbox uses (it decides how NXS2 smoothing behaves). Not checked.
- The race probability is a guess (EQ CPU time ÷ buffer period).
- Whether the DJM-900NXS hardware's LOW band is also a bell (that is, whether the sub-bass leak in point 3c is intentional emulation): not verifiable from the binary.
- The isolator crossover frequencies and gain table are taken straight from the code. I did not compare them with the hardware.
