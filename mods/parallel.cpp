// parallel: rekordbox's audio pass on three cores instead of one.
//
// CBeatAnalyzeThread::analyze reads the track in blocks and feeds each block, one after the other on the
// analysis thread, to the beat detector (BALibWrapper::SetSample, ~55% of the pass) and the waveform
// builder (WaveCreator::readWaveData, ~35%). The two are independent objects that only read the block,
// so each gets its own serial worker queue: the analysis thread copies the block, hands it over and goes
// on decoding the next one. Every block still reaches each object in order, so the results are the same.
// All exits of the block loop wait for both queues before rekordbox reads the results.
//   config: parallel=0 to disable
#include "../core/rbmod.h"
#include <string.h>
#include <memory>
#include <mutex>
#include <condition_variable>
#include <string>
#include <vector>

using namespace rbm;

static volatile bool g_on = true;
static const long kMaxQueued = 64;  // blocks per queue ahead of the worker

namespace {  // file-local (one dylib: same-named types in two mods would share their implicit members)
struct Block { std::vector<float> l, r; };

// Counts free places in the two queues (std::counting_semaphore would do too; this one has no maximum).
struct Room {
    std::mutex m;
    std::condition_variable cv;
    long n;
    explicit Room(long n) : n(n) {}
    void wait() {
        std::unique_lock<std::mutex> l(m);
        cv.wait(l, [&] { return n > 0; });
        n--;
    }
    void signal() {
        { std::lock_guard<std::mutex> l(m); n++; }
        cv.notify_one();
    }
};

// Per analysis thread.
struct Pipe {
    plat::Queue beats{"rbmods.parallel.beats"};
    plat::Queue wave{"rbmods.parallel.wave"};
    Room room{2 * kMaxQueued};
    bool busy = false;
};
}  // namespace
static thread_local Pipe* t_pipe;

static Pipe& pipe() {
    if (!t_pipe) t_pipe = new Pipe;  // analysis threads are reused, so one per thread for good
    return *t_pipe;
}

static std::shared_ptr<Block> copy(const float* l, const float* r, size_t n) {
    auto b = std::make_shared<Block>();
    b->l.assign(l, l + n);
    if (r && r != l) b->r.assign(r, r + n);
    return b;
}

static void submit(plat::Queue& q, std::function<void()> job) {
    Pipe& p = pipe();
    p.busy = true;
    p.room.wait();
    Room* room = &p.room;
    q.async([job, room] {
        job();
        room->signal();
    });
}

extern "C" bool rbm_hook_an_setSample(void* wrapper, float* l, float* r, int n) {
    auto orig = RBM_ORIG(an_setSample, bool (*)(void*, float*, float*, int));
    if (!g_on || n <= 0 || !l) {
        if (t_pipe && t_pipe->busy) t_pipe->beats.sync([] {});  // keep the order
        return orig(wrapper, l, r, n);
    }
    auto b = copy(l, r, n);
    submit(pipe().beats, [=] {
        float* bl = b->l.data();
        orig(wrapper, bl, b->r.empty() ? (r ? bl : nullptr) : b->r.data(), n);
    });
    return true;  // (unused by the caller)
}

// quickdeck shows the overview waveform while the track is still being read: every so often (it says when) it gets
// the waveform builder between two blocks, on the queue that feeds it, so that it sees every block before and none
// half done.
static void partialOverview(void* creator, bool queued) {
    auto due = service<bool (*)(const char**, const void**)>("quickdeck.waveDue");
    const char* uri = nullptr;
    const void* thread = nullptr;
    if (!due || !due(&uri, &thread)) return;
    auto show = service<void (*)(const void*, const char*, const void*)>("quickdeck.partialWave");
    if (!show) return;
    if (!queued) return show(creator, uri, thread);
    std::string u = uri;
    submit(pipe().wave, [=] { show(creator, u.c_str(), thread); });
}

extern "C" void rbm_hook_an_readWave(void* creator, float* l, float* r, unsigned long n) {
    auto orig = RBM_ORIG(an_readWave, void (*)(void*, float*, float*, unsigned long));
    if (!g_on || n == 0 || !l) {
        if (t_pipe && t_pipe->busy) t_pipe->wave.sync([] {});
        orig(creator, l, r, n);
        partialOverview(creator, false);
        return;
    }
    auto b = copy(l, r, n);
    submit(pipe().wave, [=] {
        float* bl = b->l.data();
        orig(creator, bl, b->r.empty() ? (r ? bl : nullptr) : b->r.data(), n);
    });
    partialOverview(creator, true);
}

extern "C" void rbm_hook_an_loopEnd(void* scratch) {
    if (Pipe* p = t_pipe; p && p->busy) {
        double t = now();
        p->beats.sync([] {});
        p->wave.sync([] {});
        p->busy = false;
        if (now() - t > 0.05) logf("parallel: waited %.2fs for the beat/waveform workers", now() - t);
    }
    RBM_ORIG(an_loopEnd, void (*)(void*))(scratch);
}

RBM_MOD({.name = "parallel", .enabled = &g_on});
