# rekordbox 7.2.19 (macOS arm64) Color FX "Filter" DSP — reverse-engineering notes

(work in progress; updated as analysis proceeds)

## Symbols / addresses
- ColorFxFilterDjm900NXS2: ctor 0x1029d1fc4, allocateBufferIfNeeded 0x1029d2260, updateFilterCoef 0x1029d2304,
  samplingRateChanged 0x1029d25e0, processSampleNumChanged 0x1029d263c, processEffectConcrete 0x1029d2658..0x1029d30d0,
  onCalledMakeSelected 0x1029d30d0, onCalledTurnOn 0x1029d3248, onCalledTurnOff 0x1029d3258, calculateMixRatio 0x1029d33b8
- ColorFxFilterBase: ctor 0x1029d15e0, processEffectConcrete 0x1029d1964, FcTableReadingHelper::getFcFromTable 0x1029d1ba8
- InstFxHPF::updateParameter 0x1029d1cb0, InstFxLPF::updateParameter 0x1029d1ea4
- FilterCore: processEffectConcrete 0x1029d4724, handleFilterProcessing 0x1029d47ac
- IirFilter: handleFilterCore1st 0x1028bebe8.., handleFilterCore2nd 0x1028bf174.., calcCoefLpf2ndOrder 0x1028bfbc0, calcCoefHpf2ndOrder 0x1028bfb1c
- Tables (124 x f32 each):
  - ColorFxFilterManipulandumCutoffFreqConversionTable::HPF_FC_vs_MANIPULANDUM_TABLE 0x1038db6ec: 61 Hz -> 8951 Hz (rising)
  - ColorFxFilterManipulandumCutoffFreqConversionTable::LPF_FC_vs_MANIPULANDUM_TABLE 0x1038db8dc: 12000 Hz -> 93 Hz (falling)
  - ColorFxCrushFilterCutoffTable::HPF_FC_vs_MANIPULANDUM_TABLE 0x1038db4fc: 20 Hz -> 4129 Hz (pure exponential, ~4.4%/step)

## KEY FINDING 1: the channel CFX "Filter" is ColorFxFilterDjm900NXS2 and does NOT use the cutoff tables
- RekordboxDjColorFxLayer::createEffectBase calls ColorFxFilterDjm900NXS2 ctor (bl at 0x102a64520).
- The 124-entry tables (ColorFxFilterManipulandumCutoffFreqConversionTable) are used only by InstFxLPF/InstFxHPF
  (ColorFxFilterBase subclasses), which are created by RekordboxDJMergeFxLayer / RekordboxDjPadFxLayer (Merge FX / Pad FX)
  and the Beat FX echo/ping-pong. ColorFxCrush::IfxHPF uses ColorFxCrushFilterCutoffTable.
- NXS2 filter cutoff is computed by formula in updateFilterCoef (0x1029d2304), knob quantised to an int 0..255.

## Signal flow of ColorFxFilterDjm900NXS2::processEffectConcrete (0x1029d2658)
- float in L/R copied to 4 double buffers (0x388/0x390 = LPF path, 0x398/0x3a0 = HPF path)  -> PARALLEL LPF and HPF, not series.
- knob param (obj+0x260, 0..1) -> round(x*255) -> SmoothShiftInteger at obj+0x360 (setValue 0x10297d58c).
  second param (obj+0x2e0, the CFX PARAMETER knob = resonance) -> round(x*255) -> SmoothShiftInteger obj+0x368.
- SmoothShiftInteger = integer slew limiter: 1 step per N samples, N = (int)(timeMs*0.001*fs) (0x10297d210, const 0.001 = 0x3a83126f).
  timeMs = ColorFx::COLOR_MANIPULANDUM_SMOOTHING_TIME_MS @0x1038e0764 = 0.3333 ms -> N = 14 samples @44.1k, 16 @48k.
  => knob value glides 1/255 per 14 samples (full 0->255 sweep takes ~81 ms @44.1k). Coefficients recomputed after every step
  (block is split at step boundaries, updateFilterCoef call at 0x1029d28d0). So coefficient update rate ~3.1 kHz while moving.
- Each sub-block: Filter::filter2(double) on both paths (0x1029d29f0 LPF obj+0x378, 0x1029d2a0c HPF obj+0x370).
- Three gains, each linearly ramped per sample toward its target with slope 100/fs per sample (obj+0x3b8 = 100/fs set in ctor
  0x1029d21f4 -> 10 ms for a 0->1 change): dry (obj+0x3b0), LPF (obj+0x3a8), HPF (obj+0x3ac).
  out = dry*in + (gL*lpf + gH*hpf)  (sum in double, then float add)
- Targets (p = current smoothed knob int, 0..255), only when effect is ON (obj+0x3b4, set by onCalledTurnOn 0x1029d3248):
    p <= 109        : LPF 1, dry 0, HPF 0
    110..125        : dry = (p-110)/16, LPF = 1-dry          (crossfade, const 1/16 = 0x3d800000)
    126..130        : dry 1 (dead zone, 5 of 256 steps ~ 2%)
    131..146        : HPF = (p-130)/16, dry = 1-HPF
    >= 147          : HPF 1
  OFF: dry 1, LPF 0, HPF 0 (ramped 10 ms) -> turning off is click-free; filters keep running (state warm).
- At centre / off: out = in*1.0 + float(0*lpf+0*hpf) = bit-exact dry. Transparent.

## Cutoff / Q mapping (updateFilterCoef 0x1029d2304; constants decoded)
- LPF side p<=125: fcLPF = 100 + 25000*(p/126)^3 Hz  (1/126=0x3c020821, 25000/126=198.41=0x434669a7, 100=0x42c80000); HPF parked at 20 Hz.
- HPF side p>=131: fcHPF = 20 + 8200*((p-130)/126)^3 Hz (8200=0x46002000); LPF parked at 25100 Hz (double 0x40d8830000000000).
- dead zone 126..130: LPF 25100, HPF 20.
- p=255 -> HPF analog fc 8026 Hz; p=0 -> LPF 100 Hz.  Cubic (not log) curve.
- Resonance: d1 passed to make_lpf2/make_hpf2 is 1/Q (verified from coefficient formula, see below).
    r = PARAMETER knob int 0..255: r<=127: 1/Q = 0.5 + 3.5*(1-r/128)^2 ; r>127: 1/Q = 0.5 - (r-128)*0.003662 (=-15/4096, 0xbb700000)
    r=0 -> 1/Q=4 (Q 0.25); r=128 (centre, default) -> 1/Q=0.5 (Q=2, +6.3 dB peak); r=255 -> 1/Q=0.035 (Q~28.6, ~+29 dB peak)
- HPF low-cutoff resonance floor: if fcHPF < 249.9 Hz: y=(fc-20)/230 (35.652=8200/230), 1/Q >= min(2, 4(1-y)^3+0.03);
  i.e. Q<=0.5 just past centre, normal Q only above ~137 Hz (at default reso). Prevents sub-bass boom when engaging HPF.
- Gain compensation g (d2, same value for both filters): k = (1/Q < 0.5) ? 2/Q : 1 ; d = |p-64|/128 (LPF half) or |p-192|/128 (HPF half);
  g = (0.5+d)*k + 0.5 - d.  At default reso g=1 (no compensation). At max reso g = k = 0.07 (-23 dB) at the ends of each half,
  but only 0.5k+0.5 (~-5.4 dB) at mid-sweep (p=64 / p=192) -> resonance much louder mid-sweep (deliberate "character"?).

## Filter (generic biquad class) — 0x102aac6a8..
- Filter::initialize(fs, prewarpF, x) 0x102aad008: stores fs, T=1/fs, T^2. NXS2 passes prewarp 0 -> no prewarp.
- make_lpf2(double fc, double invQ, double gain) 0x102aad1fc and make_hpf2(...) 0x102aad4ec:
  bilinear transform with K=2/T and NO frequency pre-warping:
    B = (2*pi*fc*T)^2, A = 4*pi*fc*invQ*T, n = 1/(4+A+B)
    LPF: b0=b2=gain*B*n, b1=2*b0 ; HPF: b0=b2=4*gain*n, b1=-2*b0 ; a1=(2B-8)n, a2=(4-A+B)n
  (the (double,double,bool) overloads 0x102aad0d0/0x102aad3bc DO prewarp with tan(); NXS2 uses the unwarped ones)
- Filter::filter2(double) 0x102aadd24: Transposed Direct Form II in double precision; when coefficients changed (flag +0xf8)
  the TDF-II state is rebuilt from stored x[n-1],x[n-2],y[n-1],y[n-2] with the NEW coefficients (= DF-I semantics on
  coefficient change -> no TDF-II state-mismatch transients). Good design.

## Defaults / plumbing
- PARAMETER knob default 0.5 (RbxCfxControlBehavior ctor stores 0x3f000000 at +0x100, 0x100b98cb8; getCfxParameterKnobValue 0x100b9a21c)
  -> r = 128 -> 1/Q = 0.5 -> Q = 2 (+6.3 dB resonant peak) is the default sound. The FLX4 has no parameter knob.
- Knob centre 0.5 -> round(127.5) = 128 (ties-to-even via +1.5*2^52 trick at 0x1029d2824) -> inside dead zone 126..130.
- RekordboxDjColorFxLayer::createEffectBase 0x102a644c0: case Filter -> new ColorFxFilterDjm900NXS2(fs=(int)sampleRate, blockSize),
  then setParameter(0, layer+0xe8) and setParameter(1, layer+0xec). Filter gets NO setSubsonicHPF (other CFX get 30 Hz).
- Smart CFX (djplay::SmartCFX): in RbxCfxControlBehavior::setCfxKnobValue (0x100b99380) it only branches on
  isSmartCfxEnabled/getSmartCfxSelect when |knob-0.5| > 1e-4 to log Analytics events (preset 0..7). No separate filter DSP found there.

## Simulated response (exact coefficient formulas, 44.1 kHz; script $SP/dsp_filter/sim.py)
default reso (Q=2):
  p   analogFc  realFc(warped)  -3dB      peak
  0     100        100          148 Hz    +6.3 dB @ 94 Hz      (fully left)
  64   3376       3313         4.8 kHz   +6.3 dB @ 3.1 kHz
  96  11157       9427        12.2 kHz   +6.3 dB @ 9.0 kHz
  109 16285      12063        14.7 kHz   +6.3 dB @ 11.6 kHz   (last pure-LPF step)
  118 (50% dry) -> +2.5 dB @ 12.5 kHz, -3 dB @ 14.5 kHz
  125 (94% dry) -> +0.2 dB @ 12.7 kHz
  146 HPF 37 Hz, Q forced 0.5;  160 HPF 131 Hz (+5 dB @144);  192 HPF 997 Hz (+6.3 @1.06k); 255 HPF 8026 analog -> 7291 real, -3 dB 5.2 kHz
max reso (r=255, Q~28.6): peak +6 dB at the ends of each half but +23.7 dB at mid-sweep (p=64 / 192) because of the
  position-dependent gain compensation.

## Step size (cutoff staircase), script $SP/dsp_filter/steps.py
- Knob is quantised to 256 ints (126 per side). Real-cutoff jump per step: LPF 1.0-1.34 semitones in 140-900 Hz, 0.8 @2.7k,
  0.35 @10k; HPF 1.5-1.6 semitones in 40-130 Hz, 1.2 @280 Hz, 0.85 @900 Hz. Bottom of LPF (p 0..10) barely moves (100->112 Hz).
- Knob travel per octave is reasonably even (cubic+offset is near-log in the middle), so the curve itself is OK.

## Other filter-like effects (brief)
- ColorFxSweep::updateFilterCoef 0x102a48cf0: same unwarped Filter::make_lpf2/make_hpf2 (LPF max 20000 Hz -> ~13.4 kHz real
  @44.1k; HPF 60 Hz base), exp() mapping; process 0x102a491c4 calls Filter::clear() on both filters (0x102a492ec/0x102a492f4)
  under a flag (+0x280) -> state reset, possible click (condition not fully traced).
- ColorFxCrush::IfxHPF and InstFxLPF/HPF (Merge/Pad FX, Beat FX echo) use ColorFxFilterBase + FilterCore + IirFilter:
  FcTableReadingHelper::getFcFromTable 0x1029d1ba8 = table[v>>1] (v<4 -> [0], v>247 -> [count-1]), NO interpolation;
  IirFilter::calcCoef* 0x1028bfa4c.. ARE prewarped (tan), Butterworth Q=0.7071 (0x3fe6a09e667f3bcd), float coefficients and
  float DF-I state (FilterCore::handleFilterProcessing 0x1029d47ac), coefficients computed once per block -> stepped per block.
  2nd-order + 1st-order sections. Not the channel filter.
- ColorFxNoise: own updateFilterCoef 0x102a0a184 (not analysed in depth).

## Channel plumbing
- DjMixerUnit::setChannelFx 0x102aa8960 -> InputChannelGraph::setEffectUnit 0x102a91608 (per-channel graph: Trim, EQ/Iso,
  effect slots via addEffectUnit(EffectPosition_id), fader...). Exact position of the CFX slot in the graph not traced
  (expected post-EQ, pre-fader like a DJM; UNVERIFIED).
- Each channel gets its own ColorFxFilterDjm900NXS2 instance through RekordboxDjColorFxLayer (one layer per channel, guess).
- vtable __ZTV23ColorFxFilterDjm900NXS2 address point 0x105341660: +0xc0 samplingRateChanged, +0xc8 processSampleNumChanged,
  +0xd0 processEffectConcrete, +0xd8 calculateMixRatio (empty), +0xe0 onCalledMakeSelected, +0xf0 onCalledTurnOn, +0xf8 onCalledTurnOff.
- EffectBase fields: +0xa0 sample rate (int), +0xa4 block size.

## Minor bugs / robustness
- samplingRateChanged (0x1029d25e0) re-inits the two Filters (clears state) and coefficients, but NOT the SmoothShiftIntegers'
  rate (their onSamplingRateChanged isn't called) nor the 100/fs gain-ramp slope at +0x3b8 (set only in ctor 0x1029d21f4)
  -> slightly wrong slew/ramp times if the device rate differs from construction rate. Inaudible-ish.
- processEffectConcrete returns 1 (unprocessed -> dry) if n > allocated buffer (+0x380) (0x1029d2684): filter silently drops
  out for a block if the block size grows before processSampleNumChanged runs. Edge case.
- Gain compensation (d2) makes max-resonance peaks reach +23.7 dB mid-sweep -> will slam the limiter / clip. Default reso OK.
- Fully-left LPF has +6.3 dB bump at 94 Hz (Q=2 by default): DJM-like character but a loudness/headroom jump on bass-heavy tracks.

## Verdict (ranked by audible impact)
1. No bilinear pre-warping in Filter::make_lpf2/make_hpf2(double,double,double) (0x102aad1fc/0x102aad4ec). The LPF's top end
   is compressed: designed 16-25 kHz becomes 12-14.9 kHz real at 44.1k (15.6k at 48k, ~21k at 96k -> sound depends on the
   device rate). Leaving the dead zone toward LPF therefore cross-fades (p 110..125) into an already ~12-14.7 kHz resonant
   (+6 dB @11.6-12.6k) LPF instead of sweeping in smoothly from above hearing; full LPF begins at p=109 with fc 12 kHz.
   Audible as an immediate "dulling + 12 kHz presence bump" on first touch. HPF top (8026 -> 7291 Hz) is barely affected.
2. 8-bit knob quantisation -> cutoff staircase of up to ~1.3-1.6 semitones per step in the bass/low-mid region. Fast sweeps are
   fine (glide 1 step/14 samples, coefs updated every step); slow sweeps on sustained/resonant material step audibly.
   (The FLX4 itself is 14-bit capable; rekordbox throws that away at round(x*255).)
3. Resonance/gain design: default Q=2 (+6.3 dB) on both sides incl. 94 Hz boom at full LPF; with PARAMETER up, peaks to
   +24 dB mid-sweep (position-dependent compensation) -> clipping/limiter pumping. Partly intended "DJM character".
4. Centre handling is GOOD: parallel LPF/HPF, both always running, dry/LPF/HPF gains ramped per sample (10 ms), dead zone
   126..130 bit-exact dry, crossfades of 16 steps each side, no state reset when crossing centre or switching off -> no clicks.
5. Numerics are GOOD: double-precision TDF-II, state rebuilt DF-I style on coefficient change (no TDF-II modulation transients),
   stable at 20 Hz and at 25.1 kHz analog (bilinear maps it inside Nyquist). Denormals irrelevant (double, arm64).
6. Minor: smoother/ramp not updated on sample-rate change; block-size overrun bypass.

## Best-sounding implementation (recommendation)
- TPT/ZDF state-variable filter (Zavalishin/Simper "SVF"), g = tan(pi*fc/fs) (exact pre-warp, clamp fc <= ~0.45 fs),
  k = 1/Q; per-sample smoothed log-cutoff (one-pole ~5-10 ms or linear ramp in log2 Hz) driven by the continuous knob value
  (not 8-bit). Keep rekordbox's good parts: parallel LP and HP paths always running, dead zone, per-sample dry/wet ramps.
- 12 dB/oct matches the current (and DJM-900NXS2-style) sound; optional 24 dB/oct (two cascaded SVFs with Q split, or a
  ZDF ladder) for a steeper "club mixer" filter. Log mapping LPF ~20 kHz -> ~80 Hz, HPF ~20 Hz -> ~10 kHz.
- Resonance: keep Q~1.4-2 default; for high resonance add soft saturation (tanh on the band-pass state in the loop) and
  position-independent gain compensation so peaks stay < ~+9 dB; never self-oscillate unless asked.

## Mod feasibility
- Minimal fix (pre-warp only): HOOK the two call sites in ColorFxFilterDjm900NXS2::updateFilterCoef
  (__ZN23ColorFxFilterDjm900NXS216updateFilterCoefEv): `bl Filter::make_hpf2(ddd)` at 0x1029d24f0 (count 1, kind -1) and the
  tail call `b Filter::make_lpf2(ddd)` at 0x1029d2514 (count 1, kind -2). Hook gets (Filter* f, double fc, double invQ,
  double gain); fs = *(double*)(f+0x8). Pass fc' = fs/pi*tan(pi*min(fc,0.45*fs)/fs) to the original. ~10 lines, fixes #1.
  (Same targets inside ColorFxSweep::updateFilterCoef, 2+2 sites, for Sweep.)
- Full replacement: HOOK the two `bl Filter::filter2(const double*,const double*,double*,double*,int)` sites in
  __ZN23ColorFxFilterDjm900NXS221processEffectConcreteEPfS0_i (count 2: nth 0 = LPF path 0x1029d29f0, nth 1 = HPF 0x1029d2a0c)
  and run an SVF in-place on the same double buffers, with targets captured by the make_* hooks keyed by Filter*. Blocks are
  already split at every knob step (<=14-16 samples), so per-sample smoothing inside the hook removes the staircase.
  For the continuous (un-quantised) knob: map Filter* -> effect (effect+0x370 = HPF Filter*, +0x378 = LPF Filter*) and read
  LinearParameter at effect+0x260 (knob) / +0x2e0 (PARAMETER) via its vtable+0x40 getter (writes a float), or replace the
  whole virtual processEffectConcrete via vtable slot +0xd0 of __ZTV23ColorFxFilterDjm900NXS2 (0x105341660).
  Effect state available: +0x360/+0x368 SmoothShiftInteger* (current int at +0x10, target +0x14), +0x370/+0x378 Filter*,
  +0x388..+0x3a0 double work buffers, +0x3a8/+0x3ac/+0x3b0 LPF/HPF/dry gains, +0x3b4 on flag, +0xa0 fs, +0xa4 block size.
