// autocue's arrangement analysis (header-only, no rekordbox in it): a C++ port of the autocue-test project's
// autocue.py. Each part of the track (the four stems, or three bands of the mix without stems) is cut into bars on
// the beat grid, and every bar is matched against every other: bars that are copies of each other get the same
// clip letter. Where the parts switch on, off or to another clip are the section changes; how loud the mix and its
// low end are says which sections are drops. From that come rule-based cues (A intro .. H fake drop) and a "sheet":
// the track as text, for a language model that places the cues (the DJ's own instructions on top; see autocue.cpp).
//   analyse(mix, stems, rate 22050, beats, duration) -> Analysis
#pragma once
#include "../engines/dsp.h"
#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <climits>
#include <cstring>
#include <functional>
#include <map>
#include <numeric>
#include <set>
#include <string>
#include <vector>
#include <stdio.h>

namespace autocue_sheet {

constexpr int SR = 22050, NFFT = 2048, HOP = 256, NB = 64, SUB = 16, BPB = 4;
constexpr double FPS = (double)SR / HOP;
constexpr int INACTIVE = 0, UNIQUE = -1;
enum { DRUMS, BASS, OTHER, VOCALS, NSTEMS };

inline const char* const* slotNames() {  // A..H
    static const char* const n[] = {"Intro", "Build", "Drop", "Breakdown", "Build 2", "Drop 2", "Outro", "Fake drop"};
    return n;
}

struct Cue { char slot; int bar; std::string why; };  // bar: 0-based here, 1-based in the sheet and to the model
struct Section { int start, end; bool high; };
struct Lane { int start, end, id; std::string label; };  // id: -2 silent, -1 never repeats, else its letter's number
struct Analysis {
    int bars = 0;
    double bpm = 0;
    bool stems = false;
    int downMoved = 0;                // beats the bar line was moved from rekordbox's (0: kept)
    std::vector<double> barTimes;     // seconds, bars + 1
    std::vector<Cue> cues;            // the rules' cues
    std::vector<Section> sections;
    std::vector<Lane> lanes[NSTEMS];  // clip blocks per part
    std::vector<float> amp, hi;       // the waveform: 600 columns, 0..1
    double duration = 0;              // of the audio, seconds (the waveform's width)
    double copiedShare = 0;           // bars with a near-identical twin: < 0.5 is not a pasted-together track
    std::string sheet;
};

namespace detail {

inline double pct(std::vector<double> v, double q) {  // numpy's percentile (linear)
    if (v.empty()) return 0;
    double k = (v.size() - 1) * q / 100;
    size_t lo = (size_t)std::floor(k), hi = std::min(lo + 1, v.size() - 1);
    std::nth_element(v.begin(), v.begin() + lo, v.end());
    double a = v[lo];
    if (hi == lo) return a;
    double b = *std::min_element(v.begin() + lo + 1, v.end());
    return a + (b - a) * (k - lo);
}
template <class It> double pctOf(It a, It b, double q) { return pct(std::vector<double>(a, b), q); }
inline double meanOf(const std::vector<double>& v, int a, int b) {
    a = std::max(a, 0), b = std::min<int>(b, (int)v.size());
    if (b <= a) return NAN;
    double s = 0;
    for (int i = a; i < b; i++) s += v[i];
    return s / (b - a);
}
inline std::vector<double> sliceOf(const std::vector<double>& v, int a, int b) {
    a = std::max(a, 0), b = std::min<int>(b, (int)v.size());
    return b > a ? std::vector<double>(v.begin() + a, v.begin() + b) : std::vector<double>();
}

struct Run { int a, b, v; };
inline std::vector<Run> runs(const std::vector<int>& v) {
    std::vector<Run> out;
    int s = 0;
    for (int i = 1; i <= (int)v.size(); i++)
        if (i == (int)v.size() || v[i] != v[s]) out.push_back({s, i, v[s]}), s = i;
    return out;
}

inline const std::vector<double>& bandEdges() {
    static std::vector<double> e = [] {
        std::vector<double> e(NB + 1);
        for (int i = 0; i <= NB; i++) e[i] = 35 * std::pow(10000.0 / 35, (double)i / NB);
        return e;
    }();
    return e;
}
inline double bandCenter(int b) { return std::sqrt(bandEdges()[b] * bandEdges()[b + 1]); }
// FFT bin -> band (or -1), as autocue.py's band_matrix: the bins in [lo, hi), else the bin nearest the centre.
inline const std::vector<std::vector<int>>& bandBins() {
    static std::vector<std::vector<int>> m = [] {
        std::vector<std::vector<int>> m(NB);
        const auto& e = bandEdges();
        for (int b = 0; b < NB; b++) {
            for (int k = 0; k <= NFFT / 2; k++) {
                double f = (double)k * SR / NFFT;
                if (f >= e[b] && f < e[b + 1]) m[b].push_back(k);
            }
            if (m[b].empty()) {
                double c = std::sqrt(e[b] * e[b + 1]);
                int best = 0;
                for (int k = 1; k <= NFFT / 2; k++)
                    if (std::fabs((double)k * SR / NFFT - c) < std::fabs((double)best * SR / NFFT - c)) best = k;
                m[b].push_back(best);
            }
        }
        return m;
    }();
    return m;
}

// Band power spectrogram [frames][NB] (torch.stft: centred, reflect padding, periodic Hann).
inline std::vector<float> spec(const std::vector<float>& x, int* frames) {
    static dsp::RealFFT fft(11);
    static std::vector<float> win = [] {
        std::vector<float> w(NFFT);
        for (int i = 0; i < NFFT; i++) w[i] = (float)(0.5 - 0.5 * std::cos(2 * 3.14159265358979323846 * i / NFFT));
        return w;
    }();
    int n = (int)x.size(), nf = 1 + n / HOP;
    *frames = nf;
    std::vector<float> out((size_t)nf * NB, 0.f);
    if (n < NFFT / 2 + 1) return out;
    std::vector<float> buf(NFFT), re(NFFT / 2), im(NFFT / 2), p(NFFT / 2 + 1);
    const auto& bins = bandBins();
    for (int t = 0; t < nf; t++) {
        int c = t * HOP;
        for (int i = 0; i < NFFT; i++) {
            int j = c - NFFT / 2 + i;
            if (j < 0) j = -j;
            if (j >= n) j = 2 * (n - 1) - j;
            buf[i] = x[std::clamp(j, 0, n - 1)] * win[i];
        }
        fft.forward(buf.data(), re.data(), im.data());  // (scaled by 2)
        p[0] = re[0] * re[0] / 4, p[NFFT / 2] = im[0] * im[0] / 4;
        for (int k = 1; k < NFFT / 2; k++) p[k] = (re[k] * re[k] + im[k] * im[k]) / 4;
        float* o = &out[(size_t)t * NB];
        for (int b = 0; b < NB; b++) {
            double s = 0;
            for (int k : bins[b]) s += p[k];
            o[b] = (float)s;
        }
    }
    return out;
}

// Time-normalised spectrogram: [beats - 1][SUB][NB] mean power (autocue.py's beat_features).
inline std::vector<float> beatFeatures(const std::vector<float>& S, int frames, const std::vector<double>& beats) {
    int nb = (int)beats.size();
    if (nb < 2) return {};
    int ne = (nb - 1) * SUB + 1;
    std::vector<double> pos(ne);
    for (int e = 0; e < ne; e++) {
        double k = (double)e / SUB;
        int i = std::min((int)k, nb - 2);
        double t = beats[i] + (k - i) * (beats[i + 1] - beats[i]);
        pos[e] = std::clamp(t * FPS + 0.5, 0.0, (double)frames);
    }
    std::vector<double> cs((size_t)(frames + 1) * NB, 0.0);
    for (int f = 0; f < frames; f++)
        for (int b = 0; b < NB; b++) cs[(size_t)(f + 1) * NB + b] = cs[(size_t)f * NB + b] + S[(size_t)f * NB + b];
    std::vector<double> c((size_t)ne * NB);
    for (int e = 0; e < ne; e++) {
        int i = std::min((int)std::floor(pos[e]), frames - 1);
        double fr = pos[e] - i;
        for (int b = 0; b < NB; b++) {
            double a = cs[(size_t)i * NB + b], d = cs[(size_t)(i + 1) * NB + b];
            c[(size_t)e * NB + b] = a + fr * (d - a);
        }
    }
    std::vector<float> out((size_t)(ne - 1) * NB);
    for (int e = 0; e + 1 < ne; e++) {
        double w = std::max(pos[e + 1] - pos[e], 1e-6);
        for (int b = 0; b < NB; b++) out[(size_t)e * NB + b] = (float)((c[(size_t)(e + 1) * NB + b] - c[(size_t)e * NB + b]) / w);
    }
    return out;
}

// Per beat phase: how much spectral change lands on it (a tie-breaker for the bar line).
inline std::vector<double> downbeatVotes(const std::vector<float>* bf, int nBeats) {
    std::vector<double> votes(BPB, 0.0);
    for (int s = 0; s < NSTEMS; s++) {
        const std::vector<float>& F = bf[s];
        int n = nBeats;
        std::vector<double> P((size_t)n * NB, 0.0);
        double mx = 0;
        for (int i = 0; i < n; i++)
            for (int b = 0; b < NB; b++) {
                double v = 0;
                for (int k = 0; k < SUB; k++) v += F[((size_t)i * SUB + k) * NB + b];
                P[(size_t)i * NB + b] = v / SUB, mx = std::max(mx, v / SUB);
            }
        std::vector<double> cs((size_t)(n + 1) * NB, 0.0);
        for (int i = 0; i < n; i++)
            for (int b = 0; b < NB; b++) cs[(size_t)(i + 1) * NB + b] = cs[(size_t)i * NB + b] + std::log1p(1e4 * P[(size_t)i * NB + b] / (mx + 1e-12));
        for (int w : {4, 8}) {
            std::vector<int> bs;
            std::vector<double> nov;
            for (int b = w; b < n - w; b++) {
                double d = 0;
                for (int k = 0; k < NB; k++) {
                    double nx = (cs[(size_t)(b + w) * NB + k] - cs[(size_t)b * NB + k]) / w, pv = (cs[(size_t)b * NB + k] - cs[(size_t)(b - w) * NB + k]) / w;
                    d += (nx - pv) * (nx - pv);
                }
                bs.push_back(b), nov.push_back(std::sqrt(d));
            }
            if (nov.empty()) continue;
            double p75 = pct(nov, 75);
            size_t m = nov.size();
            for (size_t i = 0; i < m; i++) {
                double prev = nov[(i + m - 1) % m], next = nov[(i + 1) % m];
                if (nov[i] > prev && nov[i] >= next && nov[i] > p75) votes[bs[i] % BPB] += nov[i] * nov[i];
            }
        }
    }
    double mx = *std::max_element(votes.begin(), votes.end());
    for (double& v : votes) v /= mx + 1e-12;
    return votes;
}

// Cosine similarity between bars of one part. bars: [n][BPB * SUB][NB] power.
inline std::vector<float> barSimilarity(const float* bars, int n, const std::vector<bool>& active, double ref) {
    const int D = BPB * SUB * NB;
    std::vector<float> X((size_t)n * D);
    for (size_t i = 0; i < X.size(); i++) X[i] = (float)(std::max(10 * std::log10(bars[i] / ref + 1e-12) + 50, 0.0) / 50);
    int na = 0;
    for (int i = 0; i < n; i++) na += active[i];
    if (na >= 2) {  // remove the part's average timbre (per band)
        std::vector<double> m(NB, 0.0);
        for (int i = 0; i < n; i++)
            if (active[i])
                for (int f = 0; f < BPB * SUB; f++)
                    for (int b = 0; b < NB; b++) m[b] += X[(size_t)i * D + f * NB + b];
        for (double& v : m) v /= (double)na * BPB * SUB;
        for (int i = 0; i < n; i++)
            for (int f = 0; f < BPB * SUB; f++)
                for (int b = 0; b < NB; b++) X[(size_t)i * D + f * NB + b] -= (float)m[b];
    }
    for (int i = 0; i < n; i++) {
        double s = 0;
        for (int k = 0; k < D; k++) s += (double)X[(size_t)i * D + k] * X[(size_t)i * D + k];
        float inv = (float)(1 / (std::sqrt(s) + 1e-9));
        for (int k = 0; k < D; k++) X[(size_t)i * D + k] *= inv;
    }
    std::vector<float> XT((size_t)D * n), S((size_t)n * n);
    for (int i = 0; i < n; i++)
        for (int k = 0; k < D; k++) XT[(size_t)k * n + i] = X[(size_t)i * D + k];
    dsp::sgemm(n, n, D, X.data(), XT.data(), S.data());
    for (int i = 0; i < n; i++)
        for (int j = 0; j < n; j++)
            if (!active[i] || !active[j]) S[(size_t)i * n + j] = 0;
    return S;
}

inline double otsu(const std::vector<float>& S, int n, double lo, double hi) {
    std::vector<double> v;
    for (int i = 0; i < n; i++)
        for (int j = i + 1; j < n; j++)
            if (S[(size_t)i * n + j] > 0.2f) v.push_back(S[(size_t)i * n + j]);
    if (v.size() < 20) return (lo + hi) / 2;
    double hist[80] = {};
    double tot = 0;
    for (double x : v) {
        if (x > 1.0) continue;  // (numpy's histogram leaves out what's past its range)
        int b = std::min(79, (int)((x - 0.2) / 0.8 * 80));
        hist[b]++, tot++;
    }
    double w0 = 0, m0 = 0, mids[80], var[80], mt = 0;
    for (int i = 0; i < 80; i++) mids[i] = 0.2 + 0.01 * i + 0.005, mt += hist[i] / tot * mids[i];
    for (int i = 0; i < 80; i++) {
        w0 += hist[i] / tot, m0 += hist[i] / tot * mids[i];
        double d = w0 * (1 - w0), x = (mt * w0 - m0) * (mt * w0 - m0) / d;
        var[i] = std::isfinite(x) ? x : 0;
    }
    int best = (int)(std::max_element(var, var + 80) - var);
    return std::clamp(mids[best], lo, hi);
}

// Cluster bars idx so every cluster's mean pairwise similarity stays >= thr: a cluster number per bar of idx.
inline std::vector<int> averageLinkage(const std::vector<float>& S, int n, const std::vector<int>& idx, double thr) {
    int m = (int)idx.size();
    std::vector<int> lab(m);
    std::iota(lab.begin(), lab.end(), 0);
    if (!m) return lab;
    std::vector<double> D((size_t)m * m), size(m, 1.0);
    for (int a = 0; a < m; a++)
        for (int b = 0; b < m; b++) D[(size_t)a * m + b] = a == b ? -1 : S[(size_t)idx[a] * n + idx[b]];
    std::vector<double> nw(m);
    for (;;) {
        size_t f = std::max_element(D.begin(), D.end()) - D.begin();
        int a = (int)(f / m), b = (int)(f % m);
        if (D[f] < thr) break;
        for (int x = 0; x < m; x++) nw[x] = (D[(size_t)a * m + x] * size[a] + D[(size_t)b * m + x] * size[b]) / (size[a] + size[b]);
        for (int x = 0; x < m; x++) D[(size_t)a * m + x] = nw[x], D[(size_t)x * m + a] = nw[x];
        D[(size_t)a * m + a] = -1;
        for (int x = 0; x < m; x++) D[(size_t)b * m + x] = -1, D[(size_t)x * m + b] = -1;
        size[a] += size[b];
        for (int& l : lab)
            if (l == b) l = a;
    }
    return lab;
}

// Per bar: 0 silent, -1 never repeats, > 0 its material (shared by copies).
inline std::vector<int> labelStem(const std::vector<float>& S, int n, const std::vector<bool>& active, double thr) {
    std::vector<int> idx;
    for (int i = 0; i < n; i++)
        if (active[i]) idx.push_back(i);
    std::vector<int> cl = averageLinkage(S, n, idx, thr);
    std::vector<int> types(n, -1);
    for (size_t k = 0; k < idx.size(); k++) types[idx[k]] = cl[k];
    auto C = [&](int i, int j) { return i != j && S[(size_t)i * n + j] >= thr; };
    std::map<int, int> parent;
    std::function<int(int)> find = [&](int x) {
        for (;;) {
            auto it = parent.find(x);
            if (it == parent.end()) { parent[x] = x; return x; }
            if (it->second == x) return x;
            x = it->second;
        }
    };
    // bars i and i + 1 belong to one looping clip if the pair repeats at a short lag
    std::vector<std::pair<std::pair<int, int>, int>> links;
    for (int i = 0; i + 1 < n; i++) {
        if (!(active[i] && active[i + 1]) || types[i] == types[i + 1]) continue;
        for (int lag : {1, 2, 4, 8, -1, -2, -4, -8}) {
            int j = i + lag;
            if (0 <= j && j + 1 < n && C(i, j) && C(i + 1, j + 1)) {
                std::pair<int, int> k = std::minmax(types[i], types[i + 1]);
                auto it = std::find_if(links.begin(), links.end(), [&](auto& l) { return l.first == k; });
                if (it == links.end()) links.push_back({k, 1});
                else it->second++;
                break;
            }
        }
    }
    std::vector<int> counts(n + 1, 0);
    for (int i : idx) counts[types[i]]++;
    for (auto& [k, c] : links)
        if (c >= 2 || c >= 0.5 * std::min(counts[k.first], counts[k.second])) parent[find(k.first)] = find(k.second);
    std::map<int, std::vector<int>> groups;
    for (int i : idx) groups[find(types[i])].push_back(i);
    std::vector<std::vector<int>> gs;
    for (auto& g : groups) gs.push_back(g.second);
    std::sort(gs.begin(), gs.end(), [](const auto& a, const auto& b) { return a[0] < b[0]; });
    std::vector<int> out(n, 0);
    int nxt = 1;
    for (auto& g : gs) {
        if (g.size() == 1) out[g[0]] = UNIQUE;
        else {
            for (int i : g) out[i] = nxt;
            nxt++;
        }
    }
    // a lone odd bar inside a block (drum fill, edit) doesn't split the block
    for (int i = 1; i + 1 < n; i++)
        if (out[i] != out[i - 1] && out[i - 1] == out[i + 1] && out[i - 1] > 0 && out[i] != INACTIVE) out[i] = out[i - 1];
    // drop materials that only ever appear as scattered single bars
    for (int m = 1; m < nxt; m++) {
        std::vector<int> w;
        for (int i = 0; i < n; i++)
            if (out[i] == m) w.push_back(i);
        bool adj = false;
        for (size_t k = 1; k < w.size(); k++) adj |= w[k] - w[k - 1] == 1;
        if (!w.empty() && !adj && w.size() < 3)
            for (int i : w) out[i] = UNIQUE;
    }
    return out;
}

struct Levels { std::vector<double> mix, low, st[NSTEMS]; };

inline std::string fmt(const char* f, ...) __attribute__((format(printf, 1, 2)));
inline std::string fmt(const char* f, ...) {
    char b[512];
    va_list ap;
    va_start(ap, f);
    vsnprintf(b, sizeof b, f, ap);
    va_end(ap);
    return b;
}
inline std::string join(const std::vector<std::string>& v, const char* sep) {
    std::string o;
    for (size_t i = 0; i < v.size(); i++) o += (i ? sep : "") + v[i];
    return o;
}

// The rules' cues and the sections (autocue.py's find_cues).
inline void findCues(const std::vector<int>* labels, const Levels& lv, const char* const* names, std::vector<Cue>* cuesOut, std::vector<Section>* sectionsOut) {
    int n = (int)lv.mix.size();
    std::vector<double> L(n), E(n), st[NSTEMS];
    for (int i = 0; i < n; i++) L[i] = std::max(lv.mix[i], -30.0), E[i] = std::max(lv.low[i], -30.0);
    for (int s = 0; s < NSTEMS; s++) {
        st[s].resize(n);
        for (int i = 0; i < n; i++) st[s][i] = std::max(lv.st[s][i], -24.0);
    }
    std::map<char, Cue> cues;
    auto put = [&](char slot, int bar, const std::string& why) { cues[slot] = {slot, bar, why}; };
    auto finish = [&] {
        cuesOut->clear();
        for (auto& c : cues) cuesOut->push_back(c.second);  // (std::map: A..H)
    };
    int first = 0;
    while (first < n && !(L[first] > -30)) first++;
    if (first == n) first = 0;
    put('A', first, "first bar with sound");
    if (n < 16) return finish();

    auto jump = [&](const std::vector<double>& v, int w) {
        std::vector<double> out(n, 0.0);
        for (int b = 1; b < n; b++) out[b] = meanOf(v, b, b + w) - meanOf(v, b - w, b);
        return out;
    };
    auto changed = [&](int s, int b) {
        const std::vector<int>& l = labels[s];
        if (b < 1 || b >= (int)l.size()) return false;  // (bar 0 has no bar before it)
        if (l[b] == l[b - 1] || std::max(st[s][b], st[s][b - 1]) <= -24) return false;
        return (b + 1 < n && l[b + 1] == l[b]) || (b >= 2 && l[b - 2] == l[b - 1]);
    };
    std::vector<double> jumps[NSTEMS], sharp[NSTEMS];
    for (int s = 0; s < NSTEMS; s++) {
        jumps[s] = jump(st[s], 4);
        std::vector<double> j2 = jump(st[s], 2);
        sharp[s].resize(n);
        for (int i = 0; i < n; i++) sharp[s][i] = 0.5 * (jumps[s][i] + j2[i]);
    }
    std::vector<double> jL = jump(L, 4), jL2 = jump(L, 2), sharpL(n), nov(n);
    for (int i = 0; i < n; i++) sharpL[i] = 0.5 * (jL[i] + jL2[i]);
    for (int i = 0; i < n; i++) {
        nov[i] = 1.5 * std::min(std::fabs(sharpL[i]), 9.0) / 9;
        for (int s = 0; s < NSTEMS; s++) nov[i] += std::min(std::fabs(sharp[s][i]), 12.0) / 12;
    }
    for (int b = 1; b < n; b++) {
        int k = 0;
        for (int s = 0; s < NSTEMS; s++) k += changed(s, b);
        nov[b] += 0.5 * k;
    }
    // producers arrange in 8-bar phrases: where this track's phrases start, from where repeating clips start
    std::vector<double> edge(n + 1, 0.0), starts(n + 1, 0.0);
    for (int s = 0; s < NSTEMS; s++) {
        double wgt = s == DRUMS || s == BASS ? 1.5 : 1.0;
        for (const Run& r : runs(labels[s]))
            if (r.v > 0 && r.b - r.a >= 4 && meanOf(st[s], r.a, r.b) > -22) edge[r.a] += wgt, edge[r.b] += 0.3 * wgt, starts[r.a] += wgt;
    }
    std::vector<double> onset(n);
    for (int i = 0; i < n; i++) {
        double o = std::max(sharpL[i], 0.0) / 9;
        for (int s = 0; s < NSTEMS; s++) o += std::max(sharp[s][i], 0.0) / 12;
        onset[i] = o;
    }
    double votes[8] = {};
    for (int p = 0; p < 8; p++)
        for (int i = p; i < n; i += 8) votes[p] += starts[i];
    if (*std::max_element(votes, votes + 8) < 3)
        for (int p = 0; p < 8; p++) {
            votes[p] = 0;
            for (int i = p; i < n; i += 8) votes[p] += onset[i];
        }
    int phase = (int)(std::max_element(votes, votes + 8) - votes);
    std::vector<double> score(n);
    for (int i = 0; i < n; i++) score[i] = nov[i] + 0.75 * (i % 8 == phase) + 0.25 * (i % 4 == phase % 4);
    std::vector<int> bounds{0};
    for (int b = 2; b < n - 1; b++) {
        double mx = -1e18;
        for (int k = std::max(0, b - 2); k < std::min(n, b + 3); k++) mx = std::max(mx, score[k]);
        if (nov[b] >= 0.9 && score[b] >= mx && b - bounds.back() >= 2) bounds.push_back(b);
    }
    std::vector<int> snapped{0};
    for (size_t k = 1; k < bounds.size(); k++) {
        int b = bounds[k], c = -1;
        double best = -1e18;
        for (int x = std::max(1, b - 2); x <= std::min(n - 1, b + 2); x++) {
            double v = edge[x] + 1.0 * (x % 8 == phase) + 0.2 * (x % 4 == phase % 4) + 0.1 * (x == b);
            if (v > best) best = v, c = x;
        }
        if (edge[c] > 0 && c > snapped.back() + 1) snapped.push_back(c);
        else if (b > snapped.back() + 1) snapped.push_back(b);
    }
    bounds = snapped;
    bounds.push_back(n);
    auto isBound = [&](int x) { return std::find(bounds.begin(), bounds.end(), x) != bounds.end(); };

    double lTop = pct(L, 85), eTop = pct(E, 85), dTop = pct(st[DRUMS], 85);
    bool hasDrums = dTop > -24;
    auto isHigh = [&](int a, int b) {
        return pct(sliceOf(L, a, b), 50) >= lTop - 4.5 && pct(sliceOf(E, a, b), 70) >= eTop - 8 && (!hasDrums || pct(sliceOf(st[DRUMS], a, b), 70) >= dTop - 8);
    };
    std::vector<Section> sections;
    for (size_t k = 0; k + 1 < bounds.size(); k++) sections.push_back({bounds[k], bounds[k + 1], isHigh(bounds[k], bounds[k + 1])});
    *sectionsOut = sections;
    std::vector<int> high(n, 0);
    for (const Section& s : sections)
        for (int i = s.start; i < s.end; i++) high[i] = s.high;
    std::vector<std::pair<int, int>> regions;
    for (int minLen : {8, 4}) {
        for (const Run& r : runs(high))
            if (r.v && r.b - r.a >= minLen) regions.push_back({r.a, r.b});
        if (!regions.empty()) break;
    }
    auto loudBar = [&](int i) { return L[i] >= lTop - 4.5 && E[i] >= eTop - 8; };
    auto anyHigh = [&](int a, int b) {
        for (int i = std::max(a, 0); i < std::min(b, n); i++)
            if (high[i]) return true;
        return false;
    };
    for (auto& r : regions) {  // a drop that thins out for its last bars still ends on the phrase
        int a = r.first, b = r.second, g = b + (((phase - b) % 8) + 8) % 8, quiet = 0;
        for (int i = b; i < std::min(g, n); i++) quiet += L[i] < lTop - 8;
        if (g != b && g - b <= 6 && quiet <= 2 && isBound(g) && g < n && nov[g] >= 2 && !anyHigh(g, g + 4)) { r = {a, g}; continue; }
        for (int k = 0; k < 2; k++)
            if (b < n && loudBar(b) && !high[b]) b++;
        while (b - a > 8 && L[b - 1] < lTop - 8 && nov[std::min(b, n - 1)] < 2) b--;
        for (int k = 0; k < 2; k++)
            if (b - a > 8 && !loudBar(a) && loudBar(a + 1) && L[a] < lTop - 8) a++;
        r = {a, b};
    }
    if (regions.empty()) return finish();

    auto whatChanges = [&](int b) {
        std::vector<std::string> ins, outs, sw, parts;
        for (int s = 0; s < NSTEMS; s++) {
            if (jumps[s][b] >= 6) ins.push_back(names[s]);
            if (jumps[s][b] <= -6) outs.push_back(names[s]);
            if (changed(s, b) && std::fabs(jumps[s][b]) < 6) sw.push_back(names[s]);
        }
        if (!ins.empty()) parts.push_back(join(ins, "+") + " in");
        if (!outs.empty()) parts.push_back(join(outs, "+") + " out");
        if (!sw.empty()) parts.push_back(join(sw, "+") + " switch clip");
        return parts.empty() ? fmt("level %+.0f dB", jL[b]) : join(parts, ", ");
    };
    auto buildBefore = [&](int d, int lo, std::string* why) -> int {
        int prev = -1;
        double best = -1e18;
        for (int b : bounds)
            if (lo < b && 4 <= d - b && d - b <= 16 && nov[b] > best) best = nov[b], prev = b;
        if (prev < 0)
            for (int b : bounds)
                if (b < d) prev = std::max(prev, b);
        if (prev > lo && d - prev <= 32) {
            *why = fmt("%d bars before the drop: ", d - prev) + whatChanges(prev);
            return prev;
        }
        // the whole stretch before the drop is one section: look for the riser inside it
        int b = d;
        auto uniq = [&](int i) {
            for (int s = 0; s < NSTEMS; s++)
                if (labels[s][i] == UNIQUE) return true;
            return false;
        };
        while (b - 1 > lo && uniq(b - 1) && d - b < 16) b--;
        if (d - b >= 4) {
            b = d - (d - b) / 4 * 4;
            *why = fmt("%d bars of never-repeated material (risers/fills) before the drop", d - b);
            return b;
        }
        if (d - lo > 8) {
            *why = "default: 8 bars before the drop";
            return d - 8;
        }
        return -1;
    };
    auto level = [&](std::pair<int, int> r) {
        std::vector<double> v = sliceOf(L, r.first, r.second);
        std::sort(v.begin(), v.end());
        double s = 0;
        for (size_t i = v.size() / 2; i < v.size(); i++) s += v[i];
        return s / (v.size() - v.size() / 2);
    };
    double bestLevel = -1e18;
    for (auto& r : regions) bestLevel = std::max(bestLevel, level(r));
    std::vector<std::pair<int, int>> all = regions;
    regions.clear();
    for (auto& r : all)
        if (level(r) >= bestLevel - 2.5) regions.push_back(r);
    auto d1 = regions[0];
    put('C', d1.first, fmt("first loud section with low end and drums (%d bars): ", d1.second - d1.first) + whatChanges(d1.first));
    std::string why;
    if (d1.first - first >= 8) {
        int b = buildBefore(d1.first, first, &why);
        if (b >= 0) put('B', b, why);
    }
    if (d1.second < n - 4) put('D', d1.second, fmt("after %d bars of drop: ", d1.second - d1.first) + whatChanges(d1.second));
    if (regions.size() > 1) {
        auto d2 = regions[1];
        std::vector<std::string> same;
        for (int s = 0; s < NSTEMS; s++) {
            std::set<int> a, b;
            for (int i = d1.first; i < std::min(n, d1.first + 8); i++)
                if (labels[s][i] > 0) a.insert(labels[s][i]);
            for (int i = d2.first; i < std::min(n, d2.first + 8); i++)
                if (labels[s][i] > 0 && a.count(labels[s][i])) b.insert(labels[s][i]);
            if (!b.empty()) same.push_back(names[s]);
        }
        put('F', d2.first, fmt("next loud section, %d bars after drop 1; ", d2.first - d1.first) + (same.empty() ? "no clips shared with drop 1" : "same " + join(same, "+") + " clips as drop 1"));
        int b = buildBefore(d2.first, d1.second, &why);
        if (b >= 0) put('E', b, why);
    }
    auto last = all.back();
    if (last.second < n - 3 && (!cues.count('D') || last.second != cues['D'].bar))
        put('G', last.second, fmt("last loud section ends, %d bars left: ", n - last.second) + whatChanges(last.second));
    // fake drop: 1-2 loud bars that stop again shortly before a real drop
    std::vector<int> barHigh(n);
    for (int i = 0; i < n; i++) barHigh[i] = L[i] >= lTop - 2 && E[i] >= eTop - 8 && (st[DRUMS][i] >= dTop - 8 || !hasDrums);
    auto anyBH = [&](int a, int b) {
        for (int i = std::max(a, 0); i < std::min(b, n); i++)
            if (barHigh[i]) return true;
        return false;
    };
    for (const Run& r : runs(barHigh)) {
        int a = r.a, b = r.b;
        if (r.v && b - a <= 2 && a >= 2 && !anyBH(a - 2, a) && !anyBH(b, b + 2)) {
            int nxt = -1;
            for (auto& g : regions)
                if (2 < g.first - a && g.first - a <= 16) { nxt = g.first; break; }
            bool nearEnd = false;
            for (auto& g : all) nearEnd |= g.first - 2 < a && a < g.second + 2;
            if (nxt >= 0 && !high[a] && !nearEnd) {
                put('H', a, fmt("%d loud bar(s) that stop again, %d bars before the drop", b - a, nxt - a));
                break;
            }
        }
    }
    finish();
}

// The order Python iterates set(values) in (CPython's open addressing, small ints hash to themselves, -1 to -2):
// autocue.py breaks ties with it.
inline std::vector<int> pySetOrder(const std::vector<int>& values) {
    std::vector<long long> table(8, LLONG_MIN);
    size_t used = 0, fill = 0;
    auto insertClean = [](std::vector<long long>& t, long long key) {
        size_t mask = t.size() - 1, h = (size_t)(key == -1 ? -2 : key), i = h & mask, perturb = h;
        for (;;) {
            if (t[i] == LLONG_MIN) { t[i] = key; return; }
            if (i + 9 <= mask)
                for (int j = 0; j < 9; j++)
                    if (t[i + 1 + j] == LLONG_MIN) { t[i + 1 + j] = key; return; }
            perturb >>= 5;
            i = (i * 5 + 1 + perturb) & mask;
        }
    };
    for (int v : values) {
        size_t mask = table.size() - 1, h = (size_t)(v == -1 ? -2 : v), i = h & mask, perturb = h;
        bool added = false, found = false;
        while (!added && !found) {
            size_t probes = i + 9 <= mask ? 9 : 0, k = i;
            for (;;) {
                if (table[k] == LLONG_MIN) { table[k] = v, added = true; break; }
                if (table[k] == v) { found = true; break; }
                if (!probes--) break;
                k++;
            }
            perturb >>= 5;
            i = (i * 5 + 1 + perturb) & mask;
        }
        if (!added) continue;
        used++, fill++;
        if (fill * 5 >= mask * 3) {  // resize to the next power of two above used * 4
            size_t size = 8;
            while (size <= used * 4) size <<= 1;
            std::vector<long long> t(size, LLONG_MIN);
            for (long long k : table)
                if (k != LLONG_MIN) insertClean(t, k);
            table.swap(t), fill = used;
        }
    }
    std::vector<int> out;
    for (long long k : table)
        if (k != LLONG_MIN) out.push_back((int)k);
    return out;
}

// What the model reads instead of hearing the track (autocue.py's build_sheet).
inline std::string buildSheet(const std::vector<int>* labels, const Levels& lv, const std::map<std::pair<int, int>, std::string>& letters,
                              const std::vector<Section>& sections, double bpm, bool hasStems) {
    int n = (int)lv.mix.size();
    auto dg = [](double v) { return (int)std::clamp((v + 36) / 4, 0.0, 9.0); };
    static const char* const shortS[] = {"dr", "ba", "me", "vo"};
    static const char* const shortB[] = {"hi", "lo", "mi", "--"};
    const char* const* sh = hasStems ? shortS : shortB;
    std::vector<double> L(n), E(n), st[NSTEMS];
    for (int i = 0; i < n; i++) L[i] = std::max(lv.mix[i], -36.0), E[i] = std::max(lv.low[i], -36.0);
    for (int s = 0; s < NSTEMS; s++) {
        st[s].resize(n);
        for (int i = 0; i < n; i++) st[s][i] = std::max(lv.st[s][i], -36.0);
    }
    std::vector<Section> raw = sections;
    if (raw.empty()) raw.push_back({0, n, false});
    auto clip = [&](int s, int a, int b) -> std::string {
        int len = b - a, silent = 0;
        std::map<int, int> count;
        std::vector<int> order;
        for (int i = a; i < b; i++) {
            int v = labels[s][i];
            if (v == INACTIVE) { silent++; continue; }
            if (!count[v]++) order.push_back(v);
        }
        if (silent > 0.5 * len) return ".";
        int act = len - silent, top = 0, topN = -1;
        for (int k : pySetOrder(order))  // the most played; a tie goes to the first in Python's set order
            if (count[k] > topN) topN = count[k], top = k;
        std::string name = top == UNIQUE ? "~" : letters.at({s, top});
        return name + (topN < 0.7 * act ? "*" : "");
    };
    struct Stats { int a, b; double mix, low, lev[NSTEMS]; std::string clips[NSTEMS]; };
    auto stats = [&](int a, int b) {
        Stats r{a, b, meanOf(L, a, b), meanOf(E, a, b), {}, {}};
        for (int s = 0; s < NSTEMS; s++) r.lev[s] = meanOf(st[s], a, b), r.clips[s] = clip(s, a, b);
        return r;
    };
    auto moved = [](const Stats& p, const Stats& q, int nst, std::vector<int>* ins, std::vector<int>* outs) {
        for (int s = 0; s < nst; s++) {
            double d = std::max(q.lev[s], -24.0) - std::max(p.lev[s], -24.0);
            if (d >= 6) ins->push_back(s);
            if (d <= -6) outs->push_back(s);
        }
    };
    auto strip = [](std::string c) {
        while (!c.empty() && c.back() == '*') c.pop_back();
        return c;
    };
    std::vector<std::vector<Stats>> blocks;
    for (const Section& sec : raw) {
        Stats cur = stats(sec.start, sec.end);
        if (!blocks.empty()) {
            Stats first = stats(blocks.back()[0].a, blocks.back()[0].b);
            const Stats& last = blocks.back().back();
            std::vector<int> ins, outs;
            moved(last, cur, 3, &ins, &outs);  // drums, bass, melody
            if (ins.empty() && outs.empty() && std::fabs(cur.mix - last.mix) < 3 && std::fabs(cur.mix - first.mix) < 4) {
                blocks.back().push_back(cur);
                continue;
            }
        }
        blocks.push_back({cur});
    }
    char bpmS[32];
    snprintf(bpmS, sizeof bpmS, "%g", bpm);
    std::vector<std::string> out{fmt("%d bars, %s BPM", n, bpmS), "", "SECTIONS",
                                 fmt("  #  bars      len | mix  low | %s %s %s %s | clips        | at start", sh[0], sh[1], sh[2], sh[3])};
    std::vector<std::pair<int, int>> starts;
    const Stats* prev = nullptr;
    std::vector<Stats> rows;
    rows.reserve(blocks.size());
    for (size_t bi = 0; bi < blocks.size(); bi++) {
        const auto& parts = blocks[bi];
        int i = (int)bi + 1, a = parts[0].a, b = parts.back().b;
        rows.push_back(stats(a, b));
        const Stats& row = rows.back();
        starts.push_back({i, a});
        std::vector<std::string> ch;
        if (prev) {
            std::vector<int> ins, outs;
            moved(*prev, row, NSTEMS, &ins, &outs);
            for (int s : ins) ch.push_back(std::string("+") + sh[s]);
            for (int s : outs) ch.push_back(std::string("-") + sh[s]);
            std::vector<std::string> sw;
            for (int s = 0; s < NSTEMS; s++) {
                const std::string &c0 = prev->clips[s], &c1 = row.clips[s];
                bool io = std::find(ins.begin(), ins.end(), s) != ins.end() || std::find(outs.begin(), outs.end(), s) != outs.end();
                if (strip(c0) != strip(c1) && c0 != "." && c1 != "." && !io) sw.push_back(sh[s]);
            }
            if (!sw.empty()) ch.push_back("new clip: " + join(sw, ","));
            double d = row.mix - prev->mix;
            if (std::fabs(d) >= 2) ch.push_back(fmt("mix %+.0f dB", d));
        }
        std::string range = fmt("%d-%d", a + 1, b), clips;
        for (int s = 0; s < NSTEMS; s++) clips += (s ? " " : "") + fmt("%-2s", row.clips[s].c_str());
        out.push_back(fmt("%3d  %-9s %3d | %3.0f %4.0f |  %d  %d  %d  %d | %s | ", i, range.c_str(), b - a, row.mix, row.low, dg(row.lev[0]), dg(row.lev[1]), dg(row.lev[2]), dg(row.lev[3]), clips.c_str()) + join(ch, ", "));
        std::vector<std::string> inside;
        for (size_t k = 1; k < parts.size(); k++) {
            std::vector<std::string> sw;
            for (int s = 0; s < NSTEMS; s++)
                if (strip(parts[k - 1].clips[s]) != strip(parts[k].clips[s])) sw.push_back(sh[s]);
            if (!sw.empty() && parts[k].b - parts[k].a >= 4) inside.push_back(fmt("bar %d new clip ", parts[k].a + 1) + join(sw, ","));
        }
        if (!inside.empty()) out.push_back("       same section continues: " + join(inside, "; "));
        prev = &rows.back();
    }
    out.push_back("");
    out.push_back("EDGES (3 bars before > 4 bars from the section start)");
    auto edges = [&](const std::vector<double>& v, int a) {
        std::string s;
        for (int i = std::max(0, a - 3); i < a; i++) s += (s.empty() ? "" : " ") + std::to_string(dg(v[i]));
        s += " >";
        for (int i = a; i < std::min(n, a + 4); i++) s += " " + std::to_string(dg(v[i]));
        return s;
    };
    for (auto& [i, a] : starts)
        if (a) out.push_back(fmt("%3d  bar %-4d mix %s   low %s", i, a + 1, edges(lv.mix, a).c_str(), edges(lv.low, a).c_str()));
    out.push_back("");
    out.push_back("STRIP (mix level of every bar)");
    for (int a = 0; a < n; a += 16) {
        std::string s = fmt("%4d:", a + 1);
        for (int i = a; i < std::min(n, a + 16); i++) s += " " + std::to_string(dg(lv.mix[i]));
        out.push_back(s);
    }
    return join(out, "\n");
}

}  // namespace detail

// mix: the track's mono audio at 22050 Hz. stems: drums, bass, other, vocals at 22050 Hz (nullptr: three bands of the
// mix stand in, as autocue.py with stems off). beatSec/beatNum: rekordbox's grid (seconds, bar position 1-4).
inline Analysis analyse(const std::vector<float>& mix, const std::vector<float>* stems, const std::vector<double>& beatSec, const std::vector<int>& beatNum, double duration) {
    using namespace detail;
    Analysis A;
    A.stems = stems != nullptr;
    if (beatSec.size() < 8 || mix.size() < (size_t)SR * 10) return A;
    int frames = 0;
    std::vector<float> M = spec(mix, &frames), F[NSTEMS];
    if (stems) {
        int f2;
        for (int s = 0; s < NSTEMS; s++) {
            F[s] = spec(stems[s], &f2);
            if (f2 != frames) return A;  // (a stem of another length would be read past its end)
        }
    } else {  // stand-ins cut from the mix, each scaled to the mix's level so the same thresholds work
        std::vector<double> tot(frames);
        for (int f = 0; f < frames; f++) {
            double s = 0;
            for (int b = 0; b < NB; b++) s += M[(size_t)f * NB + b];
            tot[f] = s;
        }
        double ref = pct(tot, 90) + 1e-12;
        for (int s = 0; s < 3; s++) {
            auto in = [&](int b) { double c = bandCenter(b); return s == DRUMS ? c > 4000 : s == BASS ? c < 150 : c >= 150 && c <= 4000; };
            F[s].assign(M.size(), 0.f);
            std::vector<double> bt(frames);
            for (int f = 0; f < frames; f++) {
                double v = 0;
                for (int b = 0; b < NB; b++)
                    if (in(b)) v += M[(size_t)f * NB + b];
                bt[f] = v;
            }
            double k = std::min(1e4, ref / (pct(bt, 90) + 1e-12));
            for (int f = 0; f < frames; f++)
                for (int b = 0; b < NB; b++)
                    if (in(b)) F[s][(size_t)f * NB + b] = (float)(M[(size_t)f * NB + b] * k);
        }
        F[VOCALS].assign(M.size(), 0.f);
    }
    // the grid: rekordbox's beats, extended at its average period to cover the whole file
    double period = (beatSec.back() - beatSec[0]) / (beatSec.size() - 1);
    if (!(period > 0.1 && period < 4)) return A;  // (a grid that doesn't advance would loop below for ever)
    A.bpm = std::round(60 / period * 100) / 100;
    std::vector<double> pre;
    for (double t = beatSec[0] - period; t > -period; t -= period) pre.push_back(t);
    std::reverse(pre.begin(), pre.end());
    std::vector<double> beats = pre;
    beats.insert(beats.end(), beatSec.begin(), beatSec.end());
    for (double t = beatSec.back() + period; t < duration + 2 * period; t += period) beats.push_back(t);
    int firstOne = 0;
    while (firstOne < (int)beatNum.size() && beatNum[firstOne] != 1) firstOne++;
    if (firstOne == (int)beatNum.size()) firstOne = 0;
    int rbDown = (int)((pre.size() + firstOne) % BPB);
    std::vector<float> bf[NSTEMS], bfMix = beatFeatures(M, frames, beats);
    for (int s = 0; s < NSTEMS; s++) bf[s] = beatFeatures(F[s], frames, beats);
    int nBeats = (int)beats.size() - 1;
    std::vector<bool> lowBand(NB);
    for (int b = 0; b < NB; b++) lowBand[b] = bandCenter(b) < 120;
    const int D = BPB * SUB * NB;

    struct Labelled { int nBars; Levels lv; std::vector<int> labels[NSTEMS]; std::vector<double> best; };
    auto labelAll = [&](int down) {
        Labelled R;
        int nBars = (nBeats - down) / BPB;
        while (nBars && beats[down + nBars * BPB] > duration + 0.05) nBars--;
        R.nBars = nBars;
        if (nBars < 2) return R;
        auto barOf = [&](const std::vector<float>& f, int bar) { return f.data() + ((size_t)(down + bar * BPB) * SUB) * NB; };
        std::vector<double> mixPow(nBars), lowPow(nBars);
        for (int i = 0; i < nBars; i++) {
            const float* p = barOf(bfMix, i);
            double s = 0, lo = 0;
            for (int k = 0; k < D; k++) s += p[k];
            for (int f = 0; f < BPB * SUB; f++)
                for (int b = 0; b < NB; b++)
                    if (lowBand[b]) lo += p[f * NB + b];
            mixPow[i] = s / D, lowPow[i] = lo / (BPB * SUB);
        }
        double mixRef = pct(mixPow, 90) + 1e-12, lowRef = pct(lowPow, 90) + 1e-12;
        R.lv.mix.resize(nBars), R.lv.low.resize(nBars);
        for (int i = 0; i < nBars; i++) R.lv.mix[i] = 10 * std::log10(mixPow[i] / mixRef + 1e-9), R.lv.low[i] = 10 * std::log10(lowPow[i] / lowRef + 1e-9);
        for (int s = 0; s < NSTEMS; s++) {
            const float* B = barOf(bf[s], 0);
            std::vector<double> pw(nBars);
            for (int i = 0; i < nBars; i++) {
                double v = 0;
                for (int k = 0; k < D; k++) v += B[(size_t)i * D + k];
                pw[i] = v / D;
            }
            R.lv.st[s].resize(nBars);
            for (int i = 0; i < nBars; i++) R.lv.st[s][i] = 10 * std::log10(pw[i] / mixRef + 1e-9);
            double ref = pct(pw, 90) + 1e-12;
            bool present = ref > mixRef * std::pow(10, -2.8);
            std::vector<bool> active(nBars);
            int na = 0;
            for (int i = 0; i < nBars; i++) na += active[i] = present && pw[i] > ref * 0.01 && pw[i] > mixRef * std::pow(10, -3.2);
            double p995 = pctOf(B, B + (size_t)nBars * D, 99.5) + 1e-12;
            std::vector<float> S = barSimilarity(B, nBars, active, p995);
            double thr = otsu(S, nBars, 0.60, 0.85);
            R.labels[s] = labelStem(S, nBars, active, thr);
            if (na > 8 && s != VOCALS)
                for (int i = 0; i < nBars; i++)
                    if (active[i]) {
                        float mx = 0;
                        for (int j = 0; j < nBars; j++)
                            if (j != i) mx = std::max(mx, S[(size_t)i * nBars + j]);
                        R.best.push_back(mx);
                    }
        }
        return R;
    };
    // pasted clips start on the bar: the right phase gives the fewest, cleanest blocks. rekordbox's bar line stands
    // unless the copies clearly say it is off by a beat or more.
    std::vector<double> votes = downbeatVotes(bf, nBeats);
    double cost[BPB];
    for (int d = 0; d < BPB; d++) {
        Labelled R = labelAll(d);
        double blocks = 0;
        for (int s = 0; s < 3; s++) {
            blocks += runs(R.labels[s]).size();
            for (int v : R.labels[s]) blocks += 2 * (v == UNIQUE);
        }
        double b2 = 0;
        for (double b : R.best) b2 += 1 - b;
        cost[d] = blocks + 4 * b2;
    }
    int down = 0;
    for (int d = 1; d < BPB; d++)
        if (cost[d] - 0.3 * votes[d] < cost[down] - 0.3 * votes[down]) down = d;
    if (down != rbDown && (cost[rbDown] - cost[down]) / cost[rbDown] > 0.06) A.downMoved = ((down - rbDown) % BPB + BPB) % BPB;
    else down = rbDown;
    Labelled R = labelAll(down);
    int n = A.bars = R.nBars;
    if (n < 2) return A;
    for (int i = 0; i <= n; i++) A.barTimes.push_back(beats[down + BPB * i]);
    int copied = 0;
    for (double b : R.best) copied += b >= 0.9;
    A.copiedShare = R.best.empty() ? 0 : (double)copied / R.best.size();

    // letters shared across the parts, in order of first appearance
    std::map<std::pair<int, int>, int> letterOf;
    for (int b = 0; b < n; b++)
        for (int s = 0; s < NSTEMS; s++) {
            int v = R.labels[s][b];
            if (v > 0 && !letterOf.count({s, v})) {
                int k = (int)letterOf.size();
                letterOf[{s, v}] = k;
            }
        }
    auto letter = [](int i) {
        const char* a = "ABCDEFGHIJKLMNOPQRSTUVWXYZ";
        return i < 26 ? std::string(1, a[i]) : std::string(1, a[std::min(i / 26 - 1, 25)]) + a[i % 26];
    };
    std::map<std::pair<int, int>, std::string> letters;
    for (auto& [k, v] : letterOf) letters[k] = letter(v);
    for (int s = 0; s < NSTEMS; s++)
        for (const Run& r : runs(R.labels[s]))
            A.lanes[s].push_back({r.a, r.b, r.v == INACTIVE ? -2 : r.v == UNIQUE ? -1 : letterOf[{s, r.v}], r.v == INACTIVE ? "" : r.v == UNIQUE ? "~" : letters[{s, r.v}]});
    static const char* const stemNames[] = {"drums", "bass", "melody", "vocals"};
    static const char* const bandNames[] = {"highs", "lows", "mids", "vocals"};
    findCues(R.labels, R.lv, stems ? stemNames : bandNames, &A.cues, &A.sections);
    A.sheet = buildSheet(R.labels, R.lv, letters, A.sections, A.bpm, stems != nullptr);

    // the waveform: height from the level, tint from the share of highs
    const int cols = 600;
    A.duration = frames / FPS;
    std::vector<double> amp(cols), hi(cols);
    for (int c = 0; c < cols; c++) {
        int a = (int)((double)frames * c / cols), b = std::max(a + 1, (int)((double)frames * (c + 1) / cols));
        double tot = 0, h = 0;
        for (int f = a; f < std::min(b, frames); f++)
            for (int k = 0; k < NB; k++) {
                double v = M[(size_t)f * NB + k];
                tot += v;
                if (bandCenter(k) > 4000) h += v;
            }
        amp[c] = std::sqrt(tot / (b - a)), hi[c] = h / (tot + 1e-12);
    }
    double ap = pct(amp, 99.5) + 1e-9, hp = pct(hi, 95) + 1e-9;
    for (int c = 0; c < cols; c++) A.amp.push_back((float)std::min(1.0, amp[c] / ap)), A.hi.push_back((float)std::clamp(std::sqrt(hi[c] / hp), 0.0, 1.0));
    return A;
}

// ---- the language model --------------------------------------------------------------------------------------
// The prompt (autocue.py's LLM_PROMPT); the DJ's instructions go after it. The answer: {"cues": [{"slot", "section",
// "bar", "why"}], "notes"}.
inline std::string prompt(bool hasStems) {
    std::string p =
        "You place DJ cue points on electronic music tracks. You cannot hear the track. You get three views of it, measured from the audio {HOW}.\n\n"
        "1. SECTIONS. The track cut at every bar where the arrangement changes. One row per section:\n"
        "   - #: section number. bars: first-last bar (1-based, 4 beats per bar). len: length in bars.\n"
        "   - mix, low: average level of the whole mix and of its low end (<120 Hz), in dB below the loudest stretch of the track (0 = as loud as it gets, -6 is clearly quieter, -20 is very quiet).\n"
        "   - {COLUMNS}\n"
        "   - clips: what each of those four parts mostly plays in this section. Same letter = the same material as wherever else that letter appears. '~' = material that never repeats (fills, risers, one-offs). '.' = silent. A '*' after a letter means the section mixes several clips.\n"
        "   - at start: what changed at the section's first bar compared with the section before.\n"
        "2. EDGES. For every section start: the mix and low level (0-9, each step 4 dB, 9 = loudest) of the 3 bars before it, a '>' and then the 4 bars from it on. Use this to settle the exact bar.\n"
        "3. STRIP. The mix level (0-9) of every bar, 16 bars per line, in case the sectioning missed a change.\n\n"
        "Known flaws in the measurements:\n"
        "{FLAWS}\n"
        "- Clip letters are noisy on parts with long evolving sounds. Trust long sections, not short ones.\n"
        "- A pickup bar before a section and a crash or reverb tail after it are not the section's first bar.\n"
        "- A \"same section continues\" line under a row lists clip switches inside that section (drop part A to part B, a new vocal phrase). They are not section starts and not a new drop.\n\n"
        "How this music is arranged: sections are 8, 16 or 32 bars long, so real section starts usually sit a multiple of 8 bars apart. Some tracks are offset by a few bars. A row that starts off that grid is usually a fill, a gap or a pickup inside a section, not a new section.\n\n"
        "Cue slots (give only the ones this track has):\n"
        "A Intro: first bar with sound.\n"
        "B Build: first bar of the build-up into the first drop.\n"
        "C Drop: first bar of the first main drop, the first stretch that is as loud as the track gets (mix near 0), with drums and low end. A quieter drums-only or bass-only stretch is not a drop.\n"
        "D Breakdown: first bar after the first drop ends.\n"
        "E Build 2: first bar of the build-up into the second drop.\n"
        "F Drop 2: first bar of the second main drop.\n"
        "G Outro: first bar after the last loud stretch ends.\n"
        "H Fake drop: one or two loud bars that stop again shortly before a real drop. Rare.\n\n"
        "Placing a cue: pick the first bar of a row in SECTIONS. Move it by one or two bars only when EDGES clearly shows the change there instead.\n\n"
        "Answer with JSON only, no other text:\n"
        "{\"cues\": [{\"slot\": \"C\", \"section\": 7, \"bar\": 97, \"why\": \"at most 12 words\"}], \"notes\": \"one sentence: how sure you are and why\"}";
    auto rep = [&](const char* k, const char* v) {
        size_t at = p.find(k);
        if (at != std::string::npos) p.replace(at, strlen(k), v);
    };
    if (hasStems) {
        rep("{HOW}", "by stem separation and bar-by-bar copy matching");
        rep("{COLUMNS}", "dr, ba, me, vo: level of the drums, bass, melody (everything else) and vocal stems, 0-9 (each step 4 dB, 9 = as loud as the mix gets).");
        rep("{FLAWS}", "- The separator often files a drop's bass under drums or melody. Judge drops by mix and low together, not by the ba column alone.");
    } else {
        rep("{HOW}", "by bar-by-bar copy matching (this track was not split into stems)");
        rep("{COLUMNS}", "hi, lo, mi, --: no stems here, so three bands of the mix instead. hi = above 4 kHz (hats, cymbals, air), lo = below 150 Hz (kick and bass together), mi = everything between. Each 0-9 on its own scale (9 = that band's loudest). The fourth column is unused.");
        rep("{FLAWS}", "- Kick and bass cannot be told apart, and neither can drums and melody. Judge drops by mix and low together; hi rising usually means hats or a riser.");
    }
    return p;
}

inline std::string systemPrompt(bool hasStems, const std::string& instructions) {
    std::string s = prompt(hasStems);
    if (!instructions.empty()) s += "\n\nThe DJ's own instructions. Where they conflict with the slot definitions above, follow these:\n" + instructions;
    return s;
}

}  // namespace autocue_sheet
