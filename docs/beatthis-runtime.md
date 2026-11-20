# beatthis runtime (C++/ObjC++, Accelerate + CoreML)

Files: `engines/beatthis.h` (API), `engines/beatthis.mm` (everything: resampler, STFT/mel, CoreML, chunking, postprocess, fitGrid),
`tools/dev/beatthis_test.mm` + `beatthis_test.sh` (CLI harness). Model: `models/beatthis.mlmodelc` (fp32, see beatthis-model.md).
Build: `clang++ -std=c++20 -O2 -arch arm64 -fobjc-arc -framework Foundation -framework Accelerate -framework CoreML engines/beatthis.mm ...`
The header is plain C++ (pimpl), so it can be included from .cpp files; only engines/beatthis.mm needs ObjC++.

## API (namespace beatthis)
- `Model::load(mlmodelcPath, &err, ComputeUnits = CpuAndGpu)`; `analyze(mono, n, sampleRate, Debug* = nullptr)` -> `Result{beats, downbeats}` (s).
  Any sample rate (rational polyphase resampler to 22050 Hz); needs >= 1 s audio else returns empty Result.
- `fitGrid(Result, minBpm=70, maxBpm=200)` -> `Grid{bpm, firstBeat in [0,60/bpm), downbeatIndex 0..3, confidence, ok}`.
  Beat k is at firstBeat + k*60/bpm; it is a downbeat iff k%4 == downbeatIndex. ok=false with < 8 beats.
- Exposed for tests: `resampleTo22k`, `logMel`, `postprocess`, `Model::analyzeMel`, `Debug` (mel, aggregated logits, timings).

## Pipeline fidelity
- Resample: Kaiser-windowed sinc (cutoff 0.957 of output Nyquist, beta 12, +-96 output periods), tuned to soxr HQ
  (what beat_this uses). vs soxr: Demo 1 max abs 4.5e-4, rms -77 dB; Demo 2 max 1.9e-4, rms -82 dB. 48 kHz input path checked too.
- STFT/mel from identical samples (soxr output): max abs err 1.5e-4, mean 2e-6 (log-mel range 0..8.7).
- Full pipeline (our resampler) vs reference mel.npy: Demo 1 max 0.0215 / mean 1.35e-4; Demo 2 max 0.018 / mean 9.3e-5.
- Logits (CoreML fp32, aggregated over the track) vs torch reference: Demo 1 max 0.029 / mean 1.8e-3 (1 sign flip of 8622 frames),
  Demo 2 max 0.009 / mean 4e-4 (0 flips). Same on CPU_ONLY and CPU_AND_GPU.
- Beats: Demo 1 362/362 identical times (exact); downbeats 96 vs 97 (one borderline logit crossing 0 flips one downbeat).
  Demo 2: 257/257 beats and 65/65 downbeats exact.

## Speed (M-series, 6 cores+; Demo 1 = 172 s, 6 chunks; ~6-min track = 13 chunks)
| stage | time |
| resample (parallel) | 12 ms (172 s) -> ~25 ms per 6 min |
| STFT+mel (parallel) | 6 ms -> ~15 ms |
| CoreML CPU_AND_GPU fp32 | 330 ms / 6 chunks (~55 ms/chunk) -> ~0.7 s per 6 min |
| CoreML CPU_ONLY | 2.5 s / 6 chunks -> ~5.5 s per 6 min |
| postprocess | < 1 ms |
Model load 40-400 ms (first predict of a session compiles/warms up; load once and reuse the Model).

## Memory
RSS ~200 MB, peak footprint ~370 MB for a 172 s track (model 78 MB fp32, mostly CoreML + GPU buffers).
Per call: resampled audio 4 B * 22050 * s (32 MB per 6 min) + padded copy + mel (4*128*50*s = 12 MB per 6 min) + the 44.1 kHz input is not copied except for a zero-padded working copy (n+~400 floats).

## Threading
`Model` is copyable (shared state) and `analyze` is const; concurrent calls are safe: resample/mel run in parallel
(libdispatch global queue), `predictionFromFeatures` calls are serialised by an internal mutex. Preprocessing already uses all cores
via dispatch_apply, so there is little to gain from calling it from several threads.

## Grid fit
Median-of-16-beat-spans slope (Theil-Sen style) -> sequential beat-index unwrapping (tolerates missing/spurious beats) -> LS line on
inliers (30-70 ms band, iterated) -> bpm rounded to 0.01, folded by octaves only if outside [min,max]; firstBeat = line intercept mod period
(anchored at track start); downbeatIndex = vote of detected downbeats mod 4; confidence = fraction of all detected beats within 70 ms.
Results: Demo 1 127.99 BPM (ref-beats 127.98) conf 0.964, Demo 2 120.02 conf 1.0. Synthetic 120 s (44.1 kHz, through the model):
124 BPM 4/4 house -> 124.00, firstBeat 0.372 (truth 0.370), conf 1.0; 174 BPM DnB -> 174.00, 0.032 (0.025), downbeatIndex 1 (truth 1), conf 1.0;
128 BPM with 30 s pad-only intro -> 128.00, 0.366 (0.370), conf 0.995.

## Caveats
- Clips shorter than 1500 frames (30 s) are zero-padded to the model's fixed length (differs slightly from torch which runs a short sequence).
- Tempo octave: the tracker's tempo is kept when inside [minBpm,maxBpm] (a DnB track tracked at 87 stays 87). Triple meter is not modelled (4 beats/bar assumed).
- downbeatIndex is a tie (returns 0) when the model labels every beat as a downbeat (happens on the bare synthetic 124 BPM pattern, same in torch).
- Constant tempo only: a drifting track gets low confidence; beats at the track start are what the intercept follows.
- Resampler constants were tuned on the demo tracks; other material should see similarly tiny mel differences (the filter only differs from soxr in the 10.3-11 kHz transition band).
- Logit threshold rounding: peaks near logit 0 may flip vs torch (1 of 97 downbeats on Demo 1).
