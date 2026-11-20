// The signal-processing primitives the engines (keycnn, beatthis, betterstems, bettergrid) need from the platform:
// macOS has them in Accelerate (dsp_mac.mm); a Windows backend can use any FFT/BLAS it likes as long as it keeps
// the conventions noted here.
#pragma once
#include <stddef.h>
#include <memory>

namespace dsp {

// Row-major C[m][n] = A[m][k] * B[k][n], all dense (leading dimensions k, n, n).
void sgemm(int m, int n, int k, const float* a, const float* b, float* c);

// Real FFT of n = 1 << log2n samples. Spectra are split-complex halves of n/2 floats, packed like vDSP's:
// bin 0 in re[0] (DC) and im[0] (Nyquist), then bins 1 .. n/2 - 1.
class RealFFT {
public:
    explicit RealFFT(int log2n);
    ~RealFFT();
    RealFFT(const RealFFT&) = delete;
    RealFFT& operator=(const RealFFT&) = delete;
    // The spectrum of in[n], scaled by 2 (vDSP's convention). Safe to call from several threads.
    void forward(const float* in, float* re, float* im) const;
    // The signal of a spectrum packed as above (a true DFT, without the factor 2): out[n] = n * the signal.
    void inverse(const float* re, const float* im, float* out) const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

// vDSP_deq22: c[i + 2] = a[i + 2] * b[0] + a[i + 1] * b[1] + a[i] * b[2] - c[i + 1] * b[3] - c[i] * b[4] for
// i = 0 .. n - 1; a and c hold n + 2 floats, c[0..1] being the filter's initial state.
void biquad(const float* a, const float b[5], float* c, size_t n);
float sumSquares(const float* a, size_t n);
float dot(const float* a, const float* b, size_t n);
// out[i] = sum over j < taps of x[i * stride + j] * filter[j], for i < n (vDSP_desamp).
void decimate(const float* x, size_t stride, const float* filter, float* out, size_t n, size_t taps);
void log1p(float* x, int n);  // in place (vvlog1pf)

}  // namespace dsp
