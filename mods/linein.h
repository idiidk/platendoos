// linein: the audio inputs, per platform (mods/linein_mac.mm: CoreAudio; mods/linein_win.cpp: WASAPI). mods/linein.cpp
// is the rest: which deck plays what, rekordbox's side and the reading of the ring (resampler, control loop).
// Everything here is called on the main thread; a Capture's ring is written by the platform's capture thread and read
// by rekordbox's audio thread.
#pragma once
#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace linein {

const int kMaxChannels = 32;  // of an input device (the first ones are kept)
const int kRing = 1 << 15;    // frames: 0.7 s at 48 kHz

// A device with input channels, as the system lists it now.
struct Input {
    uint64_t id = 0;          // the system's handle for it (macOS: its AudioObjectID, which changes when it is plugged in again; Windows: 0)
    std::string name, uid;    // UTF-8; uid: CoreAudio's device UID, Windows' endpoint ID
    int channels = 0;         // input channels
    double rate = 0;          // its nominal rate (Windows: the shared-mode format's)
};

// One running input device. The capture thread is its only writer: interleaved float frames of `channels` channels
// into ring (kRing frames, the frame at w is ring[(w & (kRing - 1)) * channels]), then written += frames (release).
struct Capture {
    uint64_t id = 0;                       // Input::id it was opened from
    std::string uid, name;
    double rate = 0;                       // the rate its frames come at
    double listedRate = 0;                 // Input::rate when it was opened (a change in the list means: open it anew)
    int channels = 0;                      // input channels kept (in the ring)
    uint32_t block = 0;                    // its IO block, frames
    std::vector<float> ring;               // kRing frames, interleaved
    std::atomic<uint64_t> written{0};      // frames written since it started
    std::atomic<uint32_t> lastBlock{0};    // frames of its last callback
    std::atomic<bool> lost{false};         // it stopped delivering for good (unplugged): manage() lets go of it (Windows)
    bool started = false;
    double stoppedAt = 0;
    void* sys = nullptr;                   // the platform's own state
};

namespace sys {
// Every device with input channels (any order).
std::vector<Input> inputs();
// Whether the system lets rekordbox read audio inputs now; false: *why says what it waits for (macOS asks for the
// permission the first time).
bool allowed(std::string* why);
// Starts capturing the device: a Capture with its ring, rate, channels and block filled in and started set, or null
// with *err saying why ("its audio isn't in a format this can read", ...).
std::unique_ptr<Capture> open(const Input& in, std::string* err);
// Stops it and lets go of the platform's state (started false); its ring stays.
void stop(Capture* c);
}  // namespace sys

}  // namespace linein
