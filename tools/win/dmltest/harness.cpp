// dmltest's harness: the real mods/betterstems.cpp + betterstems_ort.cpp + engines/ort.cpp (+ dsp_win.cpp), with a
// stand-in for the mods' core. Runs under Wine against rekordbox's own onnxruntime.dll / DirectML.dll (in the exe's
// folder): README.md.
//   harness.exe <modelDir> [in.f32 (stereo interleaved, 44.1 kHz) [out.f32 (8 planar channels)]] [key=value ...]
//   key=value: the mods' config (gpu=0, betterstems_slow=1, betterstems_safe=2, ...), and harness_chunk=<samples>
//   (default 1032192, three model segments), harness_threads=<n> (the chunks dealt out to n threads at once).
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <cmath>
#include <map>
#include <string>
#include <vector>
#include <functional>
#include <chrono>
#include <atomic>
#include <thread>
#include "rbmod.h"

static std::map<std::string, std::string> g_cfg;
static std::string g_dir;
struct Provided { void* fn; };
static std::map<std::string, void*>& table() { static auto* t = new std::map<std::string, void*>; return *t; }

namespace rbm {
std::vector<Mod>& mods() { static auto* v = new std::vector<Mod>; return *v; }
void provide(const char*, const char* name, void* fn, volatile bool*) { table()[name] = fn; }
void* provided(const char* name) { auto it = table().find(name); return it == table().end() ? nullptr : it->second; }
const char* providerOf(const char*) { return ""; }
std::string cfgStr(const char* key, const char* def) { auto it = g_cfg.find(key); return it == g_cfg.end() ? def : it->second; }
double cfgNum(const char* key, double def) { auto it = g_cfg.find(key); return it == g_cfg.end() ? def : atof(it->second.c_str()); }
void logf(const char* fmt, ...) {
    va_list ap; va_start(ap, fmt); printf("LOG  "); vprintf(fmt, ap); printf("\n"); fflush(stdout); va_end(ap);
}
double now() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
void startupTask(std::function<void()> fn) { fn(); }
bool analysisBeatStage() { return false; }
namespace slowdrive { bool deckPlays() { return false; } }
namespace plat {
std::filesystem::path fsPath(const std::string& utf8) { return std::filesystem::u8path(utf8); }
std::string resourcesDir() { return g_dir; }
}
}  // namespace rbm

static double db(double rms) { return 20 * log10(rms + 1e-12); }

int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    if (argc < 2) return 2;
    g_dir = argv[1];
    std::string in, out;
    for (int i = 2; i < argc; i++) {
        std::string a = argv[i];
        size_t eq = a.find('=');
        if (eq != std::string::npos) g_cfg[a.substr(0, eq)] = a.substr(eq + 1);
        else if (in.empty()) in = a;
        else out = a;
    }
    for (auto& m : rbm::mods()) if (m.init) { printf("init %s\n", m.name); m.init(); }
    auto active = (bool (*)())rbm::provided("betterstems.active");
    auto separate = (bool (*)(const float*, const float*, int, float* const[8]))rbm::provided("betterstems.separate");
    printf("active after launch: %d\n", active ? (int)active() : -1);
    if (in.empty()) return 0;
    FILE* f = fopen(in.c_str(), "rb");
    if (!f) { printf("can't open %s\n", in.c_str()); return 2; }
    std::vector<float> raw;
    float buf[8192];
    for (size_t n; (n = fread(buf, sizeof(float), 8192, f)) > 0;) raw.insert(raw.end(), buf, buf + n);
    fclose(f);
    int n = (int)(raw.size() / 2);
    std::vector<float> l(n), r(n);
    for (int i = 0; i < n; i++) l[i] = raw[2 * i], r[i] = raw[2 * i + 1];
    std::vector<std::vector<float>> st(8, std::vector<float>(n));
    float* outs[8];
    for (int o = 0; o < 8; o++) outs[o] = st[o].data();
    // as stemcache: one chunk at a time (here ~23.4 s chunks, three segments each)
    int chunk = atoi(rbm::cfgStr("harness_chunk", "1032192").c_str());
    double t0 = rbm::now();
    int done = 0, failedAt = -1;
    int threads = atoi(rbm::cfgStr("harness_threads", "1").c_str());
    if (threads > 1) {   // the chunks dealt out to several threads at once (as two decks loading)
        std::vector<int> starts;
        for (int at = 0; at < n; at += chunk) starts.push_back(at);
        std::atomic<int> next{0}, fails{0};
        std::vector<std::thread> ts;
        for (int t = 0; t < threads; t++)
            ts.emplace_back([&] {
                for (int k; (k = next++) < (int)starts.size();) {
                    int at = starts[k], len = std::min(chunk, n - at);
                    float* o2[8];
                    for (int o = 0; o < 8; o++) o2[o] = outs[o] + at;
                    bool ok = separate(l.data() + at, r.data() + at, len, o2);
                    printf("chunk at %.1f s: %s\n", at / 44100.0, ok ? "ok" : "FAILED");
                    if (!ok) fails++;
                }
            });
        for (auto& t : ts) t.join();
        done = fails ? 0 : n;
        if (fails) failedAt = 0;
        printf("%d threads: %d chunks failed, active %d\n", threads, (int)fails, (int)active());
    } else
    for (int at = 0; at < n; at += chunk) {
        int len = std::min(chunk, n - at);
        float* o2[8];
        for (int o = 0; o < 8; o++) o2[o] = outs[o] + at;
        bool ok = separate(l.data() + at, r.data() + at, len, o2);
        printf("chunk at %.1f s (%.1f s): %s, active %d\n", at / 44100.0, len / 44100.0, ok ? "ok" : "FAILED", (int)active());
        if (!ok) { failedAt = at; break; }
        done = at + len;
    }
    printf("separated %.1f s of audio in %.1f s\n", done / 44100.0, rbm::now() - t0);
    if (done > 0) {
        double mix = 0, rest = 0;
        for (int i = 0; i < done; i++) {
            mix += (double)l[i] * l[i] + (double)r[i] * r[i];
            double sl = 0, sr = 0;
            for (int s = 0; s < 4; s++) sl += st[2 * s][i], sr += st[2 * s + 1][i];
            rest += (sl - l[i]) * (sl - l[i]) + (sr - r[i]) * (sr - r[i]);
        }
        printf("mix %.1f dB, sources' sum off by %.1f dB;", db(sqrt(mix / (2.0 * done))), db(sqrt(rest / (2.0 * done))));
        for (int s = 0; s < 4; s++) {
            double e = 0;
            for (int i = 0; i < done; i++) e += (double)st[2 * s][i] * st[2 * s][i] + (double)st[2 * s + 1][i] * st[2 * s + 1][i];
            printf(" source %d %.1f dB", s, db(sqrt(e / (2.0 * done))));
        }
        printf("\n");
        if (!out.empty()) {
            FILE* g = fopen(out.c_str(), "wb");
            for (int o = 0; o < 8; o++) fwrite(st[o].data(), sizeof(float), done, g);
            fclose(g);
            printf("wrote %s (8 planar channels, %d samples)\n", out.c_str(), done);
        }
    }
    return failedAt >= 0 ? 1 : 0;
}
