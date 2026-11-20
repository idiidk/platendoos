#!/usr/bin/env python3
"""Reference artefacts for the Beat This! C++ runtime.

Usage: python beatthis_ref.py input_mono_44100.f32 OUTDIR [checkpoint=final0]

Pipeline (identical to beat_this.inference.Audio2Beats, dbn=False):
  float32 mono 44.1 kHz -> soxr.resample(float64 signal, 44100 -> 22050, default quality 'HQ')
  -> torch float32 -> LogMelSpect (torchaudio MelSpectrogram, see docs/beatthis-model.md) -> split_predict_aggregate
  (chunk 1500, border 6, overlap 'keep_first') -> Postprocessor('minimal').
Writes: mel.npy [frames,128], beat_logits.npy, downbeat_logits.npy [frames], beats.txt, downbeats.txt (s),
        mel_fb.npy [513,128] (torchaudio filterbank, f_min 30, f_max 11000, slaney scale, norm=None),
        window.npy [1024] (periodic Hann).
"""
import os, sys, numpy as np, torch, soxr
from beat_this.inference import load_model, split_predict_aggregate
from beat_this.model.postprocessor import Postprocessor
from beat_this.preprocessing import LogMelSpect

src, out = sys.argv[1], sys.argv[2]
ckpt = sys.argv[3] if len(sys.argv) > 3 else "final0"
os.makedirs(out, exist_ok=True)
x = np.fromfile(src, dtype="<f4").astype(np.float64)
sig = soxr.resample(x, 44100, 22050)
spect = LogMelSpect()
mel = spect(torch.tensor(sig, dtype=torch.float32))
model = load_model(ckpt, "cpu")
with torch.inference_mode():
    o = split_predict_aggregate(mel, 1500, 6, "keep_first", model)
bl, dl = o["beat"].float(), o["downbeat"].float()
beats, downs = Postprocessor("minimal")(bl, dl)
np.save(f"{out}/mel.npy", mel.numpy())
np.save(f"{out}/beat_logits.npy", bl.numpy())
np.save(f"{out}/downbeat_logits.npy", dl.numpy())
np.savetxt(f"{out}/beats.txt", beats, fmt="%.2f")
np.savetxt(f"{out}/downbeats.txt", downs, fmt="%.2f")
np.save(f"{out}/mel_fb.npy", spect.spect_class.mel_scale.fb.numpy() if hasattr(spect.spect_class.mel_scale, "fb") else spect.spect_class.mel_scale.fb)
np.save(f"{out}/window.npy", spect.spect_class.spectrogram.window.numpy())
print(f"{src}: mel {tuple(mel.shape)}, {len(beats)} beats, {len(downs)} downbeats")
