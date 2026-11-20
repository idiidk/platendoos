// spotifyapi: lets rekordbox make several Spotify track-list requests at once.
// rekordbox's Spotify Service holds one lock around each API request, including the wait for the HTTP reply, so
// while the library syncs every playlist, lookup and the playlist you click queue behind each other, and all
// streaming services share a pool of 3 background threads. Here the track-list and lookup requests take one of
// 3 slots instead of the lock (the request code only reads shared state; token refreshes happen outside the
// lock), and the pool gets 6 threads.
//   config: spotifyapi=0 to disable (the pool size applies from the next launch); spotifyapi_slots=<n> (default 3)
#include "../core/rbmod.h"
#include <algorithm>
#include <condition_variable>
#include <mutex>

using namespace rbm;

static volatile bool g_on = true;
static thread_local bool t_slot;  // this thread holds a slot rather than the lock

// A counting semaphore: this many requests at once.
namespace {
struct Slots {
    std::mutex m;
    std::condition_variable cv;
    int n = std::max(1, std::min(cfgInt("spotifyapi_slots", 3), 8));
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
}  // namespace
static Slots& slots() {
    static Slots s;
    return s;
}

typedef void (*LockFn)(const void*);
static void enter(const void* lock, LockFn orig) {
    if (!g_on) return orig(lock);
    slots().wait();
    t_slot = true;
}
static void leave(const void* lock, LockFn orig) {
    if (!t_slot) return orig(lock);
    t_slot = false;
    slots().signal();
}

extern "C" void rbm_hook_sp_tracks_enter(const void* l) { enter(l, RBM_ORIG(sp_tracks_enter, LockFn)); }
extern "C" void rbm_hook_sp_tracks_exit(const void* l) { leave(l, RBM_ORIG(sp_tracks_exit, LockFn)); }
#ifndef _WIN32
extern "C" void rbm_hook_sp_tracks_unwind(const void* l) { leave(l, RBM_ORIG(sp_tracks_unwind, LockFn)); }
#endif
extern "C" void rbm_hook_sp_lookup_enter(const void* l) { enter(l, RBM_ORIG(sp_lookup_enter, LockFn)); }
extern "C" void rbm_hook_sp_lookup_exit(const void* l) { leave(l, RBM_ORIG(sp_lookup_exit, LockFn)); }
#ifndef _WIN32
extern "C" void rbm_hook_sp_lookup_unwind(const void* l) { leave(l, RBM_ORIG(sp_lookup_unwind, LockFn)); }
#else
// windows-optional: sp_tracks_unwind sp_lookup_unwind
// The mac exception-cleanup exits have no call site on Windows: getLookupTracks has no cleanup at all, and
// getPlaylistTracks' is a table-driven funclet, the lock guard's shared destructor (a jmp to
// LeaveCriticalSection, used by every ScopedLock unwind). If a request throws while holding a slot, that
// funclet leaves a lock that wasn't entered and the slot stays taken; rekordbox's request code doesn't throw
// in practice.
#endif

// Once, on the first streaming job (startup, possibly before the mods' flags are set: read the config itself).
extern "C" void* rbm_hook_bridge_pool(void* pool, int threads, size_t stackSize, int priority) {
    if (modEnabled("spotifyapi")) threads = std::max(threads, 6);
    return RBM_ORIG(bridge_pool, void* (*)(void*, int, size_t, int))(pool, threads, stackSize, priority);
}

RBM_MOD({.name = "spotifyapi", .enabled = &g_on});
