// barphase: where in the bar the music changes, for bettergrid's bar start (header only; used by mods/bettergrid.cpp
// and tools/dev/barphase_test.cpp).
//
// Dance music changes at bar lines, and much more at phrase lines 8 or 16 bars apart (a drop, a breakdown, a new bass
// line): such a change starts on a 1. So: the music's spectrum in 12 bands per half beat, and at each half beat how
// different the music is before and after it, over 1, 2, 4 and 8 bars. Only the peaks of that count (a slow build
// raises every position alike), each scale's peaks normalised to sum to 1, added up per position in the bar (8 half
// beats). The idea is from rbxport's downbeat docs (github.com/chrisle/rbxport, crates/rbl-analysis/docs/algorithms/
// downbeat.md); this is our own implementation (no code from it).
//
// Two steps: bands() once per track on the decoded audio (12 band powers per ~23 ms frame), score() on a grid's
// beats. Measured on 597 tracks (2026-11-19): see bettergrid.cpp's barByNovelty for how it is used.
#pragma once
#include "../engines/dsp.h"
#include <math.h>
#include <stddef.h>
#include <algorithm>
#include <array>
#include <vector>

namespace barphase {

static const int kBands = 12;

struct Bands {
    double hop = 0;           // s between frames; frame i is centred on i * hop
    std::vector<float> power; // kBands per frame, normalised so the track's mean frame power (all bands) is 1
    size_t frames() const { return power.size() / kBands; }
};

// 12 log-spaced bands from 40 Hz to 10 kHz, a Hann window of ~46 ms every ~23 ms.
inline Bands bands(const float* x, size_t n, int rate) {
    Bands b;
    if (!x || rate <= 0 || n < (size_t)rate) return b;
    int log2n = 0;
    while ((1 << (log2n + 1)) <= rate * 0.0465 + 1) log2n++;  // 2048 at 44.1/48 kHz
    log2n = std::max(8, log2n);
    int len = 1 << log2n, hop = len / 2, half = len / 2;
    b.hop = (double)hop / rate;
    size_t frames = n / (size_t)hop + 1;
    double edge[kBands + 1];
    for (int k = 0; k <= kBands; k++) edge[k] = 40 * pow(10000.0 / 40, (double)k / kBands);
    int lo[kBands], hi[kBands];  // bins [lo, hi) per band
    for (int k = 0; k < kBands; k++) {
        lo[k] = std::max(1, (int)ceil(edge[k] * len / rate));
        hi[k] = std::min(half, (int)ceil(edge[k + 1] * len / rate));
        if (hi[k] <= lo[k]) hi[k] = std::min(half, lo[k] + 1);
    }
    std::vector<float> win((size_t)len), frame((size_t)len), re((size_t)half), im((size_t)half);
    for (int i = 0; i < len; i++) win[(size_t)i] = (float)(0.5 - 0.5 * cos(2 * M_PI * i / len));
    dsp::RealFFT fft(log2n);
    b.power.assign(frames * kBands, 0.f);
    double total = 0;
    for (size_t f = 0; f < frames; f++) {
        long start = (long)(f * (size_t)hop) - len / 2;  // centred on f * hop
        for (int i = 0; i < len; i++) {
            long j = start + i;
            frame[(size_t)i] = j >= 0 && (size_t)j < n ? x[j] * win[(size_t)i] : 0.f;
        }
        fft.forward(frame.data(), re.data(), im.data());
        float* p = &b.power[f * kBands];
        for (int k = 0; k < kBands; k++) {
            double s = 0;
            for (int i = lo[k]; i < hi[k]; i++) s += (double)re[(size_t)i] * re[(size_t)i] + (double)im[(size_t)i] * im[(size_t)i];
            p[k] = (float)s;
            total += s;
        }
    }
    double mean = total / (double)frames;
    if (mean > 0)
        for (float& v : b.power) v = (float)(v / mean);
    return b;
}

// The 8 half-beat positions of a 4-beat bar scored, position 0 at beats[0], 2 at beats[1]... (beats in s, rising).
// Empty (all 0) when there's too little to go by. *novelty4: the highest 4-bar novelty (for a gate).
inline std::array<double, 8> score(const Bands& b, const std::vector<double>& beats, double* novelty4 = nullptr) {
    std::array<double, 8> sc{};
    if (novelty4) *novelty4 = 0;
    size_t nb = beats.size(), F = b.frames();
    if (nb < 34 || F < 2 || !(b.hop > 0)) return sc;
    // half-beat edges: each beat and the midpoint after it; the last one a half beat past the last beat
    size_t H = 2 * nb - 1;
    std::vector<double> edges(H + 1);
    for (size_t i = 0; i < nb; i++) edges[2 * i] = beats[i];
    for (size_t i = 0; i + 1 < nb; i++) edges[2 * i + 1] = (beats[i] + beats[i + 1]) / 2;
    edges[H] = beats[nb - 1] + (beats[nb - 1] - beats[nb - 2]) / 2;
    auto frameAt = [&](double t) { return (size_t)std::min((double)F, std::max(0.0, ceil(t / b.hop))); };
    // the profile per half beat: log of the mean band power (floor 1e-4 of the track's mean frame power), as running sums
    std::vector<double> cum((H + 1) * kBands, 0.0);
    for (size_t h = 0; h < H; h++) {
        size_t a = frameAt(edges[h]), z = std::max(a + 1, frameAt(edges[h + 1]));
        for (int k = 0; k < kBands; k++) {
            double s = 0;
            size_t cnt = 0;
            for (size_t f = a; f < z && f < F; f++) s += b.power[f * kBands + (size_t)k], cnt++;
            double v = log(1e-4 + (cnt ? s / (double)cnt : 0));
            cum[(h + 1) * kBands + (size_t)k] = cum[h * kBands + (size_t)k] + v;
        }
    }
    std::vector<double> nov(H), peak(H);
    for (int scale : {1, 2, 4, 8}) {
        size_t L = (size_t)8 * scale;
        if (H < 2 * L + 2) continue;
        std::fill(nov.begin(), nov.end(), 0.0);
        for (size_t i = L; i + L <= H; i++) {
            double d2 = 0;
            for (int k = 0; k < kBands; k++) {
                double before = (cum[i * kBands + k] - cum[(i - L) * kBands + k]) / (double)L;
                double after = (cum[(i + L) * kBands + k] - cum[i * kBands + k]) / (double)L;
                d2 += (after - before) * (after - before);
            }
            nov[i] = sqrt(d2);
            if (scale == 4 && novelty4) *novelty4 = std::max(*novelty4, nov[i]);
        }
        double sum = 0;
        for (size_t i = L + 1; i + L < H; i++) {  // local maxima only
            peak[i] = nov[i] > 0 && nov[i] > nov[i - 1] && nov[i] >= nov[i + 1] ? nov[i] : 0;
            sum += peak[i];
        }
        if (!(sum > 0)) continue;
        for (size_t i = L + 1; i + L < H; i++) sc[i % 8] += peak[i] / sum;
    }
    return sc;
}

}  // namespace barphase
