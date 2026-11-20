// See beatthis.h. Pipeline (mirrors beat_this 1.1.0):
//   preprocessing.LogMelSpect  -> logMel()      (torchaudio MelSpectrogram, power=1, normalized="frame_length")
//   soxr.resample(HQ)          -> resampleTo22k()
//   inference.split_predict_aggregate -> Model::analyzeMel()
//   postprocessor "minimal"    -> postprocess()
#include "beatthis.h"

#include "beatthis_backend.h"
#include "dsp.h"
#include "../core/plat.h"

#include <string.h>

#include <algorithm>
#include <chrono>
#include <functional>
#include <cmath>
#include <numeric>

namespace beatthis {

namespace {
constexpr int kSr = 22050, kFft = 1024, kHop = 441, kBins = kFft / 2 + 1;
constexpr double kFMin = 30, kFMax = 11000;
constexpr int kBorder = 6;   // beat_this Spect2Frames defaults

double nowMs() {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

double bessel0(double x) {
    double s = 1, t = 1;
    for (int k = 1; k < 60; k++) { t *= (x / (2 * k)) * (x / (2 * k)); s += t; if (t < 1e-18 * s) break; }
    return s;
}

// ---- resampler -------------------------------------------------------------------------------
// Kaiser-windowed sinc, linear phase, designed to follow soxr "HQ" (what beat_this uses): flat to
// ~10.3 kHz, -6 dB at ~10.6 kHz, > 80 dB down from 11.0 kHz (for 22.05 kHz output).
constexpr double kCutoff = 0.957;   // fraction of output Nyquist (tuned against soxr HQ on the demo tracks)
constexpr double kHalfWidth = 96;               // zero crossings per side, in output periods
constexpr double kBeta = 12.0;

void parallelFor(size_t count, size_t block, const std::function<void(size_t b0, size_t b1)>& body) {
    size_t nb = (count + block - 1) / block;
    rbm::plat::parallelFor(nb, [&](size_t i) { body(i * block, std::min(count, (i + 1) * block)); });
}
}  // namespace

std::vector<float> resampleTo22k(const float* x, size_t n, double sampleRate) { return resample(x, n, sampleRate, kSr); }

std::vector<float> resample(const float* x, size_t n, double sampleRate, long outR) {
    long inR = std::lround(sampleRate);
    if (inR <= 0 || n == 0) return {};
    if (inR == outR) return std::vector<float>(x, x + n);
    long g = std::gcd(inR, outR);
    long P = inR / g, L = outR / g;
    double r = (double)inR / outR, s = std::max(r, 1.0);
    int H = (int)std::ceil(kHalfWidth * s), taps = 2 * H + 1;
    long nph = L <= 1024 ? L : 1024;
    std::vector<float> table((nph + 1) * taps);
    double i0b = bessel0(kBeta);
    for (long p = 0; p <= nph; p++) {
        double frac = (double)p / nph, sum = 0;
        std::vector<double> h(taps);
        for (int j = -H; j <= H; j++) {
            double d = j - frac, u = d / (kHalfWidth * s);
            double w = std::fabs(u) < 1 ? bessel0(kBeta * std::sqrt(1 - u * u)) / i0b : 0;
            double a = M_PI * kCutoff * d / s;
            double sinc = std::fabs(a) < 1e-12 ? 1 : std::sin(a) / a;
            h[j + H] = kCutoff / s * sinc * w;
            sum += h[j + H];
        }
        for (int j = 0; j < taps; j++) table[p * taps + j] = (float)(h[j] / sum);
    }
    size_t nOut = (size_t)((double)n * L / P);
    std::vector<float> xp(n + 2 * H + 2 * P + 8, 0.f), out(nOut);
    std::copy(x, x + n, xp.begin() + H);
    const float* xpp = xp.data();
    const float* tab = table.data();
    float* o = out.data();
    parallelFor(nOut, 16384, [&](size_t k0, size_t k1) {
        if (L == 1) {
            dsp::decimate(xpp + k0 * P, (size_t)P, tab, o + k0, k1 - k0, taps);
        } else {
            for (size_t k = k0; k < k1; k++) {
                size_t kp = k * P, i0 = kp / L;
                long ph = nph == L ? (long)(kp % L) : std::lround((double)(kp % L) / L * nph);
                o[k] = dsp::dot(xpp + i0, tab + ph * taps, taps);
            }
        }
    });
    return out;
}

// ---- log-mel ---------------------------------------------------------------------------------
namespace {
double hzToMel(double f) {   // slaney
    const double fsp = 200.0 / 3, minLogHz = 1000, minLogMel = minLogHz / fsp, logstep = std::log(6.4) / 27;
    return f >= minLogHz ? minLogMel + std::log(f / minLogHz) / logstep : f / fsp;
}
double melToHz(double m) {
    const double fsp = 200.0 / 3, minLogHz = 1000, minLogMel = minLogHz / fsp, logstep = std::log(6.4) / 27;
    return m >= minLogMel ? minLogHz * std::exp(logstep * (m - minLogMel)) : m * fsp;
}
// torchaudio.functional.melscale_fbanks(513, 30, 11000, 128, 22050, norm=None, "slaney"): [bin][mel]
const std::vector<float>& melBank() {
    static std::vector<float> fb = [] {
        std::vector<float> v(kBins * kMels, 0.f);
        double mlo = hzToMel(kFMin), mhi = hzToMel(kFMax);
        std::vector<double> fp(kMels + 2);
        for (int i = 0; i < kMels + 2; i++) fp[i] = melToHz(mlo + (mhi - mlo) * i / (kMels + 1));
        for (int b = 0; b < kBins; b++) {
            double f = (double)b * (kSr / 2) / (kBins - 1);
            for (int m = 0; m < kMels; m++) {
                double down = -(fp[m] - f) / (fp[m + 1] - fp[m]);
                double up = (fp[m + 2] - f) / (fp[m + 2] - fp[m + 1]);
                v[b * kMels + m] = (float)std::max(0.0, std::min(down, up));
            }
        }
        return v;
    }();
    return fb;
}
const std::vector<float>& hann() {   // torch.hann_window(1024), periodic
    static std::vector<float> w = [] {
        std::vector<float> v(kFft);
        for (int i = 0; i < kFft; i++) v[i] = (float)(0.5 - 0.5 * std::cos(2 * M_PI * i / kFft));
        return v;
    }();
    return w;
}
}  // namespace

std::vector<float> logMel(const float* x, size_t n, int* framesOut) {
    if (n <= kFft / 2) { if (framesOut) *framesOut = 0; return {}; }
    int frames = 1 + (int)(n / kHop);
    if (framesOut) *framesOut = frames;
    // reflect padding of kFft/2 on both sides (torch pad_mode="reflect")
    const int pad = kFft / 2;
    std::vector<float> xp(n + 2 * pad);
    std::copy(x, x + n, xp.begin() + pad);
    for (int i = 0; i < pad; i++) {
        xp[pad - 1 - i] = x[i + 1];
        xp[pad + n + i] = x[n - 2 - i];
    }
    std::vector<float> mel((size_t)frames * kMels);
    const float* fb = melBank().data();
    const float* win = hann().data();
    dsp::RealFFT fft(10);
    const float* xpp = xp.data();
    float* mo = mel.data();
    const float scale = 0.5f / 32.f;   // vDSP real FFT is 2x the DFT; torch normalized="frame_length" divides by sqrt(1024)
    constexpr int kBlock = 128;
    parallelFor(frames, kBlock, [&](size_t f0, size_t f1) {
        int nf = (int)(f1 - f0);
        std::vector<float> mag((size_t)nf * kBins), fr(kFft), re(kFft / 2), im(kFft / 2);
        for (int i = 0; i < nf; i++) {
            for (int k = 0; k < kFft; k++) fr[k] = xpp[(f0 + i) * kHop + k] * win[k];
            fft.forward(fr.data(), re.data(), im.data());
            float* m = mag.data() + (size_t)i * kBins;
            m[0] = std::fabs(re[0]) * scale;
            m[kBins - 1] = std::fabs(im[0]) * scale;
            for (int k = 1; k < kFft / 2; k++) m[k] = std::sqrt(re[k] * re[k] + im[k] * im[k]) * scale;
        }
        float* out = mo + f0 * kMels;
        dsp::sgemm(nf, kMels, kBins, mag.data(), fb, out);
        int cnt = nf * kMels;
        for (int k = 0; k < cnt; k++) out[k] *= 1000.f;
        dsp::log1p(out, cnt);
    });
    return mel;
}

// ---- postprocess (Postprocessor.postp_minimal) ------------------------------------------------
namespace {
std::vector<double> peakFrames(const float* x, int n) {
    // max_pool1d(k=7, stride 1, pad 3) equality, > 0, then deduplicate_peaks(width=1)
    std::vector<int> pk;
    for (int i = 0; i < n; i++) {
        if (!(x[i] > 0)) continue;
        float m = -1e30f;
        for (int j = std::max(0, i - 3); j <= std::min(n - 1, i + 3); j++) m = std::max(m, x[j]);
        if (x[i] == m) pk.push_back(i);
    }
    std::vector<double> out;
    if (pk.empty()) return out;
    double p = pk[0];
    int c = 1;
    for (size_t i = 1; i < pk.size(); i++) {
        if (pk[i] - p <= 1) { c++; p += (pk[i] - p) / c; }
        else { out.push_back(p); p = pk[i]; c = 1; }
    }
    out.push_back(p);
    return out;
}
}  // namespace

Result postprocess(const float* beat, const float* down, int frames) {
    Result r;
    for (double f : peakFrames(beat, frames)) r.beats.push_back(f / 50.0);
    std::vector<double> db;
    for (double f : peakFrames(down, frames)) db.push_back(f / 50.0);
    if (!r.beats.empty()) {
        for (double& d : db) {
            size_t bi = 0;
            for (size_t i = 1; i < r.beats.size(); i++)
                if (std::fabs(r.beats[i] - d) < std::fabs(r.beats[bi] - d)) bi = i;
            d = r.beats[bi];
        }
    }
    std::sort(db.begin(), db.end());
    db.erase(std::unique(db.begin(), db.end()), db.end());
    r.downbeats = db;
    return r;
}

// ---- the network (beatthis_backend.h) ---------------------------------------------------------

Result Model::analyzeMel(const float* mel, int T, Debug* dbg) const {
    Result res;
    if (!impl_ || T <= 0) return res;
    Impl& I = *impl_;
    const int cs = chunkFrames(I), bs = kBorder;
    // split_piece(avoid_short_end=True)
    std::vector<long> starts;
    for (long s = -bs; s < T - bs; s += cs - 2 * bs) starts.push_back(s);
    if (T > cs - 2 * bs) starts.back() = T - (cs - bs);
    std::vector<float> beat(T, -1000.f), down(T, -1000.f);
    std::vector<float> chunk((size_t)cs * kMels), pb(cs), pd(cs);
    double t0 = nowMs();
    // keep_first: earlier chunks win, so process in reverse order and let them overwrite.
    for (int ci = (int)starts.size() - 1; ci >= 0; ci--) {
        long start = starts[ci];
        long a = std::max(start, 0L), b = std::min(start + cs, (long)T);
        long left = std::max(0L, -start), right = std::max(0L, std::min((long)bs, start + cs - T));
        std::fill(chunk.begin(), chunk.end(), 0.f);
        memcpy(chunk.data() + left * kMels, mel + a * kMels, (b - a) * kMels * sizeof(float));
        int len = (int)std::min<long>(cs, left + (b - a) + right);
        if (!predictChunk(I, chunk.data(), len, pb.data(), pd.data())) return Result{};
        // discard border, write to piece[start+bs, start+cs-bs) clipped to [0,T)
        for (long j = bs; j < len - bs; j++) {
            long dst = start + j;
            if (dst < 0 || dst >= T || dst >= start + cs - bs) continue;
            beat[dst] = pb[j];
            down[dst] = pd[j];
        }
    }
    double t1 = nowMs();
    res = postprocess(beat.data(), down.data(), T);
    if (dbg) {
        dbg->frames = T; dbg->chunks = (int)starts.size();
        dbg->inferMs = t1 - t0; dbg->postMs = nowMs() - t1;
        dbg->beatLogits = beat; dbg->downLogits = down;
    }
    return res;
}

Result Model::analyze(const float* mono, size_t n, double sampleRate, Debug* dbg) const {
    if (!impl_ || n < sampleRate) return {};
    double t0 = nowMs();
    std::vector<float> r = resampleTo22k(mono, n, sampleRate);
    double t1 = nowMs();
    int frames = 0;
    std::vector<float> mel = logMel(r.data(), r.size(), &frames);
    double t2 = nowMs();
    Result res = analyzeMel(mel.data(), frames, dbg);
    if (dbg) {
        dbg->resampleMs = t1 - t0; dbg->melMs = t2 - t1;
        dbg->mel = std::move(mel);
    }
    return res;
}

// ---- constant-tempo grid ----------------------------------------------------------------------
// How the beats are counted (each beat's index on the grid, from the gaps between them):
//   Steps:    gap by gap, a gap of k periods counted as k (the original way).
//   Anchored: from the last beat that sat on the grid; a beat between (an off-beat Beat This! marked too) is
//             counted but doesn't move the anchor.
//   Halves:   as Anchored, in half periods: off-beats get half indices and sit on the line with the beats.
// In sections where Beat This! marks the off-beats too, Steps rounds those half-period gaps up to whole beats
// (round(0.5) = 1: Beat This!'s 20 ms frames make them exact), the count runs ahead and the tempo comes out
// wrong (117.38 BPM for a 125 BPM track). Neither of the others is better on every track (tested on 257), so
// fitGrid fits all three and keeps the one most beats sit on.
enum class Count { Steps, Anchored, Halves };
static Grid fitCounted(const Result& r, double minBpm, double maxBpm, Count how);

Grid fitGrid(const Result& r, double minBpm, double maxBpm) {
    Grid best;
    for (Count how : {Count::Steps, Count::Anchored, Count::Halves}) {
        Grid g = fitCounted(r, minBpm, maxBpm, how);
        if (g.ok && (!best.ok || g.confidence > best.confidence)) best = g;
    }
    return best;
}

static Grid fitCounted(const Result& r, double minBpm, double maxBpm, Count how) {
    Grid g;
    const std::vector<double>& t = r.beats;
    if (t.size() < 8) return g;
    auto median = [](std::vector<double> v) {
        std::sort(v.begin(), v.end());
        return v.empty() ? 0.0 : (v.size() & 1 ? v[v.size() / 2] : 0.5 * (v[v.size() / 2 - 1] + v[v.size() / 2]));
    };
    std::vector<double> ibi;
    for (size_t i = 1; i < t.size(); i++) ibi.push_back(t[i] - t[i - 1]);
    double m = median(ibi);
    if (m <= 0) return g;
    // Beat index of each detected beat by unwrapping (tolerates missing beats and small tempo errors in m;
    // see Count).
    std::vector<double> idx(t.size(), 0.0);
    auto unwrap = [&](double period) {
        if (how == Count::Steps) {
            for (size_t i = 1; i < t.size(); i++)
                idx[i] = idx[i - 1] + std::floor((t[i] - t[i - 1]) / period + 0.5);   // 0 for a spurious extra beat
            return;
        }
        double step = how == Count::Halves ? 0.5 : 1.0, lastT = t[0], lastI = 0;
        for (size_t i = 1; i < t.size(); i++) {
            double q = (t[i] - lastT) / (period * step), k = std::floor(q + 0.5);
            idx[i] = lastI + k * step;
            if (k >= 1 && std::fabs(q - k) < 0.25) lastT = t[i], lastI = idx[i];
        }
    };
    // Robust start: slope from the median of 16-beat spans (Theil-Sen style), intercept from the
    // median, inliers = within 50 ms of that line. Then least squares over inliers with iterated
    // outlier rejection.
    std::vector<char> in(t.size(), 1);
    double a = t[0], b = m;
    unwrap(m);
    {
        std::vector<double> sl;
        for (size_t i = 0; i + 16 < t.size(); i++)
            if (idx[i + 16] > idx[i]) sl.push_back((t[i + 16] - t[i]) / (idx[i + 16] - idx[i]));
        if (!sl.empty()) b = median(sl);
        unwrap(b);
        std::vector<double> ic;
        for (size_t i = 0; i < t.size(); i++) ic.push_back(t[i] - b * idx[i]);
        a = median(ic);
        for (size_t i = 0; i < t.size(); i++) in[i] = std::fabs(t[i] - (a + b * idx[i])) <= 0.05;
    }
    for (int it = 0; it < 8; it++) {
        unwrap(b);
        double sx = 0, sy = 0, sxx = 0, sxy = 0, cnt = 0;
        for (size_t i = 0; i < t.size(); i++) if (in[i]) {
            sx += idx[i]; sy += t[i]; sxx += idx[i] * idx[i]; sxy += idx[i] * t[i]; cnt++;
        }
        double den = cnt * sxx - sx * sx;
        if (cnt < 4 || den <= 0) return g;
        b = (cnt * sxy - sx * sy) / den;
        a = (sy - b * sx) / cnt;
        std::vector<double> res;
        for (size_t i = 0; i < t.size(); i++) res.push_back(std::fabs(t[i] - (a + b * idx[i])));
        std::vector<double> inRes;
        for (size_t i = 0; i < t.size(); i++) if (in[i]) inRes.push_back(res[i]);
        double thr = std::max(0.03, 4.0 * 1.4826 * median(inRes));   // >= 30 ms
        thr = std::min(thr, 0.07);
        int changed = 0;
        for (size_t i = 0; i < t.size(); i++) {
            char n = res[i] <= thr;
            if (n != in[i]) changed++;
            in[i] = n;
        }
        if (!changed && it > 0) break;
    }
    // The beats are at the indices most detected beats sit on: whole, or half when the count started on an off-beat.
    {
        int whole = 0, half = 0;
        for (size_t i = 0; i < t.size(); i++) if (in[i]) {
            double f = idx[i] - std::floor(idx[i]);
            (std::fabs(f - 0.5) < 0.25 ? half : whole)++;
        }
        if (half > whole) a += b / 2;
    }
    double bpm = 60.0 / b;
    // Fold into the allowed range (octave choice: keep the tracker's tempo when it is in range).
    while (bpm < minBpm && bpm * 2 <= maxBpm * 1.0001) bpm *= 2;
    while (bpm > maxBpm && bpm / 2 >= minBpm * 0.9999) bpm /= 2;
    bpm = std::round(bpm * 100) / 100;
    double per = 60.0 / bpm;
    // Phase: the fitted line's intercept (anchored at the start of the track, where DJs care), taken
    // modulo the rounded period.
    double fb = std::fmod(a, per);
    if (fb < 0) fb += per;
    int near = 0;
    for (double x : t) {
        double d = std::fmod(x - fb, per);
        if (d < 0) d += per;
        if (std::min(d, per - d) <= 0.07) near++;
    }
    int votes[4] = {0, 0, 0, 0};
    for (double d : r.downbeats) {
        long k = std::lround((d - fb) / per);
        votes[((k % 4) + 4) % 4]++;
    }
    int best = 0;
    for (int i = 1; i < 4; i++) if (votes[i] > votes[best]) best = i;
    g.bpm = bpm; g.firstBeat = fb; g.downbeatIndex = best;
    g.confidence = (double)near / t.size();
    g.ok = true;
    return g;
}

}  // namespace beatthis
