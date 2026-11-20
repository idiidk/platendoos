// The genres mod's style detection: Discogs-EffNet (MTG / Essentia, CC BY-NC-SA 4.0) scores 400 Discogs styles from
// the audio. Backends: genres_model_mac.mm (Core ML, models/effnet.mlmodelc), genres_model_ort.cpp (ONNX Runtime,
// effnet.onnx). Other mods use it through the genres mod's services (genres.h), not directly.
#pragma once
#include "genres.h"
#include <string>
#include <vector>

namespace genres {

bool modelAvailable();  // the model is there and loads (loads it the first time; any thread)
// mono at rate (any). False if there's under ~3 s of audio or the model can't run. Any thread (the model runs one
// batch at a time). maxPatches > 0: listens to at most that many 2 s patches spread over the track (not its first and
// last tenth) instead of one every second, to bound the cost on long tracks.
bool detect(const std::vector<float>& mono, int rate, Style* out, int maxPatches = 0);

}  // namespace genres

// The backend: one batch of patches [batch][128 frames][96 mel bands] -> [batch][400] style activations.
namespace genres_backend {
bool load();
bool predict(const float* mel, int batch, std::vector<float>& out, std::string* err);
}
