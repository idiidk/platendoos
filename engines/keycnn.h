// keycnn: musical key recognition with the CNN of Korzeniowski & Widmer, "Genre-Agnostic Key
// Classification with Convolutional Neural Networks" (ISMIR 2018), as shipped in madmom 0.16.1
// (features/key.py, models/key/2018/key_cnn.pkl). Re-implemented with Accelerate; given the same
// 44.1 kHz mono signal it matches madmom's output to ~1e-7.
//
// Model file (keycnn.bin, exported by tools/models/export_keycnn.py): the log filterbank and the layers.
// Note: madmom's model files are licensed CC BY-NC-SA 4.0 (non-commercial use).
#pragma once
#include <string>
#include <vector>

namespace keycnn {

// Class order as madmom's KEY_LABELS, which is also rekordbox's key code order:
// 0 A, 1 Bb, 2 B, 3 C, 4 Db, 5 D, 6 Eb, 7 E, 8 F, 9 F#, 10 G, 11 Ab (major), 12..23 the same minor.
extern const char* const kKeyNames[24];

class Model {
public:
    bool load(const std::string& path, std::string* error = nullptr);
    bool loaded() const { return !layers_.empty(); }
    // mono: audio in [-1, 1] at sampleRate (resampled to 44.1 kHz internally). Returns 24 class
    // probabilities (empty if the audio is too short).
    std::vector<float> predict(const float* mono, size_t n, double sampleRate) const;

private:
    struct Layer {
        int type = 0;                   // 1 pad, 2 conv, 3 batchnorm(+elu), 4 maxpool 2x2, 5 average
        int pad = 0;
        int ci = 0, co = 0, kh = 0, kw = 0;
        std::vector<float> w;           // conv: [(a*kw+b)*ci + c][co]
        std::vector<float> scale, shift;
        bool elu = false;
    };
    int bands_ = 0, binLo_ = 0, binHi_ = 0;
    std::vector<float> filterbank_;     // [(bin - binLo) * bands + band]
    std::vector<Layer> layers_;
};

}  // namespace keycnn
