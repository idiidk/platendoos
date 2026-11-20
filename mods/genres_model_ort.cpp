// The genres mod's style model on ONNX Runtime (Windows; see genres_model.h): effnet.onnx next to rbmods.dll.
#include "genres_model.h"
#include "../core/rbmod.h"
#include "../engines/ort.h"
#include <algorithm>
#include <filesystem>
#include <thread>

using namespace rbm;

namespace genres_backend {
namespace {
std::shared_ptr<ort::Session> g_session;
}

bool load() {
    std::string path = plat::resourcesDir() + "/effnet.onnx", err;
    if (!std::filesystem::exists(plat::fsPath(path))) {
        logf("genres: no style model (%s)", path.c_str());
        return false;
    }
    double t = now();
    unsigned hw = std::thread::hardware_concurrency();
    // (on the CPU a run takes under half a second; half the cores at most, a deck may be playing)
    g_session = ort::open(path, (int)std::clamp<unsigned>(hw / 2, 1, 4), &err, cfgInt("gpu", 1) != 0);
    if (!g_session) {
        logf("genres: loading the style model failed: %s", err.c_str());
        return false;
    }
    logf("genres: style model loaded in %.1f s on the %s", now() - t, ort::provider(*g_session).c_str());
    return true;
}

bool predict(const float* mel, int batch, std::vector<float>& out, std::string* err) {
    std::vector<std::vector<float>> res;
    if (!g_session || !ort::run(*g_session, {{"melspectrogram", mel, {batch, 128, 96}}}, {"activations"}, res, err)) return false;
    out = std::move(res[0]);
    return true;
}

}  // namespace genres_backend
