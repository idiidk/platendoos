// render <in.f32 stereo 44.1k> <out prefix> <x decay 1..100> <mix>   -> prefix_before.f32, prefix_after.f32
#include "../../mods/betterfx_dsp.h"
#include <algorithm>
#include <stdio.h>
#include <vector>
static const double FS = 44100;
static const double dly[5][2] = {{22.09375, 38.66666793823242}, {39.91666793823242, 23.229167938232422}, {23.854167938232422, 40.86458206176758}, {41.5, 24.802082061767578}, {21.5, 23.885417938232422}};
static const double apD[2][2] = {{6.979166030883789, 8.239583015441895}, {22.739582061767578, 21.47916603088379}};
struct Line { std::vector<float> b; int w = 0; int d; Line(double ms) : b(8192, 0), d((int)(ms * 0.001 * FS + 0.5)) {} float rd() { return b[(w - d) & 8191]; } void wr(float v) { b[w] = v; w = (w + 1) & 8191; } };
int main(int argc, char** argv) {
    FILE* f = fopen(argv[1], "rb"); std::vector<float> in; float t[4096]; size_t k;
    while ((k = fread(t, 4, 4096, f)) > 0) in.insert(in.end(), t, t + k);
    int n = in.size() / 2; double x = atof(argv[3]), mix = atof(argv[4]);
    double T = 2.8728029519697884e-06*x*x*x*x - 0.00033391331089660525*x*x*x + 0.01680831052362919*x*x - 0.22553139925003052*x + 1.7408629655838013;
    double scale = 1.0;  // room 50
    fprintf(stderr, "T60 (broadband loop) %.2f s\n", 6.9078 / 3 * T / scale);
    std::vector<Line> cl, cr, al, ar; double g[5][2]; bfx::Biquad damp[5][2], preL[2], preR[2];
    for (int c = 0; c < 5; c++) { cl.emplace_back(dly[c][0] * scale), cr.emplace_back(dly[c][1] * scale); for (int h = 0; h < 2; h++) g[c][h] = exp(scale * -0.003 * dly[c][h] / T), damp[c][h].c = bfx::butterLP(FS, 18000); }
    for (int a = 0; a < 2; a++) al.emplace_back(apD[a][0]), ar.emplace_back(apD[a][1]);
    preL[0].c = preR[0].c = bfx::butterLP(FS, 14000), preL[1].c = preR[1].c = bfx::butterHP(FS, 80);
    bfx::Fdn fdn; fdn.alloc();
    double d0 = dly[0][0] * scale, t60 = -6.9078 * (d0 * 0.001) / log(g[0][0]), room = d0 / 22.09375;
    double eRef = 0, eMine = 0, gain = 7.5;
    std::vector<float> before(n * 2), after(n * 2);
    const int B = 256; int onEnd = (int)(8.0 * FS);  // effect input on for 8 s, then the tail rings out
    for (int s = 0; s < n; s += B) {
        int m = std::min(B, n - s); float rl[B], rr[B], ml[B], mr[B];
        for (int i = 0; i < m; i++) {
            double send = s + i < onEnd ? 1 : std::max(0.0, 1 - (s + i - onEnd) / (0.0033 * FS));
            double l = preL[1].run(preL[0].run(in[2 * (s + i)] * 0.2 * send)), r = preR[1].run(preR[0].run(in[2 * (s + i) + 1] * 0.2 * send));
            ml[i] = (float)l, mr[i] = (float)r;
            double ol = 0, orr = 0;
            for (int c = 0; c < 5; c++) {
                double fl = g[c][0] * cl[c].rd(), fr = g[c][1] * cr[c].rd();
                ol += fl, orr += fr;
                cl[c].wr((float)(l + damp[c][1].run(fr))), cr[c].wr((float)(r + damp[c][0].run(fl)));  // cross-fed, as rekordbox
            }
            for (int a = 0; a < 2; a++) {
                double dl = al[a].rd(), vl = ol + 0.7 * dl; al[a].wr((float)vl); ol = dl - 0.7 * vl;
                double dr = ar[a].rd(), vr = orr + 0.7 * dr; ar[a].wr((float)vr); orr = dr - 0.7 * vr;
            }
            rl[i] = (float)ol, rr[i] = (float)orr;
        }
        fdn.process(ml, mr, m, t60, room, FS);
        double er = 0, em = 0;
        for (int i = 0; i < m; i++) er += rl[i] * rl[i] + rr[i] * rr[i], em += ml[i] * ml[i] + mr[i] * mr[i];
        double blk = m / FS, a = 1 - exp(-blk / 1.5);
        eRef += (er - eRef) * a, eMine += (em - eMine) * a;
        double target = gain;
        if (eMine > 1e-9 && eRef > 1e-9) target = std::min(15.0, std::max(3.75, sqrt(eRef / eMine)));
        double g0 = gain, step = (target - g0) * a / m;
        for (int i = 0; i < m; i++) {
            double gi = g0 + step * (i + 1), dryL = in[2 * (s + i)], dryR = in[2 * (s + i) + 1];
            double dg = mix >= 1 ? 0 : 1;
            before[2 * (s + i)] = (float)(dryL * dg + mix * 0.7079 * rl[i] * 1.2), before[2 * (s + i) + 1] = (float)(dryR * dg + mix * 0.7079 * rr[i] * 1.2);
            after[2 * (s + i)] = (float)(dryL * dg + mix * 0.7079 * ml[i] * gi * 1.2), after[2 * (s + i) + 1] = (float)(dryR * dg + mix * 0.7079 * mr[i] * gi * 1.2);
        }
        gain = g0 + step * m;
    }
    double eb = 0, ea = 0, pk = 0;
    for (int i = 0; i < n * 2; i++) eb += before[i] * before[i], ea += after[i] * after[i], pk = std::max(pk, (double)std::max(fabsf(before[i]), fabsf(after[i])));
    fprintf(stderr, "rms before %.2f dB, after %.2f dB, peak %.2f\n", 10 * log10(eb / n / 2), 10 * log10(ea / n / 2), pk);
    std::string p = argv[2];
    f = fopen((p + "_before.f32").c_str(), "wb"); fwrite(before.data(), 4, before.size(), f); fclose(f);
    f = fopen((p + "_after.f32").c_str(), "wb"); fwrite(after.data(), 4, after.size(), f); fclose(f);
}
