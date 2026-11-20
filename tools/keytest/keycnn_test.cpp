// Test harness: keycnn_test <model.bin> <mono float32 44.1 kHz raw file>... -> prints 24 probabilities.
#include "../../engines/keycnn.h"
#include <stdio.h>
#include <chrono>
#include <vector>
int main(int argc, char** argv) {
    keycnn::Model m;
    std::string err;
    if (argc < 3 || !m.load(argv[1], &err)) { fprintf(stderr, "load: %s\n", err.c_str()); return 1; }
    for (int i = 2; i < argc; i++) {
        FILE* f = fopen(argv[i], "rb");
        if (!f) return 1;
        std::vector<float> x;
        float buf[4096];
        for (size_t n; (n = fread(buf, 4, 4096, f)) > 0;) x.insert(x.end(), buf, buf + n);
        fclose(f);
        auto t0 = std::chrono::steady_clock::now();
        std::vector<float> p = m.predict(x.data(), x.size(), 44100);
        double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        int best = 0;
        for (int k = 0; k < (int)p.size(); k++) if (p[k] > p[best]) best = k;
        fprintf(stderr, "%s: %s p=%.2f %.0f ms\n", argv[i], p.empty() ? "?" : keycnn::kKeyNames[best], p.empty() ? 0 : p[best], ms);
        for (float v : p) printf("%.8f ", v);
        printf("\n");
    }
}
