// beatthis: beat + downbeat tracking with "Beat This!" (Foerster, Schlueter, Widmer et al., ISMIR 2024,
// MIT, github.com/CPJKU/beat_this, checkpoint final0). The network runs through CoreML; the
// preprocessing (resample to 22.05 kHz, STFT, mel, log1p), the chunking/aggregation of
// inference.split_predict_aggregate and the "minimal" postprocessor are re-implemented here with
// Accelerate so that the output matches the Python reference. Self-contained, no rekordbox deps.
//
// Threading: Model::analyze is const and may be called concurrently (preprocessing runs in
// parallel; the CoreML predict calls are serialised by an internal mutex). Model is copyable
// (shared state).
#pragma once
#include <cstddef>
#include <memory>
#include <string>
#include <vector>

namespace beatthis {

struct Result {
    std::vector<double> beats, downbeats;   // seconds from the start of the audio
};

// Constant-tempo grid fitted to a Result: beat k (k = 0,1,..) is at firstBeat + k * 60 / bpm; beat k
// is a downbeat iff k % 4 == downbeatIndex.
struct Grid {
    double bpm = 0;
    double firstBeat = 0;       // in [0, 60/bpm)
    int downbeatIndex = 0;      // 0..3
    double confidence = 0;      // fraction of detected beats within 70 ms of the grid
    bool ok = false;
};

enum class ComputeUnits { CpuOnly = 0, CpuAndGpu = 1, All = 2, CpuAndNeuralEngine = 3 };

// Optional intermediate data / timings, filled by Model::analyze when requested.
struct Debug {
    std::vector<float> mel;                 // [frames][128]
    int frames = 0;
    std::vector<float> beatLogits, downLogits;   // [frames] aggregated over the whole track
    double resampleMs = 0, melMs = 0, inferMs = 0, postMs = 0;
    int chunks = 0;
};

class Model {
public:
    // path: compiled model directory (.mlmodelc).
    bool load(const std::string& mlmodelcPath, std::string* err = nullptr,
              ComputeUnits units = ComputeUnits::CpuAndGpu);
    bool loaded() const { return impl_ != nullptr; }
    // Where it runs, for the log: "Core ML" (mac), or ONNX Runtime's "CPU" / "GPU (DirectML)" / "CPU (no GPU: ...)".
    std::string device() const;
    // The ONNX Runtime backend (Windows) only: the network is too slow here to make a DJ wait for it: it runs on the
    // CPU, or on a GPU that needed over a second for one chunk (30 s of audio) when the model was loaded.
    bool slow() const;
    // mono: audio at sampleRate (any rate; resampled to 22.05 kHz internally). Needs > 1 s of audio,
    // otherwise returns an empty Result.
    Result analyze(const float* mono, size_t n, double sampleRate, Debug* dbg = nullptr) const;
    // Run the network on a precomputed log-mel spectrogram [frames][128] (testing).
    Result analyzeMel(const float* mel, int frames, Debug* dbg = nullptr) const;
    struct Impl;   // opaque
private:
    std::shared_ptr<Impl> impl_;
};

// Building blocks (exposed for validation).
std::vector<float> resampleTo22k(const float* x, size_t n, double sampleRate);
// The same filter to any rate (autocue's style detection: 16 kHz).
std::vector<float> resample(const float* x, size_t n, double sampleRate, long outRate);
// x at 22050 Hz -> log1p(1000*mel) [frames][128], frames = 1 + n / 441 (torchaudio center=True).
std::vector<float> logMel(const float* x22k, size_t n, int* frames);
// minimal postprocessor on aggregated frame logits (50 fps).
Result postprocess(const float* beat, const float* down, int frames);

Grid fitGrid(const Result& r, double minBpm = 70, double maxBpm = 200);

}  // namespace beatthis
