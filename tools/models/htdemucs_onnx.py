#!/usr/bin/env python3
"""betterstems, Windows: exports HTDemucs v4 (Meta, MIT) without its STFT/iSTFT to ONNX (ONNX Runtime backend,
mods/betterstems_win.cpp). Same split and I/O as htdemucs_coreml.py: inputs mix (1,2,L) and mag (1,4,2048,336),
outputs spec and wave; fp32. Checks the ONNX model with onnxruntime against PyTorch.
Needs: pip install torch onnx onnxruntime einops julius openunmix dora-search omegaconf pyyaml tqdm lameenc;
       pip install --no-deps demucs==4.0.1
Usage: python htdemucs_onnx.py [models/htdemucs.onnx]
"""
import os, sys, time, torch, numpy as np
out = sys.argv[1] if len(sys.argv) > 1 else "models/htdemucs.onnx"
from einops import rearrange
from demucs.pretrained import get_model
torch.backends.mha.set_fastpath_enabled(False)  # its fused attention op doesn't convert
bag = get_model("htdemucs"); m = bag.models[0]; m.eval()
L = int(m.segment * m.samplerate)

class Core(torch.nn.Module):
    def __init__(s, m): super().__init__(); s.m = m
    def forward(s, mix, mag):
        m = s.m
        x = mag
        B, C, Fq, T = x.shape
        mean = x.mean(dim=(1, 2, 3), keepdim=True); std = x.std(dim=(1, 2, 3), keepdim=True)
        x = (x - mean) / (1e-5 + std)
        xt = mix
        meant = xt.mean(dim=(1, 2), keepdim=True); stdt = xt.std(dim=(1, 2), keepdim=True)
        xt = (xt - meant) / (1e-5 + stdt)
        saved, saved_t, lengths, lengths_t = [], [], [], []
        for idx, encode in enumerate(m.encoder):
            lengths.append(x.shape[-1]); inject = None
            if idx < len(m.tencoder):
                lengths_t.append(xt.shape[-1]); tenc = m.tencoder[idx]; xt = tenc(xt)
                if not tenc.empty: saved_t.append(xt)
                else: inject = xt
            x = encode(x, inject)
            if idx == 0 and m.freq_emb is not None:
                frs = torch.arange(x.shape[-2])
                emb = m.freq_emb(frs).t()[None, :, :, None].expand_as(x)
                x = x + m.freq_emb_scale * emb
            saved.append(x)
        if m.crosstransformer:
            if m.bottom_channels:
                b, c, f, t = x.shape
                x = rearrange(x, "b c f t-> b c (f t)"); x = m.channel_upsampler(x)
                x = rearrange(x, "b c (f t)-> b c f t", f=f); xt = m.channel_upsampler_t(xt)
            x, xt = m.crosstransformer(x, xt)
            if m.bottom_channels:
                x = rearrange(x, "b c f t-> b c (f t)"); x = m.channel_downsampler(x)
                x = rearrange(x, "b c (f t)-> b c f t", f=f); xt = m.channel_downsampler_t(xt)
        for idx, decode in enumerate(m.decoder):
            skip = saved.pop(-1); x, pre = decode(x, skip, lengths.pop(-1))
            offset = m.depth - len(m.tdecoder)
            if idx >= offset:
                tdec = m.tdecoder[idx - offset]; length_t = lengths_t.pop(-1)
                if tdec.empty:
                    pre = pre[:, :, 0]; xt, _ = tdec(pre, None, length_t)
                else:
                    skip = saved_t.pop(-1); xt, _ = tdec(xt, skip, length_t)
        S = len(m.sources)
        x = x.view(B, S, -1, Fq, T) * std[:, None] + mean[:, None]
        xt = xt.view(B, S, -1, L) * stdt[:, None] + meant[:, None]
        return x, xt

core = Core(m).eval()
mix = torch.randn(1, 2, L)
with torch.no_grad():
    z = m._spec(mix); mag = m._magnitude(z)
    ref = m(mix)
    x, xt = core(mix, mag)
    full = m._ispec(m._mask(z, x), L) + xt
    print("split matches the model:", float((full - ref).abs().max()))
os.makedirs(os.path.dirname(os.path.abspath(out)), exist_ok=True)
t = time.time()
with torch.no_grad():
    torch.onnx.export(core, (mix, mag), out, input_names=["mix", "mag"], output_names=["spec", "wave"],
                      opset_version=17, dynamo=False)
print("exported in %.0f s" % (time.time() - t))
import onnxruntime as ort
s = ort.InferenceSession(out, providers=["CPUExecutionProvider"])
t = time.time()
sp, wv = s.run(None, {"mix": mix.numpy(), "mag": mag.numpy()})
print("ort run %.1f s; spec %s wave %s" % (time.time() - t, sp.shape, wv.shape))
print("onnx vs torch: spec max diff %.2e (ref max %.2f), wave %.2e (ref max %.2f)" % (
    abs(sp - x.numpy()).max(), x.abs().max(), abs(wv - xt.numpy()).max(), xt.abs().max()))
print("saved", out, os.path.getsize(out) // 1024, "KB")
