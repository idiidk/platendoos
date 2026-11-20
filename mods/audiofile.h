// Reading audio files with the platform's codecs: macOS has them in AudioToolbox (audiofile_mac.mm);
// a Windows backend (Media Foundation) defines the same functions.
#pragma once
#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

namespace audiofile {

// Sample rate and length in frames of an audio file (any format the OS reads).
bool facts(const std::string& path, int* rate, long long* frames);
// What a file's header says (AudioFile estimates the duration for VBR files).
struct Info {
    double seconds = 0;     // 0 if unknown
    double bitRate = 0;     // bits per second, 0 if unknown
    double sampleRate = 0;  // 0 if unknown
};
bool probe(const std::string& path, Info* out);
// Decodes a whole file to mono floats (-1..1) at its own sample rate. Any format the OS reads.
bool readMono(const std::string& path, std::vector<float>* out, int* rate);
// The same as 16-bit stereo, interleaved (L, R): for a 16-bit file, its exact samples.
bool readStereo16(const std::string& path, std::vector<int16_t>* out, int* rate);
// True when both files decode to exactly the same 16-bit stereo samples (and the same length and rate).
bool sameSamples(const std::string& a, const std::string& b);

}  // namespace audiofile
