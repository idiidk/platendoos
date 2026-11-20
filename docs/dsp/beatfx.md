# rekordbox 7.2.19 (macOS arm64) Beat FX DSP review

Status: DONE (time-boxed; deep on Echo/Reverb/Filter/common path, quick pass on the rest). Addresses are VAs in the thin arm64 binary ($SP/rb64).
Tools: $SP/dsp/dis.py <symbol> (disassembly with call names), $SP/dsp/xref.py <symbol> (callers inside
0x102960000-0x102c00000, pre-disassembled into $SP/dsp/region.s).

## Class layering
- `BeatFx*` = DSP cores (BeatFxEcho, BeatFxDelay, BeatFxReverb, BeatFxFilter, ...), all derive from `EffectBase`.
- `SingleBeatFx*` = Beat FX unit "single mode" effects (DJM-style list: Delay, Echo, Ping Pong, Spiral, Reverb,
  Trans, Filter, Flanger, Phaser, Pitch, Slip Roll, Roll, Vinyl Brake, Helix...). Most simply subclass the core and
  map knob parameters in `updateParameter(XxxParameter&)` (e.g. SingleBeatFxFilter ctor 0x1029cfb78 calls
  BeatFxFilter::BeatFxFilter). Echo/Delay/PitchEcho/Spiral/Roll have their own processEffectConcrete.
- `MultiBeatFx*` = multi mode (3 FX slots). Same cores.
- The DDJ-FLX4 BEAT FX list matches the single-mode list, so FLX4 most likely drives `SingleBeatFx*` (guess, not
  verified at runtime).

## Common path: EffectBase::processEffect 0x102a5d18c
- If EffectInOutGain is active (EffectInOutGain+0x108, default 1 from ctor 0x102a5ea08):
  copy L/R -> temp (EffectBase+0xb8), `EffectInOutGain::applyInputGain` (0x102a5ed00) on temp,
  vtable `processEffectConcrete(tempL,tempR,n)`, `processSubsonicHPF`, then
  `applyOutputGainAndAdd(L,R,tempL,tempR,n)` (0x102a5ee28): **out = in*dryGain + effect(in*inGain)*wetGain**.
- dryGain/wetGain targets = EffectBase+0x20/+0x24 (the "MixRatio" {dry,wet} written by each calculateMixRatio),
  copied every block (0x102a5d360). Gains run through `SmoothFloat<float>` = **linear ramp**
  (setTargetValue 0x10297acf8, getSmoothValues 0x10297aaf4), ramp length = smoothing ms * sr / 1000
  (setSmoothingTime 0x10297ad38). So LEVEL/DEPTH changes are ramped per sample (no zipper on the mix itself).
- Only a few constructors call setInOutGainProcessActive (SingleBeatFxDelayBase ctor+0x238, BeatFxPan ctor+0xa8,
  release-FX classes); everything else keeps the default (active).
- On/off: EffectBase::turnOn 0x102a5ce5c / turnOff 0x102a5cea8 -> EffectInOutGain::startStateTransition with a
  per-effect "transition volume info" (EffectBase+0x70 for on, +0x88 for off; triples of {mode,value} for
  input/dry/wet; mode 1 = ramp, 0 = jump). During a transition the ramp time is forced to
  0x40555555 = **3.33 ms** (pollingStateUpdate 0x102a5ec1c) for all three gains. Static
  EffectInOutGain::GAIN_SMOOTHING_MSEC_FOR_FX_SWITCHING @0x1038e0794 = 3.333 ms.
  - Default (switching category 0, EffectBase::setInOutGainInfo 0x102a5bd30): off = input stays 1, dry ramps to 1,
    wet ramps to 0 in 3.3 ms => **no trails, tail cut with a 3.3 ms fade**.
  - Category 1 and SingleBeatFxEcho/SingleBeatFxDelayBase/BeatFxDelay setInOutGainInfo (0x1029c08a0, 0x1029ccac8,
    0x1029bd344): off = input ramps to 0, dry ramps to 1, wet held => **trails** (echo tail rings out).

## Shared mix laws
- `MultiBeatFxEcho::calcMixRatio` 0x1029bfa1c (used by Single/Multi Echo, SingleBeatFxDelayBase 0x1029cd1ac, and
  inlined in SingleBeatFxEcho::calcMixRatioInternal 0x1029c05a8): LEVEL x in [0,1]:
  x<0.75: dry=1, wet=x*4/3; x>=0.75: dry=4-4x, wet=1. DJM-style additive law (dry untouched below 75 %, no dip;
  linear-amplitude wet). x=0 -> dry 1, wet 0 => dry untouched at LEVEL 0. Not a crossfade, so no 3 dB hole.
- `MultiBeatFxRollBase::calcMixRatio` 0x102a2dea4 + `RollEffectMixRatioCurveTable::TABLE` @0x1038dca6c
  (129 floats, 1 - t^2 shape: T[64]=0.748): idx=(int)(x*256); x>=0.5 -> dry 0, wet 1; else dry=T[idx],
  wet=T[128-idx]. Parabolic crossfade: +0.5 dB power bump at mid for uncorrelated, no dip. Fine.
  Quantised to 129 steps but the result goes through the 3.3 ms/linear SmoothFloat, so no zipper.

## ECHO (SingleBeatFxEcho -> nested BfxEcho : BeatFxEcho -> EchoCore -> DelayModule)
- SingleBeatFxEcho::processEffectConcrete 0x1029c0930: nested BfxEcho (EffectBase::processEffect, its own mix
  {dry 0, wet 1} from BfxEcho::updateParameter 0x1029c22a0) produces wet only; wrapper mixes
  out = wet*g(0x4b0) + dryCopy*g(0x480) with SmoothFloat ramps (targets set in updateAndSetParameter 0x1029c0248
  from the echo law above); then the knob filter (InstFxHPF @+0x470 / InstFxLPF @+0x478, CenterThrough param @+0x350,
  left = LPF, right = HPF) is applied to the whole output via smoothed crossfade gains (+0x4e0.. +0x570).
- EchoCore::processEffect 0x1029b72fc per channel: d = DelayModule::read(); buf <- in + fb*d (fb SmoothFloat
  3.33 ms); out = outGain*d. Classic single-tap feedback echo, **no filtering, no saturation in the loop**,
  output is wet only. Feedback default 0.7 (0x3f333333, -3.1 dB/repeat); feedback knob (param @+0x2d0,
  CenterRecommend) right side adds up to +0.2933 while ON (max 0.993) and +0.3 while OFF/trailing (max **1.0**,
  clamped with fminnm, 0x1029c0418..0x1029c0440): with that knob at max the trail never decays (freeze-like;
  probably intentional, FLX4 cannot reach the knob). A "hold" toggle (+0x418) forces fb=1.0 (0x1029c0338).
- Delay time: SingleBeatFxEcho::setEffectBeatTimeWithPlayHead 0x1029c0cc0 stores beat ms at +0x460, clamped to
  [1, 4000] ms (0x1029c046c) -> EchoCore params -> `DelayModule::setDelayTime(ms, smooth=true)` 0x10298fc28:
  samples = (int)(ms*0.001*sr) (**truncated to whole samples**, max error 1 sample = 23 us/repeat at 44.1k:
  inaudible drift). Change handling: SignalGate::close() (3.33 ms fade out, setGainFadeProcessingTime
  0x40555554 in EchoCore ctor 0x1029b6a7c), then in DelayModule::read 0x10298ff44 jump the read pointer
  (setReadSubscriptByDelaySampleNumber at 0x102990154) and SignalGate::open(). So a beat change = ~3.3 ms fade-out,
  jump, ~3.3 ms fade-in: no pitch glide, no click, but a short **hole that is also fed back into the loop**
  (read() output, gate gain included, is what goes into in+fb*d) so every later repeat carries the dip.
  Change threshold is an absolute 2^-23 ms (movi 0x34000000), i.e. *any* BPM change retriggers the gate
  (GUESS, not verified at runtime: moving the tempo fader with echo on gives a stream of 6.7 ms dips =
  choppy/"zipper" echo while the fader moves).
- Reads use RingBuffer::readWithLerp_mono (flag DelayModule+0xb4 set in EchoCore ctor) but the delay is an
  integer, so interpolation never actually happens.
- Sample-rate change: EchoCore::onSamplingRateChanged 0x1029b6da8 updates both DelayModules and all 4
  smoothers. OK.
- Denormals: plain float feedback, no flush. On Apple Silicon subnormals are handled at full speed, so not a
  CPU problem on Mac (would matter on x64 Windows unless FTZ/DAZ is set by the host thread).

(Note: my private tools moved to $SP/dsp/bfx/ (dis.py, xref.py, vt.py, region.s) because another agent shares
$SP/dsp and replaced region.s / removed xref.py.)

## Switching category (on/off behaviour) per core, from the EffectBase ctor argument (w3)
- 0 = hard-ish off (input 1, dry->1, wet->0 over 3.3 ms; no tail): Filter, Flanger, Phaser, Trans, Pan, EnigmaJet.
- 1 = trails (input->0 over 3.3 ms, wet held, dry->1): Echo, PingPong, PitchEcho, Reverb, Spiral, Helix, Mobius,
  VinylBrake.
- 2 = Roll / SlipRoll / ReverseRoll / Stretch / BackSpin (same as 0 for on/off).
- 3 = Delay (own setInOutGainInfo: trails like 1).
  A 3.3 ms linear fade is click-free but very short: for Trans/Filter/Flanger/Phaser, switching OFF chops the
  wet signal (flanger/phaser/filter state is dropped with it); no "release" on those, by design (DJM behaviour).

## REVERB (SingleBeatFxReverb : BeatFxReverb -> ReverbCore)
- BeatFxReverb::processEffectConcrete 0x102a111ac: optional EffectFrequencyModule (3-band send EQ: band gains
  1-x via setTargetGain 0x102985944, the LOW/MID/HIGH cut toggles) -> ReverbCore::processEffect 0x102a1c43c.
- ReverbCore::handleReverbProcessing 0x102a1cd6c: input gain -> 2 IIR pre-filters (+0xc8, +0xd0) ->
  processCombFilters 0x102a1cedc = **5 parallel comb filters per channel** (each with its own IirFilter damping
  in the loop, ctor 0x102a1b4b0..0x102a1b680) -> **2 series allpasses** (ReverbAllPassFilter @+0x128/+0x130) ->
  output gain. I.e. a 1979-style Moorer/Schroeder reverb (Freeverb has 8 combs + 4 allpasses). No early
  reflections, no delay modulation, no diffusion network: low echo density, audible metallic ring/flutter on
  transients and with small rooms. Biggest "could sound better" item of the reverb.
- Comb gains derived from a decay time via exp() (SingleBeatFxReverb::getDynamicParameter 0x102a11c74 -> calls
  to the expf stub), so decay is physically consistent across combs. Good.
- Room size: integer 1..100 (default 50, SingleBeatFxReverb::updateParameter 0x102a119d8). A change walks one
  step at a time ([+0x138] -> [+0x13c], 0x102a1c960) with a smoothing time (default 40 ms, 0x42200000 in
  startPitchSmoothing 0x102a1c4b0); comb delays are SmoothFloat-ramped and read with
  RingBuffer::readByIntervalSamplesWithLerp_mono (getFeedbackSignal 0x102a19e44) = proper fractional, linearly
  interpolated delay modulation -> a short doppler glide instead of clicks. Fine.
- Mix law: same DJM echo law (SingleBeatFxReverb::updateParameter 0x102a119fc..0x102a11a38).
- Trails: category 1 (tail rings out).
- Sample rates: only 44.1/48/96 kHz are processable at all (EffectBase::isProcessableSamplingRate 0x102a5c3ec);
  per-rate constants in SingleBeatFxReverb::getStaticParameter 0x102a11b24.

## FILTER (SingleBeatFxFilter : BeatFxFilter -> LFOFilterCore)
- LFOFilterCore::processEffect 0x1029d6688: two LFOs (TriangleWave) summed and clamped, cutoff index =
  lfo*128 into 129-entry per-sample-rate coefficient tables (LPF/HPF MAIN/RESO for 44.1/48/96 k only,
  @0x1056d82d8..), **linearly interpolated per sample** (0x1029d6954..0x1029d69f8), then a per-sample
  time-varying IIR cascade (processFilter 0x1029d6ec0) + resonance branch (calcResonanceGain 0x1029d6b94,
  gain 0.4 @44.1/48k, 0.8 @96k). No zipper: coefficients move every sample.
- Filter-type change (LPF<->HPF) / LFO phase flips: SignalGate::close (fade out) -> setFilterType + reset
  (state cleared) -> SignalGate::open (BeatFxFilter::processEffectConcrete 0x1029cf584). Clean.
- Mix law (SingleBeatFxFilter::updateParameter 0x1029cfd84): LEVEL/DEPTH x <= 0.15625: t = 12.8x,
  dry = 0.5 + 0.5*sin(pi/2*(1-t)) (polynomial sine approx, coefficients 1.5703/-0.6421/0.0718) = cos^2(pi*t/4),
  wet = 1 - dry: **amplitude-complementary (sum = 1) crossfade** over the first 15.6 % of the knob; above that
  dry 0 / wet 1 and the rest of the knob is depth. With a phase-shifting filter the 50/50 point partially
  cancels near cutoff (about -3 dB) - inherent and confined to a tiny knob range. Low impact.
- If the sample rate is not 44.1/48/96 kHz the table pointers (+0x160/+0x168) are never set
  (onSamplingRateChanged 0x1029d6390) - harmless only because isProcessableSamplingRate blocks those rates.

## FLANGER (BeatFxFlanger -> FlangerCore -> SmoothingDelayModule)  [quick pass]
- FlangerCore::processEffectCore 0x102a02a44: per-sample LFO delay array -> SmoothingDelayModule::read
  0x10299386c -> readCore 0x102993878 -> RingBuffer::readByIntervalSamplesWithLerp_mono (lerp flag +0x6c set to 1
  in FlangerCore ctor 0x102a022c4): **fractional delay with linear interpolation, per-sample modulation** - correct.
  Feedback/mix gains through SmoothFloat. Linear interpolation gives slight HF dulling / modulation noise at
  fractional positions (cubic/allpass would be cleaner) - minor.
- Gate (3.33 ms, ctor 0x1029fe0ac) around LFO phase reset (jumpToHeadPosition at 0x1029fe6a8). Category 0 (no tail).

## PHASER (BeatFxPhaser -> PhaserCore)  [quick pass]
- PhaserCore::processEffect 0x102a0f2d8: frequencies prewarped (sr/pi)*tan(pi*f/sr) (0x102a0f350..0x102a0f38c,
  tan via libm stub) and SmoothFloat-ramped; per-sample LFO; variable stage count (int params +0x230/+0x2b0).
  Looks textbook. No issue found in the time available (stage-count change handling not checked).

## TRANS (SingleBeatFxTrans : BeatFxTrans -> TransCore)  [quick pass]
- TransCore::processEffect 0x102a530d0: out = in * LFO(SquareWave) per sample. LFO smoothing 3.33 ms
  (SingleBeatFxTrans ctor 0x102a519f0) - inferred to ramp the square edges (LFO::getValues handles
  discontinuities with SmoothFloat, 0x102998c14/0x102998c28; not traced in full). 3.3 ms edges = click-free but
  "hard" (DJM character). Category 0.

## ROLL / SLIP ROLL (SingleBeatFxRollBase -> RollCore -> RollBufferReader)  [quick pass]
- Mix: roll curve table (see mix laws), full wet from LEVEL 0.5 up.
- RollBufferReader::read 0x102a268b8: at the loop end / beat restart it **closes a SignalGate (default
  SmoothFloat time 3.33 ms, SmoothFloat ctor 0x10297aaa4), keeps reading past the loop end while fading out,
  then jumps to the loop start and fades in** (0x102a26ad4..0x102a26b30, 0x102a26a48..0x102a26a74).
  => each repeat has a ~6.7 ms V-shaped dip instead of a crossfade, and (inferred) every repeat starts ~3.3 ms
  after the beat point. On 1/16-1/32 rolls (29/15 ms at 128 BPM) the dip is a large fraction of the period:
  audible "chatter"/AM. Better: overlap crossfade of the loop tail into the head (two read heads, equal-gain
  since the material is correlated), starting the fade *before* the boundary so the head lands on the beat.

## PITCH ECHO (SingleBeatFxPitchEcho : BeatFxPitchEcho -> PitchEchoCore)  [quick pass]
- PitchEchoCore::processEffect 0x1029b8f98: DelayModule (same gate-on-change as Echo) -> rbxfrm::PitchShifter
  (rotating two-tap delay-line shifter with crossfade, processPitchShift 0x1028ce4ec) **inside the feedback loop**
  -> fb gain -> + input -> write. Each repeat is shifted again (intended). The rotating-head shifter's
  warble/comb artefacts accumulate per repeat - inherent to the cheap design (hardware-like). Pitch parameter is
  an int clamped to +-200 (setPitchParameter 0x1028cea48), steps of 0.5-2 % (~9-34 cents) -> audible stepping
  if the pitch is swept slowly (GUESS on how the knob maps; not traced).

## SPIRAL (SingleBeatFxSpiral -> SpiralCore)  [quick pass]
- SpiralCore::processEffectConcrete 0x102a418a0: delay with **limiter in the loop** (caluclateLimitterThres
  0x102a41dec, checkOutputGain 0x102a420d4), SpiralBuffer with delay-time acceleration (calcAccel 0x1028d62f4) and
  fractional reads (getDecimalDelayData 0x1028dac58), plus an output PitchShifter. The most carefully engineered
  of the delay family; nothing wrong spotted.
- SingleBeatFxSpiral::calculateMixRatio 0x102a3b440: dry always 1.0, wet = 2x up to x=0.5 then 1.0 (pure
  additive, dry untouched).

## Not covered / uncertain
- Helix, Mobius, EnigmaJet, Pan, Stretch, VinylBrake, Delay internals: not reviewed beyond category/mix.
- Runtime behaviour (how often setEffectBeatTimeWithPlayHead fires during tempo moves) not verified - no
  debugger attached by instruction.
- FLX4 -> Single-mode mapping assumed from the effect list.

## Ranked verdict (audible impact, highest first)
1. REVERB topology (5 combs + 2 allpasses per channel, no ER/modulation): metallic, grainy tail; the one effect
   that clearly "could sound better". Fix: replace ReverbCore::processEffect 0x102a1c43c (in-place stereo, wet
   only) with an 8/16-line FDN (Hadamard/Householder, modulated delays, damping per line, input diffusers),
   mapping room size ReverbCore+0x13c and the existing decay/damping params. Hookable by name; CPU is trivial.
2. ROLL loop wrap = gate dip (fade-out/jump/fade-in, ~6.7 ms) instead of a crossfade, head ~3.3 ms late.
   Fix: hook RollBufferReader::read 0x102a268b8 (or replace the close/open at 0x102a26ad4/0x102a26b18) with a
   two-head crossfade that starts before the boundary.
3. ECHO/PITCH ECHO delay-time change = gate dip that is fed back into the loop; threshold is 2^-23 ms so any BPM
   wobble (tempo fader move) retriggers it (GUESS on frequency). Fix options: hook DelayModule::setDelayTime
   0x10298fc28 and ignore changes < ~1 sample (cheap, safe), or implement a 2-head crossfade / tape-style glide
   in DelayModule::read 0x10298ff44.
4. Category-0 effects (Filter/Flanger/Phaser/Trans) cut with a 3.3 ms fade on OFF: click-free but abrupt; a
   20-50 ms release would be smoother (EffectBase::setInOutGainInfo 0x102a5bd30 / EffectInOutGain smoothing
   0x102a5ec1c const 3.333 ms). Arguably intended DJM behaviour.
5. Pitch Echo shifter artefacts and 1 %-step pitch param (low; design choice).
6. Filter mix crossfade is amplitude-complementary (sum 1) over 0-15.6 % LEVEL: small -3 dB notch near cutoff at
   the midpoint (low).
Not problems: dry/wet laws (DJM additive law, dry untouched at LEVEL 0, all ramps linear 3.3 ms, no 3 dB dip),
parameter smoothing (everything goes through SmoothFloat/SignalGate), sample-rate handling (each core recomputes
in onSamplingRateChanged; only 44.1/48/96 kHz supported), feedback bounded (<=0.993 while ON; 1.0 only in the
deliberate freeze/hold paths), no NaN/denormal risk on Apple Silicon (no FTZ in code; matters only on x64).
