// slowdrive: background writes to a slow drive give way to whoever reads from it (see rbmod.h).
//
// Measured on the USB stick the caches were moved to: reads alone 120 MB/s. With stems chunks stored as they were
// separated (a file each, up to 16 at once, every one flushed) and a finished Spotify track copied next to them, the
// stick went from 30-60 MB/s to one request a second after about 70 MB, and stayed there for minutes; a deck or an
// analysis reading a cached track then waited behind everything queued (one read call 24.7 s, the 29 MB file 135 s).
// With one writer that writes a piece, waits for it, and starts none while a read is under way, the same read took
// 0.3-2.5 s in the same state: it waits for the one piece in flight at most.
#include "rbmod.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>

namespace rbm::slowdrive {

static std::atomic<double> g_readUntil{0};  // plat::monotonic: a read is expected until then
static std::atomic<int> g_reads{0};         // reads under way (Reading)
static std::atomic<bool> g_deckLoads{false}, g_deckPlays{false};
static std::atomic<double> g_restUntil{0};  // the rest after the last piece
static std::mutex g_turn;                   // one writer at a time
static std::atomic<double> g_playedAt{-1e9};  // when a deck last played (tick)
static std::atomic<double> g_slowAt{-1e9};    // when the drive last took long for a piece

// A drive that has been slow gets nothing written while a deck plays (or played in the last 30 s). Even one piece at
// a time was too much for the stick in its slow state: a 1 MB piece took up to 10 s, and whatever was written made
// the system's own flush of the volume (every 30 s) take ~7 s of small writes, behind which a deck's or an analysis'
// read of a cached track waited (audio pass 7-9 s instead of 1.1 s). What waits stays in the mods' staging folders
// and moves once the decks have stopped. A drive that takes its pieces quickly is written to throughout.
const double kSlowPiece = 0.5;     // s for a piece of 1 MB at most: the drive is slow
const double kSlowMemory = 1800;   // s a drive that was slow is treated as slow
static bool liveHold() {
    double t = plat::monotonic();
    return t - g_playedAt < 30 && t - g_slowAt < kSlowMemory;
}

void reading(double seconds) {
    double until = plat::monotonic() + seconds, was = g_readUntil;
    while (until > was && !g_readUntil.compare_exchange_weak(was, until)) {}
}

Reading::Reading(bool on) : on_(on) {
    if (on_) g_reads++;
}
Reading::~Reading() {
    if (!on_) return;
    reading(0.5);  // (the next chunk of the same track follows at once)
    g_reads--;
}

bool quiet() { return g_reads == 0 && !g_deckLoads && plat::monotonic() >= g_readUntil; }
bool deckPlays() { return g_deckPlays; }

// A deck that loads reads its track (a cached one: from the drive) and the DJ waits for it. A load that doesn't end (a
// stream that never comes) holds nothing back after 20 s.
void tick() {
    static double since[8];
    double t = plat::monotonic();
    bool loads = false, plays = false;
    for (int ch = 0, n = std::min(deckCount(), 8); ch < n; ch++) {
        if (!deckLoading(ch)) since[ch] = 0;
        else if (since[ch] == 0) since[ch] = t;
        loads = loads || (since[ch] > 0 && t - since[ch] < 20);
        plays = plays || deckPlaying(ch);
    }
    if (g_deckLoads && !loads) reading(1);  // (what the load started, its analysis and stems, reads next)
    g_deckLoads = loads;
    g_deckPlays = plays;
    if (plays) g_playedAt = t;
    // (said once per hold: what waits in the staging folders is not lost)
    static bool held;
    bool hold = liveHold();
    if (hold != held) {
        held = hold;
        logf(hold ? "slowdrive: a deck plays and the cache drive has been slow: nothing is written to it until the decks have stopped (what waits stays on the internal disk)"
                  : "slowdrive: no deck plays: writing to the cache drive again");
    }
}

Turn::Turn(bool on) : on_(on) {
    if (!on_) return;
    g_turn.lock();
    while (!quiet() || plat::monotonic() < g_restUntil || liveHold()) std::this_thread::sleep_for(std::chrono::milliseconds(20));
    from_ = plat::monotonic();
}

// The rest after a piece. A drive that took the piece quickly gets the next one at once, or, while a deck plays, at
// 2 MB/s at most: what a set needs (a new track's stems and audio are ~120 MB every few minutes) without filling the
// stick's fast part. A drive that was slow (0.15 s and more for a piece: the stick's one-request-a-second state) rests
// a quarter of the time the piece took (half a second at most) while nobody plays: the reads that matter then are
// announced (reading(), a deck that loads), and what waits for the drive should get there. While a deck plays (a drive
// not yet known as slow: after that nothing is written, see liveHold) three times the piece's time.
Turn::~Turn() {
    if (!on_) return;
    double t = plat::monotonic(), took = t - from_;
    bool plays = g_deckPlays;
    if (took >= kSlowPiece) {
        if (t - g_slowAt > kSlowMemory) logf("slowdrive: the cache drive is slow (%.1f s for a piece of %.1f MB): its writes wait while a deck plays", took, bytes_ / 1e6);
        g_slowAt = t;
    }
    double rest = took >= 0.15 ? (plays ? std::min(4.0, took * 3) : std::min(0.5, took * 0.25)) : plays ? std::max(0.0, bytes_ / 2e6 - took) : 0;
    g_restUntil = t + rest;
    g_turn.unlock();
}

}  // namespace rbm::slowdrive

#ifdef _WIN32
// Windows has no per-file switch like F_NOCACHE once a file is open: false, and the caller flushes each piece (sync).
bool rbm::plat::File::direct() { return false; }
#endif
