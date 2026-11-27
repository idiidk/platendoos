// HTDemucs on ONNX Runtime (the backend of betterstems.cpp where there is no Core ML: Windows; see
// betterstems_backend.h). The model: tools/models/htdemucs_onnx.py -> models/htdemucs.onnx (fp32; inputs mix, mag;
// outputs spec, wave: the same split as the Core ML one).
//   config: betterstems_slow=0   Windows: 1: HTDemucs also where it is slow (on the CPU, or on a GPU that needs over a
//                                second for a segment); 0: rekordbox's own engine separates there
#include <algorithm>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>
#include <vector>
#include "betterstems_backend.h"
#include "../core/rbmod.h"
#include "../engines/ort.h"

using namespace rbm;

// Its settings (core/settings.h): what the settings window and the config file's list show of them.
namespace opt {
const Setting safe{"betterstems_safe", 0, {.label = "Safer GPU mode", .help = "Windows: for a graphics card that gives noisy stems.", .choices = {{"0", "Off"}, {"1", "No graph fusion"}, {"2", "Safe mode"}}, .restart = true, .advanced = true}};
}  // namespace opt

namespace betterstems_backend {

namespace {
std::shared_ptr<ort::Session> g_session;
std::string modelPath() { return plat::resourcesDir() + "/htdemucs.onnx"; }
}  // namespace

bool installed() { return std::filesystem::exists(plat::fsPath(modelPath())); }

// HTDemucs is used only where it is fast: on a GPU (DirectML) that separates a segment within kSlowSegment
// (betterstems_backend.h). Elsewhere this returns false and rekordbox's engine separates. betterstems_slow=1:
// HTDemucs anyway.

namespace {
const char* kOwn = "rekordbox's own engine separates (betterstems_slow=1 for HTDemucs anyway)";

const char* const kLevel[] = {"", " without graph fusion", " in DirectML's safe mode"};

// Opens the model; safe: ort.h's gpuSafe (1 without DirectML's graph fusion, 2 its safe mode). On a GPU: a segment of silence, twice when the first is
// slow (the first run also builds the GPU's pipeline, so the first track doesn't pay for it). It tells how fast this
// GPU is: over kSlowSegment (or on the CPU) null, unless betterstems_slow=1.
std::shared_ptr<ort::Session> openTimed(const std::string& path, bool gpu, bool anyway, int safe) {
    std::string err;
    double t = now();
    unsigned hw = std::thread::hardware_concurrency();
    // (the CPU threads: half the cores, so a playing deck and rekordbox's analysis keep the others)
    auto s = ort::open(path, (int)std::clamp<unsigned>(hw / 2, 2, 8), &err, gpu, !anyway || safe > 0, safe);
    if (!s) {
        if (gpu && (!anyway || safe) && err.rfind("no GPU session", 0) == 0)
            logf("betterstems: HTDemucs can't run on a GPU here%s (%s)%s%s", kLevel[safe], err.c_str(),
                 safe ? "" : ", and it is slow on the CPU: ", safe ? "" : kOwn);
        else
            logf("betterstems: loading the model failed: %s", err.c_str());
        return nullptr;
    }
    const auto &in = ort::inputNames(*s), &out = ort::outputNames(*s);
    if (in.size() != 2 || out.size() != 2 || in[0] != "mix" || in[1] != "mag" || out[0] != "spec" || out[1] != "wave") {
        logf("betterstems: the model has unexpected inputs/outputs");
        ort::close(*s);
        return nullptr;
    }
    std::string name = ort::gpuName();
    bool onGpu = ort::provider(*s).rfind("GPU", 0) == 0;
    logf("betterstems: HTDemucs loaded in %.1f s from %s on the %s%s%s", now() - t, path.c_str(),
         ort::provider(*s).c_str(), onGpu && !name.empty() ? ": " : "", onGpu ? name.c_str() : "");
    const auto &mixShape = ort::inputShape(*s, 0), &magShape = ort::inputShape(*s, 1);
    size_t mixN = 1, magN = 1;
    for (int64_t d : mixShape) mixN *= d > 0 ? (size_t)d : 0;
    for (int64_t d : magShape) magN *= d > 0 ? (size_t)d : 0;
    if (onGpu && mixN && magN) {
        std::vector<float> mix(mixN, 0.f), mag(magN, 0.f);
        std::vector<std::vector<float>> res;
        double took = 0;
        for (int i = 0; i < 2 && (i == 0 || took > kSlowSegment); i++) {
            double t0 = now();
            if (!ort::run(*s, {{"mix", mix.data(), mixShape}, {"mag", mag.data(), magShape}}, {"spec", "wave"}, res, &err)) {
                logf("betterstems: the model's first run failed: %s", err.c_str());
                took = 0;
                break;
            }
            took = i ? std::min(took, now() - t0) : now() - t0;
        }
        onGpu = ort::provider(*s).rfind("GPU", 0) == 0;  // (a run that fails on the GPU moves the session to the CPU)
        if (took > 0) logf("betterstems: a segment takes %.2f s on the %s", took, ort::provider(*s).c_str());
        if (!anyway && (!onGpu || took > kSlowSegment)) {
            logf("betterstems: HTDemucs is slow on this GPU%s (over %.1f s a segment): %s", kLevel[safe], kSlowSegment, kOwn);
            ort::close(*s);
            return nullptr;
        }
    }
    return s;
}

int g_level;  // the session's gpuSafe
}  // namespace

bool load(std::string* version) {
    std::string path = modelPath();
    std::error_code ec;
    auto size = std::filesystem::file_size(plat::fsPath(path), ec);
    if (ec) return false;
    bool gpu = setting("gpu").integer() != 0, anyway = setting("betterstems_slow").integer() != 0;
    if (!gpu && !anyway) {
        logf("betterstems: gpu=0, and HTDemucs is slow on the CPU: %s", kOwn);
        return false;
    }
    int safe = gpu ? std::clamp(opt::safe.integer(), 0, 2) : 0;
    auto s = openTimed(path, gpu, anyway, safe);
    if (!s) return false;
    g_level = safe;
    *version = "htdemucs-onnx-" + std::to_string((unsigned long long)size);
    g_session = s;
    return true;
}

// The GPU gave unusable stems. (One PC's did: noise ~100 dB over the mix, from DirectML's fused graph getting the
// model's Splits wrong; the model has none since, tools/models/htdemucs_onnx.py. This is for whatever else a GPU or
// its driver gets wrong.) First without DirectML's graph fusion, then DirectML's safe mode. (The stems' cache key
// stays the model's: the same weights, and correct stems are the same stems.)
int level() { return g_level; }
bool onGpu() { return g_session && ort::provider(*g_session).rfind("GPU", 0) == 0; }

bool safer(int than) {
    if (g_level > than) return g_session != nullptr;
    if (!g_session || g_level >= 2 || ort::provider(*g_session).rfind("GPU", 0) != 0) return false;
    logf("betterstems: trying the GPU%s", kLevel[g_level + 1]);
    auto s = openTimed(modelPath(), true, setting("betterstems_slow").integer() != 0, g_level + 1);
    if (!s) return false;
    ort::close(*g_session);
    g_session = s;
    g_level++;
    return true;
}

bool predict(const float* mix, int length, const float* mag, int bins, int frames, int sources,
             std::vector<float>& spec, std::vector<float>& wave, std::string* error) {
    std::vector<std::vector<float>> out;
    std::string err;
    std::string device = g_session ? ort::provider(*g_session) : "";
    bool ran = g_session && ort::run(*g_session, {{"mix", mix, {1, 2, length}}, {"mag", mag, {1, 4, bins, frames}}}, {"spec", "wave"}, out, &err);
    if (g_session && ort::provider(*g_session) != device) logf("betterstems: now on the %s", ort::provider(*g_session).c_str());
    if (!ran ||
        out[0].size() != (size_t)sources * 4 * bins * frames || out[1].size() != (size_t)sources * 2 * length) {
        *error = err.empty() ? "unexpected output" : err;
        return false;
    }
    spec = std::move(out[0]);
    wave = std::move(out[1]);
    return true;
}

void unload() {
    if (g_session) ort::close(*g_session);
    g_session.reset();
}

}  // namespace betterstems_backend
