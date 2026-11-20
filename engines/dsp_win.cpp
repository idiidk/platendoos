// See dsp.h: plain C++ (Windows). Keeps vDSP's conventions: packed real FFT (DC in re[0], Nyquist in im[0]),
// forward = 2 x the DFT, inverse of a true DFT = n x the signal, biquad = vDSP_deq22, decimate = vDSP_desamp.
#include "dsp.h"
#include <algorithm>
#include <cmath>
#include <vector>

namespace dsp {

void sgemm(int m, int n, int k, const float* a, const float* b, float* c) {
    for (int i = 0; i < m; i++) std::fill(c + (size_t)i * n, c + (size_t)(i + 1) * n, 0.f);
    const int KB = 256, NB = 512;
    for (int k0 = 0; k0 < k; k0 += KB) {
        int k1 = std::min(k, k0 + KB);
        for (int n0 = 0; n0 < n; n0 += NB) {
            int n1 = std::min(n, n0 + NB);
            for (int i = 0; i < m; i++) {
                float* ci = c + (size_t)i * n;
                const float* ai = a + (size_t)i * k;
                for (int p = k0; p < k1; p++) {
                    float av = ai[p];
                    const float* bp = b + (size_t)p * n;
                    for (int j = n0; j < n1; j++) ci[j] += av * bp[j];
                }
            }
        }
    }
}

// A real FFT of n samples is a complex FFT of m = n/2 points (even samples as re, odd as im) plus a post-pass.
struct RealFFT::Impl {
    int log2n, n, m;
    std::vector<float> cs, sn;      // cos / sin(2 pi k / n), k < m/2 + 1 (also the FFT's twiddles: n/m = 2 apart)
    std::vector<float> wc, ws;      // cos / sin(2 pi k / m), k < m/2: the m-point FFT's twiddles
    std::vector<int> rev;           // bit reversal of the m points

    // In-place m-point complex FFT on elements re[j * s], im[j * s]; sign -1 forward, +1 inverse.
    void fft(float* re, float* im, int s, bool inverse) const {
        for (int i = 0; i < m; i++) {
            int j = rev[i];
            if (j > i) {
                std::swap(re[i * s], re[j * s]);
                std::swap(im[i * s], im[j * s]);
            }
        }
        const float sg = inverse ? 1.f : -1.f;
        for (int len = 2; len <= m; len <<= 1) {
            int half = len >> 1, step = m / len;
            for (int base = 0; base < m; base += len) {
                for (int j = 0; j < half; j++) {
                    float wr = wc[j * step], wi = sg * ws[j * step];
                    float* ar = re + (size_t)(base + j) * s;
                    float* ai = im + (size_t)(base + j) * s;
                    float* br = re + (size_t)(base + j + half) * s;
                    float* bi = im + (size_t)(base + j + half) * s;
                    float tr = *br * wr - *bi * wi, ti = *br * wi + *bi * wr;
                    *br = *ar - tr, *bi = *ai - ti;
                    *ar += tr, *ai += ti;
                }
            }
        }
    }
};

RealFFT::RealFFT(int log2n) : impl(new Impl) {
    Impl& p = *impl;
    p.log2n = log2n, p.n = 1 << log2n, p.m = std::max(1, p.n / 2);
    int m = p.m, n = p.n;
    const double tau = 6.283185307179586476925286766559;
    p.cs.resize(m / 2 + 1), p.sn.resize(m / 2 + 1);
    for (int k = 0; k <= m / 2; k++) p.cs[k] = (float)std::cos(tau * k / n), p.sn[k] = (float)std::sin(tau * k / n);
    p.wc.resize(std::max(1, m / 2)), p.ws.resize(std::max(1, m / 2));
    for (int k = 0; k < m / 2; k++) p.wc[k] = (float)std::cos(tau * k / m), p.ws[k] = (float)std::sin(tau * k / m);
    p.rev.resize(m);
    int bits = std::max(0, log2n - 1);
    for (int i = 0; i < m; i++) {
        int r = 0;
        for (int b = 0; b < bits; b++)
            if (i >> b & 1) r |= 1 << (bits - 1 - b);
        p.rev[i] = r;
    }
}
RealFFT::~RealFFT() = default;

void RealFFT::forward(const float* in, float* re, float* im) const {
    const Impl& p = *impl;
    int m = p.m;
    for (int j = 0; j < m; j++) re[j] = in[2 * j], im[j] = in[2 * j + 1];
    p.fft(re, im, 1, false);
    float r0 = re[0], i0 = im[0];
    re[0] = 2 * (r0 + i0), im[0] = 2 * (r0 - i0);
    for (int k = 1; k <= m / 2; k++) {
        int q = m - k;
        float ar = re[k], ai = im[k], br = re[q], bi = -im[q];  // a = Z[k], b = conj Z[m-k]
        float er = ar + br, ei = ai + bi;
        float dr = ar - br, di = ai - bi;
        float orr = di, oi = -dr;                                // o = -i (a - b)
        float wr = p.cs[k], wi = -p.sn[k];                       // W = e^{-2 pi i k / n}
        float tr = wr * orr - wi * oi, ti = wr * oi + wi * orr;
        re[k] = er + tr, im[k] = ei + ti;                        // X[k]
        re[q] = er - tr, im[q] = -(ei - ti);                     // X[m-k] = conj(e - W o)
    }
}

void RealFFT::inverse(const float* re, const float* im, float* out) const {
    const Impl& p = *impl;
    int m = p.m;
    // Z in out as interleaved complex (re at even, im at odd positions), then an in-place m-point inverse FFT.
    float x0 = re[0], xn = im[0];
    out[0] = (x0 + xn) + 0.f;   // Xe' = X0 + Xn/2, Xo' = X0 - Xn/2, Z[0] = Xe' + i Xo'
    out[1] = x0 - xn;
    for (int k = 1; k <= m / 2; k++) {
        int q = m - k;
        float ar = re[k], ai = im[k], br = re[q], bi = -im[q];  // X[k], conj X[m-k]
        float er = ar + br, ei = ai + bi;
        float dr = ar - br, di = ai - bi;
        float wr = p.cs[k], wi = p.sn[k];                        // conj W = e^{+2 pi i k / n}
        float or_ = wr * dr - wi * di, oi = wr * di + wi * dr;   // Xo'
        // Z[k] = Xe' + i Xo'; Z[m-k] = conj Xe' + i conj Xo'
        out[2 * k] = er - oi, out[2 * k + 1] = ei + or_;
        out[2 * q] = er + oi, out[2 * q + 1] = -ei + or_;
    }
    p.fft(out, out + 1, 2, true);
}

void biquad(const float* a, const float b[5], float* c, size_t n) {
    float c1 = c[0], c2 = c[1];  // c[i], c[i + 1]
    for (size_t i = 0; i < n; i++) {
        float y = a[i + 2] * b[0] + a[i + 1] * b[1] + a[i] * b[2] - c2 * b[3] - c1 * b[4];
        c[i + 2] = y;
        c1 = c2, c2 = y;
    }
}

float sumSquares(const float* a, size_t n) {
    float s[8] = {};
    size_t i = 0;
    for (; i + 8 <= n; i += 8)
        for (int j = 0; j < 8; j++) s[j] += a[i + j] * a[i + j];
    float t = ((s[0] + s[4]) + (s[1] + s[5])) + ((s[2] + s[6]) + (s[3] + s[7]));
    for (; i < n; i++) t += a[i] * a[i];
    return t;
}

float dot(const float* a, const float* b, size_t n) {
    float s[8] = {};
    size_t i = 0;
    for (; i + 8 <= n; i += 8)
        for (int j = 0; j < 8; j++) s[j] += a[i + j] * b[i + j];
    float t = ((s[0] + s[4]) + (s[1] + s[5])) + ((s[2] + s[6]) + (s[3] + s[7]));
    for (; i < n; i++) t += a[i] * b[i];
    return t;
}

void decimate(const float* x, size_t stride, const float* filter, float* out, size_t n, size_t taps) {
    for (size_t i = 0; i < n; i++) {
        const float* p = x + i * stride;
        float s = 0;
        for (size_t j = 0; j < taps; j++) s += p[j] * filter[j];
        out[i] = s;
    }
}

void log1p(float* x, int n) {
    for (int i = 0; i < n; i++) x[i] = std::log1p(x[i]);
}

}  // namespace dsp
