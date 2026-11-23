// A small float-tensor wrapper over the ONNX Runtime C API (onnxruntime_c_api.h, MIT), loaded at run time: the
// library is not linked. Windows: onnxruntime.dll from rekordbox's own folder (it ships 1.18); elsewhere
// $ORT_DYLIB_PATH or libonnxruntime.dylib/.so (for testing on a Mac with the Python wheel's copy). Used by the
// ONNX Runtime backends (beatthis_ort.cpp, betterstems_ort.cpp). CPU, or on Windows the GPU through DirectML
// (rekordbox's onnxruntime.dll is built with it).
#pragma once
#include <stdint.h>
#include <memory>
#include <string>
#include <vector>

namespace ort {

struct Session;   // opaque

// Opens the model (UTF-8 path) with `threads` intra-op threads. null (and *err) if the runtime or the model can't be loaded.
// gpu: on the GPU (DirectML) if the runtime has it and the model compiles for it, else on the CPU. gpuOnly: no CPU
// session instead: null, and *err says why there is no GPU one. gpuSafe, for a GPU that computes wrong results
// otherwise: 1: without DirectML's graph fusion (each operator runs on its own, ~10% slower: the fused graph is where
// wrong results were seen, see tools/models/htdemucs_onnx.py); 2: DirectML's safe mode: also without the GPU maker's
// own kernels ("metacommands"), and ONNX Runtime's fusions basic only.
std::shared_ptr<Session> open(const std::string& modelPath, int threads, std::string* err, bool gpu = false,
                              bool gpuOnly = false, int gpuSafe = 0);
// Where the session runs: "CPU", "GPU (DirectML)", "GPU (DirectML, no graph fusion)", "GPU (DirectML, safe mode)",
// or "CPU (no GPU: <why>)".
const std::string& provider(const Session&);
// Windows: DirectML's GPU (DXGI's first adapter), "<name>, driver <version>"; "" elsewhere or if unknown.
std::string gpuName();

// Releases every session (they are otherwise kept until the process ends); for test programs that exit through main.
// The sessions must not be used afterwards.
void shutdown();
// Releases this one session (and its memory on the GPU); it must not be used afterwards, by any thread.
void close(Session&);

const std::vector<std::string>& inputNames(const Session&);
const std::vector<std::string>& outputNames(const Session&);
// The input's declared shape (-1 for a dynamic dimension).
const std::vector<int64_t>& inputShape(const Session&, size_t index);

struct Input {
    const char* name;
    const float* data;
    std::vector<int64_t> shape;
};
// Runs the model on float inputs; out[i] gets the float output named outNames[i] (shape in outShapes, if given).
// Thread-safe (ONNX Runtime's Run is).
bool run(Session&, const std::vector<Input>& in, const std::vector<std::string>& outNames,
         std::vector<std::vector<float>>& out, std::string* err, std::vector<std::vector<int64_t>>* outShapes = nullptr);

}  // namespace ort
