// Beat This! network on ONNX Runtime (the backend of beatthis.cpp where there is no Core ML: Windows; see
// beatthis_backend.h). The model: tools/models/export_beatthis_onnx.py -> models/beatthis.onnx, same fixed
// [1][1500][128] -> beat [1][1500], down [1][1500] network as the Core ML one.
#include "beatthis.h"
#include "beatthis_backend.h"
#include "ort.h"

#include <string.h>
#include <stdio.h>
#include <algorithm>
#include <chrono>
#include <mutex>
#include <thread>

namespace beatthis {

struct Model::Impl {
    std::shared_ptr<ort::Session> session;
    std::string inName, beatName, downName;
    int chunk = 1500;
    std::mutex mu;   // serialises predict calls (the threads are used inside one)
    double chunkTime = 0;   // s one chunk took on the GPU when the model was loaded (0: not measured)
};

static bool onGpu(const Model::Impl& I) { return ort::provider(*I.session).rfind("GPU", 0) == 0; }

std::string Model::device() const {
    if (!impl_) return "-";
    std::string d = ort::provider(*impl_->session);
    if (onGpu(*impl_) && impl_->chunkTime > 0) {
        char b[48];
        snprintf(b, sizeof b, ", %.2f s a chunk%s", impl_->chunkTime, impl_->chunkTime > kSlowChunk ? ": slow" : "");
        d += b;
    }
    return d;
}

bool Model::slow() const { return impl_ && (!onGpu(*impl_) || impl_->chunkTime > kSlowChunk); }

bool Model::load(const std::string& path, std::string* err, ComputeUnits units) {
    auto fail = [&](const std::string& m) { if (err) *err = m; impl_.reset(); return false; };
    auto impl = std::make_shared<Impl>();
    unsigned hw = std::thread::hardware_concurrency();
    std::string e;
    impl->session = ort::open(path, (int)std::clamp<unsigned>(hw / 2, 2, 8), &e, units != ComputeUnits::CpuOnly);
    if (!impl->session) return fail(e);
    const auto& ins = ort::inputNames(*impl->session);
    if (ins.size() != 1) return fail("model must have exactly one input");
    impl->inName = ins[0];
    const auto& shape = ort::inputShape(*impl->session, 0);
    if (shape.size() < 2 || shape.back() != kMels || shape[shape.size() - 2] <= 0) return fail("unexpected input shape");
    impl->chunk = (int)shape[shape.size() - 2];
    for (const std::string& k : ort::outputNames(*impl->session)) {
        std::string l = k;
        std::transform(l.begin(), l.end(), l.begin(), [](unsigned char c) { return (char)tolower(c); });
        if (l.find("down") != std::string::npos) impl->downName = k;
        else if (l.find("beat") != std::string::npos) impl->beatName = k;
    }
    if (impl->beatName.empty() || impl->downName.empty()) return fail("cannot identify beat/downbeat outputs");
    // On a GPU: how fast it is (an integrated one can be slower than the CPU). Not on the CPU (beatthis_backend.h).
    if (onGpu(*impl)) impl->chunkTime = firstChunkTime(*impl);
    impl_ = impl;
    return true;
}

int chunkFrames(const Model::Impl& impl) { return impl.chunk; }

bool predictChunk(Model::Impl& I, const float* mel, int len, float* beatOut, float* downOut) {
    std::vector<float> in((size_t)I.chunk * kMels, 0.f);
    memcpy(in.data(), mel, (size_t)len * kMels * sizeof(float));
    std::vector<std::vector<float>> out;
    {
        std::lock_guard<std::mutex> lk(I.mu);
        if (!ort::run(*I.session, {{I.inName.c_str(), in.data(), {1, I.chunk, kMels}}}, {I.beatName, I.downName}, out, nullptr))
            return false;
    }
    if ((int)out[0].size() < len || (int)out[1].size() < len) return false;
    memcpy(beatOut, out[0].data(), (size_t)len * sizeof(float));
    memcpy(downOut, out[1].data(), (size_t)len * sizeof(float));
    return true;
}

}  // namespace beatthis
