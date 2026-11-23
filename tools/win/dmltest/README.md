# dmltest: DirectML without a Windows PC

rekordbox's own `onnxruntime.dll` (1.18, with the DirectML provider) and `DirectML.dll` run under Wine on a Mac:
Wine's Direct3D 12 (vkd3d) on MoltenVK on the Mac's GPU. That is enough to run the Windows models code
(`engines/ort.cpp`, `mods/betterstems.cpp`, `mods/betterstems_ort.cpp`) for real, GPU included, and it is one more
"GPU and driver" to compare with: the Split fault below shows there exactly as on the PC that reported it.

It says nothing about speed on a PC, about a GPU maker's own kernels (DirectML's "metacommands": Wine has none),
or about anything in rekordbox itself (for that: `tools/win/emu`, or a PC).

## Setting up

- Wine for macOS: a build from https://github.com/Gcenx/macOS_Wine_builds/releases (wine-devel 11.16 was used; an
  Apple Silicon Mac runs it under Rosetta). Unpack it anywhere, remove the quarantine flag
  (`xattr -dr com.apple.quarantine <folder>`), and set `WINE` to `<Wine app>/Contents/Resources/wine/bin/wine`.
- rekordbox's two DLLs: from an unpacked installer or a PC's rekordbox folder; `RB_DLLS` names the folder.
- `RB_DLLS=<folder> tools/win/dmltest/build.sh` builds `build/win/dmltest/harness.exe` and `probe.exe` (the
  toolchain of `tools/win/build.sh`). The first run under Wine makes a Wine prefix there (~350 MB).

## The programs

- **harness.exe**: the real betterstems code with a stand-in for the mods' core. It loads the model as rekordbox's
  launch does (the timed segment, the test segment), then separates a raw audio file chunk by chunk and prints the
  mod's log lines.
  `tools/win/dmltest/run.sh harness.exe 'Z:\<path to>\models' music.f32 out.f32 [gpu=0 betterstems_slow=1 ...]`
  (music.f32: `ffmpeg -i track -ac 2 -ar 44100 -f f32le music.f32` into `build/win/dmltest`). Stems on the CPU
  (`gpu=0 betterstems_slow=1`) are the reference to compare the GPU's with.
  `ort_fault.cpp` wraps the ONNX Runtime calls: `FAULT=normal|upto1|both` makes the "GPU" return noise in the
  normal mode / also without graph fusion / in every mode, `FAULT_AFTER=<n>` only from the n-th model run on
  (mid-track), so every step of betterstems' fallback (below) runs: `FAULT=upto1 FAULT_AFTER=6 ... run.sh
  harness.exe ... harness_threads=3 harness_chunk=200000`.
- **probe.exe**: one model with each DirectML setting on its own, every output compared with the CPU's:
  `run.sh probe.exe 'Z:\...\htdemucs.onnx' mix=in_mix.f32 mag=in_mag.f32 -- fused:path=legacy,opt=99
  nofusion:path=legacy,opt=99,fusion=0 safe:path=generic,opt=1,meta=0,fusion=0` (inputs: the harness writes them
  with `DUMP_INPUTS=in_`; without files the inputs are noise).
- **locate.py**: where in a model the GPU's numbers go wrong: the model with chosen intermediate tensors as its
  outputs, through probe.exe (`locate.py sample 16`, then `range` / `at`).

## What it found (2026-11-23)

A user's PC returned noise from HTDemucs through DirectML ("a source at 89.0 dB" for a mix at -11.3 dB), an RX 580
did not. Under Wine the same happens, and there it could be traced:

- With DirectML's **graph fusion** (the default: the whole model compiled as one DirectML graph) the output is
  wrong; without it (`ep.dml.disable_graph_fusion`) it is right, ~10% slower. ONNX Runtime's optimisation level and
  metacommands make no difference.
- The first wrong tensor is behind the first `Split`: of `a, b = Split(x)` (Demucs' GLU, `a * sigmoid(b)`), `a`
  gets `b`'s data, so the model computes `b * sigmoid(b)` (matches to 74 dB). When the Split's outputs are also
  the model's outputs they are right: it is the fused graph's handling of a Split that feeds other operators.
- The model with every Split's first output rewired to its second, run on the CPU, gives the GPU's wrong output to
  57-70 dB, and on music the same log line as the user's PC (-11.2 dB mix: a source at 88.1 dB, the sum off by
  83.0 dB; the user's: -11.3, 89.0, 84.0). So it is this fault on that PC too, as far as can be told from here.
- With the 48 Splits written as two `Slice`s each (`tools/models/htdemucs_onnx.py`), the fused graph is right, at
  the same speed. Beat This! and the genre model have no Split (and were right on both machines).
- rekordbox 7.2.19's and 7.2.20's DLLs behave the same (they are the same binaries but for their signatures:
  ONNX Runtime 1.18.0 at commit 45737400a2, DirectML 1.14.1).
- Others report the same: onnxruntime issues #21007 and #32146 (open; "DirectML EP miscomputes the Split ->
  Sigmoid -> Mul pattern emitted by nn.GLU", still there in onnxruntime-directml 1.24), and several stem-separation
  projects that also went to Slices, on cards from a GTX 745 to an RTX 3070 Ti. So it is not a matter of a weak or
  integrated GPU, and why some PCs are spared is not known.
- From ONNX Runtime 1.18's source (github.com/intel/onnxruntime, branch rel-1.18.0: the same commit) and the DLL's
  disassembly: the generic `SessionOptionsAppendExecutionProvider("DML", ...)` exists in this build; with
  `device_id` it picks the adapter exactly as the old `OrtSessionOptionsAppendExecutionProvider_DML` does (DXGI
  `EnumAdapters1`; without `device_id` it would go by DXCore's performance order); `disable_metacommands` takes
  only `True`/`true`/`False`/`false` and becomes `DML_EXECUTION_FLAG_DISABLE_META_COMMANDS` for single operators
  and fused graphs alike; graph fusion is on only while `ep.dml.disable_graph_fusion` is exactly "0", whatever
  the optimisation level (`ORT_ENABLE_BASIC` alone doesn't switch it off).

Which GPUs do this is not known (not the RX 580; the user's is not known). The check on every segment and the
fallback (`betterstems.cpp`: without graph fusion, then DirectML's safe mode, then rekordbox's own engine) stay,
for whatever else a GPU or driver gets wrong.
