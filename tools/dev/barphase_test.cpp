// Offline check of mods/barphase.h: the 8 half-beat position scores for a track and the time they take.
//   clang++ -std=c++17 -O2 -Wall -o $TMPDIR/barphase_test tools/dev/barphase_test.cpp engines/dsp_mac.mm \
//       -framework Accelerate && $TMPDIR/barphase_test audio.f32 44100 beats.txt
// audio.f32: mono float32, or - for stdin (ffmpeg -i x.flac -ac 1 -ar 44100 -f f32le audio.f32); beats.txt: one beat time (s) per line.
#include "../../mods/barphase.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <chrono>
#include <vector>

int main(int argc, char** argv) {
    if (argc < 4) {
        fprintf(stderr, "usage: %s audio.f32 rate beats.txt\n", argv[0]);
        return 2;
    }
    FILE* f = strcmp(argv[1], "-") ? fopen(argv[1], "rb") : stdin;  // "-": the audio on stdin
    if (!f) return 1;
    std::vector<float> x;
    float buf[65536];
    size_t got;
    while ((got = fread(buf, sizeof(float), 65536, f)) > 0) x.insert(x.end(), buf, buf + got);
    if (f != stdin) fclose(f);
    int rate = atoi(argv[2]);
    std::vector<double> beats;
    FILE* b = fopen(argv[3], "r");
    double t;
    while (b && fscanf(b, "%lf", &t) == 1) beats.push_back(t);
    if (b) fclose(b);
    auto t0 = std::chrono::steady_clock::now();
    barphase::Bands bands = barphase::bands(x.data(), x.size(), rate);
    auto t1 = std::chrono::steady_clock::now();
    double n4 = 0;
    auto sc = barphase::score(bands, beats, &n4);
    auto t2 = std::chrono::steady_clock::now();
    printf("scores");
    for (double s : sc) printf(" %.3f", s);
    printf("  novelty4 %.2f  bands %.1f ms, score %.2f ms (%zu frames, %zu beats, %.0f s)\n", n4,
           std::chrono::duration<double, std::milli>(t1 - t0).count(), std::chrono::duration<double, std::milli>(t2 - t1).count(),
           bands.frames(), beats.size(), x.size() / (double)rate);
    return 0;
}
