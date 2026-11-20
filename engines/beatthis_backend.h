// What beatthis.cpp needs from the platform's neural-network runtime: the Beat This! network itself. macOS has it
// on CoreML (beatthis_mac.mm); a Windows backend (e.g. ONNX Runtime / DirectML) defines the same three things.
#pragma once
#include "beatthis.h"

namespace beatthis {

constexpr int kMels = 128;   // the network's input: [frames][kMels] log-mel

// Model::load (declared in beatthis.h) is defined by the backend: loads the network at the path, checks it has
// one [chunk][kMels] input and beat / downbeat outputs, and sets impl_.

// The number of frames of the network's fixed input.
int chunkFrames(const Model::Impl& impl);
// Runs the network on `len` (<= chunkFrames) frames, zero-extended to its fixed length; writes `len` beat and
// downbeat logits. Serialised internally (may be called from several threads).
bool predictChunk(Model::Impl& impl, const float* mel, int len, float* beatOut, float* downOut);

}  // namespace beatthis
