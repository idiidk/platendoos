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
// The model gave unusable stems while it ran at safety level `than` (level() just before that predict): it is opened
// again one level safer where there is one (Windows, on the GPU: 1 without DirectML's graph fusion, 2 DirectML's safe
// mode), still within kSlowSegment. true if it now runs safer than `than` (also when another call got it there),
// false if there is nothing safer (the caller then unloads it). Not during a predict.
int level();
bool safer(int than);
// The model runs on a GPU (so a test segment at launch costs little).
bool onGpu();
// Releases the model after a load that succeeded (it is too slow here, or gave unusable stems). Not during a predict.
void unload();

// Windows and Intel Macs: HTDemucs is used only where it is fast: on a GPU that separates a segment (7.8 s of audio)
// within kSlowSegment at launch (betterstems_slow=1: anyway). On a CPU a segment takes 2.5-5 s on all the cores it
// gets (a track: two minutes of a 6-core PC at 75%, next to a playing deck), and an integrated GPU can be slower
// still; rekordbox's own engine does a whole track in 15-30 s.
constexpr double kSlowSegment = 1.0;  // s

}  // namespace betterstems_backend
