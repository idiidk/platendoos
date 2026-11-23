// dmltest: engines/ort.cpp unchanged, with ort::run wrapped: FAULT in the environment makes a "GPU" return garbage,
// as one user's did.  FAULT=normal: whenever the session is on "GPU (DirectML)" (not the safe mode); FAULT=upto1: also without graph fusion; FAULT=both: on any GPU
// session; FAULT_AFTER=<n>: only from the n-th run on (0-based, counting every run).
// DUMP_INPUTS=<prefix>: the first run with a signal writes its inputs to <prefix><name>.f32 (for probe.exe).
#include <stdio.h>
#include <stdlib.h>
#include <mutex>
#include <thread>
#include <string>
#include <vector>
#include <memory>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dxgi.h>
#include "onnxruntime_c_api.h"
#include "ort.h"
#define run run_real
#include "ort.cpp"
#undef run
namespace ort {
bool run(Session& s, const std::vector<Input>& in, const std::vector<std::string>& outNames,
         std::vector<std::vector<float>>& out, std::string* err, std::vector<std::vector<int64_t>>* outShapes) {
    static int runs;
    if (const char* d = getenv("DUMP_INPUTS")) {   // the first run with a signal: its inputs, as <d><name>.f32
        static bool done;
        double e = 0;
        size_t c0 = 1;
        for (int64_t x : in[0].shape) c0 *= (size_t)x;
        for (size_t i = 0; i < c0; i += 97) e += in[0].data[i] * in[0].data[i];
        if (!done && e > 0) {
            done = true;
            for (auto& t : in) {
                size_t c = 1;
                for (int64_t x : t.shape) c *= (size_t)x;
                FILE* f = fopen((std::string(d) + t.name + ".f32").c_str(), "wb");
                fwrite(t.data, sizeof(float), c, f);
                fclose(f);
            }
        }
    }
    bool ok = run_real(s, in, outNames, out, err, outShapes);
    const char* fault = getenv("FAULT");
    const char* after = getenv("FAULT_AFTER");
    int n = runs++;
    if (!ok || !fault || n < (after ? atoi(after) : 0)) return ok;
    const std::string& p = provider(s);
    bool hit = !strcmp(fault, "both") ? p.rfind("GPU", 0) == 0 : !strcmp(fault, "normal") ? p == "GPU (DirectML)" :
               !strcmp(fault, "upto1") ? p == "GPU (DirectML)" || p == "GPU (DirectML, no graph fusion)" : false;
    if (hit) {
        printf("FAULT run %d on %s: garbage\n", n, p.c_str());
        unsigned seed = 7;
        for (auto& o : out)
            for (size_t i = 0; i < o.size() / 4; i++) {  // the first source: loud noise
                seed = seed * 1664525u + 1013904223u;
                o[i] = ((int)(seed >> 16) - 32768) * 1.0f;
            }
    }
    return ok;
}
}
