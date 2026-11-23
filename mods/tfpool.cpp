// tfpool: keep rekordbox's beat-detection neural nets loaded between tracks.
//
// rekordbox's beat detection (AnalyzeJobBeatDetector::highPrecisionBeatAnalyze -> madmom::analyze) builds
// a TFWrapper::Model for every track (detect_beat/model8; model7 on another path): it loads the TensorFlow
// graph from disk and creates a session with std::thread::hardware_concurrency() threads, runs once, and
// is deleted again. Loading is ~0.3-0.5 s, ~15% of beat detection.
// This keeps a pool of loaded models per model file: a track takes a loaded one (or loads one), and
// deleting it returns it to the pool. A model is used by one analysis at a time, as before, and gives
// the same beat grids. (Fewer TensorFlow threads per model made no measurable difference.)
//   config: tfpool=0 to disable
//           tfpool_warmup=0 to skip the warm-up at launch (see below)
#include "../core/rbmod.h"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <map>
#include <thread>
#include <tuple>
#include <mutex>
#include <string>
#include <vector>

using namespace rbm;

static const Addr A_modelVptr{"__ZTVN9TFWrapper5ModelE", 0x10};  // its vtable pointer (D1, D0)

static volatile bool g_on = true;

namespace {  // file-local
struct Key {
    std::string path;
    bool a, b;
    int threads;
    bool operator<(const Key& o) const {
        return std::tie(path, a, b, threads) < std::tie(o.path, o.a, o.b, o.threads);
    }
};
static std::mutex g_lock;
static std::map<Key, std::vector<void*>> g_idle;  // loaded models (Pimpl*) not in use
static std::map<void*, Key> g_keys;               // every pooled Pimpl -> its key
static int g_loaded, g_reused;

// TFWrapper::Model is { vptr, Pimpl* }; deleting it deletes the Pimpl (the loaded session).
struct Model { void** vptr; void* pimpl; };
}  // namespace

static void giveBack(Model* m) {
    std::lock_guard<std::mutex> l(g_lock);
    auto it = g_keys.find(m->pimpl);
    if (it != g_keys.end()) g_idle[it->second].push_back(m->pimpl);
    m->pimpl = nullptr;
}
#ifdef _WIN32
// MSVC: one virtual, the scalar deleting destructor (this, flags; flag 1 = free the memory), after the
// complete object locator. The model is deleted through it (`call [rax]`, edx = 1, in both users of the
// constructor). The pimpl is taken away first, so the original destructor deletes nothing of it (it tests
// for null) and frees the Model's memory with rekordbox's own allocator.
static void* (*g_origDtor)(Model*, unsigned);
static void* modelDeleting(Model* m, unsigned flags) {
    giveBack(m);
    return g_origDtor(m, flags);
}

// The original vtable with its destructor returning the model to the pool.
static void** pooledVptr() {
    static void** vt = [] {
        void** orig = at<void**>(A_modelVptr);
        g_origDtor = (void* (*)(Model*, unsigned))orig[0];
        void** copy = (void**)malloc(2 * sizeof(void*));
        copy[0] = orig[-1];  // complete object locator
        copy[1] = (void*)&modelDeleting;
        return copy + 1;
    }();
    return vt;
}
#else
static void modelD1(Model* m) { giveBack(m); }
static void modelD0(Model* m) { giveBack(m); ::operator delete(m); }

// The original vtable with both destructors returning the model to the pool.
static void** pooledVptr() {
    static void** vt = [] {
        void** orig = at<void**>(A_modelVptr);
        void** copy = (void**)malloc(4 * sizeof(void*));
        copy[0] = orig[-2], copy[1] = orig[-1];  // offset to top, typeinfo
        copy[2] = (void*)&modelD1, copy[3] = (void*)&modelD0;
        return copy + 2;
    }();
    return vt;
}
#endif

// (MSVC's constructor returns this, and rekordbox's caller uses it: the hooks return the model.)
static Model* construct(Model* m, const std::string& path, bool a, int threads, bool b) {
    auto orig = RBM_ORIG(tf_model1, Model* (*)(Model*, const std::string&, bool, int, bool));
    if (!g_on) return orig(m, path, a, threads, b), m;
    Key k{path, a, b, threads};
    {
        std::lock_guard<std::mutex> l(g_lock);
        auto& idle = g_idle[k];
        if (!idle.empty()) {
            m->vptr = pooledVptr();
            m->pimpl = idle.back();
            idle.pop_back();
            if (++g_reused % 50 == 1) logf("tfpool: reused a loaded model (%d loaded, %d reuses)", g_loaded, g_reused);
            return m;
        }
    }
    double t0 = now();
    orig(m, path, a, threads, b);
    std::lock_guard<std::mutex> l(g_lock);
    if (m->pimpl) g_keys[m->pimpl] = k;  // (a model that failed to load isn't pooled)
    g_loaded++;
    m->vptr = pooledVptr();
    logf("tfpool: loaded %s (%d threads) in %.2fs, %d models loaded", path.c_str(), k.threads, now() - t0, g_loaded);
    return m;
}

// madmom::analyze(String, String, float*, float*, ...) -> TFWrapper::Model::Model, once per model file.
extern "C" Model* rbm_hook_tf_model1(Model* m, const std::string& path, bool a, int threads, bool b) {
    return construct(m, path, a, threads, b);
}
extern "C" Model* rbm_hook_tf_model2(Model* m, const std::string& path, bool a, int threads, bool b) {
    return construct(m, path, a, threads, b);
}

// windows-optional: tf_madmom
#if defined(__aarch64__) || (defined(__APPLE__) && defined(__x86_64__))
// Warm-up. The first beat detection after a launch also loads the model (~1.3 s) and has TensorFlow optimise the graph on
// its first run (another ~1 s), so the first track loaded gets its grid ~2 s later than the rest. madmom's call (its two
// model paths, the sample rate and the flag that picks the model; madmom::analyze(String, String, float* L, float* R, int,
// long long frames, bool) -> a result in memory: through x8, on x86_64 a pointer in rdi) is noted in a file, and at
// the next launch it runs once in the background on a few seconds of quiet noise, once for each model: that leaves
// loaded, already optimised models in the pool.
static std::string warmupFile() { return plat::configDir() + "/tfpool_warmup.txt"; }
static std::atomic<bool> g_noted{false};

extern "C" void rbm_tf_note(const JStr* a, const JStr* b, const float*, const float*, int rate, long long, bool flag) {
    if (!g_on || !a || !b || !a->text || !b->text || g_noted.exchange(true)) return;
    std::string want = std::string(a->text) + "\n" + b->text + "\n" + std::to_string(rate) + "\n" + (flag ? "1" : "0") + "\n";
    std::string have;
    if (FILE* f = plat::openFile(warmupFile(), "r")) {
        char buf[4096];
        size_t n = fread(buf, 1, sizeof buf, f);
        have.assign(buf, n);
        fclose(f);
    }
    if (have == want) return;
    if (FILE* f = plat::openFile(warmupFile(), "w")) {
        fputs(want.c_str(), f);
        fclose(f);
    }
}
#if defined(__aarch64__)
extern "C" void* rbm_orig_tf_madmom() { return RBM_ORIG(tf_madmom, void*); }
// The call site's arguments (x0-x7) and result pointer (x8) go on to the original untouched.
asm(R"(
    .text
    .p2align 2
    .globl _rbm_hook_tf_madmom
_rbm_hook_tf_madmom:
    stp  x29, x30, [sp, #-0x60]!
    mov  x29, sp
    stp  x0, x1, [sp, #0x10]
    stp  x2, x3, [sp, #0x20]
    stp  x4, x5, [sp, #0x30]
    stp  x6, x7, [sp, #0x40]
    str  x8, [sp, #0x50]
    bl   _rbm_tf_note
    bl   _rbm_orig_tf_madmom
    mov  x16, x0
    ldr  x8, [sp, #0x50]
    ldp  x6, x7, [sp, #0x40]
    ldp  x4, x5, [sp, #0x30]
    ldp  x2, x3, [sp, #0x20]
    ldp  x0, x1, [sp, #0x10]
    ldp  x29, x30, [sp], #0x60
    br   x16
)");
#else
// Intel Macs: the result pointer is the first argument (rdi; returned in rax), the Strings are pointers to the
// caller's copies (rsi, rdx; the caller destroys them), L and R in rcx and r8, the rate in r9d, the frame count and
// the flag on the stack (7.2.19 and 7.2.20, AnalyzeJobBeatDetector's madmom::analyze overload): a plain C hook.
extern "C" void* rbm_hook_tf_madmom(void* result, const JStr* a, const JStr* b, float* L, float* R, int rate,
                                    long long frames, bool flag) {
    rbm_tf_note(a, b, L, R, rate, frames, flag);
    return RBM_ORIG(tf_madmom, void* (*)(void*, const JStr*, const JStr*, float*, float*, int, long long, bool))(
        result, a, b, L, R, rate, frames, flag);
}
#endif

namespace {
struct MadmomResult { alignas(16) char bytes[4096]; };  // (bigger than the real one; its contents are left alone)
}  // namespace
static const Addr A_madmom{"__ZN6madmom7analyzeEN4juce6StringES1_PfS2_ixb"};

static void warmUp() {
    if (!cfgInt("tfpool_warmup", 1)) return;
    std::vector<std::string> lines;
    if (FILE* f = plat::openFile(warmupFile(), "r")) {
        char buf[4096];
        while (fgets(buf, sizeof buf, f)) lines.push_back(std::string(buf, strcspn(buf, "\n")));
        fclose(f);
    }
    if (lines.size() < 4) return;  // not noted yet: after the first beat detection
#ifdef __APPLE__
    // The models are in the app's own Resources: one noted by another copy of the app (platendoos and a test install
    // share the config folder) is this app's same file.
    const std::string res = "/Contents/Resources/", own = plat::resourcesDir().substr(0, plat::resourcesDir().rfind('/') + 1);
    for (int i = 0; i < 2; i++)
        if (size_t p = lines[i].find(res); p != std::string::npos && own.size() > 1) lines[i] = own + lines[i].substr(p + res.size());
#endif
    std::thread([lines] {
        std::this_thread::sleep_for(std::chrono::seconds(3));  // after rekordbox's own start-up work
        {
            std::lock_guard<std::mutex> l(g_lock);
            if (g_loaded) return;  // an analysis came first
        }
        double t0 = now();
        int rate = atoi(lines[2].c_str());
        if (rate < 8000 || rate > 192000) rate = 44100;
        const long long frames = (long long)rate * 4;
        std::vector<float> L(frames), R(frames);
        uint32_t seed = 12345;
        for (long long i = 0; i < frames; i++) {
            seed = seed * 1664525 + 1013904223;
            L[i] = R[i] = ((seed >> 9) / 8388608.0f - 0.5f) * 0.01f;
        }
        // The flag picks the model: true the first path's (model7), false the second's (model8); rekordbox picks per track.
        typedef MadmomResult (*Fn)(const JStr*, const JStr*, float*, float*, int, long long, bool);
        for (bool flag : {lines[3] == "1", lines[3] != "1"}) {
            JStr a(lines[0].c_str()), b(lines[1].c_str());
            auto* res = new MadmomResult();  // kept: its contents aren't ours to free
            *res = at<Fn>(A_madmom)(&a, &b, L.data(), R.data(), rate, frames, flag);
        }
        logf("tfpool: warmed up in %.2fs (%d models loaded)", now() - t0, g_loaded);
    }).detach();
}
#else
static void warmUp() {}
#endif

RBM_MOD({.name = "tfpool", .init = warmUp, .enabled = &g_on});
