# dmltest: which tensors of a model come out wrong from DirectML (under Wine), against the CPU: the model with chosen
# intermediate tensors as its outputs, through probe.exe. Needs onnx (pip) and run.sh's environment (README.md).
#   locate.py sample <n>            n evenly spaced float tensors as the model's outputs
#   locate.py at <i> [<i> ...]      these node indices' first outputs
#   locate.py range <lo> <hi> <n>   n tensors between node lo and hi
# environment: MODEL (default models/htdemucs.onnx), INPUTS ("mix=in_mix.f32 mag=in_mag.f32": files in the run
# folder, from the harness' DUMP_INPUTS), CFGS (probe configs, default "fused:path=legacy,opt=99").
# Note: a tensor that is an output is materialised, which can hide a fault of the fused graph (the Split one did
# hide when the Split's own outputs were picked): pick tensors downstream as well.
import sys, os, subprocess, re, onnx
from onnx import shape_inference, TensorProto, helper
here = os.path.dirname(os.path.abspath(__file__))
root = os.path.dirname(os.path.dirname(os.path.dirname(here)))
R = root + '/build/win/dmltest'
src = os.environ.get('MODEL', root + '/models/htdemucs.onnx')
m = onnx.load(src)
inf = shape_inference.infer_shapes(m)
vi = {v.name: v for v in list(inf.graph.value_info) + list(inf.graph.output)}
nodes = [n for n in m.graph.node]
cand = []  # (node index, tensor name) for float tensors made by non-Constant nodes
for i, n in enumerate(nodes):
    if n.op_type in ('Constant', 'Shape', 'ConstantOfShape'): continue
    t = n.output[0]
    if t in vi and vi[t].type.tensor_type.elem_type == TensorProto.FLOAT: cand.append((i, t))
def run(picks, cfgs):
    mm = onnx.load(src)
    del mm.graph.output[:]
    for i, t in picks: mm.graph.output.append(vi[t])
    out = R + '/sub.onnx'
    onnx.save(mm, out)
    p = subprocess.run([here + '/run.sh', 'probe.exe', 'sub.onnx'] + os.environ.get('INPUTS', 'mix=in_mix.f32 mag=in_mag.f32').split()
                       + ['--'] + cfgs, capture_output=True, text=True)
    res = {}
    for line in p.stdout.splitlines():
        if line.startswith('RESULT'):
            label = line.split()[1]
            parts = line.split(' | ')[1:]
            res[label] = [(re.match(r'(\S+): SNR (\S+) dB, max \|x\| (\S+)', x).groups()) for x in parts]
        elif 'ERROR' in line or line.startswith('  shape'): print(line)
    return res
mode = sys.argv[1]
cfgs = os.environ.get('CFGS', 'fused:path=legacy,opt=99').split()
if mode == 'sample':
    n = int(sys.argv[2]); picks = [cand[int(k * (len(cand) - 1) / (n - 1))] for k in range(n)]
elif mode == 'at':
    want = [int(x) for x in sys.argv[2:]]; picks = [c for c in cand if c[0] in want]
elif mode == 'range':
    lo, hi, n = int(sys.argv[2]), int(sys.argv[3]), int(sys.argv[4])
    cs = [c for c in cand if lo <= c[0] <= hi]; picks = [cs[int(k * (len(cs) - 1) / max(1, n - 1))] for k in range(min(n, len(cs)))]
    picks = sorted(set(picks))
print(len(cand), 'candidate tensors,', len(nodes), 'nodes')
res = run(picks, cfgs)
for label, r in res.items():
    print('==', label)
    for (i, t), (name, snr, mx) in zip(picks, r):
        print('%5d %-22s %-60s SNR %8s dB  max %s' % (i, nodes[i].op_type, t[:60], snr, mx))
