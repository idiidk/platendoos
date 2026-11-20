// betterfx DSP: the signal processing of the betterfx mod, plain C++ without rekordbox, so that
// tools/dev/betterfx_test.cpp can run it offline. See mods/betterfx.mm and docs/dsp/*.md.
#pragma once
#include <math.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <atomic>
#include <algorithm>

namespace bfx {

static inline double clampd(double x, double lo, double hi) { return x < lo ? lo : x > hi ? hi : x; }
static inline double onePole(double fs, double ms) { return 1.0 - exp(-1.0 / (fs * ms * 0.001)); }

// ---- Color FX "Filter" ---------------------------------------------------------------------------------------
// Replaces ColorFxFilterDjm900NXS2's DSP. Kept from rekordbox: the knob law (cutoff curves, centre dead zone,
// LPF on the left, HPF on the right), the parallel LP and HP paths that both always run, the dry/LP/HP
// crossfades and their 10 ms ramps, the resonance law of the PARAMETER knob and the reduced resonance just past
// the centre on the HPF side. Changed: the cutoff is pre-warped exactly (rekordbox's isn't, which pulled the
// top of the LPF down to ~12-15 kHz), the knob is used unrounded and smoothed per sample (no 256-step
// staircase), the filters are zero-delay-feedback state-variable filters with per-sample coefficients, and the
// resonance gain compensation keeps peaks under ~+9 dB (rekordbox's reaches +24 dB mid-sweep at full
// PARAMETER). At the default PARAMETER (Q = 2) the level is the same as rekordbox's.
struct Svf {  // Simper's trapezoidal SVF, double state
    double ic1 = 0, ic2 = 0;
    inline void run(double x, double a1, double a2, double a3, double k, double* lp, double* hp) {
        double v3 = x - ic2;
        double v1 = a1 * ic1 + a2 * v3;
        double v2 = ic2 + a2 * ic1 + a3 * v3;
        ic1 = 2 * v1 - ic1;
        ic2 = 2 * v2 - ic2;
        *lp = v2;
        *hp = x - k * v1 - v2;
    }
    bool ok() const { return isfinite(ic1) && isfinite(ic2) && fabs(ic1) < 1e6 && fabs(ic2) < 1e6; }
};

struct CfxFilter {
    void reclaim() { *this = CfxFilter(); }
    double fs = 0;
    double p = -1, r = -1;              // smoothed knob and PARAMETER, 0..255 like rekordbox's integers
    double gD = 1, gL = 0, gH = 0;      // dry / LPF / HPF gains (ramped)
    double cp = -1e9, cr = -1e9;        // what the coefficients below were made for
    double la1, la2, la3, lk, lc;       // LPF: SVF coefficients, k = 1/Q, output compensation
    double ha1, ha2, ha3, hk, hc;       // HPF
    double aSmooth = 0, ramp = 0;
    Svf lp[2], hp[2];

    void reset(double newFs) {
        fs = newFs;
        p = r = -1;
        gD = 1, gL = gH = 0;
        cp = cr = -1e9;
        for (int c = 0; c < 2; c++) lp[c] = Svf(), hp[c] = Svf();
        aSmooth = onePole(fs, 4.0);  // knob smoothing: ~4 ms
        ramp = 100.0 / fs;           // gain ramps: 0 -> 1 in 10 ms (rekordbox's)
    }

    // 1/Q from the PARAMETER knob (rekordbox's law: 0.25 .. 2 (default, centre) .. ~28).
    static double invQ(double r) {
        return r <= 128 ? 0.5 + 3.5 * (1 - r / 128) * (1 - r / 128) : std::max(0.03, 0.5 - (r - 128) * (15.0 / 4096));
    }

    void design(double fc, double k, double* a1, double* a2, double* a3) const {
        double g = tan(M_PI * std::min(fc, 0.45 * fs) / fs);
        *a1 = 1 / (1 + g * (g + k));
        *a2 = g * *a1;
        *a3 = g * *a2;
    }

    void coefficients() {
        cp = p, cr = r;
        double pl = std::min(p, 126.0) / 126, ph = std::max(p - 130, 0.0) / 126;
        double fcL = 100 + 25000 * pl * pl * pl;  // rekordbox: 100 Hz (full left) .. 25.1 kHz (centre)
        double fcH = 20 + 8200 * ph * ph * ph;    // 20 Hz (centre) .. 8 kHz (full right)
        double k = invQ(r);
        double kh = k;
        if (fcH < 249.9) {  // just past the centre the HPF starts without resonance (no sub-bass boom)
            double y = (fcH - 20) / 230;
            kh = std::max(k, std::min(2.0, 4 * (1 - y) * (1 - y) * (1 - y) + 0.03));
        }
        lk = k, hk = kh;
        // A 2nd-order peak is ~Q high: keep it under 2.8 (+9 dB) at any cutoff. 1 at Q <= 2.8 (the default is 2).
        lc = std::min(1.0, 2.8 * k);
        hc = std::min(1.0, 2.8 * kh);
        design(fcL, lk, &la1, &la2, &la3);
        design(fcH, hk, &ha1, &ha2, &ha3);
    }

    // In place, like rekordbox's processEffectConcrete: out = dry*in + lpf*gL + hpf*gH.
    void process(float* L, float* R, int n, double knob, double param, bool on, double newFs) {
        if (newFs != fs) reset(newFs);
        double tp = clampd(knob, 0, 1) * 255, tr = clampd(param, 0, 1) * 255;
        if (p < 0) p = tp, r = tr;
        float* ch[2] = {L, R};
        for (int i = 0; i < n; i++) {
            p += (tp - p) * aSmooth;
            r += (tr - r) * aSmooth;
            if (fabs(p - cp) > 1e-3 || fabs(r - cr) > 1e-3) coefficients();
            double tD = 1, tL = 0, tH = 0;
            if (on) {
                if (p < 128) tD = clampd((p - 110) / 16, 0, 1), tL = 1 - tD;
                else tH = clampd((p - 130) / 16, 0, 1), tD = 1 - tH;
            }
            gD += clampd(tD - gD, -ramp, ramp);
            gL += clampd(tL - gL, -ramp, ramp);
            gH += clampd(tH - gH, -ramp, ramp);
            double wl = gL * lc, wh = gH * hc;
            for (int c = 0; c < 2; c++) {
                if (!ch[c]) continue;
                double x = ch[c][i], l, h, dummy;
                lp[c].run(x, la1, la2, la3, lk, &l, &dummy);
                hp[c].run(x, ha1, ha2, ha3, hk, &dummy, &h);
                ch[c][i] = (float)(gD * x + wl * l + wh * h);
            }
        }
        for (int c = 0; c < 2; c++)
            if (!lp[c].ok() || !hp[c].ok()) lp[c] = Svf(), hp[c] = Svf();
    }
};

// ---- biquads for the EQs -------------------------------------------------------------------------------------
// Digital biquad from an analog prototype (b2 s^2 + b1 s + b0) / (a2 s^2 + a1 s + a0) by the bilinear transform
// with K = 2 fs (the prototypes use rekordbox's pre-warped w0 = 2 fs tan(pi f0 / fs)).
struct Coefs { double b0, b1, b2, a1, a2; };
static inline Coefs bilinear(double fs, double b2, double b1, double b0, double a2, double a1, double a0) {
    double K = 2 * fs, K2 = K * K;
    double d0 = a2 * K2 + a1 * K + a0;
    Coefs c;
    c.b0 = (b2 * K2 + b1 * K + b0) / d0;
    c.b1 = (2 * b0 - 2 * b2 * K2) / d0;
    c.b2 = (b2 * K2 - b1 * K + b0) / d0;
    c.a1 = (2 * a0 - 2 * a2 * K2) / d0;
    c.a2 = (a2 * K2 - a1 * K + a0) / d0;
    return c;
}
static inline double prewarp(double fs, double f0) { return 2 * fs * tan(M_PI * f0 / fs); }

// rekordbox's Filter::make_eq_high_shelving / make_eq_low_shelving / make_eq_peaking (see docs/dsp/eq.md).
static inline Coefs highShelf(double fs, double f0, double d, double dB) {
    double A = pow(10, dB / 20), w = prewarp(fs, f0), sA = sqrt(A);
    if (A >= 1) return bilinear(fs, A, A * d * w, A * w * w, 1, d * sA * w, A * w * w);
    return bilinear(fs, A, d * sA * w, w * w, 1, d * w, w * w);
}
static inline Coefs lowShelf(double fs, double f0, double d, double dB) {
    double A = pow(10, dB / 20), w = prewarp(fs, f0), sA = sqrt(A);
    if (A >= 1) return bilinear(fs, 1, d * w, w * w, 1, d * w / sA, w * w / A);
    return bilinear(fs, 1, d * sA * w, A * w * w, 1, d * w, w * w);
}
static inline Coefs peaking(double fs, double f0, double q, double dB) {
    double A = pow(10, dB / 20), w = prewarp(fs, f0);
    if (A >= 1) return bilinear(fs, 1, A * w / q, w * w, 1, w / q, w * w);
    return bilinear(fs, 1, w / q, w * w, 1, w / (q * A), w * w);
}
static inline Coefs butterLP(double fs, double f0) {
    double w = prewarp(fs, f0);
    return bilinear(fs, 0, 0, w * w, 1, M_SQRT2 * w, w * w);
}
static inline Coefs butterHP(double fs, double f0) {
    double w = prewarp(fs, f0);
    return bilinear(fs, 1, 0, 0, 1, M_SQRT2 * w, w * w);
}

// Direct form I in double: changing coefficients mid-stream is benign (rekordbox's TDF-II rebuilds its state to
// get the same behaviour).
struct Biquad {
    Coefs c{1, 0, 0, 0, 0};
    double x1 = 0, x2 = 0, y1 = 0, y2 = 0;
    inline double run(double x) {
        double y = c.b0 * x + c.b1 * x1 + c.b2 * x2 - c.a1 * y1 - c.a2 * y2;
        x2 = x1, x1 = x, y2 = y1, y1 = y;
        return y;
    }
    void clear() { x1 = x2 = y1 = y2 = 0; }
    bool ok() const { return isfinite(y1) && isfinite(y2) && fabs(y1) < 1e6; }
};

// ---- DJM-900NXS2 EQ (rekordbox's default EQ type) ------------------------------------------------------------
// The same three filters and gain laws as EqualizerNXS2 (high shelf 2.2 kHz, bell 1 kHz from rekordbox's own
// tables, low shelf 250 Hz; +6 .. -26 dB), with a fixed smoothing time instead of "spread over one audio buffer"
// (a fast cut with small buffers changed by up to 26 dB in 1.5 ms, a thump on bass) and the knob read every
// buffer (rekordbox's dirty flag could drop a change).
struct Nxs2Eq {
    void reclaim() { *this = Nxs2Eq(); }
    static constexpr int kMaxCh = 8;  // constexpr: std::min binds it by reference (an -O0 link needs a definition otherwise)
    double fs = 0;
    double k[3] = {-1, -1, -1};      // smoothed knobs (HI, MID, LOW), 0..255
    double made[3] = {-1e9, -1e9, -1e9};
    Biquad f[3][kMaxCh];
    double a = 0;
    const int16_t* knobTable = nullptr;  // rekordbox's MID tables: knob -> index -> Q, dB
    const float* qTable = nullptr;
    const float* gainTable = nullptr;

    void reset(double newFs) {
        fs = newFs;
        for (int b = 0; b < 3; b++) {
            k[b] = -1, made[b] = -1e9;
            for (auto& q : f[b]) q = Biquad();
        }
        a = onePole(fs, 5.0) ;
    }
    // HI and LOW: rekordbox's closed form (0x102ab6750..).
    static double shelfDb(double k) {
        k = clampd(k, 0, 255);
        if (k < 126) return -20 * (1.15 - k / 126) * (1.15 - k / 126) + 0.45;
        if (k <= 129) return 0;
        return (k - 129) / 21;
    }
    void midLaw(double k, double* q, double* dB) const {
        k = clampd(k, 0, 255);
        int k0 = (int)floor(k), k1 = std::min(k0 + 1, 255);
        double t = k - k0;
        int i0 = knobTable[k0], i1 = knobTable[k1];
        *q = qTable[i0] + (qTable[i1] - qTable[i0]) * t;
        *dB = gainTable[i0] + (gainTable[i1] - gainTable[i0]) * t;
    }
    Coefs design(int b, double kb) const {
        if (b == 0) return highShelf(fs, 2200, 2.0, shelfDb(kb));
        if (b == 2) return lowShelf(fs, 250, 1.5, shelfDb(kb));
        double q, dB;
        midLaw(kb, &q, &dB);
        return peaking(fs, 1000, q, dB);
    }
    // ch[c] + start .. + n, in place; target knobs 0..255.
    void process(float* const* ch, int nch, int start, int n, const float target[3], double newFs) {
        if (newFs != fs) reset(newFs);
        nch = std::min(nch, kMaxCh);
        for (int b = 0; b < 3; b++)
            if (k[b] < 0) k[b] = target[b];
        const int step = 16;  // coefficients follow the smoothed knobs every 16 samples
        for (int i0 = 0; i0 < n; i0 += step) {
            int m = std::min(step, n - i0);
            double am = 1 - pow(1 - a, m);
            for (int b = 0; b < 3; b++) {
                k[b] += (target[b] - k[b]) * am;
                if (fabs(k[b] - target[b]) < 1e-3) k[b] = target[b];
                if (fabs(k[b] - made[b]) > 1e-3) {
                    made[b] = k[b];
                    Coefs c = design(b, k[b]);
                    for (int c2 = 0; c2 < nch; c2++) f[b][c2].c = c;
                }
            }
            for (int c = 0; c < nch; c++) {
                float* s = ch[c] + start + i0;
                for (int i = 0; i < m; i++) s[i] = (float)f[2][c].run(f[1][c].run(f[0][c].run(s[i])));
            }
        }
        for (int b = 0; b < 3; b++)
            for (int c = 0; c < nch; c++)
                if (!f[b][c].ok()) f[b][c].clear();
    }
};

// ---- Isolator ------------------------------------------------------------------------------------------------
// rekordbox's 3-band Linkwitz-Riley 4th-order split at 284 / 4752 Hz with its gain table (true kill at 0); the
// band gains move with a ~8 ms smoothing curve instead of a 3.3 ms straight ramp (a full bass kill in 3.3 ms is
// close to gating 50 Hz content: a click).
struct Isolator {
    void reclaim() { *this = Isolator(); }
    static constexpr int kMaxCh = 8;  // constexpr: std::min binds it by reference (an -O0 link needs a definition otherwise)
    double fs = 0;
    double g[3] = {-1, -1, -1};  // smoothed gains HI, MID, LOW
    double a = 0;
    Biquad lo1[kMaxCh], lo2[kMaxCh], mh1[kMaxCh], mh2[kMaxCh], ml1[kMaxCh], ml2[kMaxCh], hi1[kMaxCh], hi2[kMaxCh];

    void reset(double newFs) {
        fs = newFs;
        g[0] = g[1] = g[2] = -1;
        a = onePole(fs, 8.0 / 3);  // time constant 2.7 ms: 95% of the way in 8 ms
        Coefs lpLo = butterLP(fs, 284), hpLo = butterHP(fs, 284), lpHi = butterLP(fs, 4752), hpHi = butterHP(fs, 4752);
        for (int c = 0; c < kMaxCh; c++) {
            lo1[c] = lo2[c] = Biquad(), lo1[c].c = lo2[c].c = lpLo;
            mh1[c] = mh2[c] = Biquad(), mh1[c].c = mh2[c].c = hpLo;
            ml1[c] = ml2[c] = Biquad(), ml1[c].c = ml2[c].c = lpHi;
            hi1[c] = hi2[c] = Biquad(), hi1[c].c = hi2[c].c = hpHi;
        }
    }
    void process(float* const* ch, int nch, int start, int n, const double target[3], double newFs) {
        if (newFs != fs) reset(newFs);
        nch = std::min(nch, kMaxCh);
        for (int b = 0; b < 3; b++)
            if (g[b] < 0) g[b] = target[b];
        double g0[3] = {g[0], g[1], g[2]};
        for (int c = 0; c < nch; c++) {
            float* s = ch[c] + start;
            double gh = g0[0], gm = g0[1], gl = g0[2];
            for (int i = 0; i < n; i++) {
                gh += (target[0] - gh) * a, gm += (target[1] - gm) * a, gl += (target[2] - gl) * a;
                double x = s[i];
                double lo = lo2[c].run(lo1[c].run(x));
                double mid = ml2[c].run(ml1[c].run(mh2[c].run(mh1[c].run(x))));
                double hi = hi2[c].run(hi1[c].run(x));
                s[i] = (float)(gl * lo + gm * mid + gh * hi);
            }
            if (c == nch - 1) g[0] = gh, g[1] = gm, g[2] = gl;
            if (!lo2[c].ok() || !hi2[c].ok() || !ml2[c].ok()) reset(fs);
        }
    }
};

// ---- Reverb --------------------------------------------------------------------------------------------------
// Replaces ReverbCore's 5 parallel combs + 2 allpasses per channel (a 1979-style Schroeder/Moorer reverb: metallic
// ring, grainy tail) with an 8-line feedback delay network: input diffusion (4 allpasses per channel), a
// Householder feedback matrix (every line feeds every other: dense, smooth tail), slowly modulated delays (no
// fixed resonances) and per-line damping so highs die out faster than lows. Its decay time and room size are
// rekordbox's own (read from its combs every block); the caller matches its level to rekordbox's network.
// Fixed pool of FDN buffers, made off the audio thread (Fdn::prepare() on the main thread; lazily on first use in
// the offline test). The audio thread only takes and gives buffers (atomics: no malloc, no lock, no log). A
// buffer that is given back is dirty; scrubPool() (main thread, every tick) zeroes it. If the audio thread needs
// one before that happened it zeroes a dirty one itself (a ~1.2 MB memset, still no allocation).
static const int kFdnN = 8, kFdnLen = 1 << 15, kFdnAp = 4096;
struct FdnPool {
    static const int K = 8;  // reverbs running at once (rekordbox has a handful of FX units); more keep rekordbox's own
    static size_t floats() { return (size_t)kFdnN * kFdnLen + 8 * kFdnAp; }
    std::atomic<float*> buf[K] = {};
    std::atomic<int> st[K] = {};  // 0 free and clean, 1 in use, 2 dirty, 3 being scrubbed
    std::atomic<bool> ready{false};
    void init() {
        if (ready.load(std::memory_order_acquire)) return;
        for (int i = 0; i < K; i++) {
            float* p = (float*)calloc(floats(), sizeof(float));
            if (p) memset(p, 0, floats() * sizeof(float));  // touch every page now, not in the audio thread
            buf[i].store(p), st[i].store(p ? 0 : 1);
        }
        ready.store(true, std::memory_order_release);
    }
    float* take() {
        if (!ready.load(std::memory_order_acquire)) return nullptr;
        for (int pass = 0; pass < 2; pass++)
            for (int i = 0; i < K; i++) {
                int want = pass == 0 ? 0 : 2, e = want;
                if (st[i].compare_exchange_strong(e, 1)) {
                    float* p = buf[i].load();
                    if (want == 2) memset(p, 0, floats() * sizeof(float));
                    return p;
                }
            }
        return nullptr;
    }
    void give(float* p) {
        for (int i = 0; i < K; i++)
            if (buf[i].load() == p) return st[i].store(2);
    }
    void scrub() {  // main thread
        for (int i = 0; i < K; i++) {
            int e = 2;
            if (st[i].compare_exchange_strong(e, 3)) {
                memset(buf[i].load(), 0, floats() * sizeof(float));
                st[i].store(0);
            }
        }
    }
};

struct Fdn {
    static const int N = 8, kLen = 1 << 15;  // per line: up to 0.34 s at 96 kHz
    float* buf = nullptr;                    // N lines of kLen, then the diffusers
    static const int kApLen = 4096;
    int w = 0;                               // write index (shared by the lines)
    double fs = 0;
    double delay[N] = {};                    // smoothed line lengths, samples
    double lp[N] = {};                       // damping filter states
    double phase[N] = {};
    int apW[2][4] = {};
    double t60 = -1, scale = -1;

    // milliseconds at room size 50 (rekordbox's combs span 21.5 .. 41.5 ms there); mutually prime-ish
    static constexpr double kMs[N] = {31.71, 37.11, 40.23, 44.14, 50.47, 56.89, 61.73, 68.71};
    static constexpr double kApMs[2][4] = {{4.77, 3.59, 12.73, 9.31}, {5.13, 3.27, 13.41, 8.89}};

    static FdnPool& pool() {
        static FdnPool p;
        return p;
    }
    static void prepare() { pool().init(); }     // main thread, before the audio thread can need a buffer
    static void scrubPool() { pool().scrub(); }  // main thread, every tick
    bool alloc() {  // audio thread: a clean buffer from the pool (never allocates); false when all are taken
        if (!buf) {
            if (!pool().ready.load()) pool().init();  // only the offline test gets here
            buf = pool().take();
        }
        return buf != nullptr;
    }
    void release() {  // give the buffer back (it is scrubbed off the audio thread)
        if (buf) pool().give(buf);
        buf = nullptr;
    }
    void reclaim() {  // forget everything, including the buffer
        release();
        *this = Fdn();
    }
    void renew() {  // silence: swap in a clean buffer rather than clearing this one
        float* old = buf;
        buf = nullptr;
        double keep = fs;
        *this = Fdn();
        fs = keep;
        buf = pool().take();
        if (old) {
            if (buf) pool().give(old);
            else buf = old, memset(buf, 0, FdnPool::floats() * sizeof(float));  // pool empty: clear it in place
        }
    }
    void clear() { renew(); }  // silence, same rate
    float* line(int j) { return buf + (size_t)j * kLen; }
    float* ap(int c, int k) { return buf + (size_t)N * kLen + (size_t)(c * 4 + k) * kApLen; }

    inline double diffuse(int c, double x) {
        for (int k = 0; k < 4; k++) {
            int len = std::min(kApLen - 1, (int)(kApMs[c][k] * 0.001 * fs * std::max(0.6, std::min(scale, 1.6))));
            float* a = ap(c, k);
            int r = (apW[c][k] - len) & (kApLen - 1);
            double d = a[r];
            double v = x + 0.6 * d;  // allpass: g = 0.6
            a[apW[c][k]] = (float)v;
            apW[c][k] = (apW[c][k] + 1) & (kApLen - 1);
            x = d - 0.6 * v;
        }
        return x;
    }

    // In place on L/R (n samples): replaces the input with the reverb's wet signal (like rekordbox's combs).
    // decay: T60 in seconds; room: rekordbox's room scale (1 at size 50).
    void process(float* L, float* R, int n, double decay, double room, double newFs) {
        if (!buf) return;
        if (fs == 0) fs = newFs;  // the first block: the buffer is as clean as it gets (renew took a second one from the pool)
        else if (newFs != fs) {
            renew();
            fs = newFs;
        }
        decay = clampd(decay, 0.05, 30);
        room = clampd(room, 0.2, 2.4);
        if (scale < 0) {
            scale = room;
            for (int j = 0; j < N; j++) delay[j] = kMs[j] * 0.001 * fs * room;
        }
        t60 = decay;
        const double aDelay = onePole(fs, 60);  // room size changes glide (like rekordbox's)
        const double depth = 0.00022 * fs;      // modulation: +-0.22 ms
        double gain[N], damp[N];
        for (int j = 0; j < N; j++) {
            double target = kMs[j] * 0.001 * fs * room;
            double d = delay[j] + (target - delay[j]) * (1 - pow(1 - aDelay, n));
            double g = pow(10, -3 * d / (decay * fs));                // DC: decays to -60 dB in T60
            double gHi = pow(10, -3 * d / (decay * 0.45 * fs));        // highs: in 45% of T60
            double r = gHi / g;
            gain[j] = g, damp[j] = (1 - r) / (1 + r);
        }
        scale = room;
        static const double outL[N] = {1, 0.45, -1, -0.45, 1, -0.45, -1, 0.45};
        static const double outR[N] = {0.45, 1, 0.45, -1, -0.45, 1, -0.45, -1};
        for (int i = 0; i < n; i++) {
            double xl = diffuse(0, L[i]), xr = diffuse(1, R[i]);
            double y[N], sum = 0;
            for (int j = 0; j < N; j++) {
                double target = kMs[j] * 0.001 * fs * room;
                delay[j] += (target - delay[j]) * aDelay;
                double dd = delay[j];
                if (j < 4) {  // four lines are modulated, at different slow rates
                    phase[j] += (0.31 + 0.17 * j) / fs;
                    if (phase[j] >= 1) phase[j] -= 1;
                    dd += depth * sin(2 * M_PI * phase[j]);
                }
                dd = clampd(dd, 2, kLen - 4);
                double rp = w - dd;
                int r0 = (int)floor(rp);
                double fr = rp - r0;
                float* ln = line(j);
                double a = ln[r0 & (kLen - 1)], b = ln[(r0 + 1) & (kLen - 1)];
                double v = a + (b - a) * fr;
                lp[j] = v * (1 - damp[j]) + lp[j] * damp[j];  // damping
                y[j] = lp[j] * gain[j];
                sum += y[j];
            }
            double h = sum * (2.0 / N);  // Householder reflection: x - 2/N * sum
            double ol = 0, orr = 0;
            for (int j = 0; j < N; j++) {
                ol += outL[j] * y[j], orr += outR[j] * y[j];
                double in = (j & 1) ? xr : xl;
                line(j)[w] = (float)(y[j] - h + 0.5 * in);
            }
            w = (w + 1) & (kLen - 1);
            L[i] = (float)(ol * 0.35), R[i] = (float)(orr * 0.35);
        }
        for (int j = 0; j < N; j++)
            if (!isfinite(lp[j])) { clear(); return; }
    }
};

}  // namespace bfx
