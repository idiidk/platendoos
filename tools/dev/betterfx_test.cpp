// Offline checks of mods/betterfx_dsp.h: responses, transparency at the centre, peaks, sweep smoothness.
//   clang++ -std=c++17 -O2 -o /tmp/bfxtest tools/dev/betterfx_test.cpp && /tmp/bfxtest
#include "../../mods/betterfx_dsp.h"
#include <stdio.h>
#include <vector>
#include <functional>

static const double FS = 44100;
static int failures = 0;
static void check(bool ok, const char* what) {
    printf("%s %s\n", ok ? "  ok  " : "  FAIL", what);
    if (!ok) failures++;
}

// Steady-state gain (dB) of a block processor at frequency f.
template <class P> static double gainDb(P proc, double f) {
    const int n = 44100;
    std::vector<float> L(n), R(n);
    for (int i = 0; i < n; i++) L[i] = R[i] = (float)sin(2 * M_PI * f * i / FS) * 0.25f;
    proc(L.data(), R.data(), n);
    double in = 0, out = 0;
    for (int i = n / 2; i < n; i++) {
        double x = sin(2 * M_PI * f * i / FS) * 0.25;
        in += x * x, out += (double)L[i] * L[i];
    }
    return 10 * log10(out / in);
}

static double peakOf(std::vector<double>& freqs, std::function<double(double)> g, double* at) {
    double best = -1e9;
    for (double f : freqs) {
        double v = g(f);
        if (v > best) best = v, *at = f;
    }
    return best;
}

int main() {
    std::vector<double> freqs;
    for (double f = 20; f < 20000; f *= 1.03) freqs.push_back(f);

    printf("Color FX Filter\n");
    {
        // transparency at the centre
        bfx::CfxFilter f;
        std::vector<float> L(4096), R(4096), L0, R0;
        for (int i = 0; i < 4096; i++) L[i] = (float)sin(i * 0.1) * 0.5f, R[i] = (float)cos(i * 0.07) * 0.5f;
        L0 = L, R0 = R;
        f.process(L.data(), R.data(), 4096, 0.5, 0.5, true, FS);
        bool same = L == L0 && R == R0;
        check(same, "knob at the centre: output bit-identical to the input");
        // -3 dB point of the LPF vs the intended (analog) cutoff, and the resonance peak
        for (double knob : {0.0, 0.25, 0.375, 0.42}) {
            auto g = [&](double fr) {
                bfx::CfxFilter x;
                return gainDb([&](float* l, float* r, int n) { x.process(l, r, n, knob, 0.5, true, FS); }, fr);
            };
            double p = knob * 255, pl = std::min(p, 126.0) / 126, fc = 100 + 25000 * pl * pl * pl, at = 0;
            double pk = peakOf(freqs, g, &at);
            double m3 = 0;
            for (double fr : freqs)
                if (fr > at && g(fr) < -3) { m3 = fr; break; }
            printf("    LPF knob %.3f: design %6.0f Hz, peak %+5.1f dB at %6.0f Hz, -3 dB at %6.0f Hz\n", knob, fc, pk, at, m3);
            if (knob == 0.375) check(at > fc * 0.85 && at < fc * 1.15, "peak lands on the designed cutoff (pre-warped)");
            check(pk < 7.0, "default PARAMETER: peak under +7 dB");
        }
        for (double knob : {0.25, 0.75}) {
            auto g = [&](double fr) {
                bfx::CfxFilter x;
                return gainDb([&](float* l, float* r, int n) { x.process(l, r, n, knob, 1.0, true, FS); }, fr);
            };
            double at = 0, pk = peakOf(freqs, g, &at);
            printf("    full PARAMETER, knob %.2f: peak %+5.1f dB at %6.0f Hz\n", knob, pk, at);
            check(pk < 9.5, "full PARAMETER: peak under +9.5 dB");
        }
        // fast sweep over a sine: no jumps far beyond the signal's own slope
        bfx::CfxFilter s;
        s.reset(FS);
        double worst = 0, prev = 0;
        const int n = 44100;
        for (int blk = 0; blk < n / 64; blk++) {
            float L2[64], R2[64];
            for (int i = 0; i < 64; i++) L2[i] = R2[i] = (float)(0.5 * sin(2 * M_PI * 440 * (blk * 64 + i) / FS));
            double knob = 0.5 + 0.5 * sin(blk * 64 / FS * 2 * M_PI * 2);  // two full sweeps a second
            s.process(L2, R2, 64, knob, 0.8, true, FS);
            for (int i = 0; i < 64; i++) worst = std::max(worst, fabs(L2[i] - prev)), prev = L2[i];
        }
        printf("    fast sweep, largest step between samples %.4f (a 440 Hz sine at 0.5 has 0.031)\n", worst);
        check(worst < 0.2, "fast sweep: no clicks");
        // on -> off -> on
        bfx::CfxFilter o;
        prev = 0, worst = 0;
        for (int blk = 0; blk < 200; blk++) {
            float L2[64], R2[64];
            for (int i = 0; i < 64; i++) L2[i] = R2[i] = (float)(0.5 * sin(2 * M_PI * 100 * (blk * 64 + i) / FS));
            o.process(L2, R2, 64, 0.1, 0.5, (blk / 50) % 2 == 0, FS);
            for (int i = 0; i < 64; i++) worst = std::max(worst, fabs(L2[i] - prev)), prev = L2[i];
        }
        check(worst < 0.05, "switching on and off: no clicks");
    }

    printf("EQ (DJM-900NXS2)\n");
    {
        // rekordbox's MID tables aren't here: a stand-in law (index = knob, flat in the middle)
        static int16_t knob[256];
        static float q[256], gain[256];
        for (int i = 0; i < 256; i++) {
            knob[i] = (int16_t)i;
            gain[i] = i < 127 ? -26.0f * (127 - i) / 127 : i > 128 ? 6.0f * (i - 128) / 127 : 0;
            q[i] = 0.4f;
        }
        auto run = [&](float hi, float mid, float lo, double f) {
            bfx::Nxs2Eq eq;
            eq.knobTable = knob, eq.qTable = q, eq.gainTable = gain;
            float t[3] = {hi, mid, lo};
            return gainDb([&](float* l, float* r, int n) {
                float* ch[2] = {l, r};
                eq.process(ch, 2, 0, n, t, FS);
            }, f);
        };
        double flat = 0;
        for (double f : {30.0, 250.0, 1000.0, 5000.0, 15000.0}) flat = std::max(flat, fabs(run(127.5f, 127.5f, 127.5f, f)));
        check(flat < 0.01, "all knobs at the centre: flat");
        double lowKill = run(127.5f, 127.5f, 0, 30), hiKill = run(0, 127.5f, 127.5f, 18000);
        printf("    LOW kill at 30 Hz %.1f dB, HI kill at 18 kHz %.1f dB (rekordbox: -24.7 / about -25)\n", lowKill, hiKill);
        check(lowKill < -23 && hiKill < -23, "kills reach about -25 dB");
        double boost = run(255, 127.5f, 127.5f, 15000);
        check(boost > 5.5 && boost < 6.5, "HI at full: +6 dB");
        // a fast LOW kill in small buffers: smooth
        bfx::Nxs2Eq eq;
        eq.knobTable = knob, eq.qTable = q, eq.gainTable = gain;
        double prev = 0, worst = 0;
        for (int blk = 0; blk < 400; blk++) {
            float L2[32], R2[32];
            for (int i = 0; i < 32; i++) L2[i] = R2[i] = (float)(0.5 * sin(2 * M_PI * 60 * (blk * 32 + i) / FS));
            float* ch[2] = {L2, R2};
            float t[3] = {127.5f, 127.5f, blk < 200 ? 127.5f : 0.0f};
            eq.process(ch, 2, 0, 32, t, FS);
            for (int i = 0; i < 32; i++) worst = std::max(worst, fabs(L2[i] - prev)), prev = L2[i];
        }
        printf("    LOW kill in 32-sample buffers: largest step %.4f (a 60 Hz sine at 0.5 has 0.0043)\n", worst);
        check(worst < 0.01, "fast kill: no thump");
    }

    printf("Isolator\n");
    {
        auto run = [&](double hi, double mid, double lo, double f) {
            bfx::Isolator iso;
            double t[3] = {hi, mid, lo};
            return gainDb([&](float* l, float* r, int n) {
                float* ch[2] = {l, r};
                iso.process(ch, 2, 0, n, t, FS);
            }, f);
        };
        double flat = 0;
        for (double f : freqs) flat = std::max(flat, fabs(run(1, 1, 1, f)));
        printf("    unity: largest deviation %.3f dB\n", flat);
        check(flat < 0.06, "unity: flat (rekordbox: within 0.04 dB)");
        double kill = run(1, 1, 0, 50);
        printf("    LOW kill at 50 Hz: %.0f dB\n", kill);
        check(kill < -50, "LOW kill");
    }
    printf("Reverb (FDN)\n");
    {
        // impulse response: decay rate vs the requested T60, stereo spread, density, stability at the extremes
        for (double t60 : {0.6, 2.0, 6.0}) {
            bfx::Fdn f;
            f.alloc();
            const int n = (int)(FS * std::min(8.0, t60 * 1.3)), blk = 256;
            std::vector<float> L(n, 0), R(n, 0);
            L[0] = R[0] = 1;
            for (int i = 0; i < n; i += blk) f.process(&L[i], &R[i], std::min(blk, n - i), t60, 1.0, FS);
            // energy in 50 ms windows; slope between 0.1 s and 0.7 * t60 (dB per second)
            auto winDb = [&](double t) {
                int a = (int)(t * FS), b = std::min(n, a + (int)(0.05 * FS));
                double e = 0;
                for (int i = a; i < b; i++) e += (double)L[i] * L[i] + (double)R[i] * R[i];
                return 10 * log10(e / (b - a) + 1e-30);
            };
            double t1 = 0.1, t2 = std::max(0.3, 0.7 * t60);
            double slope = (winDb(t2) - winDb(t1)) / (t2 - t1), measured = -60 / slope;
            double lr = 0, ll = 0, rr = 0;
            for (int i = 0; i < n; i++) lr += (double)L[i] * R[i], ll += (double)L[i] * L[i], rr += (double)R[i] * R[i];
            double corr = lr / sqrt(ll * rr);
            printf("    T60 %.1f s: measured %.2f s (broadband, highs decay faster), L/R correlation %+.2f\n", t60, measured, corr);
            check(measured > t60 * 0.5 && measured < t60 * 1.15, "decays at about the requested rate");
            check(fabs(corr) < 0.3, "wide stereo");
            f.release();
        }
        // echo density: after 80 ms nearly every sample is non-zero (no flutter of distinct echoes)
        {
            bfx::Fdn f;
            f.alloc();
            std::vector<float> L(8820, 0), R(8820, 0);
            L[0] = 1;
            f.process(L.data(), R.data(), 8820, 2.0, 1.0, FS);
            int nz = 0;
            for (int i = 3528; i < 8820; i++) nz += fabs(L[i]) > 1e-6;
            printf("    density 80..200 ms: %.0f%% of samples non-zero\n", 100.0 * nz / (8820 - 3528));
            check(nz > 0.95 * (8820 - 3528), "dense tail");
            f.release();
        }
        // stability: longest decay, room sweeps across the whole range, loud noise in; and a room change: no click
        {
            bfx::Fdn f;
            f.alloc();
            double peak = 0, prev = 0, worst = 0;
            uint32_t seed = 1;
            for (int blk = 0; blk < 2000; blk++) {
                float L2[128], R2[128];
                for (int i = 0; i < 128; i++) {
                    seed = seed * 1664525 + 1013904223;
                    L2[i] = R2[i] = blk < 1000 ? ((seed >> 9) / 4194304.0f - 1) * 0.5f : 0;
                }
                double room = 0.2 + 2.2 * (0.5 + 0.5 * sin(blk * 0.01));
                f.process(L2, R2, 128, 30, room, FS);
                for (int i = 0; i < 128; i++) {
                    peak = std::max(peak, (double)fabs(L2[i]));
                    if (blk > 1000) worst = std::max(worst, fabs(L2[i] - prev));
                    prev = L2[i];
                }
            }
            printf("    30 s decay, room swept, noise in: peak %.2f, largest step in the tail %.3f\n", peak, worst);
            check(isfinite(peak) && peak < 20, "stable");
            f.release();
        }
        // the pool: a reverb past its buffers gets none; one given back (betterfx: the object's destructor, or 30 s
        // idle) is scrubbed and the next reverb starts from silence in it
        {
            bfx::Fdn::scrubPool();
            bfx::Fdn f[bfx::FdnPool::K + 1];
            int got = 0;
            for (int k = 0; k < bfx::FdnPool::K; k++) got += f[k].alloc();
            check(got == bfx::FdnPool::K && !f[bfx::FdnPool::K].alloc(), "pool: one more reverb than buffers gets none");
            float L2[256] = {1}, R2[256] = {1};
            f[0].process(L2, R2, 256, 6, 1.0, FS);  // leaves a tail in its buffer
            float* was = f[0].buf;
            f[0].reclaim();
            bfx::Fdn::scrubPool();
            bool ok = f[bfx::FdnPool::K].alloc() && f[bfx::FdnPool::K].buf == was;
            bool clean = ok;
            for (size_t i = 0; ok && i < bfx::FdnPool::floats(); i++) clean = clean && was[i] == 0;
            check(ok && clean, "pool: a reclaimed buffer comes back clean");
            for (auto& x : f) x.release();
            bfx::Fdn::scrubPool();
        }
    }
    printf(failures ? "%d FAILED\n" : "all passed\n", failures);
    return failures ? 1 : 0;
}
