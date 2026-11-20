// What betterstems.cpp needs from the platform's neural-network runtime: the HTDemucs network (without its STFT /
// inverse STFT). macOS runs it on CoreML (betterstems_mac.mm); a Windows backend defines the same three functions.
#pragma once
#include <string>
#include <vector>

namespace betterstems_backend {

// The model is installed (so loading it is worth a try at launch).
bool installed();
// Loads the model (compiling it first if needed); blocking, called once. false if it can't be used; *version
// names the model's weights, so stems cached from another model aren't mixed up with these.
bool load(std::string* version);
// One segment: mix [1][2][length] and mag [1][4][bins][frames] (re, im of both channels) -> spec
// [sources][4][bins][frames] and wave [sources][2][length]. One call at a time.
bool predict(const float* mix, int length, const float* mag, int bins, int frames, int sources,
             std::vector<float>& spec, std::vector<float>& wave, std::string* error);

}  // namespace betterstems_backend
