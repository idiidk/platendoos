# Discogs-EffNet (MTG / Essentia, CC BY-NC-SA 4.0) for the genres mod's style detection: the ONNX model -> Core ML
# (an .mlpackage; compile it with `xcrun coremlcompiler compile <it> models/`) and checked against ONNX Runtime. FP32: FP16
# changes the scores. Input "melspectrogram" [batch, 128 frames, 96 mel bands] (genres_model.cpp makes it), output "activations" [batch, 400].
#   python3 effnet_coreml.py <discogs-effnet-bsdynamic-1.onnx> <out.mlpackage>
import sys
import numpy as np
import onnx, onnx2torch, torch, coremltools as ct, onnxruntime as ort

src, dst = sys.argv[1], sys.argv[2]
net = onnx2torch.convert(onnx.load(src)).eval()


class Activations(torch.nn.Module):  # the style activations only (the model also gives embeddings)
    def __init__(self, net):
        super().__init__()
        self.net = net

    def forward(self, x):
        out = self.net(x)
        return out[0] if isinstance(out, (tuple, list)) else out


m = Activations(net).eval()
x = torch.randn(8, 128, 96)
ref = ort.InferenceSession(src).run(['activations'], {'melspectrogram': x.numpy()})[0]
with torch.no_grad():
    got = m(x).numpy()
print('torch vs onnx: max diff', float(np.abs(got - ref).max()))
traced = torch.jit.trace(m, x)
model = ct.convert(traced, inputs=[ct.TensorType(name='melspectrogram', shape=(ct.RangeDim(1, 64, 16), 128, 96))],
                   outputs=[ct.TensorType(name='activations')], minimum_deployment_target=ct.target.macOS13,
                   compute_precision=ct.precision.FLOAT32)
model.short_description = 'Discogs-EffNet 400 music styles (MTG, CC BY-NC-SA 4.0)'
model.save(dst)
got = ct.models.MLModel(dst).predict({'melspectrogram': x.numpy()})['activations']
print('core ml vs onnx: max diff', float(np.abs(got - ref).max()))
