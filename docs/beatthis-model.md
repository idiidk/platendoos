# Beat This! model spec for the runtime (final0, MIT)

Files: `models/beatthis.mlmodelc` (fp32, 78 MB), `tools/models/export_beatthis.py` (rebuild), `tools/models/beatthis_ref.py` (reference dumps).
Reference dumps: scratchpad `beat/ref/<track>/` (mel.npy, beat_logits.npy, downbeat_logits.npy, beats.txt, downbeats.txt, mel_fb.npy [513x128], window.npy [1024]).
Python env needed for export: scratchpad `beat/v13` (py3.13; py3.15 venv lacks coremltools BlobWriter).

## Model I/O (from compiled metadata)
- in  `mel`  : MultiArray Float32 [1,1500,128]   (batch, frames, mel bins), fixed shape
- out `beat` : Float32 [1,1500]  beat logits (already beat+downbeat head sum, as beat_this)
- out `down` : Float32 [1,1500]  downbeat logits
- Output frame i corresponds to input frame i. 50 fps (20 ms). Logits: >0 means p>0.5.

## Preprocessing (exactly beat_this.preprocessing.LogMelSpect, verified to 1e-4 vs numpy re-implementation)
1. Mono mix; resample to 22050 Hz with **soxr** (python `soxr.resample`, default quality "HQ", float64 in).
   (C++: libsoxr HQ, or any good resampler; small differences are tolerable.)
2. STFT: n_fft=1024, win_length=1024, hop=441, periodic Hann window (torch.hann_window(1024, periodic=True)),
   center=True with **reflect** padding of 512 each side. Frames = 1 + floor(N/441). onesided (513 bins).
3. Magnitude (power=1), divided by sqrt(1024)=32 (torchaudio normalized="frame_length").
4. Mel: 128 bands, torchaudio filterbank, mel_scale="slaney", norm=None (NO area normalisation; slaney mel *scale* only),
   f_min=30, f_max=11000, sample_rate 22050. mel = mag[T,513] @ fb[513,128] (use mel_fb.npy to avoid reimplementing).
5. log1p(1000 * mel). No further normalisation. Output [frames,128] float32.

## Chunking / aggregation (beat_this.inference.split_predict_aggregate, chunk=1500, border=6, overlap="keep_first")
- n = number of frames. starts = arange(-6, n-6, 1488). If n > 1488: starts[-1] = n - 1494 (last chunk ends at n).
- chunk = spect[max(s,0) : min(s+1500, n)], zero-padded with max(0,-s) frames on the left and
  max(0, min(6, s+1500-n)) frames on the right (zero rows in *log-mel domain*).
- Run model per chunk, drop the first and last 6 output frames of each chunk, write the remaining 1488 frames at
  [s+6, s+1494) into a full-length array initialised to -1000. "keep_first": earlier chunks win on overlap
  (write chunks in reverse order, or skip already-written frames).
- Pieces with n <= 1488 give one short chunk: the model needs exactly 1500 frames, so right-pad with zeros to 1500 and
  use only the first n outputs. Not identical to torch (it runs a short sequence; ours attends over padding):
  22 s test gave beats within 20 ms, 1 downbeat of 23 differing. Acceptable; tracks are normally longer.

## Postprocessing ("minimal", fps=50, frame i -> i/50 s)
- Per logit sequence (beat and downbeat separately): keep frame i if logit[i] == max(logit[i-3..i+3]) (max_pool 7, pad 3,
  -1000 outside) AND logit[i] > 0.
- Deduplicate adjacent peaks: scan sorted frames; group while next - running_mean_of_group_last... precisely: p=first, c=1;
  for p2: if p2 - p <= 1: c++, p += (p2-p)/c else emit p, p=p2, c=1; emit final p. (p may be fractional, e.g. x.5; running mean.)
- time = frame/50. Snap each downbeat to nearest beat time (argmin |beat - d|), then unique (sorted) the downbeats. If no beats, skip snapping.
- Optional DBN (madmom) is NOT used; dbn=False is the minimal variant above.

## Precision / compute units (measured on this Mac, final0)
fp16 is REJECTED: CPU_ONLY off by ~0.7 logits (ok-ish, peaks mostly same), but GPU/ALL output off by ~15 logits
(Demo Track 1 on ALL: 391 beats vs 362, 209 downbeats vs 97). fp32 compiled: bit-identical to torch on all units.
Timing of model only (all chunks), fp32:
| track | CPU_ONLY | CPU_AND_GPU | ALL |
| Demo 1 (172 s, 6 chunks) | 2.37 s | 0.51 s | 0.40 s |
| Demo 2 (128 s)           | 2.15 s | 0.30 s | 0.31 s |
| 60 s synth               | 1.2 s  | 0.18 s | 0.17 s |
Per 1500-frame chunk after warm-up: ~0.055 s on CPU_AND_GPU/ALL, ~0.35-0.5 s CPU_ONLY. ALL gives no ANE benefit for fp32
(it ends on GPU). First predict (model load/compile) costs extra; torch full pipeline for comparison is 3.0-3.5 s/track.
Recommendation: `MLComputeUnitsCPUAndGPU` (or ALL), fp32. Does not contend with rekordbox's CPU as much as CPU_ONLY.

## Accuracy (CoreML fp32 through same preprocessing + beat_this chunking vs torch, dbn=False, 70 ms F-measure)
| track | beat F | downbeat F | peak deviation |
| Demo Track 1 | 1.000 | 1.000 | 0 ms (362 beats / 97 downbeats) |
| Demo Track 2 | 1.000 | 1.000 | 0 ms (257 / 65) |
| synth DnB 174 BPM 60 s | 1.000 | 1.000 | 0 ms; vs ground truth beat 0.994, down 0.989 |
| synth house 124 BPM 60 s | 1.000 | 1.000 | 0 ms; vs ground truth beat 1.000, down 0.667 (model picks a 2-beat bar phase on a bare pattern: ambiguity of the test, same in torch) |
| synth house 22 s (short path) | 1.000 | 0.978 | beats <=20 ms, 22 vs 23 downbeats (padding effect above) |
Note: Beat This logits are tempo-agnostic; a global BPM should be derived from the median inter-beat interval of `beats.txt` output (not part of the model).

## Notes
- First chunk start s=-6, so output frame 0 comes from chunk 0 index 6 (left zero-pad 6 rows).
- License: Beat This! code and checkpoints are MIT (CPJKU). The paper notes training data from datasets with their own
  terms (e.g. Hainsworth, GTZAN, Beatles, etc. annotations; audio not redistributed); weights themselves are MIT-licensed, but keep the MIT notice
  and cite Foscarini et al., ISMIR 2024 ("Beat This! Accurate Beat Tracking Without DBN Postprocessing").
- Exact compare for C++ port: compare against `mel.npy` (tolerance ~1e-3) and `beat_logits.npy` (fp32 CoreML ~1e-3).
