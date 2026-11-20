#include "keycnn.h"
#include "dsp.h"
#include "../core/plat.h"
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <algorithm>

namespace keycnn {

const char* const kKeyNames[24] = {"A", "Bb", "B", "C", "Db", "D", "Eb", "E", "F", "F#", "G", "Ab",
                                   "Am", "Bbm", "Bm", "Cm", "C#m", "Dm", "D#m", "Em", "Fm", "F#m", "Gm", "G#m"};

static const int kRate = 44100, kFrame = 8192, kLog2Frame = 13, kHop = kRate / 5;

bool Model::load(const std::string& path, std::string* error) {
    auto fail = [&](const char* why) { if (error) *error = why; layers_.clear(); return false; };
    FILE* f = rbm::plat::openFile(path, "rb");
    if (!f) return fail("cannot open model file");
    std::vector<char> d;
    char buf[65536];
    for (size_t n; (n = fread(buf, 1, sizeof buf, f)) > 0;) d.insert(d.end(), buf, buf + n);
    fclose(f);
    size_t o = 0;
    auto rd = [&](void* p, size_t n) { if (o + n > d.size()) return false; memcpy(p, d.data() + o, n); o += n; return true; };
    auto ints = [&](int* p, int n) { return rd(p, sizeof(int) * n); };
    auto floats = [&](std::vector<float>& v, size_t n) { v.resize(n); return rd(v.data(), sizeof(float) * n); };

    char magic[4];
    int hdr[3], count;
    if (!rd(magic, 4) || memcmp(magic, "RBKC", 4) || !ints(hdr, 3)) return fail("bad model header");
    bands_ = hdr[0], binLo_ = hdr[1], binHi_ = hdr[2];
    if (!floats(filterbank_, (size_t)(binHi_ - binLo_) * bands_) || !ints(&count, 1)) return fail("truncated filterbank");
    layers_.clear();
    for (int i = 0; i < count; i++) {
        Layer l;
        if (!ints(&l.type, 1)) return fail("truncated layers");
        if (l.type == 1) {
            if (!ints(&l.pad, 1)) return fail("truncated pad");
        } else if (l.type == 2) {
            int s[4];
            std::vector<float> w;
            if (!ints(s, 4) || !floats(w, (size_t)s[0] * s[1] * s[2] * s[3])) return fail("truncated conv");
            l.ci = s[0], l.co = s[1], l.kh = s[2], l.kw = s[3];
            // [ci][co][a][b] (already flipped for correlation) -> [(a*kw+b)*ci + c][co] for one GEMM
            l.w.resize(w.size());
            for (int c = 0; c < l.ci; c++)
                for (int k = 0; k < l.co; k++)
                    for (int a = 0; a < l.kh; a++)
                        for (int b = 0; b < l.kw; b++)
                            l.w[((size_t)(a * l.kw + b) * l.ci + c) * l.co + k] = w[(((size_t)c * l.co + k) * l.kh + a) * l.kw + b];
        } else if (l.type == 3) {
            int s[2];
            if (!ints(s, 2) || !floats(l.scale, s[0]) || !floats(l.shift, s[0])) return fail("truncated batchnorm");
            l.co = s[0], l.elu = s[1] != 0;
        } else if (l.type != 4 && l.type != 5) {
            return fail("unknown layer type");
        }
        layers_.push_back(std::move(l));
    }
    return true;
}

// Linear-interpolation resampling: only content below ~2.1 kHz matters for the model.
static std::vector<float> resample(const float* x, size_t n, double rate) {
    if (fabs(rate - kRate) < 0.5) return std::vector<float>(x, x + n);
    size_t m = (size_t)floor(n * (double)kRate / rate);
    std::vector<float> y(m);
    double step = rate / kRate;
    for (size_t i = 0; i < m; i++) {
        double p = i * step;
        size_t j = (size_t)p;
        double fr = p - j;
        y[i] = j + 1 < n ? (float)(x[j] * (1 - fr) + x[j + 1] * fr) : x[n - 1];
    }
    return y;
}

std::vector<float> Model::predict(const float* mono, size_t n, double sampleRate) const {
    if (!loaded() || !mono || sampleRate <= 0) return {};
    std::vector<float> x = resample(mono, n, sampleRate);

    // madmom: frames centred on i*hop (zero padded), np.hanning window, |rfft| without Nyquist,
    // log filterbank (bins binLo..binHi), log10(1 + x).
    int T = (int)((x.size() + kHop - 1) / kHop), F = bands_, nb = binHi_ - binLo_;
    if (T < 16) return {};
    std::vector<float> win(kFrame), frame(kFrame), re(kFrame / 2), im(kFrame / 2), mag((size_t)T * nb);
    for (int i = 0; i < kFrame; i++) win[i] = 0.5f - 0.5f * cosf(2 * (float)M_PI * i / (kFrame - 1));
    dsp::RealFFT fft(kLog2Frame);
    for (int t = 0; t < T; t++) {
        long start = (long)t * kHop - kFrame / 2;
        for (int i = 0; i < kFrame; i++) {
            long s = start + i;
            frame[i] = s >= 0 && s < (long)x.size() ? x[s] * win[i] : 0;
        }
        fft.forward(frame.data(), re.data(), im.data());
        for (int b = 0; b < nb; b++) {  // vDSP's real FFT is scaled by 2
            int k = binLo_ + b;
            mag[(size_t)t * nb + b] = 0.5f * hypotf(re[k], im[k]);
        }
    }
    std::vector<float> a((size_t)T * F);
    dsp::sgemm(T, F, nb, mag.data(), filterbank_.data(), a.data());
    for (float& v : a) v = log10f(v + 1);

    // Network on a [T][F][C] tensor.
    int C = 1;
    std::vector<float> b;
    for (const Layer& l : layers_) {
        if (l.type == 1) {
            int p = l.pad, T2 = T + 2 * p, F2 = F + 2 * p;
            b.assign((size_t)T2 * F2 * C, 0.f);
            for (int t = 0; t < T; t++) memcpy(&b[((size_t)(t + p) * F2 + p) * C], &a[(size_t)t * F * C], sizeof(float) * F * C);
            a.swap(b), T = T2, F = F2;
        } else if (l.type == 2) {
            // im2col over blocks of output rows, one GEMM per block.
            int To = T - l.kh + 1, Fo = F - l.kw + 1, K = l.kh * l.kw * l.ci;
            if (To <= 0 || Fo <= 0) return {};
            b.assign((size_t)To * Fo * l.co, 0.f);
            const int rows = 32;
            std::vector<float> col((size_t)rows * Fo * K);
            for (int t0 = 0; t0 < To; t0 += rows) {
                int nt = std::min(rows, To - t0);
                for (int t = 0; t < nt; t++)
                    for (int f = 0; f < Fo; f++) {
                        float* c = &col[((size_t)t * Fo + f) * K];
                        for (int ka = 0; ka < l.kh; ka++)
                            memcpy(c + (size_t)ka * l.kw * l.ci, &a[((size_t)(t0 + t + ka) * F + f) * C], sizeof(float) * l.kw * l.ci);
                    }
                dsp::sgemm(nt * Fo, l.co, K, col.data(), l.w.data(), &b[(size_t)t0 * Fo * l.co]);
            }
            a.swap(b), T = To, F = Fo, C = l.co;
        } else if (l.type == 3) {
            size_t n = (size_t)T * F;
            for (size_t i = 0; i < n; i++)
                for (int c = 0; c < C; c++) {
                    float& v = a[i * C + c];
                    v = v * l.scale[c] + l.shift[c];
                    if (l.elu && v < 0) v = expm1f(v);
                }
        } else if (l.type == 4) {
            int T2 = T / 2, F2 = F / 2;
            b.resize((size_t)T2 * F2 * C);
            for (int t = 0; t < T2; t++)
                for (int f = 0; f < F2; f++)
                    for (int c = 0; c < C; c++) {
                        auto v = [&](int dt, int df) { return a[((size_t)(2 * t + dt) * F + 2 * f + df) * C + c]; };
                        b[((size_t)t * F2 + f) * C + c] = std::max(std::max(v(0, 0), v(0, 1)), std::max(v(1, 0), v(1, 1)));
                    }
            a.swap(b), T = T2, F = F2;
        } else if (l.type == 5) {
            std::vector<double> s(C, 0.0);
            for (size_t i = 0; i < (size_t)T * F; i++)
                for (int c = 0; c < C; c++) s[c] += a[i * C + c];
            a.assign(C, 0.f);
            for (int c = 0; c < C; c++) a[c] = (float)(s[c] / ((double)T * F));
            T = F = 1;
        }
    }
    if (a.size() != 24) return {};
    float mx = *std::max_element(a.begin(), a.end()), sum = 0;
    for (float& v : a) sum += (v = expf(v - mx));
    for (float& v : a) v /= sum;
    return a;
}

}  // namespace keycnn
