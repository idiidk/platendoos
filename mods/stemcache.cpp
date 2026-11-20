// stemcache: keep separated stems on disk, so a track's stems are there at once the next time it loads.
//
// rekordbox separates a track into stems (STEMS) whenever it loads, in chunks of ~11-12 s, and throws them away
// shortly after the track leaves the decks. The separation engine is a plugin (Frameworks/libSpleeter.dylib or
// libDemucs3.dylib) that AudioPartLibrary::load finds by name: <engine>OperationAtIndexOtherWithBass (3 stems) and
// <engine>OperationAtIndexSeparatedBass (4 stems) separate one chunk:
//   bool op(handle, chunk, inL, inR, out0L, out0R, out1L, out1R, out2L, out2R, out3L, out3R)
// planar floats; out3 is NULL for 3 stems. This hands rekordbox wrappers instead: a chunk whose input audio was
// separated before (same engine, model version and stem count) gets its stems from disk; any other is separated as
// before and stored. Keyed by the audio itself (SHA-256 of the input), so it can't give a track another's stems.
// Stored as AAC (stemcodec.h; 256 kbps per stem by default: ~1.5 MB per chunk of 4 stems), or 16-bit PCM packed
// losslessly (mid/side, order-2 prediction, byte planes, LZFSE: ~4-5 MB). Chunks of either kind are read.
//
// "Separate stems" in the stems window (a playlist's menu: Open stems window) fills the cache ahead of time: each
// track goes through rekordbox's own separation (AudioPart::load on a deck number no deck uses, then AudioPart::unload), one at a time,
// so its chunks are decoded exactly as on a deck and hash the same.
//   config: stemcache=0 to disable
//           stemcache_spotify=0   no stems for Spotify tracks (otherwise: on a deck at once, separated from the stream)
//           stemcache_memory_mb=384   Spotify tracks' stems kept in memory (never on disk) for loading
//                                     them again in this session; the least recently used go first (0: none)
//           stemcache_dir=<cache folder>/stems (mac ~/Library/Caches/rekordbox-mods/stems)
//           stemcache_quality=256   how new chunks are stored: lossless | 256 | 192 | 128 (AAC kbps per stem)
//           stemcache_max_gb=10   delete the least recently used chunks above this size (0 = no limit)
//           stemcache_min_free_gb=5   store nothing new while the disk has less free space than this
//           stemcache_staging=auto   new chunks go to the internal disk first (<the mods' cache folder>/stems-staging)
//                                    and one background thread moves them to stemcache_dir, a piece at a time and
//                                    never while a deck or an analysis reads from that drive. auto: only when
//                                    stemcache_dir is on an external drive | 1: always | 0: never
//           stemcache_staging_gb=4   how much may wait on the internal disk; past it a deck's chunks are left out
//                                    (separated again the next time) and the stems window's run waits
//
// A cache folder on a USB stick: storing each chunk there as it was separated (a flushed file each, up to 16 at once)
// put the stick into a state where it takes one request a second, and a deck or an analysis that read from it
// waited behind them for tens of seconds (see core/slowdrive.cpp). Staged chunks are served from the staging
// folder until they have moved; which chunks the cache folder has is known from its listing, so a chunk it doesn't
// have is not looked for on the drive.
#include "../core/rbmod.h"
#include "../core/library.h"
#include "stemcache.h"
#include "stemcodec.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

using namespace rbm;

static volatile bool g_on = true;

namespace {

typedef bool (*OpFn)(void* handle, int chunk, float* inL, float* inR, float* o0L, float* o0R, float* o1L, float* o1R,
                     float* o2L, float* o2R, float* o3L, float* o3R);
typedef int (*InitFn)(const char* modelDir, int a, int b, bool bigMemory, int threads, int c);
typedef bool (*PreFn)(void** handle, unsigned samples, bool parallel);

// An engine as rekordbox drives it (AudioPartSeparator::OperationAtIndex): the input is a chunk plus context on both
// sides, the stems are the chunk.
struct Engine {
    const char* prefix;
    int inSamples, outSamples;
    InitFn init = nullptr;
    PreFn pre = nullptr;
    OpFn op[2] = {nullptr, nullptr};  // OtherWithBass (3 stems), SeparatedBass (4 stems)
    std::string version = "0";        // its model's separator_ver.txt
};
Engine g_engines[] = {{"spleeter", 519168 + 2 * 4096, 519168}, {"demucs3", 483052 + 2 * 1024, 483052}};

}  // namespace

static std::mutex g_lock;  // g_dir
static std::string g_dir;
static std::atomic<int> g_hits, g_misses, g_stored;
static std::atomic<long long> g_bytes;  // what the cache holds (prune)
static std::atomic<int> g_chunksStarted;   // chunks the engines were asked for (hits and misses)
static std::atomic<int> g_trackChunks;     // chunks of the last track an engine started (PreOperation)
static std::atomic<double> g_lastOpAt;

// One separation (an engine handle, from its PreOperation on): how many chunks it has and how many were asked for, so
// the stems window's run follows its own track and not the chunks of a deck that loads meanwhile.
namespace {
struct Separation { int serial = 0, want = 0, started = 0, hits = 0; double lastOpAt = 0; int spotify = -1; };  // spotify: 1 a Spotify track's, 0 not, -1 not known
}  // namespace
static std::mutex g_sepLock;
static std::map<void*, Separation> g_seps;  // by handle
static int g_sepSerial;                     // of the last one made (g_sepLock)
static std::atomic<bool> g_sepSeen;         // an engine call came with a handle PreOperation made: they can be told apart

static void noteSeparation(void* handle, int chunks, int spotify) {
    std::lock_guard<std::mutex> l(g_sepLock);
    if (g_seps.size() > 64)  // (handles of tracks long gone)
        for (auto it = g_seps.begin(); it != g_seps.end();) it = it->second.serial + 32 < g_sepSerial ? g_seps.erase(it) : std::next(it);
    g_seps[handle] = Separation{++g_sepSerial, chunks, 0, 0, now(), spotify};
}
// Whether a separation (by its engine handle) is of a Spotify track: 1 yes, 0 no, -1 not known.
static int separationSpotify(void* handle) {
    std::lock_guard<std::mutex> l(g_sepLock);
    auto it = g_seps.find(handle);
    return it == g_seps.end() ? -1 : it->second.spotify;
}

// Which track a separation is of. The engine only gets audio, but rekordbox's AudioPartSeparator (one per track) asks
// isSeparatableUri about its own uri member when it starts (stem_sep4), and hands the engine's PreOperation the
// address of its handle member, a little further on in the same object (macOS 7.2.19/7.2.20: uri at +0x1e8, handle at
// +0x358). So the uri whose address lies closest below the handle's is the separation's track; none within 4 KB: not
// known. (No offsets needed, so it holds where the layout differs, as on Windows.)
static std::mutex g_uriLock;
static std::map<const void*, std::pair<std::string, int>> g_sepUris;  // the uri String's address -> (uri, serial)
static int g_sepUriSerial;
static void noteSeparatorUri(const JStr& uri) {
    std::lock_guard<std::mutex> l(g_uriLock);
    g_sepUris[&uri] = {uri.text ? uri.text : "", ++g_sepUriSerial};
    if (g_sepUris.size() > 64)
        for (auto it = g_sepUris.begin(); it != g_sepUris.end();) it = it->second.second + 32 < g_sepUriSerial ? g_sepUris.erase(it) : std::next(it);
}
static int handleSpotify(const void* handlePtr) {
    std::lock_guard<std::mutex> l(g_uriLock);
    auto it = g_sepUris.lower_bound(handlePtr);  // the first at or above it
    if (it == g_sepUris.begin()) return -1;
    --it;
    if ((const char*)handlePtr - (const char*)it->first > 4096) return -1;
    return !strncmp(it->second.first.c_str(), "spotify:", 8) ? 1 : 0;
}
static void noteChunk(void* handle, bool hit, bool started) {
    std::lock_guard<std::mutex> l(g_sepLock);
    auto it = g_seps.find(handle);
    if (it == g_seps.end()) return;
    g_sepSeen = true;
    it->second.started += started, it->second.hits += hit, it->second.lastOpAt = now();
}

static std::string dir() {
    std::lock_guard<std::mutex> l(g_lock);
    return g_dir;
}

// One read for the whole file (a chunk is ~1-5 MB; reading it through stream iterators cost more than decoding it).
static std::string readFile(const std::string& path) {
    std::string out;
    plat::readFile(path, &out);
    return out;
}

// The file a chunk's stems are (or were, before 16-bit keys) in: <dir>/<engine>-<model>-<3|4>/<sha256 of the input>.
static std::string chunkPath(const Engine& e, const std::string& model, int stems, const float* inL, const float* inR) {
    std::vector<float> in((size_t)2 * e.inSamples);  // both channels, hashed as one
    memcpy(in.data(), inL, e.inSamples * sizeof(float));
    memcpy(in.data() + e.inSamples, inR, e.inSamples * sizeof(float));
    std::string hex = plat::sha256Hex(in.data(), in.size() * sizeof(float));
    return dir() + "/" + e.prefix + "-" + model + "-" + std::to_string(stems) + "/" + hex;
}

// The same, keyed by the input as 16-bit samples ("q" + sha256), which new chunks are stored under. A Spotify
// track's deck load separates the stream's float audio; autocue separates it from 16-bit samples (spotifyaudio's, or
// a 16-bit file of the track: x * 32767 rounded, read back as q / 32768), whose floats differ, so the exact hash above
// never matched on Windows. Input already on the 1/32768 grid is taken as such samples (q = y * 32768); any other is
// rounded the same way (q = x * 32767, x clamped to -1..1 first: overs, common in decoded lossy audio, become -32767,
// not -32768). Either way both come to the same samples.
static std::string chunkPath16(const Engine& e, const std::string& model, int stems, const float* inL, const float* inR) {
    int len = e.inSamples;
    bool grid = true;
    for (const float* ch : {inL, inR})
        for (int i = 0; i < len && grid; i++) {
            float v = ch[i] * 32768.f;
            grid = v == std::nearbyint(v) && std::fabs(v) <= 32768.f;
        }
    std::vector<int16_t> q((size_t)2 * len);
    for (int c = 0; c < 2; c++) {
        const float* ch = c ? inR : inL;
        for (int i = 0; i < len; i++) {
            float v = grid ? ch[i] * 32768.f : std::nearbyint(std::max(-1.f, std::min(1.f, ch[i])) * 32767.f);
            q[(size_t)c * len + i] = (int16_t)std::max(-32768.f, std::min(32767.f, v));
        }
    }
    std::string hex = plat::sha256Hex(q.data(), q.size() * sizeof(int16_t));
    return dir() + "/" + e.prefix + "-" + model + "-" + std::to_string(stems) + "/q" + hex;
}

// ---- the cache folder on a slow drive, and what waits for it ---------------------------------

static std::atomic<bool> g_external{false};      // stemcache_dir is on an external drive (watcher, when the folder appears)
static std::atomic<bool> g_staging{false};       // new chunks go to g_stage first (stemcache_staging)
static std::string g_stage;                      // <the mods' cache folder>/stems-staging (set by init, before any thread)
static std::atomic<long long> g_stagedBytes{0};  // what waits there for the cache folder
static std::atomic<int> g_stagedFiles{0};
static std::atomic<bool> g_stageFull{false};     // the internal disk has less than stemcache_min_free_gb left (watcher)

// A chunk's name in the cache folder and in the staging folder: "<engine>-<model>-<stems>/<hash>".
static std::string relOf(const std::string& path) {
    size_t a = path.rfind('/');
    size_t b = a == std::string::npos || a == 0 ? std::string::npos : path.rfind('/', a - 1);
    return b == std::string::npos ? path : path.substr(b + 1);
}

// The chunks the cache folder has, by name, with when each was last used (seconds since 1970): listed by prune, kept
// up to date by whoever adds or removes one. Until the folder has been listed a chunk is looked for on the drive.
static std::mutex g_namesLock;
static std::unordered_map<std::string, long long> g_names;
static bool g_namesKnown;                                          // the folder has been listed (g_namesLock)
static std::vector<std::pair<std::string, long long>> g_namesNew;  // added since the listing that is under way began
static std::vector<std::string> g_toTouch;                         // chunks whose "last used" is to be written (mover)
static void noteName(const std::string& rel) {
    std::lock_guard<std::mutex> l(g_namesLock);
    long long t = (long long)time(nullptr);
    g_names[rel] = t;
    g_namesNew.push_back({rel, t});
}
static void forgetName(const std::string& rel) {
    std::lock_guard<std::mutex> l(g_namesLock);
    g_names.erase(rel);
}
// 1: the cache folder has it, 0: it doesn't, -1: not known (not listed yet).
static int nameKnown(const std::string& rel) {
    std::lock_guard<std::mutex> l(g_namesLock);
    if (g_names.count(rel)) return 1;
    return g_namesKnown ? 0 : -1;
}
// A chunk was served: its file's time is what prune goes by. On an external drive that is a write to the drive, so it
// is left to the mover's turn, and only for a chunk that wasn't used in the last week.
static void noteUsed(const std::string& path, const std::string& rel) {
    if (!g_external) {
        std::error_code ec;
        std::filesystem::last_write_time(plat::fsPath(path), std::filesystem::file_time_type::clock::now(), ec);
        return;
    }
    std::lock_guard<std::mutex> l(g_namesLock);
    long long t = (long long)time(nullptr);
    auto it = g_names.find(rel);
    if (it != g_names.end() && t - it->second < 7 * 86400) return;
    g_names[rel] = t;
    g_toTouch.push_back(rel);
}

// Fills the outputs from one file; false if there is none (or it doesn't fit).
static bool loadFile(const std::string& path, int samples, float* const* out, int n, bool* removed = nullptr) {
    std::string data;
    if (!plat::readFile(path, &data)) return false;  // (a read error isn't a bad chunk: kept)
    bool damaged = false;
    std::error_code ec;
    if (!stemcodec::decode(data, samples, out, n, &damaged)) {
        // Its CRC doesn't match (written just before the drive went away): removed, so it is separated and stored
        // again; a chunk that is served refreshes its time, and prune would never get to it.
        if (damaged && std::filesystem::remove(plat::fsPath(path), ec)) {
            logf("stemcache: removed a damaged chunk, %s", path.c_str());
            if (removed) *removed = true;
        }
        return false;
    }
    return true;
}

// Fills the outputs from a stored chunk (either format): one that still waits in the staging folder, or the cache
// folder's. False if there is none (or it doesn't fit).
static bool load(const std::string& path, int samples, float* const* out, int n) {
    std::string rel = relOf(path);
    if (g_stagedFiles > 0 && loadFile(g_stage + "/" + rel, samples, out, n)) return true;
    if (nameKnown(rel) == 0) return false;  // (the drive isn't asked for what its listing doesn't have)
    bool removed = false;
    {
        slowdrive::Reading reading(g_external);  // (what is written to that drive in the background waits)
        if (!loadFile(path, samples, out, n, &removed)) {
            if (removed) forgetName(rel);
            return false;
        }
    }
    noteUsed(path, rel);
    return true;
}
// Whether a chunk is stored (staged or in the cache folder), without reading it.
static bool stored(const std::string& path) {
    std::string rel = relOf(path);
    if (g_stagedFiles > 0 && plat::fileInfo(g_stage + "/" + rel, nullptr)) return true;
    int known = nameKnown(rel);
    return known < 0 ? plat::fileInfo(path, nullptr) : known > 0;
}

// stemcache_dir may be on a drive that is slow, unplugged or hung: a USB stick that stopped answering blocks every call
// that touches it until it is replugged. One thread (watchMain) looks at it every 2 s; the main thread and rekordbox's
// separation threads go by what it saw last. A look that hasn't answered within 10 s counts as "not there" until it
// does (it may never return: no second look is started meanwhile). Until the first look has answered: not there.
static std::atomic<bool> g_diskFull{true};  // less free space than stemcache_min_free_gb, or the folder isn't there (watcher)
static std::atomic<bool> g_dirGone{true};   // the folder isn't there (its drive not connected)
static std::atomic<double> g_lookSince{0};  // when the look that is still out began (plat::monotonic; 0: none is)
static std::atomic<bool> g_hungSaid;        // "doesn't answer" was logged
static bool dirHung() {
    double since = g_lookSince;
    return since > 0 && plat::monotonic() - since > 10;
}
// Whether chunks may be looked for in the folder now (separation threads: a folder that isn't there or doesn't answer is
// separated without the cache).
static bool dirReady() { return !g_dirGone && !dirHung(); }
static std::atomic<int> g_writing;    // chunks handed to store() whose file isn't written yet
static std::atomic<int> g_pending;    // chunks waiting to be encoded (each holds a copy of its stems)

// How much may wait in the staging folder (stemcache_staging_gb; 0: as much as the disk's free-space limit allows).
static bool stagingFull() {
    double cap = cfgNum("stemcache_staging_gb", 4) * 1e9;
    return g_stageFull || (cap > 0 && (double)g_stagedBytes >= cap);
}

// Stores a separated chunk (copied here, encoded and written in the background; skipped if the encoder is behind).
static std::mutex g_flightLock;
static std::set<std::string> g_inFlight;  // chunks being encoded/written (two writers of one path would share a temp file)

static void store(const std::string& path, int samples, float* const* out, int n) {
    if (g_diskFull || dirHung()) return;
    bool staged = g_staging;
    // The cache folder's drive is behind by more than may wait for it: the chunk is left out (and separated again the
    // next time) rather than filling the internal disk.
    if (staged && stagingFull()) {
        static std::atomic<long long> saidAt;
        long long t = (long long)now();
        if (t - saidAt.exchange(t) > 60)
            logf("stemcache: %.1f GB of stems wait on the internal disk for the cache folder%s, leaving chunks out",
                 g_stagedBytes / 1e9, g_stageFull ? " and that disk is nearly full" : " (stemcache_staging_gb)");
        return;
    }
    {
        std::lock_guard<std::mutex> l(g_flightLock);
        if (!g_inFlight.insert(path).second) return;
    }
    int kbps = stemcodec::qualityKbps(cfgStr("stemcache_quality", "256"));
    // AAC takes ~0.3 s a chunk; when chunks come faster (rekordbox's own engine, several decks) and the encoder falls
    // behind, store lossless (~0.08 s) rather than skip: a chunk left out would be separated again next time.
    if (g_pending >= 8) kbps = 0;
    // A drive that takes the chunks slower than they come must not make them pile up in memory: past this many
    // waiting to be written, a chunk is left out (and separated again the next time).
    if (g_writing >= 16) {
        static std::atomic<long long> saidAt;
        long long t = (long long)now();
        if (t - saidAt.exchange(t) > 60) logf("stemcache: the cache folder takes chunks slower than they come, leaving some out");
        std::lock_guard<std::mutex> l(g_flightLock);
        g_inFlight.erase(path);
        return;
    }
    auto pcm = std::make_shared<std::vector<float>>((size_t)n * samples);
    for (int o = 0; o < n; o++) memcpy(pcm->data() + (size_t)o * samples, out[o], samples * sizeof(float));
    g_pending++;
    std::string file = path;  // the block copies it (a reference would dangle)
    g_writing++;
    plat::background([=]() mutable {
        std::string bytes;
        const float* ch[8];
        for (int o = 0; o < n; o++) ch[o] = pcm->data() + (size_t)o * samples;
        bytes = stemcodec::encode(samples, ch, n, kbps);
        pcm.reset();  // (17 MB: not kept while the write waits for the drive)
        g_pending--;
        const std::string& path = file;
        if (!bytes.empty()) {
            std::error_code ec;
            std::string rel = relOf(path), to = staged ? g_stage + "/" + rel : path;
            std::filesystem::create_directories(plat::fsPath(to.substr(0, to.rfind('/'))), ec);
            // Straight into the cache folder: flushed before the rename, so a stick pulled without ejecting leaves no
            // chunk, or a whole one (the CRC catches what the drive's own cache still loses). On a worker, never a
            // separation thread. Staged: on the internal disk, the mover does the same when it moves it.
            if (plat::writeFileAtomic(to, bytes.data(), bytes.size(), !staged)) {
                g_stored++;
                if (staged) g_stagedBytes += (long long)bytes.size(), g_stagedFiles++;
                else g_bytes += bytes.size(), noteName(rel);
            }
        }
        g_writing--;
        std::lock_guard<std::mutex> l(g_flightLock);
        g_inFlight.erase(path);
    });
}

// ---- Spotify tracks' stems in memory ---------------------------------------------------------

// A Spotify track's stems are never written to disk: its chunks (and those of a separation whose track isn't known)
// are kept here instead, compressed as they would be on disk, so the track loaded again in this session gets its
// stems at once. stemcache_memory_mb (0: none); the least recently used go first. Gone when rekordbox quits.
static std::mutex g_memLock;
static std::map<std::string, std::pair<std::shared_ptr<const std::string>, double>> g_mem;  // name -> (chunk, last used)
static long long g_memBytes;
static std::atomic<int> g_memEncoding;
static std::atomic<int> g_memHits, g_memStored;
static double memCap() { return std::max(0.0, cfgNum("stemcache_memory_mb", 384)) * 1e6; }
static void memTrim(double cap) {  // (g_memLock held)
    while ((double)g_memBytes > cap && !g_mem.empty()) {
        auto oldest = g_mem.begin();
        for (auto it = g_mem.begin(); it != g_mem.end(); ++it)
            if (it->second.second < oldest->second.second) oldest = it;
        g_memBytes -= (long long)oldest->second.first->size();
        g_mem.erase(oldest);
    }
}
static bool memLoad(const std::string& name, int samples, float* const* out, int n) {
    std::shared_ptr<const std::string> chunk;
    {
        std::lock_guard<std::mutex> l(g_memLock);
        auto it = g_mem.find(name);
        if (it == g_mem.end()) return false;
        it->second.second = now();
        chunk = it->second.first;
    }
    if (!stemcodec::decode(*chunk, samples, out, n)) return false;
    g_memHits++;
    return true;
}
static void memStore(const std::string& name, int samples, float* const* out, int n) {
    double cap = memCap();
    if (cap <= 0 || g_memEncoding >= 8) return;  // (an encoder that is behind: left out, separated again next time)
    {
        std::lock_guard<std::mutex> l(g_memLock);
        if (g_mem.count(name)) return;
    }
    int kbps = stemcodec::qualityKbps(cfgStr("stemcache_quality", "256"));
    auto pcm = std::make_shared<std::vector<float>>((size_t)n * samples);
    for (int o = 0; o < n; o++) memcpy(pcm->data() + (size_t)o * samples, out[o], samples * sizeof(float));
    g_memEncoding++;
    plat::background([=] {
        const float* ch[8];
        for (int o = 0; o < n; o++) ch[o] = pcm->data() + (size_t)o * samples;
        auto bytes = std::make_shared<const std::string>(stemcodec::encode(samples, ch, n, kbps));
        if (!bytes->empty()) {
            std::lock_guard<std::mutex> l(g_memLock);
            if (!g_mem.count(name)) {
                g_mem[name] = {bytes, now()};
                g_memBytes += (long long)bytes->size();
                g_memStored++;
                memTrim(memCap());
            }
        }
        g_memEncoding--;
    });
}

// ---- staged chunks move to the cache folder --------------------------------------------------

// Moves one staged chunk into the cache folder: written as <name>.tmpmv a piece at a time, each piece in a turn of its
// own at the drive (slowdrive: none while a deck or an analysis reads from it), flushed, renamed; the staged file goes
// last, so the chunk is served throughout. False if the cache folder didn't take it.
static bool moveChunk(const std::string& rel) {
    std::string from = g_stage + "/" + rel, to = dir() + "/" + rel, tmp = to + ".tmpmv";
    std::string data;
    std::error_code ec;
    if (!plat::readFile(from, &data)) return true;  // gone
    if (data.empty()) {  // (nothing to keep)
        std::filesystem::remove(plat::fsPath(from), ec);
        return true;
    }
    bool gentle = g_external, ok = true, direct = false;
    plat::File out;
    const size_t piece = 1 << 20;
    static std::string madeFolder;  // (one mover)
    for (size_t at = 0; ok && at < data.size(); at += piece) {
        size_t n = std::min(piece, data.size() - at);
        slowdrive::Turn turn(gentle);
        if (!dirReady() || g_diskFull) return false;  // (the drive went away or filled up while this waited for its turn)
        if (at == 0) {
            std::string folder = to.substr(0, to.rfind('/'));
            if (folder != madeFolder) std::filesystem::create_directories(plat::fsPath(folder), ec), madeFolder = folder;
            ok = out.open(tmp, true);
            direct = ok && out.direct();
        }
        ok = ok && out.writeAt(at, data.data() + at, n) && (direct || out.sync());
        turn.wrote(n);
    }
    {
        slowdrive::Turn turn(gentle);  // the flush and the rename
        ok = ok && out.sync();
        out.close();
        if (ok) std::filesystem::rename(plat::fsPath(tmp), plat::fsPath(to), ec), ok = !ec;
        if (!ok) std::filesystem::remove(plat::fsPath(tmp), ec);
    }
    if (!ok) return false;
    noteName(rel);
    g_bytes += (long long)data.size();
    std::filesystem::remove(plat::fsPath(from), ec);
    g_stagedBytes -= (long long)data.size();
    g_stagedFiles--;
    return true;
}

// One thread moves the staged chunks, one after the other, and writes the "last used" times of chunks that were served.
static void moverMain() {
    plat::lowPriorityThread();
    namespace fs = std::filesystem;
    double saidAt = -1e9, startedAt = 0;
    int moved = 0;
    long long movedBytes = 0;
    for (bool first = true;; first = false) {
        if (!first) std::this_thread::sleep_for(std::chrono::seconds(1));
        if (!g_on) continue;
        // what waits (also what a previous run left there); unfinished files of a previous run go
        std::vector<std::pair<std::string, long long>> staged;
        long long bytes = 0;
        std::error_code ec;
        if (g_stagedFiles > 0 || first || g_staging)
            for (fs::directory_iterator sub(plat::fsPath(g_stage), ec), end; !ec && sub != end; sub.increment(ec)) {
                std::error_code ec2;
                if (!sub->is_directory(ec2)) continue;
                std::string folder = plat::utf8(sub->path().filename());
                for (fs::directory_iterator f(sub->path(), ec2), end2; !ec2 && f != end2; f.increment(ec2)) {
                    std::string name = plat::utf8(f->path().filename());
                    std::error_code ec3;
                    if (name.find(".tmp") != std::string::npos) {
                        if (first) fs::remove(f->path(), ec3);
                        continue;
                    }
                    long long size = (long long)f->file_size(ec3);
                    if (ec3) continue;
                    staged.push_back({folder + "/" + name, size});
                    bytes += size;
                }
            }
        if (g_writing == 0) g_stagedBytes = bytes, g_stagedFiles = (int)staged.size();  // (no chunk is on its way there)
        if (!dirReady() || g_diskFull) continue;
        std::vector<std::string> touch;
        {
            std::lock_guard<std::mutex> l(g_namesLock);
            touch.swap(g_toTouch);
        }
        for (const std::string& rel : touch) {
            slowdrive::Turn turn(g_external);
            fs::last_write_time(plat::fsPath(dir() + "/" + rel), fs::file_time_type::clock::now(), ec);
        }
        if (staged.empty()) continue;
        if (!moved) startedAt = now();
        for (const auto& item : staged) {
            if (!g_on || !dirReady() || g_diskFull) break;
            if (!moveChunk(item.first)) {
                if (now() - saidAt > 60) saidAt = now(), logf("stemcache: the cache folder didn't take a chunk, %.1f MB of stems stay staged in %s", g_stagedBytes / 1e6, g_stage.c_str());
                std::this_thread::sleep_for(std::chrono::seconds(10));
                break;
            }
            moved++, movedBytes += item.second;
        }
        if (moved && g_stagedFiles <= 0) {
            logf("stemcache: moved %d chunks to the cache folder (%.1f MB in %.0f s)", moved, movedBytes / 1e6, now() - startedAt);
            moved = 0, movedBytes = 0;
        }
    }
}

// betterstems' HTDemucs, by service: inactive (as if it were off) when that mod is off or not built in.
static bool betterActive() {
    auto f = service<bool (*)()>("betterstems.active");
    return f && f();
}
static std::string betterVersion() {
    auto f = service<std::string (*)()>("betterstems.version");
    return f ? f() : "";
}
static bool betterSeparate(const float* inL, const float* inR, int n, float* const out[8]) {
    auto f = service<bool (*)(const float*, const float*, int, float* const*)>("betterstems.separate");
    return f && f(inL, inR, n, out);
}
typedef bool (*SeparatableFn)(const JStr& uri, int model);

// betterstems: a chunk's stems from HTDemucs, in rekordbox's slots: 0 inst ("other"), 1 vocals, 2 drums, 3 bass
// (3 stems: inst = other + bass). HTDemucs gets the whole input, context included, and the chunk is its middle.
static bool separateBetter(const Engine& e, const float* inL, const float* inR, float* const* out, int n) {
    int len = e.inSamples, ctx = (e.inSamples - e.outSamples) / 2;
    std::vector<float> buf((size_t)8 * len);
    float* src[8];  // drums, bass, other, vocals (L, R each)
    for (int i = 0; i < 8; i++) src[i] = buf.data() + (size_t)i * len;
    if (!betterSeparate(inL, inR, len, src)) return false;
    for (int c = 0; c < 2; c++) {
        const float *drums = src[c] + ctx, *bass = src[2 + c] + ctx, *other = src[4 + c] + ctx, *vocals = src[6 + c] + ctx;
        for (int i = 0; i < e.outSamples; i++) {
            out[c][i] = other[i] + (n == 6 ? bass[i] : 0);
            out[2 + c][i] = vocals[i];
            out[4 + c][i] = drums[i];
            if (n == 8) out[6 + c][i] = bass[i];
        }
    }
    return true;
}

template <int E, int K>
static bool opWrapper(void* handle, int chunk, float* inL, float* inR, float* o0L, float* o0R, float* o1L, float* o1R,
                      float* o2L, float* o2R, float* o3L, float* o3R) {
    Engine& e = g_engines[E];
    OpFn orig = e.op[K];
    bool better = betterActive();
    if ((!g_on && !better) || !inL || !inR || !o0L) return orig(handle, chunk, inL, inR, o0L, o0R, o1L, o1R, o2L, o2R, o3L, o3R);
    g_chunksStarted++;
    g_lastOpAt = now();
    noteChunk(handle, false, true);
    float* out[8] = {o0L, o0R, o1L, o1R, o2L, o2R, o3L, o3R};
    int n = o3L ? 8 : 6, stems = n / 2;
    std::string model = better ? betterVersion() : e.version;
    // A Spotify track's chunks (or a separation's whose track isn't known) are kept in memory, never on disk.
    bool inMemory = g_on && separationSpotify(handle) != 0;
    bool cached = g_on && dirReady();  // (a folder that isn't there or doesn't answer: separated as without the cache)
    std::string path = cached || inMemory ? chunkPath16(e, model, stems, inL, inR) : "";
    if ((inMemory && memLoad(relOf(path), e.outSamples, out, n)) ||
        (cached && (load(path, e.outSamples, out, n) || load(chunkPath(e, model, stems, inL, inR), e.outSamples, out, n)))) {
        g_hits++;
        noteChunk(handle, true, false);
        return true;
    }
    bool viaBetter = better && separateBetter(e, inL, inR, out, n);
    if (!viaBetter && !orig(handle, chunk, inL, inR, o0L, o0R, o1L, o1R, o2L, o2R, o3L, o3R)) return false;
    g_misses++;
    if (viaBetter || !better) {  // (not rekordbox's stems under betterstems' key)
        if (inMemory) memStore(relOf(path), e.outSamples, out, n);
        else if (cached) store(path, e.outSamples, out, n);
    }
    return true;
}

template <int E>
static int initWrapper(const char* modelDir, int a, int b, bool bigMemory, int threads, int c) {
    std::string v = readFile(std::string(modelDir ? modelDir : "") + "/separator_ver.txt");
    v.erase(std::remove_if(v.begin(), v.end(), [](char ch) { return !isalnum((unsigned char)ch); }), v.end());
    g_engines[E].version = v.empty() ? "0" : v;
    logf("stemcache: %s model %s", g_engines[E].prefix, g_engines[E].version.c_str());
    return g_engines[E].init(modelDir, a, b, bigMemory, threads, c);
}

template <int E>
static bool preWrapper(void** handle, unsigned samples, bool parallel) {
    const Engine& e = g_engines[E];
    g_trackChunks = (int)((samples + e.outSamples - 1) / e.outSamples);
    bool ok = e.pre(handle, samples, parallel);
    if (ok && handle && *handle) noteSeparation(*handle, g_trackChunks, handleSpotify(handle));
    return ok;
}

static const PreFn kPres[2] = {preWrapper<0>, preWrapper<1>};
static const OpFn kOps[2][2] = {{opWrapper<0, 0>, opWrapper<0, 1>}, {opWrapper<1, 0>, opWrapper<1, 1>}};
static const InitFn kInits[2] = {initWrapper<0>, initWrapper<1>};

// AudioPartLibrary::load -> juce::DynamicLibrary::getFunction(<engine> + "Initialize" / "OperationAtIndex...").
// (Windows: getFunction is inlined, the sites are its GetProcAddress(module, name) import calls.)
typedef void* (*GetFunctionFn)(void* lib, const JStr& name);
static void* wrap(void* fn, const char* s) {
    if (!s) s = "";
    for (int e = 0; e < 2; e++) {
        size_t n = strlen(g_engines[e].prefix);
        if (!fn || strncmp(s, g_engines[e].prefix, n)) continue;
        const char* rest = s + n;
        if (!strcmp(rest, "Initialize")) return g_engines[e].init = (InitFn)fn, (void*)kInits[e];
        if (!strcmp(rest, "PreOperation")) return g_engines[e].pre = (PreFn)fn, (void*)kPres[e];
        if (!strcmp(rest, "OperationAtIndexOtherWithBass")) return g_engines[e].op[0] = (OpFn)fn, (void*)kOps[e][0];
        if (!strcmp(rest, "OperationAtIndexSeparatedBass")) return g_engines[e].op[1] = (OpFn)fn, (void*)kOps[e][1];
    }
    if (fn) logf("stemcache: unknown engine function %s, not cached", s);
    return fn;
}
#ifdef _WIN32
typedef void* (*GetProcAddressFn)(void* module, const char* name);
#define STEM_GETFN(hook)                                                                    \
    extern "C" void* rbm_hook_##hook(void* module, const char* name) {                     \
        return wrap(RBM_ORIG(hook, GetProcAddressFn)(module, name), name);                \
    }
#else
#define STEM_GETFN(hook)                                                                    \
    extern "C" void* rbm_hook_##hook(void* lib, const JStr& name) {                         \
        return wrap(RBM_ORIG(hook, GetFunctionFn)(lib, name), name.text);                  \
    }
#endif
STEM_GETFN(stem_init)
STEM_GETFN(stem_pre)
STEM_GETFN(stem_op3)
STEM_GETFN(stem_op4)

// Deletes the least recently used chunks above stemcache_max_gb.
// A chunk's folder is <engine>-<model>-<stems>, its file the hash of its audio (64 hex digits, "q" in front for the
// quantised one), or such a name with ".tmp<pid>" behind it while it is written. Nothing else is counted or deleted:
// stemcache_dir may be a folder that holds other things too.
static bool chunkFolder(const std::string& name) {
    for (const Engine& e : g_engines) {
        size_t n = strlen(e.prefix);
        if (name.size() > n + 2 && !name.compare(0, n, e.prefix) && name[n] == '-' &&
            (!name.compare(name.size() - 2, 2, "-3") || !name.compare(name.size() - 2, 2, "-4")))
            return true;
    }
    return false;
}
static bool chunkFile(const std::string& name) {
    size_t i = !name.empty() && name[0] == 'q' ? 1 : 0, hex = 0;
    while (i + hex < name.size() && isxdigit((unsigned char)name[i + hex])) hex++;
    return hex == 64 && (i + hex == name.size() || !name.compare(i + hex, 4, ".tmp"));
}

static void prune() {
    static std::atomic<bool> pruning;  // (at the start two are asked for at once, and one may take longer than a minute)
    if (pruning.exchange(true)) return;
    struct Done { ~Done() { pruning = false; } } done;
    if (!dirReady()) return;  // (not there, or it doesn't answer: the watcher says which)
    double maxGb = cfgNum("stemcache_max_gb", 10);
    std::string root = dir();
    namespace fs = std::filesystem;
    struct F { std::string path; fs::file_time_type used; long long size; };
    std::vector<F> files;
    long long total = 0;
    std::error_code ec;
    // The listing also says which chunks the folder has (g_names), so a chunk it doesn't have is not looked for on
    // the drive. What is added while it runs is kept (g_namesNew).
    std::unordered_map<std::string, long long> names;
    {
        std::lock_guard<std::mutex> l(g_namesLock);
        g_namesNew.clear();
    }
    auto clockNow = fs::file_time_type::clock::now();
    long long timeNow = (long long)time(nullptr);
    bool whole = true;
    for (fs::directory_iterator sub(plat::fsPath(root), ec), end; !ec && sub != end; sub.increment(ec)) {
        std::string folder = plat::utf8(sub->path().filename());
        if (!chunkFolder(folder) || !sub->is_directory(ec)) continue;
        std::error_code ec2;
        for (fs::directory_iterator f(sub->path(), ec2), end2; !ec2 && f != end2; f.increment(ec2)) {
            std::error_code ec3;
            std::string name = plat::utf8(f->path().filename());
            if (!chunkFile(name) || !f->is_regular_file(ec3)) continue;
            auto used = f->last_write_time(ec3);
            long long size = (long long)f->file_size(ec3);
            if (ec3) continue;
            files.push_back({plat::utf8(f->path()), used, size});
            total += size;
            if (name.find(".tmp") == std::string::npos)
                names[folder + "/" + name] = timeNow - (long long)std::chrono::duration_cast<std::chrono::seconds>(clockNow - used).count();
        }
        whole = whole && !ec2;
    }
    whole = whole && !ec;
    if (whole && root == dir()) {
        std::lock_guard<std::mutex> l(g_namesLock);
        for (const auto& added : g_namesNew) names[added.first] = added.second;
        g_names.swap(names);
        g_namesKnown = true;
    }
    g_bytes = total;
    if (maxGb <= 0 || total <= maxGb * 1e9) return;
    std::sort(files.begin(), files.end(), [](const F& a, const F& b) { return a.used < b.used; });
    int dropped = 0;
    for (const F& f : files) {
        if (total <= maxGb * 1e9) break;
        std::error_code rec;
        slowdrive::Turn turn(g_external);  // (a delete writes to the drive too)
        if (fs::remove(plat::fsPath(f.path), rec) && !rec) total -= f.size, dropped++, forgetName(relOf(f.path));
    }
    g_bytes = total;
    logf("stemcache: dropped %d chunks, %.1f GB left", dropped, total / 1e9);
}

// ---- Spotify tracks -------------------------------------------------------------------------

// rekordbox separates a streaming track only if it downloaded the track itself (Service::isInitialized(uri)), which
// it never does for Spotify. isSeparatableUri says yes for Spotify tracks too, and the separator then reads the
// track like a deck does: through rekordbox's own Spotify reader, kept up with the download (core/spotifyread.cpp),
// so the stems arrive during the first load (tried 2026-11-13). The stems themselves stay in rekordbox's memory, as
// for any streaming track; this mod's copies of the chunks stay in memory (see "Spotify tracks' stems in memory"). A
// track with a local file (rbmod.h, "trackfiles") is read from that file.
//   config: stemcache_spotify=0 to leave Spotify tracks without stems
static const Addr A_enabled{"__ZN6rbxfrm9audiopart9AudioPart18isAudioPartEnabledEv"};
static const Addr A_model{"__ZN6rbxfrm9audiopart9AudioPart17getAudioPartModelEv"};
static const Addr A_separatable{"__ZN6rbxfrm9audiopart9AudioPart16isSeparatableUriERKN4juce6StringENS0_5MODELE"};
static const Addr A_sepInit{"__ZN6rbxfrm9audiopart25AudioPartSeparatorManager13isInitializedENS0_5MODELE"};
static const Addr A_load{"__ZN6rbxfrm9audiopart9AudioPart4loadEiRKN4juce6StringEPNS0_26AudioPartProgressInterfaceE"};
static const Addr A_unload{"__ZN6rbxfrm9audiopart9AudioPart6unloadEi"};
static const Addr A_separated{"__ZN6rbxfrm9audiopart9AudioPart25isSeparatedSeparatableUriERKN4juce6StringENS0_5MODELE"};
enum { kPrepDeck = 100 };  // AudioPart keeps decks in a map: any number works, and the real ones are 0-3

static volatile bool g_spotify = true;  // stemcache_spotify
static volatile bool g_streamReadable;  // rekordbox's Spotify readers keep up with the download (set by init)

static bool isSpotifyUri(const char* u) { return u && !strncmp(u, "spotify:", 8); }
// Something can serve the separator the track's audio: the stream itself, or a local file of it.
static bool spotifyUsable(const char*) { return g_streamReadable || trackFilesOn(); }
static bool spotifySeparatable(const JStr& uri, int model) {
    if (!g_on || !g_spotify || !isSpotifyUri(uri.text) || model < 0 || model > 3) return false;
    if (!at<bool (*)()>(A_enabled)() || !at<bool (*)(int)>(A_sepInit)(model)) return false;
    return spotifyUsable(uri.text);
}
#define RBM_STEM_SEP_HOOK(n)                                                        \
    extern "C" bool rbm_hook_stem_sep##n(const JStr& uri, int model) {              \
        return RBM_ORIG(stem_sep##n, SeparatableFn)(uri, model) || spotifySeparatable(uri, model); \
    }
static double g_refusedAt = -1e9;  // when the deck's check (isAvailableSeparatableUri) last said no for a Spotify track
extern "C" bool rbm_hook_stem_sep1(const JStr& uri, int model) {
    bool yes = RBM_ORIG(stem_sep1, SeparatableFn)(uri, model) || spotifySeparatable(uri, model);
    if (!yes && isSpotifyUri(uri.text)) g_refusedAt = now();
    return yes;
}
RBM_STEM_SEP_HOOK(2)
RBM_STEM_SEP_HOOK(3)
// AudioPartSeparator::start (checkAndPrepare on 7.2.20) asks about its own uri member: noted, to tell which track the
// separation that follows is of (noteSeparatorUri).
extern "C" bool rbm_hook_stem_sep4(const JStr& uri, int model) {
    noteSeparatorUri(uri);
    return RBM_ORIG(stem_sep4, SeparatableFn)(uri, model) || spotifySeparatable(uri, model);
}
RBM_STEM_SEP_HOOK(5)
RBM_STEM_SEP_HOOK(6)
RBM_STEM_SEP_HOOK(7)
// Where the stems go (AudioPartDataManager::getOutput): a streaming track's stay in memory, and only a track
// rekordbox downloaded counts as one; a Spotify track is treated as such a track.
typedef bool (*UriFn)(const JStr& uri);
static bool spotifyStemsTrack(const JStr& uri) {
    return g_on && g_spotify && isSpotifyUri(uri.text) && spotifyUsable(uri.text);
}
extern "C" bool rbm_hook_stem_streaming(const JStr& uri) {
    return RBM_ORIG(stem_streaming, UriFn)(uri) || spotifyStemsTrack(uri);
}
extern "C" bool rbm_hook_stem_streamingReady(const JStr& uri) {
    return RBM_ORIG(stem_streamingReady, UriFn)(uri) || spotifyStemsTrack(uri);
}
// rekordbox's status-bar message for a Spotify track without stems ("The STEMS function is unavailable for Spotify
// tracks") is only still true when nothing can read the track for the separator (this rekordbox build lacks what
// core/spotifyread.cpp needs): then it says that instead, in English whatever rekordbox's language. Recognised by
// when it comes, not by its text: UiPlayer::showTrackSeparationCautionIfNeeded asks isAvailableSeparatableUri
// (stem_sep1) and shows it right after, on the main thread.
// windows-optional: stem_banner
#ifndef _WIN32
typedef void (*ComponentMessageFn)(void* component, const JStr& text, int type);
extern "C" void rbm_hook_stem_banner(void* component, const JStr& text, int type) {
    auto orig = RBM_ORIG(stem_banner, ComponentMessageFn);
    bool ours = g_on && g_spotify && now() - g_refusedAt < 0.3 && !spotifyUsable(nullptr) &&
                at<bool (*)()>(A_enabled)() && at<bool (*)(int)>(A_sepInit)(at<int (*)()>(A_model)());
    if (!ours) return orig(component, text, type);
    std::string s = "STEMS for Spotify tracks: the mods can't read Spotify tracks in this rekordbox build.";
    static double loggedAt;
    if (now() - loggedAt > 1) loggedAt = now(), logf("stemcache: rekordbox's \"no STEMS for Spotify\" message shown as: %s", s.c_str());
    JStr t(s.c_str());
    orig(component, t, type);
}
#endif

static bool separatable(const char* uri, int model) {
    JStr u(uri);
    // (the stems window's runs: a Spotify track only from a local file of it: there is no deck to keep the stems of a
    // track separated ahead of time otherwise)
    return at<SeparatableFn>(A_separatable)(u, model) || (spotifySeparatable(u, model) && trackFile(uri, nullptr));
}

// Why rekordbox won't separate a track ahead of time: a Spotify track (on a deck only; or without its local file yet,
// where a mod keeps them), or anything else it doesn't take.
static stemcache::State refusal(const char* uri) {
    if (!isSpotifyUri(uri) || !g_spotify) return stemcache::State::Unsupported;
    return trackFilesMod()[0] ? stemcache::State::NeedsFile : stemcache::State::DeckOnly;
}

// ---- tracks separated before ----------------------------------------------------------------

// The stems are keyed by audio, not by track, so which tracks were separated is kept beside them: tracks.txt, one
// URI per line, added when a playlist run separates a track. Read and written by the watcher (the folder may be on a
// drive that doesn't answer); the main thread goes by what it read.
static std::mutex g_tracksLock;  // g_separated, g_separatedDir, g_toMark (never held over file I/O)
static std::set<std::string> g_separated;
static std::string g_separatedDir;  // the folder g_separated was read from ("" until a read worked)
static std::vector<std::pair<std::string, std::string>> g_toMark;  // (folder, uri) still to be added to its tracks.txt
static bool wasSeparated(const std::string& uri) {
    std::lock_guard<std::mutex> l(g_tracksLock);
    return g_separated.count(uri) > 0;
}
// Read again when stemcache_dir changes, and until the folder is there (rekordbox started before the stick was in).
// Then what was separated since is added to it. Watcher thread.
static void syncSeparated(const std::string& d, bool there) {
    std::string was;
    {
        std::lock_guard<std::mutex> l(g_tracksLock);
        was = g_separatedDir;
    }
    if (d != was) {
        std::set<std::string> read;
        if (there) {
            std::ifstream f(plat::fsPath(d + "/tracks.txt"));  // (none yet: nothing separated there)
            for (std::string l; std::getline(f, l);)
                if (!l.empty()) read.insert(l);
        }
        std::lock_guard<std::mutex> l(g_tracksLock);
        g_separated.swap(read);
        g_separatedDir = there ? d : "";
        g_toMark.clear();
        return;
    }
    std::vector<std::pair<std::string, std::string>> add;
    {
        std::lock_guard<std::mutex> l(g_tracksLock);
        add.swap(g_toMark);
    }
    if (add.empty() || !there) return;
    std::ofstream f(plat::fsPath(d + "/tracks.txt"), std::ios::app);
    for (const auto& a : add)
        if (a.first == d) f << a.second << "\n";
}
static void markSeparated(const std::string& uri) {
    std::lock_guard<std::mutex> l(g_tracksLock);
    if (g_separatedDir.empty() || !g_separated.insert(uri).second) return;  // (no folder: its stems weren't stored either)
    g_toMark.push_back({g_separatedDir, uri});
}

// ---- separating a playlist ahead of time ---------------------------------------------------

namespace {
struct Prep {
    bool exists = false, running = false, collecting = false;
    std::string playlistId, name, finishedWhy;
    double startedAt = 0, endedAt = 0, stableSince = 0;
    int lastRows = -1;
    std::vector<stemcache::TrackView> items;
    size_t next = 0;  // index of the next item to start
    int cur = -1;     // the item separating now
    int done = 0, skipped = 0;
    int chunksAt = 0, hitsAt = 0;  // g_chunksStarted / g_hits when the current track started
    int sepAfter = 0;              // g_sepSerial then: the track's separation is the first one made after it
    double trackAt = 0;
    bool onDeck = false;  // the current track is on a deck: rekordbox separates it there (one separation per track)
};
}  // namespace
static Prep g_prep;  // main thread

static void finishTrack() {
    if (g_prep.cur < 0) return;
    if (!g_prep.onDeck) at<void (*)(int)>(A_unload)(kPrepDeck);
    g_prep.cur = -1;
    g_prep.onDeck = false;
}

static bool onADeck(const std::string& uri) {
    for (int d = 0; d < 4; d++)
        if (deckPath(d) == uri) return true;
    return false;
}

static void stopPrep(const char* why) {
    if (g_prep.cur >= 0) g_prep.items[g_prep.cur].state = stemcache::State::Waiting;  // interrupted
    finishTrack();
    if (g_prep.running) {
        int unsupported = 0, spotify = 0;
        for (const auto& t : g_prep.items)
            unsupported += t.state == stemcache::State::Unsupported, spotify += t.state == stemcache::State::NeedsFile || t.state == stemcache::State::DeckOnly;
        logf("stemcache: %s: %d tracks separated, %d skipped (%d Spotify tracks on a deck only or without a local file, %d not separable), of %d in %s",
             why, g_prep.done, g_prep.skipped, spotify, unsupported, (int)g_prep.items.size(), g_prep.name.c_str());
    }
    g_prep.running = g_prep.collecting = false;
    g_prep.finishedWhy = why;
    g_prep.endedAt = now();
}

// One step of the run (tick): collect the playlist's tracks once its list has settled, then separate them one by one.
static void stepPrep() {
    double t = now();
    if (g_diskFull || dirHung()) {
        stopPrep(dirHung() ? "no answer" : g_dirGone ? "no folder" : "disk full");
        return;
    }
    if (!at<bool (*)()>(A_enabled)()) {
        stopPrep("stems off");
        return;
    }
    if (g_prep.collecting && isSelection(g_prep.playlistId)) {
        // A selection of tracks (core's selectionPlaylist): they are known, so nothing waits for the list, and its
        // other rows stay out of the run. (Their titles come from the library, see fillNames.)
        int model = at<int (*)()>(A_model)();
        for (const std::string& uri : playlistTracks(g_prep.playlistId).uris) {
            stemcache::TrackView tv;
            tv.row = (int)g_prep.items.size();
            tv.uri = uri;
            if (!separatable(uri.c_str(), model)) tv.state = refusal(uri.c_str());
            g_prep.items.push_back(tv);
        }
        g_prep.collecting = false;
        for (const auto& i : g_prep.items) g_prep.skipped += i.state != stemcache::State::Waiting;
        logf("stemcache: separating the stems of the selection '%s': %d tracks (%d can't be separated now)", g_prep.name.c_str(),
             (int)g_prep.items.size(), g_prep.skipped);
        if (g_prep.items.empty()) return stopPrep("empty list");
    }
    if (g_prep.collecting) {
        void* lv = browserListViewer();
        int rows = lv ? listRows(lv) : 0;
        bool onPlaylist = shownPlaylist() == g_prep.playlistId;
        if (rows != g_prep.lastRows || !onPlaylist) g_prep.lastRows = rows, g_prep.stableSince = t;
        if (t - g_prep.startedAt < 2.5 || t - g_prep.stableSince < 1.5 || rows <= 0) {
            if (t - g_prep.startedAt > 30) stopPrep(onPlaylist ? "empty list" : "playlist not shown");
            return;
        }
        int model = at<int (*)()>(A_model)();
        for (int r = 0; r < rows; r++)
            if (const char* row = listRow(lv, r))
                if (const char* p = trackPath(row); p && *p) {
                    stemcache::TrackView tv;
                    tv.row = r;
                    tv.uri = p;
                    tv.title = trackTitle(row);
                    tv.artist = trackArtist(row);
                    if (!separatable(p, model)) tv.state = refusal(p);
                    g_prep.items.push_back(tv);
                }
        g_prep.collecting = false;
        int refused = 0;
        for (const auto& i : g_prep.items) refused += i.state != stemcache::State::Waiting;
        logf("stemcache: separating the stems of %d tracks in %s (%d can't be separated now)", (int)g_prep.items.size(),
             g_prep.name.c_str(), refused);
        g_prep.skipped = refused;
    }
    if (g_prep.cur >= 0 && g_prep.onDeck) {
        // Loading it again would share the deck's separation and pass no chunks through here: wait for the deck's.
        auto& it = g_prep.items[g_prep.cur];
        it.seconds = t - g_prep.trackAt;
        JStr u(it.uri.c_str());
        if (at<bool (*)(const JStr&, int)>(A_separated)(u, at<int (*)()>(A_model)())) {
            g_prep.done++;
            it.state = stemcache::State::Done;
            markSeparated(it.uri);
            logf("stemcache: separated %s on a deck", it.uri.c_str());
            finishTrack();
        } else if (!onADeck(it.uri)) {  // it left the deck before its stems were done: separate it here
            it.state = stemcache::State::Waiting;
            g_prep.next = g_prep.cur;
            finishTrack();
        } else if (t - g_prep.trackAt > 600) {
            g_prep.skipped++;
            it.state = stemcache::State::GaveUp;
            logf("stemcache: gave up waiting for %s on a deck", it.uri.c_str());
            finishTrack();
        } else {
            return;
        }
    }
    if (g_prep.cur >= 0) {
        auto& it = g_prep.items[g_prep.cur];
        int chunks = g_chunksStarted - g_prep.chunksAt, want = g_trackChunks, hits = g_hits - g_prep.hitsAt;
        double lastOp = g_lastOpAt;
        if (g_sepSeen) {  // this track's own separation (else, engines whose handles can't be followed: every chunk since it started)
            Separation mine;
            {
                std::lock_guard<std::mutex> l(g_sepLock);
                for (auto& kv : g_seps)
                    if (kv.second.serial > g_prep.sepAfter && (!mine.serial || kv.second.serial < mine.serial)) mine = kv.second;
            }
            chunks = mine.started, want = mine.want, hits = mine.hits, lastOp = mine.serial ? mine.lastOpAt : 0;
        }
        bool finished = want > 0 && chunks >= want && t - lastOp > 1;  // the last chunk is stored right after
        bool idle = t - std::max(g_prep.trackAt, lastOp) > 15;
        it.seconds = t - g_prep.trackAt, it.chunks = chunks, it.want = want;
        if (!finished && !idle && t - g_prep.trackAt < 600) return;
        if (finished) {
            g_prep.done++;
            it.state = hits >= chunks ? stemcache::State::FromDisk : stemcache::State::Done;
            markSeparated(it.uri);
        } else {
            g_prep.skipped++;
            it.state = stemcache::State::GaveUp;
        }
        const char* file = strrchr(it.uri.c_str(), '/');
        logf("stemcache: %s %s: %d of %d chunks in %.0f s", finished ? "separated" : "gave up on", file ? file + 1 : it.uri.c_str(),
             chunks, want, t - g_prep.trackAt);
        finishTrack();
    }
    // The cache folder's drive is behind by as much as may wait for it: the next track starts once it has caught up
    // (below 80%), so no chunk of the run is left out.
    static bool holding;
    bool hold = g_staging && (stagingFull() || (holding && g_stagedBytes > cfgNum("stemcache_staging_gb", 4) * 0.8e9));
    if (hold != holding) {
        holding = hold;
        logf(hold ? "stemcache: %.1f GB of stems wait for the cache folder: the run pauses until it has caught up"
                  : "stemcache: the cache folder has caught up (%.1f GB still wait), the run goes on", g_stagedBytes / 1e9);
    }
    if (hold) return;
    while (g_prep.next < g_prep.items.size()) {
        int i = (int)g_prep.next++;
        auto& it = g_prep.items[i];
        if (it.state != stemcache::State::Waiting) continue;
        // (a Spotify track whose local file came meanwhile is taken too)
        if (!separatable(it.uri.c_str(), at<int (*)()>(A_model)())) {
            it.state = refusal(it.uri.c_str());
            g_prep.skipped++;
            continue;
        }
        if (onADeck(it.uri)) {
            it.state = stemcache::State::Separating;
            g_prep.cur = i;
            g_prep.onDeck = true;
            g_prep.trackAt = t;
            return;
        }
        JStr u(it.uri.c_str());
        g_trackChunks = 0;
        g_prep.chunksAt = g_chunksStarted;
        g_prep.hitsAt = g_hits;
        g_prep.trackAt = t;
        {
            std::lock_guard<std::mutex> l(g_sepLock);
            g_prep.sepAfter = g_sepSerial;
        }
        if (!at<bool (*)(int, const JStr&, void*)>(A_load)(kPrepDeck, u, nullptr)) {
            g_prep.skipped++;
            it.state = stemcache::State::GaveUp;
            continue;
        }
        it.state = stemcache::State::Separating;
        g_prep.cur = i;
        return;
    }
    stopPrep("done");
}

static void startPrep(const std::string& playlistId, const std::string& name) {
    finishTrack();
    g_prep = Prep();
    g_prep.exists = g_prep.running = g_prep.collecting = true;
    g_prep.playlistId = playlistId;
    g_prep.name = name;
    g_prep.startedAt = now();
}

// For testing from a hot-loaded experiment: separate a playlist by its identifier (as shownPlaylist() gives it).
extern "C" void rbm_stemcache_prepare(const char* playlistId) { stemcache::separatePlaylist(playlistId); }
extern "C" void rbm_stemcache_stop() {
    if (g_prep.running) stopPrep("stopped");
}

namespace stemcache {

static void fillNames(TrackView& tv) {
    if (!tv.title.empty()) return;
    library::Track lt;
    if (library::track(tv.uri, &lt)) tv.title = lt.title, tv.artist = lt.artist;
    if (tv.title.empty()) {
        const char* file = strrchr(tv.uri.c_str(), '/');
        tv.title = file ? file + 1 : tv.uri;
    }
}

static void count(View& v) {
    double sep = 0;
    int nsep = 0;
    for (const TrackView& i : v.tracks) {
        switch (i.state) {
            case State::Waiting: v.waiting++; break;
            case State::Separating: v.active++; break;
            case State::Done: v.done++, nsep++, sep += i.seconds; break;
            case State::FromDisk: v.fromDisk++; break;
            case State::Unsupported: v.unsupported++; break;
            case State::NeedsFile: v.needsFile++; break;
            case State::DeckOnly: v.deckOnly++; break;
            case State::GaveUp: v.gaveUp++; break;
            case State::NotSeparated: v.waiting++; break;
            case State::Separated: v.separated++; break;
        }
    }
    if (v.running && nsep >= 1) v.eta = sep / nsep * (v.waiting + v.active);
}

View playlistView(const std::string& id) {
    View v;
    if (id.empty() || (g_prep.exists && id == g_prep.playlistId)) {
        v.playlistId = g_prep.playlistId;
        v.playlistName = g_prep.name;
        v.isRun = v.exists = g_prep.exists;
        v.running = g_prep.running, v.collecting = g_prep.collecting;
        v.finishedWhy = g_prep.finishedWhy;
        v.shown = !v.playlistId.empty() && shownPlaylist() == playlistParent(v.playlistId);
        v.tracks = g_prep.items;
        for (TrackView& t : v.tracks) {
            fillNames(t);
            if (t.state == State::Waiting && !g_prep.running)  // a stopped run didn't get to it
                t.state = wasSeparated(t.uri) ? State::Separated : State::NotSeparated;
        }
        v.rows = (int)v.tracks.size();
        double t = g_prep.running ? now() : g_prep.endedAt;
        v.elapsed = g_prep.exists ? t - g_prep.startedAt : 0;
        count(v);
        return v;
    }
    PlaylistTracks pl = playlistTracks(id);
    v.playlistId = id;
    v.playlistName = pl.name;
    v.known = pl.known;
    v.shown = pl.shown;
    int model = at<bool (*)()>(A_enabled)() ? at<int (*)()>(A_model)() : -1;
    for (int r = 0; r < (int)pl.uris.size(); r++) {
        TrackView tv;
        tv.row = r;
        tv.uri = pl.uris[r];
        if (wasSeparated(tv.uri)) tv.state = State::Separated;
        else if (model < 0 || separatable(tv.uri.c_str(), model)) tv.state = State::NotSeparated;
        else tv.state = refusal(tv.uri.c_str());
        fillNames(tv);
        v.tracks.push_back(tv);
    }
    v.rows = (int)v.tracks.size();
    count(v);
    return v;
}

std::string runPlaylist() { return g_prep.exists ? g_prep.playlistId : ""; }

Needs needs() {
    Needs n;
    n.stemsOn = at<bool (*)()>(A_enabled)();
    n.spotifyOn = g_spotify;
    n.trackFiles = trackFilesOn();
    n.trackFilesMod = trackFilesMod();
    n.folderNoAnswer = dirHung();
    n.folder = dir();
    return n;
}

void separatePlaylist(const std::string& id) {
    if (g_prep.running || id.empty()) return;
    std::string name = playlistTracks(id).name;
    startPrep(id, name.empty() ? id : name);
    showPlaylistId(id);
}

void fetchFiles(const std::string& id) { fetchTrackFiles(id.c_str()); }

void stop() { rbm_stemcache_stop(); }

}  // namespace stemcache

// The window has the actions (separate, stop, and fetching the Spotify tracks' local files first where a mod keeps them).
static void playlistMenu(Menu* menu, void* treeItem) {
    menuAddItem(menu, "Open stems window", [treeItem] { stemcache::showWindow(treeItem); });
}

// The track list's menu: the same window on the selected tracks (a selection playlist).
static void trackMenu(Menu* menu, const std::vector<std::string>& uris, const std::string& selection) {
    menuAddItem(menu, menuWindowItem("stems window", uris.size()).c_str(), [selection] { stemcache::showWindowFor(selection); });
}

static void configure() {
    std::string d = cfgStr("stemcache_dir", "");
    if (d.empty()) d = plat::cacheDir() + "/stems";
    if (d[0] == '~') d = plat::homeDir() + d.substr(1);
    std::lock_guard<std::mutex> l(g_lock);
    g_dir = d;  // (the watcher makes it)
}

// The cache folder's watcher (see g_lookSince): every 2 s it looks whether stemcache_dir is there and how much room its
// drive has left, makes it when it is set or changed, and keeps tracks.txt. The first time it finds the folder it counts
// what it holds (prune).
static void watchMain() {
    std::string made;  // the folder last made
    std::string listed, judged;  // the folder g_names is of; the one g_external was judged for
    bool first = true, counted = false;
    for (;;) {
        std::string d = dir();
        g_lookSince = plat::monotonic();
        if (d != made) {
            made = d;
            std::error_code ec;
            std::filesystem::create_directories(plat::fsPath(d), ec);
        }
        plat::FileInfo fi;
        bool gone = !plat::fileInfo(d, &fi);  // its drive isn't connected: chunks would be encoded for nothing
        uint64_t freeBytes = gone ? 0 : plat::diskFree(d);
        if (d != listed) {  // another folder: its chunks are not known until prune has listed it
            listed = d, judged.clear(), counted = false;
            std::lock_guard<std::mutex> l(g_namesLock);
            g_names.clear();
            g_namesKnown = false;
        }
        if (!gone && d != judged) judged = d, g_external = plat::externalVolume(d);
        {
            std::string mode = cfgStr("stemcache_staging", "auto");
            bool staging = mode == "0" ? false : mode == "1" ? true : judged == d ? g_external.load() : g_staging.load();
            if (staging) {
                std::error_code ec;
                std::filesystem::create_directories(plat::fsPath(g_stage), ec);
                uint64_t stageFree = plat::diskFree(g_stage);
                g_stageFull = stageFree && (double)stageFree < cfgNum("stemcache_min_free_gb", 5) * 1e9;
            }
            if (g_staging.exchange(staging) != staging)
                logf(staging ? "stemcache: new stems go to %s first and move to the cache folder in the background (%s)"
                             : "stemcache: new stems go straight into the cache folder%.0s (%s)",
                     g_stage.c_str(), mode == "auto" ? (staging ? "the cache folder is on an external drive" : "the cache folder is on an internal disk")
                                                     : ("stemcache_staging=" + mode).c_str());
        }
        syncSeparated(d, !gone && fi.isDir);
        bool full = gone || (freeBytes && (double)freeBytes < cfgNum("stemcache_min_free_gb", 5) * 1e9);
        if (first ? full : (full != g_diskFull || gone != g_dirGone))
            logf("stemcache: %s", gone ? "the cache folder is not there (drive not connected?): not storing stems"
                                      : full ? "the disk is nearly full: not storing new stems" : "storing stems again");
        g_diskFull = full;
        g_dirGone = gone;
        g_lookSince = 0;
        if (g_hungSaid.exchange(false)) logf("stemcache: the cache folder %s answers again (%s)", d.c_str(), gone ? "not there" : "there");
        if (!gone && !counted) {
            counted = true;
            plat::background([] {
                prune();
                logf("stemcache: %s holds %.1f GB", dir().c_str(), g_bytes / 1e9);
            });
        }
        first = false;
        std::this_thread::sleep_for(std::chrono::seconds(2));
    }
}

static void init() {
    configure();
    g_streamReadable = spotifyReadersKeepUp();  // (the separator reads Spotify tracks from the stream)
    static std::once_flag once;
    std::call_once(once, [] {
        g_lookSince = plat::monotonic();  // (the first look is out from now on)
        g_stage = plat::cacheDir() + "/stems-staging";
        std::thread(watchMain).detach();
        std::thread(moverMain).detach();
    });
}

static double g_pruneAt, g_logAt;
static int g_loggedHits, g_loggedMisses;
static void tick() {
    double t = now();
    g_spotify = cfgInt("stemcache_spotify", 1) != 0;
    // "open [playlist id]" (the window, on that playlist or selection), "separate <playlist id>" (as its Separate stems button), "stop"
    std::string cmd = devCommand("stemcache");
    if (cmd.rfind("open", 0) == 0) stemcache::showWindowFor(cmd.size() > 5 ? cmd.substr(5) : "");
    else if (cmd.rfind("separate ", 0) == 0) stemcache::separatePlaylist(cmd.substr(9));
    else if (cmd == "stop") stemcache::stop();
    if (g_prep.running) stepPrep();
    static double uiAt;
    // Twice a second during a run; otherwise every few seconds (a playlist's view looks up every track).
    if (stemcache::windowVisible() && t - uiAt >= (g_prep.running ? 0.5 : 3)) uiAt = t, stemcache::refreshWindow();
    if (t > g_logAt) {
        g_logAt = t + 10;
        configure();
        int h = g_hits, m = g_misses;
        if (h != g_loggedHits || m != g_loggedMisses) {
            std::string waits;
            if (g_stagedFiles > 0) waits = "; " + std::to_string(g_stagedFiles.load()) + " chunks, " + std::to_string((int)(g_stagedBytes / 1000000)) + " MB wait for the cache folder";
            if (g_memStored || g_memHits) {
                std::lock_guard<std::mutex> l(g_memLock);
                waits += "; in memory: " + std::to_string(g_mem.size()) + " chunks of Spotify tracks, " + std::to_string(g_memBytes / 1000000) +
                         " MB, " + std::to_string(g_memHits.load()) + " served";
            }
            logf("stemcache: %d chunks from disk or memory, %d separated (%d stored, %.1f GB%s)", h, m, (int)g_stored, g_bytes / 1e9, waits.c_str());
        }
        g_loggedHits = h, g_loggedMisses = m;
    }
    if (dirHung() && !g_hungSaid.exchange(true))
        logf("stemcache: the cache folder %s doesn't answer (drive hung?): treated as not there", dir().c_str());
    if (t > g_pruneAt) {
        // (an external drive: its whole listing is read less often, and not while a deck or an analysis reads from it)
        bool wait = g_external && !slowdrive::quiet();
        g_pruneAt = t + (wait ? 5 : g_external ? 600 : 60);
        if (dirReady() && !wait) plat::background([] { prune(); });  // (one at a time: prune's own guard)
    }
}

// For other mods (see rbmod.h, "between mods"): the stems of one chunk, asked for as rekordbox asks (the first
// engine's layout, 4 stems, betterstems' model), if they are on disk. Only looks: nothing is separated or stored.
static bool provideCached(const float* inL, const float* inR, int inSamples, float* const out[8]) {
    const Engine& e = g_engines[0];
    if (!g_on || !inL || !inR || inSamples != e.inSamples || !betterActive()) return false;
    std::string model = betterVersion(), path = chunkPath16(e, model, 4, inL, inR);
    return memLoad(relOf(path), e.outSamples, out, 8) ||
           (dirReady() && (load(path, e.outSamples, out, 8) || load(chunkPath(e, model, 4, inL, inR), e.outSamples, out, 8)));
}
RBM_PROVIDE("stemcache", "stemcache.cached", provideCached, &g_on);
// ...and stores a chunk another mod separated with betterstems' model in the same layout (autocue), so a deck gets it
// from disk. out: rekordbox's slots, the chunk alone (no context).
static void provideStore(const float* inL, const float* inR, int inSamples, float* const out[8]) {
    const Engine& e = g_engines[0];
    if (!g_on || !dirReady() || !inL || !inR || inSamples != e.inSamples || !betterActive()) return;
    std::string path = chunkPath16(e, betterVersion(), 4, inL, inR);
    if (!stored(path)) store(path, e.outSamples, out, 8);
}
RBM_PROVIDE("stemcache", "stemcache.store", provideStore, &g_on);
// ...the same for a Spotify track's chunk: in memory, never on disk.
static void provideStoreSpotify(const float* inL, const float* inR, int inSamples, float* const out[8]) {
    const Engine& e = g_engines[0];
    if (!g_on || !inL || !inR || inSamples != e.inSamples || !betterActive()) return;
    memStore(relOf(chunkPath16(e, betterVersion(), 4, inL, inR)), e.outSamples, out, 8);
}
RBM_PROVIDE("stemcache", "stemcache.storeSpotify", provideStoreSpotify, &g_on);

RBM_MOD({.name = "stemcache", .init = init, .tick = tick, .playlistMenu = playlistMenu, .trackMenu = trackMenu, .enabled = &g_on, .menuOrder = 30});
