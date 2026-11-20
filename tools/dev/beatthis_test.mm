// CLI harness. Usage:
//   (build: tools/dev/beatthis_test.sh -> build/beatthis_test)
//   beatthis_test model.mlmodelc file.f32 [rate=44100] [--units cpu|gpu|all|ane (default gpu)] [--compare refdir] [--res22k soxr.npy]
//   beatthis_test model.mlmodelc --synth        (synthetic tempo tests)
// file.f32: mono float32 little-endian PCM.
#import <Foundation/Foundation.h>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <random>
#include <sstream>
#include "../../engines/beatthis.h"
#include <functional>

// The engine's one platform call (core/plat_mac.mm has the real one; this harness doesn't link core/).
namespace rbm::plat {
void parallelFor(size_t n, const std::function<void(size_t)>& fn) {
    dispatch_apply(n, dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^(size_t i) { fn(i); });
}
}
using namespace beatthis;

static std::vector<float> readF32(const std::string& p) {
    std::ifstream f(p, std::ios::binary | std::ios::ate);
    if (!f) return {};
    size_t n = f.tellg() / 4; f.seekg(0);
    std::vector<float> v(n);
    f.read((char*)v.data(), n * 4);
    return v;
}
// minimal .npy reader (float32 or float64, C order) -> floats
static bool readNpy(const std::string& p, std::vector<float>& out, std::vector<long>& shape) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return false;
    char magic[10]; f.read(magic, 10);
    int hl = (unsigned char)magic[8] | ((unsigned char)magic[9] << 8);
    std::string h(hl, ' '); f.read(&h[0], hl);
    bool f64 = h.find("<f8") != std::string::npos;
    shape.clear();
    size_t a = h.find("shape"), lp = h.find('(', a), rp = h.find(')', lp);
    std::stringstream ss(h.substr(lp + 1, rp - lp - 1));
    std::string tok; long cnt = 1;
    while (std::getline(ss, tok, ',')) if (tok.find_first_of("0123456789") != std::string::npos) { shape.push_back(atol(tok.c_str())); cnt *= shape.back(); }
    out.resize(cnt);
    if (f64) { std::vector<double> d(cnt); f.read((char*)d.data(), cnt * 8); for (long i = 0; i < cnt; i++) out[i] = (float)d[i]; }
    else f.read((char*)out.data(), cnt * 4);
    return true;
}
static std::vector<double> readTxt(const std::string& p) {
    std::vector<double> v; std::ifstream f(p); double x;
    while (f >> x) v.push_back(x);
    return v;
}
static void printGrid(const Grid& g) {
    printf("grid: ok=%d bpm=%.2f firstBeat=%.3f downbeatIndex=%d confidence=%.3f\n", g.ok, g.bpm, g.firstBeat, g.downbeatIndex, g.confidence);
}
static double med(std::vector<double> v) { if (v.empty()) return 0; std::sort(v.begin(), v.end()); return v[v.size() / 2]; }

// compare beat lists: count matches within tol; returns max |diff| of matched
static void cmpTimes(const char* name, const std::vector<double>& a, const std::vector<double>& b) {
    size_t j = 0, match = 0; double mx = 0;
    for (double x : a) {
        while (j + 1 < b.size() && std::fabs(b[j + 1] - x) < std::fabs(b[j] - x)) j++;
        if (j < b.size() && std::fabs(b[j] - x) <= 0.0201) { match++; mx = std::max(mx, std::fabs(b[j] - x)); }
    }
    printf("  %s: ours %zu ref %zu, matched(<=1 frame) %zu, max diff %.4f s, %s\n", name, a.size(), b.size(), match, mx,
           (a.size() == b.size() && match == a.size() && mx < 1e-6) ? "EXACT" : "");
}

static void compare(const Model& m, const std::vector<float>& x, double rate, const std::string& dir, const std::string& res) {
    Debug d;
    Result r = m.analyze(x.data(), x.size(), rate, &d);
    std::vector<float> ref; std::vector<long> sh;
    // resampler: compare against soxr output if provided
    if (readNpy(res.empty() ? dir + "/res22k.npy" : res, ref, sh)) {
        std::vector<float> ours = resampleTo22k(x.data(), x.size(), rate);
        size_t n = std::min(ours.size(), ref.size());
        double mx = 0, se = 0, sr = 0;
        for (size_t i = 0; i < n; i++) { double e = ours[i] - ref[i]; mx = std::max(mx, std::fabs(e)); se += e * e; sr += (double)ref[i] * ref[i]; }
        printf("resample: len ours %zu ref %zu, max abs err %.3g, rms err %.3g (%.1f dB rel. to signal rms)\n", ours.size(), ref.size(), mx, std::sqrt(se / n), 10 * log10(se / sr + 1e-30));
        // mel from soxr's samples: isolates STFT/mel implementation
        int fr; std::vector<float> mel2 = logMel(ref.data(), ref.size(), &fr);
        std::vector<float> rm;
        if (readNpy(dir + "/mel.npy", rm, sh) && (int)sh[0] == fr) {
            double mx2 = 0, s2 = 0;
            for (size_t i = 0; i < rm.size(); i++) { double e = std::fabs(mel2[i] - rm[i]); mx2 = std::max(mx2, e); s2 += e; }
            printf("mel (from soxr samples): frames %d, max abs err %.3g, mean abs err %.3g\n", fr, mx2, s2 / rm.size());
        } else printf("mel (from soxr samples): frame count mismatch ours %d ref %ld\n", fr, sh.empty() ? 0 : sh[0]);
    }
    if (readNpy(dir + "/mel.npy", ref, sh)) {
        if (sh[0] == d.frames) {
            double mx = 0, s = 0;
            for (size_t i = 0; i < ref.size(); i++) { double e = std::fabs(d.mel[i] - ref[i]); mx = std::max(mx, e); s += e; }
            printf("mel (full pipeline): frames %d, max abs err %.3g, mean abs err %.3g (mel range ~0..%.1f)\n", d.frames, mx, s / ref.size(), *std::max_element(ref.begin(), ref.end()));
        } else printf("mel frames mismatch ours %d ref %ld\n", d.frames, sh[0]);
    }
    for (int k = 0; k < 2; k++) {
        const char* nm[2][2] = {{"beat_logits.npy", "beat"}, {"down_logits.npy", "down"}};
        if (!readNpy(dir + "/" + nm[k][0], ref, sh) && !(k && readNpy(dir + "/downbeat_logits.npy", ref, sh))) continue;
        const std::vector<float>& o = k ? d.downLogits : d.beatLogits;
        if (ref.size() != o.size()) { printf("%s logits size mismatch %zu vs %zu\n", nm[k][1], o.size(), ref.size()); continue; }
        double mx = 0, s = 0; size_t sign = 0;
        for (size_t i = 0; i < ref.size(); i++) { double e = std::fabs(o[i] - ref[i]); mx = std::max(mx, e); s += e; if ((o[i] > 0) != (ref[i] > 0)) sign++; }
        printf("%s logits: max abs err %.3g, mean abs err %.3g, sign flips %zu / %zu\n", nm[k][1], mx, s / ref.size(), sign, ref.size());
    }
    auto rb = readTxt(dir + "/beats.txt"), rd = readTxt(dir + "/downbeats.txt");
    if (!rb.empty()) { cmpTimes("beats", r.beats, rb); cmpTimes("downbeats", r.downbeats, rd); }
    printGrid(fitGrid(r));
    Result rr; rr.beats = rb; rr.downbeats = rd;
    if (!rb.empty()) { printf("ref "); printGrid(fitGrid(rr)); }
}

// ---- synthetic signals ----------------------------------------------------------------------
struct Synth { std::vector<float> x; double bpm, bar0; };   // bar0: time of a downbeat (bar start)
static void kick(std::vector<float>& x, double t, double amp = 0.9, int sr = 44100) {
    size_t s = (size_t)(t * sr); double ph = 0;
    for (size_t i = 0; i < (size_t)(0.35 * sr) && s + i < x.size(); i++) {
        double tt = (double)i / sr, f = 45 + 110 * exp(-tt / 0.025);
        ph += 2 * M_PI * f / sr;
        x[s + i] += (float)(amp * sin(ph) * exp(-tt / 0.11));
    }
}
static void snare(std::vector<float>& x, double t, std::mt19937& g, double amp = 0.6, int sr = 44100) {
    std::normal_distribution<float> nd(0, 1); size_t s = (size_t)(t * sr);
    for (size_t i = 0; i < (size_t)(0.2 * sr) && s + i < x.size(); i++) {
        double tt = (double)i / sr;
        x[s + i] += (float)(amp * (0.5 * nd(g) * exp(-tt / 0.06) + 0.5 * sin(2 * M_PI * 190 * tt) * exp(-tt / 0.05)));
    }
}
static void hat(std::vector<float>& x, double t, std::mt19937& g, double amp = 0.15, int sr = 44100) {
    std::normal_distribution<float> nd(0, 1); size_t s = (size_t)(t * sr); float p = 0;
    for (size_t i = 0; i < (size_t)(0.05 * sr) && s + i < x.size(); i++) {
        float v = nd(g); float hp = v - p; p = v;
        x[s + i] += (float)(amp * hp * exp(-(double)i / sr / 0.015));
    }
}
static void pad(std::vector<float>& x, double t0, double t1, int sr = 44100) {
    for (size_t i = (size_t)(t0 * sr); i < (size_t)(t1 * sr) && i < x.size(); i++) {
        double t = (double)i / sr; double env = std::min(1.0, (t - t0) / 3.0) * 0.12;
        x[i] += (float)(env * (sin(2 * M_PI * 220 * t) + sin(2 * M_PI * 277.18 * t) + sin(2 * M_PI * 329.63 * t + 0.3 * sin(0.37 * t))));
    }
}
// kind 0: 124 BPM four on the floor + offbeat hats; kind 1: 174 DnB (kick on 1 & "and of 3", snare 2/4, 8th hats);
// kind 2: 128 BPM, 30 s pad-only intro then four-on-the-floor + clap on 2/4.
static Synth makeSynth(int kind, double secs) {
    int sr = 44100; std::mt19937 g(7);
    Synth s; s.x.assign((size_t)(secs * sr), 0.f);
    double off = 0.37;
    if (kind == 0) {
        s.bpm = 124; double b = 60 / s.bpm; s.bar0 = off;
        for (int k = 0; off + k * b < secs - 0.5; k++) { kick(s.x, off + k * b); hat(s.x, off + (k + 0.5) * b, g); }
    } else if (kind == 1) {
        s.bpm = 174; double b = 60 / s.bpm; s.bar0 = off;
        for (int bar = 0; off + bar * 4 * b < secs - 3; bar++) {
            double t = off + bar * 4 * b;
            kick(s.x, t); kick(s.x, t + 2.5 * b); snare(s.x, t + b, g); snare(s.x, t + 3 * b, g);
            for (int h = 0; h < 8; h++) hat(s.x, t + h * b / 2, g, 0.1);
        }
    } else {
        s.bpm = 128; double b = 60 / s.bpm; double st = 30.0 + off; s.bar0 = st;
        pad(s.x, 0, secs);
        for (int k = 0; st + k * b < secs - 0.5; k++) {
            kick(s.x, st + k * b); hat(s.x, st + (k + 0.5) * b, g);
            if (k % 4 == 1 || k % 4 == 3) snare(s.x, st + k * b, g, 0.35);
        }
    }
    return s;
}
static void synthTest(const Model& m) {
    const char* names[3] = {"124 BPM four-on-the-floor", "174 BPM DnB", "128 BPM, 30 s pad intro"};
    for (int k = 0; k < 3; k++) {
        Synth s = makeSynth(k, 120);
        Result r = m.analyze(s.x.data(), s.x.size(), 44100);
        Grid g = fitGrid(r);
        double per = 60 / s.bpm;
        double fbTrue = fmod(s.bar0, per);
        int diTrue = (int)lround((s.bar0 - fbTrue) / per) % 4;   // beat j (from firstBeat) is a downbeat iff j%4==diTrue
        printf("[%s] truth bpm=%.2f firstBeat=%.3f downbeatIndex=%d | beats %zu downbeats %zu\n  ", names[k], s.bpm, fbTrue, diTrue, r.beats.size(), r.downbeats.size());
        printGrid(g);
    }
}

int main(int argc, char** argv) {
    @autoreleasepool {
        if (argc < 3) { fprintf(stderr, "usage: %s model.mlmodelc (file.f32 [rate] | --synth) [--units cpu|gpu|all|ane (default gpu)] [--compare refdir] [--res22k soxr.npy]\n", argv[0]); return 1; }
        std::string file, cmp, res; double rate = 44100; bool synth = false; ComputeUnits cu = ComputeUnits::CpuAndGpu;
        for (int i = 2; i < argc; i++) {
            std::string a = argv[i];
            if (a == "--synth") synth = true;
            else if (a == "--compare" && i + 1 < argc) cmp = argv[++i];
            else if (a == "--res22k" && i + 1 < argc) res = argv[++i];   // soxr-resampled reference samples (.npy) to measure the resampler
            else if (a == "--units" && i + 1 < argc) { std::string u = argv[++i]; cu = u == "gpu" ? ComputeUnits::CpuAndGpu : u == "all" ? ComputeUnits::All : u == "ane" ? ComputeUnits::CpuAndNeuralEngine : ComputeUnits::CpuOnly; }
            else if (file.empty()) file = a;
            else rate = atof(a.c_str());
        }
        Model m; std::string err;
        auto t0 = std::chrono::steady_clock::now();
        if (!m.load(argv[1], &err, cu)) { fprintf(stderr, "load: %s\n", err.c_str()); return 1; }
        printf("model loaded in %.0f ms\n", std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
        if (synth) { synthTest(m); return 0; }
        std::vector<float> x = readF32(file);
        if (x.empty()) { fprintf(stderr, "cannot read %s\n", file.c_str()); return 1; }
        printf("%zu samples @ %.0f Hz = %.1f s\n", x.size(), rate, x.size() / rate);
        if (!cmp.empty()) { compare(m, x, rate, cmp, res); }
        Debug d;
        auto a = std::chrono::steady_clock::now();
        Result r = m.analyze(x.data(), x.size(), rate, &d);
        double tot = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - a).count();
        printf("timing ms: resample %.0f, stft+mel %.0f, inference %.0f (%d chunks), postprocess %.1f, total %.0f\n", d.resampleMs, d.melMs, d.inferMs, d.chunks, d.postMs, tot);
        printf("beats %zu, downbeats %zu\nfirst beats:", r.beats.size(), r.downbeats.size());
        for (size_t i = 0; i < r.beats.size() && i < 8; i++) printf(" %.2f", r.beats[i]);
        printf("\nfirst downbeats:");
        for (size_t i = 0; i < r.downbeats.size() && i < 4; i++) printf(" %.2f", r.downbeats[i]);
        printf("\n");
        printGrid(fitGrid(r));
        if (getenv("BT_DUMP")) {   // all beat and downbeat times, for offline checks
            FILE* f = fopen(getenv("BT_DUMP"), "w");
            for (double b : r.beats) fprintf(f, "b %.4f\n", b);
            for (double b : r.downbeats) fprintf(f, "d %.4f\n", b);
            fclose(f);
        }
    }
    return 0;
}
