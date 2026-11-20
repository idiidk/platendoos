#!/usr/bin/env python3
"""Export Beat This! (CPJKU, MIT) to ONNX for the Windows backend (engines/beatthis_win.cpp, ONNX Runtime).

Same fixed-shape network and patches as export_beatthis.py (the Core ML one), same I/O:
    in  "mel"  [1,1500,128] float32
    out "beat" [1,1500]     float32 (logits)
    out "down" [1,1500]     float32 (downbeat logits)
Checks the ONNX model with onnxruntime against the PyTorch reference.
Needs: pip install torch onnx onnxruntime einops rotary-embedding-torch soxr; pip install --no-deps beat_this (git+https://github.com/CPJKU/beat_this)
Usage: python export_beatthis_onnx.py [--audio raw_mono_44k.f32] [--out models/beatthis.onnx]
"""
import argparse, os
import numpy as np, torch, torch.nn.functional as F
import soxr
from beat_this.inference import load_model
from beat_this.preprocessing import LogMelSpect
from beat_this.model import beat_tracker as bt, roformer as rf

T = 1500
ap = argparse.ArgumentParser()
ap.add_argument("--name", default="final0")
ap.add_argument("--audio", help="raw mono float32 44.1k file for the equivalence check")
ap.add_argument("--out", default="models/beatthis.onnx")
a = ap.parse_args()

ref_model = load_model(a.name, "cpu").eval()
if a.audio:
    x = np.fromfile(a.audio, dtype="<f4")
    x = soxr.resample(x.astype(np.float64), 44100, 22050)
    sp = LogMelSpect()(torch.tensor(x, dtype=torch.float32))
    s0 = min(3000, max(0, len(sp) - T))
    test_in = sp[s0:s0 + T][None]
else:
    print("WARNING: no --audio given, equivalence check uses noise")
    test_in = torch.randn(1, T, 128)
with torch.no_grad():
    ref = ref_model(test_in)

# ---------------------------------------------------------------- patched model
m = load_model(a.name, "cpu").eval()
inv = m.transformer_blocks.layers[0][0].rotary_embed.freqs.detach().clone()  # inverse freqs

def pfwd(self, x):  # PartialFTTransformer without einops
    x = x[0].permute(2, 1, 0)
    x = x + self.attnF(x); x = x + self.ffF(x)
    x = x.permute(1, 0, 2)
    x = x + self.attnT(x); x = x + self.ffT(x)
    return x.permute(2, 0, 1).unsqueeze(0)
bt.PartialFTTransformer.forward = pfwd

def afwd(self, x):  # Attention without einops
    x = self.norm(x)
    qkv = self.to_qkv(x).unflatten(-1, (3, self.heads, -1)).permute(2, 0, 3, 1, 4)
    q, k, v = qkv[0], qkv[1], qkv[2]
    if self.rotary_embed is not None:
        q = self.rotary_embed.rotate_queries_or_keys(q)
        k = self.rotary_embed.rotate_queries_or_keys(k)
    out = F.scaled_dot_product_attention(q, k, v)
    if self.to_gates is not None:
        out = out * self.to_gates(x).permute(0, 2, 1).unsqueeze(-1).sigmoid()
    return self.to_out(out.transpose(1, 2).flatten(2))
rf.Attention.forward = afwd

def sh(self, x):  # SumHead: beat = lin0 + lin1, downbeat = lin1
    o = self.beat_downbeat_lin(x)
    return {"beat": o[..., 0] + o[..., 1], "downbeat": o[..., 1]}
bt.SumHead.forward = sh

class FixedRot:  # RoPE with constant tables (interleaved-pair convention of rotary_embedding_torch)
    def __init__(s, L):
        pos = torch.arange(L, dtype=torch.float32)
        f = torch.repeat_interleave(torch.einsum('i,j->ij', pos, inv), 2, dim=-1)
        s.c, s.s, s.d = f.cos(), f.sin(), f.shape[-1]
    def rotate_queries_or_keys(s, t):
        x = t[..., :s.d]; x1 = x[..., 0::2]; x2 = x[..., 1::2]
        rh = torch.stack((-x2, x1), -1).flatten(-2)
        return torch.cat((x * s.c + rh * s.s, t[..., s.d:]), -1)

def setr(mod, r):
    mod._modules.pop('rotary_embed', None); object.__setattr__(mod, 'rotary_embed', r)

fe = m.frontend
for blk, Fq in zip(fe.blocks, [32, 16, 8]):
    setr(blk.partial.attnF, FixedRot(Fq)); setr(blk.partial.attnT, FixedRot(T))
for l in m.transformer_blocks.layers: setr(l[0], FixedRot(T))

class TT(torch.nn.Module):
    def forward(s, x): return x.transpose(1, 2)
class UQ(torch.nn.Module):
    def forward(s, x): return x.unsqueeze(1)
class CC(torch.nn.Module):
    def forward(s, x): return x.permute(0, 3, 1, 2).flatten(2)
fe.stem.rearrange_tf = TT(); fe.stem.add_channel = UQ(); fe.concat = CC()

with torch.no_grad():
    o = m(test_in)
d = max((o['beat'] - ref['beat']).abs().max().item(), (o['downbeat'] - ref['downbeat']).abs().max().item())
print(f"patched torch vs original torch (real-audio spectrogram): max logit diff {d:.2e}")
assert d < 1e-3, "patched model deviates from the original"

class W(torch.nn.Module):
    def __init__(s, m): super().__init__(); s.m = m
    def forward(s, x):
        o = s.m(x); return o["beat"], o["downbeat"]

import onnxruntime as ort
os.makedirs(os.path.dirname(os.path.abspath(a.out)), exist_ok=True)
with torch.no_grad():
    torch.onnx.export(W(m).eval(), (test_in,), a.out, input_names=["mel"], output_names=["beat", "down"],
                      opset_version=17, dynamo=False)
s = ort.InferenceSession(a.out, providers=["CPUExecutionProvider"])
b, d = s.run(None, {"mel": test_in.numpy()})
print(f"onnx vs torch: beat max diff {abs(b - ref['beat'].numpy()).max():.2e}, down {abs(d - ref['downbeat'].numpy()).max():.2e}")
print("saved", a.out, os.path.getsize(a.out) // 1024, "KB")
