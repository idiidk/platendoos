// betterstems: separate stems with HTDemucs v4 (Hybrid Transformer Demucs, Meta, MIT licence) instead of rekordbox's
// own engine. HTDemucs scores ~9 dB SDR on MUSDB18 against ~5.8 for rekordbox's default (Spleeter) and ~7.7 for its
// quality option (Hybrid Demucs v3), and on the GPU through CoreML it is faster than either of rekordbox's on the CPU
// (~0.25 s per 7.8 s of audio on an M2 Pro).
//
// The model: tools/models/htdemucs_coreml.sh converts Meta's weights to ~/Library/Application Support/rekordbox-mods/
// htdemucs.mlpackage: the network without its STFT and inverse STFT, which are here (dsp.h), exactly as Demucs
// does them. stemcache's engine wrapper calls separate() for a chunk instead of rekordbox's engine, and keeps its
// stems apart from rekordbox's (their cache key has the model's name). Off without the model.
//   config: betterstems=0 to disable
//           betterstems_grid_first=1   0: stems of a track that is being analysed start at once, next to its beat detection
//           betterstems_warmup=1       0: no first run of the model at launch (Apple Silicon)
//           betterstems_slow=0         Windows, Intel Macs: 1: HTDemucs also where it is slow (on the CPU, or on a
//                                      GPU that needs over a second for a segment); 0: rekordbox's own engine separates there
//           betterstems_live_rest=0    while a deck plays: a rest after each model segment, this many times the segment's
//                                      own time (1 = the stems take twice as long and the GPU is free half the time)
#include <string.h>
#include <string>
#include "../core/rbmod.h"
#include "../engines/dsp.h"
#include "betterstems_backend.h"
#include <atomic>
#include <chrono>
#include <thread>
#include <cmath>
#include <mutex>
#include <vector>

using namespace rbm;

static volatile bool g_on = true;

namespace {

// The model's fixed segment (7.8 s at 44.1 kHz) and spectrogram (Demucs' _spec/_ispec: hop = nfft / 4, the signal
// reflect-padded by 1.5 hops so that the frames line up with the waveform branch).
constexpr int kL = 343980, kNfft = 4096, kHop = 1024, kBins = 2048, kFrames = 336;
constexpr int kPadL = kHop / 2 * 3, kPadR = kPadL + kFrames * kHop - kL, kY = kL + kPadL + kPadR;  // 1536, 1620, 347136
constexpr int kSources = 4;  // drums, bass, other, vocals

struct Fft {
    dsp::RealFFT dft{12};  // kNfft = 4096
    std::vector<float> window, envelope;  // hann; the inverse STFT's window-square sum at each output sample
    Fft() {
        window.resize(kNfft);
        for (int k = 0; k < kNfft; k++) window[k] = 0.5f - 0.5f * cosf(2 * (float)M_PI * k / kNfft);  // periodic
        // torch.istft (center) over kFrames + 4 frames; output sample i is at 2048 + kPadL + i of its overlap-add.
        envelope.resize(kL);
        for (int i = 0; i < kL; i++) {
            int j = kNfft / 2 + kPadL + i;
            double e = 0;
            for (int t = 0; t < kFrames + 4; t++)
                if (j >= t * kHop && j < t * kHop + kNfft) e += (double)window[j - t * kHop] * window[j - t * kHop];
            envelope[i] = (float)e;
        }
    }
};
const Fft& fft() {
    static Fft f;
    return f;
}

// Demucs' spectrogram of one channel (kL samples) into the model's input: re and im planes, [kBins][kFrames] each.
void spectrogram(const float* x, float* re, float* im) {
    const Fft& f = fft();
    std::vector<float> y(kY), frame(kNfft), orr(kNfft / 2), oi(kNfft / 2);
    for (int i = 0; i < kPadL; i++) y[i] = x[kPadL - i];
    memcpy(y.data() + kPadL, x, kL * sizeof(float));
    for (int j = 0; j < kPadR; j++) y[kPadL + kL + j] = x[kL - 2 - j];
    const float scale = 0.5f / sqrtf(kNfft);  // the forward FFT is twice the DFT; torch's "normalized"
    for (int t = 0; t < kFrames; t++) {
        for (int k = 0; k < kNfft; k++) frame[k] = y[t * kHop + k] * f.window[k];
        f.dft.forward(frame.data(), orr.data(), oi.data());
        re[t] = orr[0] * scale, im[t] = 0;  // DC (oi[0] is the Nyquist bin, which the model doesn't get)
        for (int b = 1; b < kBins; b++) re[b * kFrames + t] = orr[b] * scale, im[b * kFrames + t] = oi[b] * scale;
    }
}

// The inverse (Demucs' _ispec): one source channel's re and im planes back to kL samples, added to out.
void inverse(const float* re, const float* im, float* out) {
    const Fft& f = fft();
    std::vector<float> acc(kNfft + (kFrames + 3) * kHop), ir(kNfft / 2), ii(kNfft / 2), frame(kNfft);
    // the inverse FFT gives N * the signal for this packing; torch's "normalized" multiplies by sqrt(N).
    const float scale = sqrtf(kNfft) / kNfft;
    for (int t = 0; t < kFrames; t++) {
        ir[0] = re[t], ii[0] = 0;  // DC; the Nyquist bin is 0
        for (int b = 1; b < kBins; b++) ir[b] = re[b * kFrames + t], ii[b] = im[b * kFrames + t];
        f.dft.inverse(ir.data(), ii.data(), frame.data());
        float* a = acc.data() + (t + 2) * kHop;  // torch's frame t + 2 (Demucs pads 2 empty frames on each side)
        for (int k = 0; k < kNfft; k++) a[k] += frame[k] * scale * f.window[k];
    }
    const float* a = acc.data() + kNfft / 2 + kPadL;
    for (int i = 0; i < kL; i++) out[i] += f.envelope[i] > 1e-11f ? a[i] / f.envelope[i] : 0;
}

}  // namespace

static std::mutex g_lock;  // the model (one prediction at a time: it fills the GPU anyway)
static std::atomic<int> g_state;  // 0 not loaded, 1 loading, 2 loaded, -1 unavailable
static std::string g_version;

static bool segment(const float* l, const float* r, std::vector<float>& out, double* modelTime = nullptr);

#if RBM_MAC_X86
using betterstems_backend::kSlowSegment;

// An Intel Mac, as Windows (betterstems_ort.cpp, betterstems_backend.h): HTDemucs is kept only where it is fast. On
// the GPU one segment of a test signal, twice when the first is slow (the first run also builds the GPU's pipeline:
// it is the warm-up Apple Silicon runs too, and it is checked like every segment, below); over kSlowSegment
// rekordbox's own engine separates. On the CPU (gpu=0 betterstems_slow=1) nothing runs at launch.
static bool fastEnough() {
    bool anyway = cfgInt("betterstems_slow", 0) != 0;
    if (!cfgInt("gpu", 1)) return true;  // (betterstems_slow=1: the backend didn't load it otherwise)
    std::vector<float> l(kL), r(kL), out;
    unsigned seed = 1;
    for (int i = 0; i < kL; i++) {  // a kick at 120 BPM over a bass note and a little noise (-25 dB)
        double t = (double)i / 44100, beat = fmod(t, 0.5);
        seed = seed * 1664525u + 1013904223u;
        float noise = ((int)(seed >> 16) - 32768) / 32768.f;
        float v = (float)(0.25 * exp(-beat * 20) * sin(2 * M_PI * 55 * beat * (1 + 2 * exp(-beat * 40))) +
                          0.06 * sin(2 * M_PI * 41.2 * t) + 0.01 * noise);
        l[i] = v, r[i] = v * 0.9f;
    }
    double took = 0;
    for (int i = 0; i < 2 && (i == 0 || took > kSlowSegment); i++) {
        double t;
        if (!segment(l.data(), r.data(), out, &t)) return false;  // (segment said why)
        took = i ? std::min(took, t) : t;
    }
    logf("betterstems: a segment takes %.2f s on the GPU (Core ML)", took);
    if (!anyway && took > kSlowSegment) {
        logf("betterstems: HTDemucs is slow on this GPU (over %.1f s a segment): rekordbox's own engine separates "
             "(betterstems_slow=1 for HTDemucs anyway)", kSlowSegment);
        std::lock_guard<std::mutex> lk(g_lock);
        betterstems_backend::unload();
        return false;
    }
    return true;
}
#endif

static void loadModel() {
    int expect = 0;
    if (!g_state.compare_exchange_strong(expect, 1)) return;
    bool ok = betterstems_backend::load(&g_version);
#if RBM_MAC_X86
    ok = ok && fastEnough();
#endif
    g_state = ok ? 2 : -1;
}

#if RBM_MAC_X86 || defined(_WIN32)
// Windows and Intel Macs: whichever GPU is there runs HTDemucs, and one (on Windows, cause unknown) gave noise
// instead of stems. Each segment's stems are checked: finite, and no source, nor what the sources leave of the mix,
// much louder than the mix (with a floor for quiet passages). Unusable stems switch HTDemucs off for the session.
const double kLouder = 2, kLeft = 0.5;  // a source's RMS, the sources' sum's error's: at most this times the mix's
static bool usable(const float* l, const float* r, const std::vector<float>& out, std::string* why) {
    double mix = 0, loudest = 0, rest = 0;
    for (int i = 0; i < kL; i++) mix += (double)l[i] * l[i] + (double)r[i] * r[i];
    for (int c = 0; c < 2; c++) {
        const float* in = c ? r : l;
        for (int i = 0; i < kL; i++) {
            double sum = 0;
            for (int s = 0; s < kSources; s++) {
                float v = out[(size_t)(s * 2 + c) * kL + i];
                if (!std::isfinite(v)) {
                    *why = "not finite";
                    return false;
                }
                sum += v;
            }
            rest += (sum - in[i]) * (sum - in[i]);
        }
    }
    for (int s = 0; s < kSources; s++) {
        double e = 0;
        for (int i = 0; i < 2 * kL; i++) e += (double)out[(size_t)s * 2 * kL + i] * out[(size_t)s * 2 * kL + i];
        loudest = std::max(loudest, e);
    }
    double m = sqrt(mix / (2 * kL)), src = sqrt(loudest / (2 * kL)), res = sqrt(rest / (2 * kL)), quiet = 0.01;
    if (src <= kLouder * m + quiet && res <= kLeft * m + quiet) return true;
    char b[128];
    snprintf(b, sizeof b, "the mix at %.1f dB, a source at %.1f dB, the sources' sum off by %.1f dB",
             20 * log10(m + 1e-9), 20 * log10(src + 1e-9), 20 * log10(res + 1e-9));
    *why = b;
    return false;
}
#endif

namespace {
bool active() {
    if (!g_on) return false;
    if (g_state == 0) loadModel();
    return g_state == 2;
}

std::string version() { return active() ? g_version : ""; }
}  // namespace

const double kGridFirst = 8;  // s a segment waits at most for an analysis' beat stage (betterstems_grid_first)

// One model segment (kL samples, planar stereo) -> sources (kSources x 2 x kL, added to out). *modelTime: the model's
// own time, s.
static bool segment(const float* l, const float* r, std::vector<float>& out, double* modelTime) {
    std::vector<float> mix(2 * kL), mag((size_t)4 * kBins * kFrames);
    memcpy(mix.data(), l, kL * sizeof(float));
    memcpy(mix.data() + kL, r, kL * sizeof(float));
    for (int c = 0; c < 2; c++) {
        float* re = mag.data() + (size_t)(2 * c) * kBins * kFrames;
        spectrogram(mix.data() + (size_t)c * kL, re, re + (size_t)kBins * kFrames);
    }
    // The beat grid first. A track loaded on a deck is analysed and separated at the same moment, and the model would
    // share the GPU (or the cores) with Beat This! and rekordbox's beat detection, which the DJ needs sooner than the
    // stems: a segment waits while an analysis is at its beat stage, a few seconds at most.
    if (cfgInt("betterstems_grid_first", 1)) {
        double t0 = now();
        while (analysisBeatStage() && now() - t0 < kGridFirst) std::this_thread::sleep_for(std::chrono::milliseconds(20));
        static std::atomic<int> said{0};
        if (now() - t0 >= 0.3 && said++ < 5) logf("betterstems: waited %.1f s for an analysis' beat stage", now() - t0);
    }
    std::vector<float> spec, wave;
    {
        std::lock_guard<std::mutex> lk(g_lock);
        std::string err;
        static int timed;  // the first few segments' model time, for the log (CPU vs GPU)
        double t0 = now();
        if (!betterstems_backend::predict(mix.data(), kL, mag.data(), kBins, kFrames, kSources, spec, wave, &err)) {
            logf("betterstems: prediction failed: %s", err.c_str());
            return false;
        }
        if (modelTime) *modelTime = now() - t0;
        if (timed < 3) logf("betterstems: segment %d took %.2f s in the model", ++timed, now() - t0);
    }
    out.assign(wave.begin(), wave.end());  // the waveform branch, plus the spectrogram branch's inverse
    for (int s = 0; s < kSources; s++)
        for (int c = 0; c < 2; c++) {
            const float* re = spec.data() + (size_t)(s * 4 + 2 * c) * kBins * kFrames;
            inverse(re, re + (size_t)kBins * kFrames, out.data() + (size_t)(s * 2 + c) * kL);
        }
#if RBM_MAC_X86 || defined(_WIN32)
    if (std::string why; !usable(l, r, out, &why)) {
        bool first = g_state.exchange(-1) != -1;
        std::lock_guard<std::mutex> lk(g_lock);
        if (first) {
            logf("betterstems: HTDemucs gave unusable stems (%s): it is off until rekordbox restarts, rekordbox's own "
                 "engine separates", why.c_str());
            betterstems_backend::unload();
        }
        return false;
    }
#endif
    return true;
}

static bool separate(const float* inL, const float* inR, int n, float* const out[8]) {
    if (!active() || n <= 0) return false;
    // Segments across the input, overlapping, cross-faded with triangular weights (as Demucs' apply_model); a
    // shorter input is zero-padded around its middle.
    int count = n <= kL ? 1 : (int)ceil((n - kL) / (0.75 * kL)) + 1;
    std::vector<float> weight(kL);
    for (int j = 0; j < kL; j++) weight[j] = (float)std::min(j + 1, kL - j) / (kL / 2);
    std::vector<float> acc((size_t)8 * n, 0.f), sum(n, 0.f), l(kL), r(kL), res;
    for (int k = 0; k < count; k++) {
        int start = count == 1 ? -(kL - n) / 2 : (int)llround((double)k * (n - kL) / (count - 1));
        for (int j = 0; j < kL; j++) {
            int i = start + j;
            l[j] = i >= 0 && i < n ? inL[i] : 0, r[j] = i >= 0 && i < n ? inR[i] : 0;
        }
        double segmentAt = now();
        if (!segment(l.data(), r.data(), res)) return false;
        // While a deck plays, a rest after each segment (betterstems_live_rest times the segment's own time, half a
        // second at most; 0: none): the model fills the GPU the waveforms are drawn with, segment after segment.
        if (double rest = slowdrive::deckPlays() ? cfgNum("betterstems_live_rest", 0) : 0; rest > 0)
            std::this_thread::sleep_for(std::chrono::duration<double>(std::min(0.5, (now() - segmentAt) * rest)));
        for (int j = 0; j < kL; j++) {
            int i = start + j;
            if (i < 0 || i >= n) continue;
            float w = count == 1 ? 1 : weight[j];
            sum[i] += w;
            for (int o = 0; o < 8; o++) acc[(size_t)o * n + i] += w * res[(size_t)o * kL + j];
        }
    }
    for (int o = 0; o < 8; o++)
        for (int i = 0; i < n; i++) out[o][i] = acc[(size_t)o * n + i] / sum[i];
    return true;
}

static void init() {
    if (betterstems_backend::installed())
        startupTask([] {
            loadModel();
#if !defined(_WIN32) && !RBM_MAC_X86  // (an Intel Mac ran its timed segments in loadModel)
            // One segment of silence, so the first track of the session doesn't pay for the model's first run (the
            // GPU pipeline is built then: it took 6 s when it met Beat This!'s first run on a deck load).
            if (g_state != 2 || !cfgInt("betterstems_warmup", 1)) return;
            double t0 = now();
            std::vector<float> quiet(kL), out;
            if (segment(quiet.data(), quiet.data(), out)) logf("betterstems: model warmed up (%.1f s)", now() - t0);
#endif
        });
    else
        logf("betterstems: no model (tools/models/htdemucs_coreml.sh makes it): rekordbox's own engine separates");
}

// For other mods (see rbmod.h, "between mods"): stemcache separates with HTDemucs while betterstems is on.
RBM_PROVIDE("betterstems", "betterstems.active", active, &g_on);
RBM_PROVIDE("betterstems", "betterstems.version", version, &g_on);
RBM_PROVIDE("betterstems", "betterstems.separate", separate, &g_on);

RBM_MOD({.name = "betterstems", .init = init, .enabled = &g_on});
