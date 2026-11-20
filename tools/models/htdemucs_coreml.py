# betterstems: converts HTDemucs v4 (Meta, MIT) to CoreML without its STFT/iSTFT, which mods/betterstems.mm does.
# Run through tools/models/htdemucs_coreml.sh. Inputs mix (1,2,L) and mag (1,4,2048,336); outputs spec and wave.
import sys
import time, torch, numpy as np, coremltools as ct
from coremltools.converters.mil import Builder as mb
from coremltools.converters.mil.frontend.torch.torch_op_registry import register_torch_op
from coremltools.converters.mil.frontend.torch.ops import _get_inputs

# coremltools' int() of a constant 1-element array fails with numpy 2 (int(array([n]))): take the item.
@register_torch_op(torch_alias=["int"], override=True)
def _int(context, node):
    x = _get_inputs(context, node, expected=1)[0]
    if x.can_be_folded_to_const():
        res = mb.const(val=np.int32(np.asarray(x.val).reshape(-1)[0]), name=node.name)
    else:
        if len(x.shape) > 0: x = mb.squeeze(x=x, name=node.name + "_item")
        res = mb.cast(x=x, dtype="int32", name=node.name)
    context.add(res, node.name)
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
    ts = torch.jit.trace(core, (mix, mag), check_trace=False)
print("mag shape", tuple(mag.shape))
t = time.time()
try:
    ml = ct.convert(ts, inputs=[ct.TensorType(name="mix", shape=mix.shape), ct.TensorType(name="mag", shape=mag.shape)],
                    outputs=[ct.TensorType(name="spec"), ct.TensorType(name="wave")], convert_to="mlprogram",
                    compute_precision=ct.precision.FLOAT16, minimum_deployment_target=ct.target.macOS14)
    ml.save(sys.argv[1]); print("converted in %.0f s" % (time.time() - t))
except Exception as e:
    sys.exit("convert failed: %s %s" % (type(e).__name__, str(e)[:1500]))
