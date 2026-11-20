#!/usr/bin/env python3
"""Export Beat This! (CPJKU, MIT) to a fixed-shape CoreML mlprogram.

NOTE: default fp32 - fp16 gives garbage on GPU/ALL (logit diff ~15), see docs/beatthis-model.md.
Usage: python export_beatthis.py [final0|small0] [--precision fp32|fp16]
           [--audio raw_mono_44k.f32] [--out DIR] [--compile OUT.mlmodelc]

Model I/O after export:
    in  "mel"  [1,1500,128] float32   (log-mel frames, 50 fps, 30 s chunk)
    out "beat" [1,1500]     float32   (logits)
    out "down" [1,1500]     float32   (downbeat logits; beat logit = beat_head + down_head already summed)

Why patches are needed (coremltools cannot convert the stock graph):
  * einops rearranges            -> plain permute/unflatten/flatten
  * rotary_embedding_torch RoPE  -> precomputed cos/sin tables of fixed length
                                    (frequency axis 32/16/8 in the 3 frontend blocks, time axis 1500)
  * SumHead returns dict         -> returns tuple-friendly tensors
  * everything traced with batch 1 and T=1500 (the C++ side chunks, as beat_this does).
"""
import argparse, os, subprocess, sys, time
import numpy as np, torch, torch.nn.functional as F
import coremltools as ct
import soxr
from beat_this.inference import load_model
from beat_this.preprocessing import LogMelSpect
from beat_this.model import beat_tracker as bt, roformer as rf

T = 1500

ap = argparse.ArgumentParser()
ap.add_argument("name", nargs="?", default="final0")
ap.add_argument("--precision", choices=["fp16", "fp32"], default="fp32")
ap.add_argument("--audio", help="raw mono float32 44.1k file for the equivalence check")
ap.add_argument("--out", default=".")
ap.add_argument("--compile", help="also compile to this .mlmodelc path")
a = ap.parse_args()

# ---------------------------------------------------------------- reference model + test input
ref_model = load_model(a.name, "cpu").eval()
if a.audio:
    x = np.fromfile(a.audio, dtype="<f4")
    x = soxr.resample(x.astype(np.float64), 44100, 22050)
    sp = LogMelSpect()(torch.tensor(x, dtype=torch.float32))
    # skip the first 30 s (intro) if long enough
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
with torch.no_grad():
    tr = torch.jit.trace(W(m).eval(), test_in, check_trace=False)

prec = ct.precision.FLOAT16 if a.precision == "fp16" else ct.precision.FLOAT32
ml = ct.convert(tr, inputs=[ct.TensorType(name="mel", shape=(1, T, 128))],
                outputs=[ct.TensorType(name="beat"), ct.TensorType(name="down")],
                convert_to="mlprogram", compute_precision=prec,
                minimum_deployment_target=ct.target.macOS13)
ml.author = "Beat This! (CPJKU), MIT"
pkg = os.path.join(a.out, f"bt_{a.name}_{a.precision}.mlpackage")
ml.save(pkg); print("saved", pkg)
for cu in (ct.ComputeUnit.CPU_ONLY, ct.ComputeUnit.CPU_AND_GPU, ct.ComputeUnit.ALL):
    m2 = ct.models.MLModel(pkg, compute_units=cu)
    m2.predict({"mel": test_in.numpy()})
    t = time.time(); r = m2.predict({"mel": test_in.numpy()}); dt = time.time() - t
    print(f"{cu}: {dt:.3f}s/chunk  max logit diff vs torch {abs(r['beat'] - ref['beat'].numpy()).max():.3f}")
if a.compile:
    outdir = os.path.dirname(os.path.abspath(a.compile))
    subprocess.check_call(["xcrun", "coremlcompiler", "compile", pkg, outdir])
    built = os.path.join(outdir, os.path.basename(pkg).replace(".mlpackage", ".mlmodelc"))
    if built != os.path.abspath(a.compile):
        import shutil; shutil.rmtree(a.compile, ignore_errors=True); shutil.move(built, a.compile)
    print("compiled ->", a.compile)
