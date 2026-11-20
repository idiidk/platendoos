// See dsp.h: Accelerate.
#include "dsp.h"
#define ACCELERATE_NEW_LAPACK
#include <Accelerate/Accelerate.h>
#include <vector>

namespace dsp {

void sgemm(int m, int n, int k, const float* a, const float* b, float* c) {
    cblas_sgemm(CblasRowMajor, CblasNoTrans, CblasNoTrans, m, n, k, 1.f, a, k, b, n, 0.f, c, n);
}

struct RealFFT::Impl {
    FFTSetup setup;
    int log2n, n;
};
RealFFT::RealFFT(int log2n) : impl(new Impl{vDSP_create_fftsetup(log2n, kFFTRadix2), log2n, 1 << log2n}) {}
RealFFT::~RealFFT() { vDSP_destroy_fftsetup(impl->setup); }

void RealFFT::forward(const float* in, float* re, float* im) const {
    DSPSplitComplex z = {re, im};
    vDSP_ctoz((const DSPComplex*)in, 2, &z, 1, impl->n / 2);
    vDSP_fft_zrip(impl->setup, &z, 1, impl->log2n, kFFTDirection_Forward);
}
void RealFFT::inverse(const float* re, const float* im, float* out) const {
    std::vector<float> r(re, re + impl->n / 2), i(im, im + impl->n / 2);
    DSPSplitComplex z = {r.data(), i.data()};
    vDSP_fft_zrip(impl->setup, &z, 1, impl->log2n, kFFTDirection_Inverse);
    vDSP_ztoc(&z, 1, (DSPComplex*)out, 2, impl->n / 2);
}

void biquad(const float* a, const float b[5], float* c, size_t n) { vDSP_deq22(a, 1, b, c, 1, n); }
float sumSquares(const float* a, size_t n) {
    float e = 0;
    vDSP_svesq(a, 1, &e, n);
    return e;
}
float dot(const float* a, const float* b, size_t n) {
    float r;
    vDSP_dotpr(a, 1, b, 1, &r, n);
    return r;
}
void decimate(const float* x, size_t stride, const float* filter, float* out, size_t n, size_t taps) {
    vDSP_desamp(x, (vDSP_Stride)stride, filter, out, n, taps);
}
void log1p(float* x, int n) { vvlog1pf(x, x, &n); }

}  // namespace dsp
