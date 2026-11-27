// stemcache: keep separated stems on disk, so a track's stems are there at once the next time it loads.
//
// rekordbox separates a track into stems (STEMS) whenever it loads, in chunks of ~11-12 s, and throws them away
// shortly after the track leaves the decks. The separation engine is a plugin (Frameworks/libSpleeter.dylib or
// libDemucs3.dylib) that AudioPartLibrary::load finds by name: <engine>OperationAtIndexOtherWithBass (3 stems) and
// <engine>OperationAtIndexSeparatedBass (4 stems) separate one chunk:
//   bool op(handle, chunk, inL, inR, out0L, out0R, out1L, out1R, out2L, out2R, out3L, out3R)
// planar floats. The 3-stem op takes no out3: rekordbox leaves those two stack slots as they were (not NULL), so
// the stem count comes from which op it is, never from out3L. This hands rekordbox wrappers instead: a chunk whose
// input audio was separated before (same engine, model version and stem count) gets its stems from disk; any other
// is separated as before and stored. Keyed by the audio itself (SHA-256 of the input), so it can't give a track another's stems.
// Stored as AAC (stemcodec.h; 256 kbps per stem by default: ~1.5 MB per chunk of 4 stems), or 16-bit PCM packed
// losslessly (mid/side, order-2 prediction, byte planes, LZFSE: ~4-5 MB). Chunks of either kind are read.
//
// "Separate stems" in the stems window (a playlist's menu: Open stems window) fills the cache ahead of time: each
// track goes through rekordbox's own separation (AudioPart::load on a deck number no deck uses, then AudioPart::unload), one at a time,
// so its chunks are decoded exactly as on a deck and hash the same. With stemcache_keep=chosen those are the only
// tracks whose stems are stored; with stemcache_ondemand a deck's track isn't separated until its stems are used
// (both further down: "which stems are kept on disk, and stems on demand").
//   config: stemcache=0 to disable
//           stemcache_spotify=0   no stems for Spotify tracks (otherwise: on a deck at once, separated from the stream)
//           stemcache_memory_mb=384   stems that aren't stored on disk (Spotify tracks', and what stemcache_keep
//                                     leaves out) kept in memory for loading the track again in this session;
//                                     the least recently used go first (0: none)
//           stemcache_keep=all   which tracks' stems are saved to disk: all (every track that gets stems, on a deck
//                                or with Separate stems), chosen (only tracks separated with the stems window's
//                                Separate stems button; a track that is only loaded onto a deck gets stems that
//                                are thrown away when rekordbox quits) or none (nothing new is saved)
//           stemcache_ondemand=0   1: a deck's track is separated only once a stem control is used on it
//                                  (a stem muted, a PART ISOLATOR knob turned, the FX taken off a stem), from
//                                  where it plays; stems that are stored already still come at once
//           stemcache_quick=1   1: with betterstems' HTDemucs, a stem control used where the track has no stems yet
//                               gets the few seconds at the playhead separated first, about a third of a second
//                               (macOS); 0: it waits for that whole part of the track (11-12 s of it, a second
//                               or so)
//           stemcache_lazy_model=1   1: with betterstems' HTDemucs separating, rekordbox's own stem model (680 MB for
//                                    the quality one) is loaded only when it has to separate after all (restart);
//                                    0: at startup, as rekordbox does it
//           stemcache_log=0   1 logs every chunk and every stem control (for debugging)
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
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

using namespace rbm;

// Its settings (core/settings.h): what the settings window and the config file's list show of them.
namespace opt {
const Setting keep{"stemcache_keep", "all", {.label = "Save stems to disk", .help = "Everything: every track that gets stems. Only what I separate: a track you just load onto a deck gets stems that are thrown away when rekordbox quits.", .choices = {{"all", "Everything"}, {"chosen", "Only what I separate in the stems window"}, {"none", "Nothing"}}}};
const Setting ondemand{"stemcache_ondemand", false, {.label = "Separate only when a stem control is used", .help = "A deck's track is separated from the moment you mute a stem, turn a PART ISOLATOR knob or take the FX off a stem, starting where it plays."}};
const Setting spotify{"stemcache_spotify", true, {.label = "Stems for Spotify tracks"}};
const Setting quality{"stemcache_quality", "256", {.label = "Quality of saved stems", .help = "256 kbps takes about 35 MB for a 5-minute track, lossless about 125 MB.", .choices = {{"128", "128 kbps"}, {"192", "192 kbps"}, {"256", "256 kbps"}, {"lossless", "Lossless"}}, .needs = "stemcache_keep!=none"}};
const Setting maxGb{"stemcache_max_gb", 10, {.label = "Disk space for stems", .help = "Past this the stems used longest ago are deleted. 0: no limit.", .unit = "GB", .min = 0, .max = 2000, .step = 1, .needs = "stemcache_keep!=none"}};
const Setting memoryMb{"stemcache_memory_mb", 384, {.label = "Memory for stems that aren't saved", .unit = "MB", .min = 0, .max = 8192, .step = 64}};
const Setting dir{"stemcache_dir", "", {.label = "Folder", .help = "Empty: the default folder.", .wide = true, .needs = "stemcache_keep!=none"}};
const Setting minFreeGb{"stemcache_min_free_gb", 5, {.label = "Save nothing new below this much free space", .unit = "GB", .min = 0, .max = 500, .step = 1, .advanced = true, .needs = "stemcache_keep!=none"}};
const Setting staging{"stemcache_staging", "auto", {.label = "Write to the internal disk first", .choices = {{"0", "Never"}, {"1", "Always"}, {"auto", "When the folder is on an external drive"}}, .advanced = true, .needs = "stemcache_keep!=none"}};
const Setting stagingGb{"stemcache_staging_gb", 4, {.label = "How much may wait on the internal disk", .unit = "GB", .min = 1, .max = 200, .step = 1, .advanced = true, .needs = "stemcache_staging!=0"}};
const Setting quick{"stemcache_quick", true, {.label = "Stems at the playhead at once", .help = "With HTDemucs: when you use a stem control where the track has no stems yet, a few seconds at the playhead are separated first, before the rest of that part of the track (macOS)."}};
const Setting lazyModel{"stemcache_lazy_model", true, {.label = "Load rekordbox's own stem model only when needed", .help = "With HTDemucs separating, rekordbox's own model (680 MB for the quality one) stays out of memory until it has to separate after all.", .restart = true}};
const Setting log{"stemcache_log", false, {.label = "Log every chunk and stem control", .debug = true}};
}  // namespace opt

#define RBM_NOINLINE __attribute__((noinline))

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
static std::atomic<int> g_quicks;  // windows published ahead of their chunk
// The time of day in seconds (to the ms), for stemcache_log's lines: what happened when between a stem control and the stems.
static double wallClock() {
    double t = std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count();
    return fmod(t, 1000);
}
static std::atomic<long long> g_bytes;  // what the cache holds (prune)
static std::atomic<int> g_chunksStarted;   // chunks the engines were asked for (hits and misses)
static std::atomic<int> g_trackChunks;     // chunks of the last track an engine started (PreOperation)
static std::atomic<double> g_lastOpAt;

// One separation (an engine handle, from its PreOperation on): how many chunks it has and how many were asked for, so
// the stems window's run follows its own track and not the chunks of a deck that loads meanwhile.
namespace {
// A window of stems handed to rekordbox ahead of its chunk ("stems at the playhead at once").
struct Quick {
    long long w0 = 0;           // the window's first sample (the separator's samples, from the track's start)
    long long a = 0, b = 0;     // what rekordbox got of it: [a, b), inside one chunk
    long long chunkEnd = 0;     // where that chunk ends (or the track)
    int len = 0;                // the window's length
    std::vector<float> planes;  // its stems as an engine's outputs: 8 planes of len (stem 0 L, R, stem 1 L, ...)
};
struct Separation {
    int serial = 0, want = 0, started = 0, hits = 0;
    double lastOpAt = 0;
    int spotify = -1;  // 1 a Spotify track's, 0 not, -1 not known
    std::string uri;   // its track ("" not known)
    // For the windows published ahead of their chunk (further down, "stems at the playhead at once"):
    char* separator = nullptr;  // rekordbox's AudioPartSeparator, while its engine calls run (null: not known)
    long long total = 0;        // the track's length, in the separator's samples
    long long chunkLen = 0;     // a chunk's, in the same
    std::set<int> done;         // chunks whose stems rekordbox has
    std::vector<std::shared_ptr<Quick>> quicks;  // windows published, until their chunk is done
    double quickFailedAt = -1e9;
};
}  // namespace
static std::mutex g_sepLock;
static std::map<void*, Separation> g_seps;  // by handle
static int g_sepSerial;                     // of the last one made (g_sepLock)
static std::atomic<bool> g_sepSeen;         // an engine call came with a handle PreOperation made: they can be told apart

static void noteSeparation(void* handle, int chunks, const std::string& uri, char* separator = nullptr, long long total = 0, long long chunkLen = 0) {
    int spotify = uri.empty() ? -1 : !strncmp(uri.c_str(), "spotify:", 8) ? 1 : 0;
    std::lock_guard<std::mutex> l(g_sepLock);
    if (g_seps.size() > 64)  // (handles of tracks long gone)
        for (auto it = g_seps.begin(); it != g_seps.end();) it = it->second.serial + 32 < g_sepSerial ? g_seps.erase(it) : std::next(it);
    Separation& s = g_seps[handle] = Separation{++g_sepSerial, chunks, 0, 0, now(), spotify, uri};
    s.separator = separator, s.total = total, s.chunkLen = chunkLen;
}
// A separation's track, by its engine handle ("" not known).
static std::string separationUri(void* handle) {
    std::lock_guard<std::mutex> l(g_sepLock);
    auto it = g_seps.find(handle);
    return it == g_seps.end() ? "" : it->second.uri;
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
static std::string handleUri(const void* handlePtr) {
    std::lock_guard<std::mutex> l(g_uriLock);
    auto it = g_sepUris.lower_bound(handlePtr);  // the first at or above it
    if (it == g_sepUris.begin()) return "";
    --it;
    if ((const char*)handlePtr - (const char*)it->first > 4096) return "";
    return it->second.first;
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
    double cap = opt::stagingGb.number() * 1e9;
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
    int kbps = stemcodec::qualityKbps(opt::quality.text());
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
static double memCap() { return std::max(0.0, opt::memoryMb.number()) * 1e6; }
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
    int kbps = stemcodec::qualityKbps(opt::quality.text());
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
        parkIfQuitting();
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

// ---- which stems are kept on disk, and stems on demand ----------------------------------------

// stemcache_keep: whose stems are written to the cache folder. all: every track that is separated; chosen: only the
// tracks separated from the stems window (tracks.txt: now or before); none: nothing new. What isn't kept on disk is
// kept in memory like a Spotify track's (stemcache_memory_mb), for loading the track again in this session. Stems
// that are on disk already are read whatever this says.
enum { kKeepAll, kKeepChosen, kKeepNone };
static std::atomic<int> g_keep{kKeepAll};
static bool wasSeparated(const std::string& uri);

// stemcache_ondemand=1: a track on a deck isn't separated when it loads. Its stems that are in the cache (or in
// memory) still come at once. The first chunk that has to be separated is separated, and then the separator's thread
// is kept inside that engine call until a stem control is used on a deck holding the track (wantStems, or a control
// that is on already: g_stemState). Kept after the chunk rather than before it, because rekordbox's separator takes
// the chunks the deck is waiting for first (Spleeter models: AudioPartData::isReadyPosition asks for the playhead's
// chunk with priority), but only when it picks its next chunk: released after the chunk, the next one it separates
// is the playhead's (stems about 1.3 s after the control was used, measured with HTDemucs on an M2 Pro; released
// before it, the chunk it had been given at load came first: 2 s). Meanwhile the deck plays the unseparated track,
// whatever its stem buttons say, as it does during any separation.
// The wait ends when rekordbox tells the thread to stop (the track left the decks: AudioPartSeparator::cancel waits
// for the thread without a time limit), so only on a juce thread; a separation whose track isn't known never waits.
// Windows: the same code, checked against rekordbox.exe in the emulator (tools/win/emu/test_stemcache.py), not yet
// run on a Windows machine.
static std::atomic<int> g_onDemand{0};
static std::atomic<bool> g_logChunks{false};  // stemcache_log
static std::mutex g_wantLock;            // g_wanted, g_runUri
static std::set<std::string> g_wanted;   // tracks whose stems were asked for, while they are on a deck
static std::string g_runUri;             // the track the stems window's run is separating
static std::atomic<int> g_held;          // separations waiting to be asked for
// Per deck, the stem controls that are on (from the calls that set them): bits 0-3 a muted stem, 4-7 a PART ISOLATOR
// knob off its centre, 8-11 a stem the FX don't apply to. A deck with any of them needs its track's stems.
static std::atomic<unsigned> g_stemState[4];
static std::atomic<bool> g_deckNeeds[4];  // a deck with a track needs its stems now (tick)
static std::atomic<bool> g_quick{true};   // stemcache_quick (further down: "stems at the playhead at once")
static std::atomic<double> g_jumpAt[4];   // when a deck's playhead last jumped (tick)
// Service "stemcache.stemsNeeded": a deck's stems are needed now (a stem control on a deck with a track). betterstems
// then doesn't hold its separation back for an analysis' beat stage (any thread).
static bool stemsNeeded() {
    for (auto& n : g_deckNeeds)
        if (n) return true;
    return false;
}
static const Addr A_currentThread{"__ZN4juce6Thread16getCurrentThreadEv"};
static const Addr A_threadShouldExit{"__ZNK4juce6Thread16threadShouldExitEv"};

RBM_NOINLINE static bool isWanted(const std::string& uri) {
    std::lock_guard<std::mutex> l(g_wantLock);
    return uri == g_runUri || g_wanted.count(uri) > 0;
}
static bool isRunTrack(const std::string& uri) {
    std::lock_guard<std::mutex> l(g_wantLock);
    return !uri.empty() && uri == g_runUri;
}
// A stem control was used on a deck holding this track (any thread).
static void wantStems(const std::string& uri, const char* why) {
    if (uri.empty()) return;
    {
        std::lock_guard<std::mutex> l(g_wantLock);
        if (!g_wanted.insert(uri).second) return;
    }
    if (g_onDemand) logf("stemcache: stems of %s asked for (%s)%s", uri.c_str(), why, g_held > 0 ? ": separating" : "");
}
// Waits while nobody has asked for the track's stems (separator thread). (Functions of their own, not inlined: the
// Windows emulator test calls them, tools/win/emu/test_stemcache.py.)
RBM_NOINLINE static void waitForStems(const std::string& uri) {
    if (uri.empty() || isWanted(uri)) return;
    void* thread = at<void* (*)()>(A_currentThread)();
    if (!thread) return;  // (not a thread rekordbox can tell to stop: no waiting on it)
    g_held++;
    logf("stemcache: the rest of %s is separated when a stem control is used (stemcache_ondemand)", uri.c_str());
    while (!at<bool (*)(void*)>(A_threadShouldExit)(thread) && g_onDemand && g_on && !isWanted(uri))
        std::this_thread::sleep_for(std::chrono::milliseconds(15));
    g_held--;
}
// After a chunk was separated.
static void waitUntilWanted(void* handle) {
    if (g_onDemand) waitForStems(separationUri(handle));
}

// A deck's stem controls, whoever moves them (the buttons on screen, pads, a controller, the keyboard), end in three
// calls on the deck's player through its control interface (rbxfrm::IDjPlayerControl): setPartMuteState,
// setPartIsoValue (the PART ISOLATOR knobs) and setPartFxState (which stems the FX apply to). What a deck hands out
// as its control (DjSystem::getPlayerControl) is a wrapper, rbxfrm::DjPlayerStreamingEventSender, that passes every
// call on to the player behind it, through the same slot of the same interface. The vtable to change is the one the
// player behind a deck's wrapper points at: found on the running objects, which also holds where a compiler puts that interface's vtable
// elsewhere in the class (MSVC: one vtable per base; clang: a part of the class's one).
// In those three slots a call that takes a stem away or changes it asks for the deck's stems.
static const Addr A_senderVt{"__ZTVN6rbxfrm28DjPlayerStreamingEventSenderE"};
#ifdef _WIN32
// The wrapper's own slots (MSVC lays its vtable out as the interface's; the table has them from the vtable).
static const Offset V_setPartMute{0x2a0, "__ZTVN6rbxfrm28DjPlayerStreamingEventSenderE", "vt:__ZN6rbxfrm28DjPlayerStreamingEventSender16setPartMuteStateENS_9audiopart4PARTEb", 0};
static const Offset V_setPartIso{0x2a8, "__ZTVN6rbxfrm28DjPlayerStreamingEventSenderE", "vt:__ZN6rbxfrm28DjPlayerStreamingEventSender15setPartIsoValueENS_9audiopart4PARTEd", 0};
static const Offset V_setPartFx{0x2b0, "__ZTVN6rbxfrm28DjPlayerStreamingEventSenderE", "vt:__ZN6rbxfrm28DjPlayerStreamingEventSender14setPartFxStateENS_9audiopart4PARTEb", 0};
static const size_t kSenderVptr = 0x10;
#else
// The slot each of the wrapper's three functions passes its call on to.
static const Offset V_setPartMute{0x298, "__ZN6rbxfrm28DjPlayerStreamingEventSender16setPartMuteStateENS_9audiopart4PARTEb", "ldr x8", 0};
static const Offset V_setPartIso{0x2a0, "__ZN6rbxfrm28DjPlayerStreamingEventSender15setPartIsoValueENS_9audiopart4PARTEd", "ldr x8", 0};
static const Offset V_setPartFx{0x2a8, "__ZN6rbxfrm28DjPlayerStreamingEventSender14setPartFxStateENS_9audiopart4PARTEb", "ldr x8", 0};
static const size_t kSenderVptr = 0x18;  // (the wrapper has a virtual base: one offset word more before its vtable's slots)
#endif
static const Addr A_djSystem{"__ZNK8djengine10DjEngineIF11getDjSystemEv"};
static const Addr A_playerControl{"__ZNK8DjSystem16getPlayerControlEi"};
static const Offset O_senderControl{0x40, "__ZN6rbxfrm28DjPlayerStreamingEventSender16setPartMuteStateENS_9audiopart4PARTEb", "ldr x0", 0};  // DjPlayerStreamingEventSender: the IDjPlayerControl it passes on to
static void (*o_setPartMute)(void*, int, bool);
static void (*o_setPartIso)(void*, int, double);
static void (*o_setPartFx)(void*, int, bool);

// Which deck (0-3) a player control is, -1 if none.
static int deckOfControl(void* control) {
    void* e = engine();
    void* sys = e ? at<void* (*)(void*)>(A_djSystem)(e) : nullptr;
    for (int d = 0; sys && d < deckCount() && d < 4; d++) {
        // (what the deck hands out is a wrapper that reports to the streaming services, the player behind it)
        char* c = at<char* (*)(void*, int)>(A_playerControl)(sys, d);
        if (c && (c == control || *(void**)(c + O_senderControl) == control)) return d;
    }
    return -1;
}
// The playhead first. rekordbox's separator keeps a list of chunks with priority besides the in-order one, and the
// deck's stem reader puts the playhead's chunk on it (AudioPartData::isReadyPosition ->
// AudioPartDataManager::requestSeparateAt(.., true)), but only where AudioPart::isRandomAccessible(MODEL) says so:
// the Spleeter models (0, 2), and only while the deck reads stems. With Demucs3 (1, 3: STEMS Analysis Process set
// to quality; betterstems' HTDemucs then runs behind it as well) the separator goes from the track's start, so a
// stem muted 2 minutes in waited for every chunk before it (a tester: 5-8 s). The separator itself doesn't care
// about the model (its run takes the priority list first, the chunks land in a SparseSet), so the same call is made
// here, for every deck with a track, whether its stems are in use or not: then they are there when a stem control
// is used. From the tick, at once after a jump of the playhead and else twice a second, and when a stem control is
// used. Only the playhead's chunk and, in its last 5 s, the next one: the priority list is sorted, so a chunk asked
// for at a playhead that has since jumped back would come first. Asked of all four models, whichever the track is
// separated with: the others aren't found, a chunk that is done or not in a list is left alone, and a track whose
// separation is over isn't looked at (the manager checks).
// Windows: both verified for 7.2.19 in tools/win/verify/stems.out.json (requestSeparateAt is a static function there
// too: String, MODEL, position, count, bool).
static const Addr A_requestSeparateAt{"__ZN6rbxfrm9audiopart20AudioPartDataManager17requestSeparateAtERKN4juce6StringENS0_5MODELExib"};
static const Addr A_getPlayingTime{"__ZNK8djengine10DjEngineIF14getPlayingTimeENS_15EnPlayerChannelE"};  // ms
static int playingMs(int d) {  // a deck's playhead, ms (0 if none)
    void* e = engine();
    int ms = e && d >= 0 && d < 4 ? at<int (*)(void*, int)>(A_getPlayingTime)(e, d) : 0;
    return ms > 0 ? ms : 0;
}
// ms: where (-1 the playhead; the "separateat" test command gives one).
RBM_NOINLINE static void separateAtPlayhead(int d, const std::string& uri, int ms = -1) {
    if (!engine() || uri.empty() || d < 0 || d >= 4) return;
    if (ms < 0) ms = playingMs(d);
    long long pos = (long long)ms * 441 / 10;  // the separator's samples (44.1 kHz)
    const int span = 5 * 44100;                // (chunks are 11-12 s)
    JStr u(uri.c_str());
    for (int model = 0; model < 4; model++)
        at<void (*)(const JStr&, int, long long, int, bool)>(A_requestSeparateAt)(u, model, pos, span, true);
}

static void buffersAtPlayhead();
// kind 0 mute, 1 isolator, 2 FX; on: the control now needs the stems.
static void stemControlUsed(void* control, int kind, int part, double value, bool on) {
    static const char* const kWhat[] = {"a stem was muted", "a PART ISOLATOR knob was turned", "the FX were taken off a stem"};
    int d = deckOfControl(control);
    if (d < 0 || d >= 4 || part < 0 || part >= 4) return;
    unsigned bit = 1u << (kind * 4 + part);
    if (on) g_stemState[d].fetch_or(bit); else g_stemState[d].fetch_and(~bit);
    if (g_logChunks) logf("stemcache: [%.3f] deck %d: %s, stem %d = %g%s", wallClock(), d + 1, kind == 0 ? "mute" : kind == 1 ? "isolator" : "FX", part, value, on ? " (needs the stems)" : "");
    if (!on) return;
    std::string uri = deckPath(d);
    if (!uri.empty()) g_deckNeeds[d] = true;  // (at once, not at the next tick: betterstems stops waiting)
    // Before the held separation is let go (wantStems): its next pick is then the playhead's chunk.
    separateAtPlayhead(d, uri);
    if (g_onDemand) wantStems(uri, kWhat[kind]);
    // Stems that are there already: the deck is asked to take them now, not when its playhead enters the next page.
    if (g_quick) plat::onMain(buffersAtPlayhead);
}
static void setPartMute(void* control, int part, bool muted) {
    stemControlUsed(control, 0, part, muted, muted);
    o_setPartMute(control, part, muted);
}
static void setPartIso(void* control, int part, double value) {  // 0..1, centre 0.5 (what PartIsoControlComp::resetPartIsoValue sends)
    stemControlUsed(control, 1, part, value, fabs(value - 0.5) > 0.02);
    o_setPartIso(control, part, value);
}
static void setPartFx(void* control, int part, bool on) {
    stemControlUsed(control, 2, part, on, !on);
    o_setPartFx(control, part, on);
}
// Swaps the three slots, once a deck's player is there to find the vtable on (main thread, from the tick).
// macOS: from the start, so that a control left on is known. Windows, where this hasn't run on a real machine yet:
// only once stemcache_ondemand is switched on.
static bool g_followed;  // the slots were looked for
RBM_NOINLINE static void followStemControls() {
    bool& done = g_followed;
#ifdef _WIN32
    if (!g_onDemand) return;
#endif
    if (done) return;
    void* e = engine();
    void* sys = e ? at<void* (*)(void*)>(A_djSystem)(e) : nullptr;
    char* wrapper = sys && deckCount() > 0 ? at<char* (*)(void*, int)>(A_playerControl)(sys, 0) : nullptr;
    if (!wrapper) return;  // (no decks yet: the next tick)
    done = true;
    void*** player = *(void**)wrapper == at<char*>(A_senderVt) + kSenderVptr ? *(void****)(wrapper + O_senderControl) : nullptr;
    void** vt = player ? *player : nullptr;
    void* was[3] = {vt ? vt[V_setPartMute / 8] : nullptr, vt ? vt[V_setPartIso / 8] : nullptr, vt ? vt[V_setPartFx / 8] : nullptr};
    // Not what was expected (a deck's control that isn't the wrapper, a player without a vtable, slots that aren't
    // three different functions): nothing is changed.
    if (!vt || vt == *(void***)wrapper || !was[0] || !was[1] || !was[2] || was[0] == was[1] || was[1] == was[2] || was[0] == was[2]) {
        logf("stemcache: the decks' stem controls can't be followed here: stemcache_ondemand has no way to start a separation");
        return;
    }
    o_setPartMute = (void (*)(void*, int, bool))was[0];
    o_setPartIso = (void (*)(void*, int, double))was[1];
    o_setPartFx = (void (*)(void*, int, bool))was[2];
    if (!patchSlot(&vt[V_setPartMute / 8], (void*)&setPartMute) || !patchSlot(&vt[V_setPartIso / 8], (void*)&setPartIso) || !patchSlot(&vt[V_setPartFx / 8], (void*)&setPartFx))
        logf("stemcache: the decks' stem controls can't be followed (their vtable can't be changed): stemcache_ondemand has no way to start a separation");
    else if (g_logChunks) logf("stemcache: following the decks' stem controls");
}

// ---- rekordbox's own model only when it is needed -------------------------------------------------------------------
//
// rekordbox loads the selected engine's model when it starts (AudioPartSeparator::initialize -> the library's
// Initialize): 680 MB for Demucs3 (measured: the app's memory with and without that call), which stays for the
// session (the engine's Finalize gives next to nothing back). With betterstems every chunk comes from HTDemucs or the
// cache and that model is never run. So while betterstems is expected to separate ("betterstems.expected"), the
// engine's Initialize is held back, and rekordbox gets stand-ins for what it does with an engine besides separating:
// the functions sit in a table per engine (AudioPartLibrary::s_library: Initialize, Finalize, IsInitialized,
// PreOperation, PostOperation, CancelOperation and the two OperationAtIndex, in that order; four of them are the
// wrappers of this file already, which is how the others' places are found), and Finalize, IsInitialized,
// PostOperation and CancelOperation are swapped in it when Initialize is called. A separation then gets a handle
// of this mod's own (PreOperation). Whenever the engine itself has to separate after all (betterstems off or failed
// for a chunk, no HTDemucs when a track starts), it is initialised then, with what rekordbox passed, and the
// separation gets a real handle from its PreOperation.
// stemcache_lazy_model=0: as rekordbox does it (restart).
namespace {
struct StandIn {  // a separation's handle while the engine isn't initialised
    int engine;
    unsigned samples;
    bool parallel;
    void* real = nullptr;  // the engine's own handle, once it had to separate
};
struct LazyEngine {
    bool held = false;  // Initialize was held back, and the table's other functions are this mod's
    bool real = false;  // the engine's own Initialize has run
    std::string modelDir;
    int a = 0, b = 0, threads = 0, c = 0;
    bool bigMemory = false;
    uintptr_t (*finalize)() = nullptr;
    bool (*isInitialized)() = nullptr;
    uintptr_t (*post)(void*) = nullptr;
    uintptr_t (*cancel)(void*) = nullptr;
};
}  // namespace
static LazyEngine g_lazy[2];
static std::atomic<bool> g_ownEngine{false};  // test command "ownengine 1": chunks go to rekordbox's engine, HTDemucs or not
static std::recursive_mutex g_lazyLock;  // g_lazy, g_standIns
static std::set<void*> g_standIns;
static bool betterExpected() {
    auto f = service<bool (*)()>("betterstems.expected");
    return f && f();
}
static StandIn* standIn(void* handle) {
    std::lock_guard<std::recursive_mutex> l(g_lazyLock);
    return g_standIns.count(handle) ? (StandIn*)handle : nullptr;
}
// The engine's own Initialize, if it was held back (any thread; once).
static void engineNeeded(int E) {
    std::lock_guard<std::recursive_mutex> l(g_lazyLock);
    LazyEngine& z = g_lazy[E];
    if (!z.held || z.real) return;
    double t0 = now();
    g_engines[E].init(z.modelDir.c_str(), z.a, z.b, z.bigMemory, z.threads, z.c);
    z.real = true;
    logf("stemcache: rekordbox's %s model is needed after all: loaded in %.1f s", g_engines[E].prefix, now() - t0);
}
// The engine's own handle for a separation (its PreOperation's), null if it gives none.
static void* engineHandle(int E, void* handle) {
    StandIn* s = standIn(handle);
    if (!s) return handle;
    engineNeeded(E);
    std::lock_guard<std::recursive_mutex> l(g_lazyLock);
    if (!s->real && !g_engines[E].pre(&s->real, s->samples, s->parallel)) s->real = nullptr;
    return s->real;
}
// The engine separates a chunk itself.
static bool engineOp(int E, int K, void* handle, int chunk, float* inL, float* inR, float* o0L, float* o0R, float* o1L, float* o1R,
                     float* o2L, float* o2R, float* o3L, float* o3R) {
    void* real = engineHandle(E, handle);
    return real && g_engines[E].op[K](real, chunk, inL, inR, o0L, o0R, o1L, o1R, o2L, o2R, o3L, o3R);
}
template <int E>
static uintptr_t finalizeWrapper() {
    std::lock_guard<std::recursive_mutex> l(g_lazyLock);
    LazyEngine& z = g_lazy[E];
    bool was = z.real;
    logf("stemcache: rekordbox closes its %s engine (%s)", g_engines[E].prefix, was ? "its model was loaded" : z.held ? "its model was never loaded" : "not held back");
    z.real = false, z.held = false;
    return was && z.finalize ? z.finalize() : 0;
}
template <int E>
static bool isInitializedWrapper() {
    {
        std::lock_guard<std::recursive_mutex> l(g_lazyLock);
        if (g_lazy[E].held && !g_lazy[E].real) return true;
    }
    return g_lazy[E].isInitialized && g_lazy[E].isInitialized();
}
template <int E>
static uintptr_t postWrapper(void* handle) {
    StandIn* s = standIn(handle);
    if (!s) return g_lazy[E].post ? g_lazy[E].post(handle) : 0;
    uintptr_t result = s->real && g_lazy[E].post ? g_lazy[E].post(s->real) : 1;
    std::lock_guard<std::recursive_mutex> l(g_lazyLock);
    g_standIns.erase(handle);
    delete s;
    return result;
}
template <int E>
static uintptr_t cancelWrapper(void* handle) {
    StandIn* s = standIn(handle);
    if (!s) return g_lazy[E].cancel ? g_lazy[E].cancel(handle) : 0;
    return s->real && g_lazy[E].cancel ? g_lazy[E].cancel(s->real) : 1;
}
// (Windows: 0x145e029e0 in 7.2.19, the same rows; tools/win/verify/stems.out.json)
static const Addr A_libraries{"__ZN6rbxfrm9audiopart16AudioPartLibrary9s_libraryE"};
static bool lazyModelWanted() { return opt::lazyModel.integer() != 0; }
// This file's functions for an engine: Initialize (0), PreOperation (1), the two OperationAtIndex (2, 3), and the
// stand-ins: Finalize (4), IsInitialized (5), PostOperation (6), CancelOperation (7).
static void* engineWrapper(int E, int which);

// ---- stems at the playhead at once ---------------------------------------------------------------------------------
//
// rekordbox separates a track in chunks of 11-12 s and a deck gets a chunk's stems when the whole chunk is done:
// with HTDemucs two runs of the model (0.5 s on an M2 Pro), after the chunk that was under way when the stems were
// asked for (as long again). But nothing in rekordbox needs whole chunks. Its separator writes each stem into a stream
// at the sample's place (AudioPartSeparator::OperationAtIndex: setPosition(sample * 8), write), keeps what is there
// as ranges (two SparseSets: samples and bytes) and hands those to the track's data (AudioPartDataManager::update),
// which is what the deck's reader asks (AudioPartData::isReadyPosition -> numContains) and what tells it to look
// again (its listeners; MultiPagingAudioSource::onAudioPartUpdateMessage).
// So when a deck needs stems where it plays and there are none yet, one window of the model at the playhead is
// separated and handed over the same way, at once: from inside the engine call that is under way (betterstems gives
// its caller a turn before each of its segments: "stemcache.betweenSegments"; also after a chunk, and when a
// separation that waited is asked for), on the separator's own thread, as everything rekordbox does with these
// streams and ranges. The window's audio is the engine call's own input when it lies in that chunk, else read with
// the separator's reader (getStreamAt: any position).
// The chunk the window lies in is separated as always afterwards (it has priority: separateAtPlayhead). What the
// deck may have played already must not change under it, so the window is kept and laid into the chunk's stems
// before they go to rekordbox and to the cache (chunkDone): the published samples as they were, and a short
// cross-fade to the chunk's own stems on each side, inside the part of the window that was not published.
// A window covers [w0, w0 + len) (len: one segment of the model, 7.8 s) inside one chunk's input. Published is
// [a, b): the window without half a second at each end (the model is least sure there, and the fades need room),
// except where the window starts or ends with the chunk's own input: there the chunk itself has no more to go on.
// macOS only so far (the separator's fields and functions below have no Windows table entries yet).
static std::string g_deckUri[4];          // the decks' tracks (tick; g_wantLock)
namespace {
struct OpContext {  // the engine call this thread is in
    void* handle = nullptr;
    int engine = 0, chunk = 0, stems = 0;
    const float *inL = nullptr, *inR = nullptr;  // its input (the chunk with its context)
    bool busy = false;                           // a window is being separated (no window inside a window)
    int segments = 0;                            // of the chunk's own, begun (betterstems' turns)
};
}  // namespace
static thread_local OpContext t_op;
const int kQuickMargin = 22050;  // samples of a window left out at each end
const int kQuickFade = 4096;     // the cross-fade to the chunk's own stems
// A deck reads stems in pages of a second, a page only when all of it is there, and looks for the next pages when
// its playhead enters one (below: "the deck's side"). So stems are wanted from over a page behind the playhead to
// two seconds ahead of it: the page it is in and the next one. (Further ahead the chunks themselves are in time:
// the playhead's and the next have priority, half a second each.)
const int kQuickPage = 48510;    // 1.1 s: a page, and what the deck's positions and the separator's may differ
const int kQuickBack = kQuickPage;
const int kQuickAhead = 88200;
const int kBufferAhead = 132300;  // what a deck is asked to buffer of its stems ahead of the playhead

// The first sample of [from, to) that has no stems: not in a chunk that is done, nor in a published window; -1 if
// all have (g_sepLock held).
static long long firstWithout(const Separation& s, long long chunkLen, long long from, long long to) {
    for (long long pos = from; pos < to;) {
        if (s.done.count((int)(pos / chunkLen))) {
            pos = (pos / chunkLen + 1) * chunkLen;
            continue;
        }
        long long next = -1;
        for (const auto& q : s.quicks)
            if (pos >= q->a && pos < q->b) next = q->b;
        if (next < 0) return pos;
        pos = next;
    }
    return -1;
}

// Where the stems are in one piece from, back from `at` (the first sample; g_sepLock held): as firstWithout forward.
static long long firstWith(const Separation& s, long long chunkLen, long long at) {
    for (long long pos = at; pos > 0;) {
        if (s.done.count((int)((pos - 1) / chunkLen))) {
            pos = (pos - 1) / chunkLen * chunkLen;
            continue;
        }
        long long before = -1;
        for (const auto& q : s.quicks)
            if (pos > q->a && pos <= q->b) before = q->a;
        if (before < 0) return pos;
        pos = before;
    }
    return 0;
}

#ifndef _WIN32
static const Addr A_separatorVt{"__ZTVN6rbxfrm9audiopart18AudioPartSeparatorE", 0x10};
static const Addr A_getStreamAt{"__ZN6rbxfrm9audiopart18AudioPartSeparator11getStreamAtEiPfS2_i"};
static const Addr A_dataUpdate{"__ZN6rbxfrm9audiopart20AudioPartDataManager6updateERKN4juce6StringENS0_5MODELENS2_9SparseSetIxEENS7_IjEE"};
static const Addr A_separatorUpdate{"__ZN6rbxfrm9audiopart25AudioPartSeparatorManager6updateERKN4juce6StringENS0_5MODELEPNS0_18AudioPartSeparatorE"};
static const Addr A_addSamples{"__ZN4juce9SparseSetIjE8addRangeENS_5RangeIjEE"};
static const Addr A_addBytes{"__ZN4juce9SparseSetIxE8addRangeENS_5RangeIxEE"};
#define RBM_SEP_PRE "__ZN6rbxfrm9audiopart18AudioPartSeparator12PreOperationEv"
#define RBM_SEP_OP "__ZN6rbxfrm9audiopart18AudioPartSeparator16OperationAtIndexEi"
// AudioPartSeparator's fields
static const Offset O_sepHandle{0x358, RBM_SEP_PRE, "add x19", 5};   // the engine's handle (what PreOperation is given the address of)
static const Offset O_sepUri{0x1e8, RBM_SEP_PRE, "add x19", 0};      // juce::String: its track
static const Offset O_sepModel{0x1f0, RBM_SEP_PRE, "ldr x19", 0};    // MODEL
static const Offset O_sepStems{0x1f4, RBM_SEP_PRE, "ldr x19", 1};    // 3 or 4
static const Offset O_sepStreams{0x2a0, RBM_SEP_PRE, "add x19", 1};  // juce::OutputStream* per stem, in the order of the engine's outputs
static const Offset O_sepTotal{0x208, RBM_SEP_PRE, "ldr x19", 3};    // the track's length in samples
static const Offset O_sepSamples{0x218, RBM_SEP_OP, "add x19", 1};   // juce::SparseSet<unsigned>: the samples that have stems
static const Offset O_sepBytes{0x228, RBM_SEP_OP, "add x19", 2};     // juce::SparseSet<long long>: the same in bytes (8 per sample)
// juce::OutputStream's slots, as OperationAtIndex calls them
static const Offset V_streamSetPosition{0x18, RBM_SEP_OP, "ldr x8", 6};
static const Offset V_streamWrite{0x28, RBM_SEP_OP, "ldr x8", 8};

// rekordbox's separator for the handle its engine's PreOperation is given the address of; null if that isn't one.
static char* separatorOf(void** handleAt) {
    char* sep = (char*)handleAt - O_sepHandle;
    return *(void**)sep == at<void*>(A_separatorVt) ? sep : nullptr;
}

// Hands rekordbox the stems of [a, b) of a window (separator thread): into the streams, into the separator's ranges,
// and from there to the track's data and its listeners, as AudioPartSeparator::run does after a chunk.
// Written is more than is announced: from a to the chunk's end, the window's own samples as far as it has any and
// silence after. A streaming track's stems are kept in memory, one array per stem that grows with what is written
// (AudioPartData::writeData), and a write that starts inside the array and ends beyond it is refused, which ends the
// whole separation (found with a Spotify track: the chunk's own write, after a window that ended short of it). With
// every write ending where a chunk ends, as rekordbox's own do, no write can lie across the array's end.
// (The buffer is made again for every stem: where memory is short rekordbox turns it into 16-bit samples in place.)
static bool publishQuick(char* sep, const Quick& q, int stems) {
    long long count = q.chunkEnd - q.a, have = std::min(count, q.w0 + q.len - q.a);
    if (count <= 0 || have < q.b - q.a) return false;
    std::vector<float> both((size_t)2 * count);
    for (int stem = 0; stem < stems; stem++) {
        void* stream = *(void**)(sep + O_sepStreams + 8 * stem);
        if (!stream) return false;
        const float* l = q.planes.data() + (size_t)(2 * stem) * q.len + (q.a - q.w0);
        const float* r = l + q.len;
        for (long long i = 0; i < have; i++) both[2 * i] = l[i], both[2 * i + 1] = r[i];
        std::fill(both.begin() + 2 * have, both.end(), 0.f);
        void** vt = *(void***)stream;
        if (!((bool (*)(void*, long long))vt[V_streamSetPosition / 8])(stream, q.a * 8)) return false;
        if (!((bool (*)(void*, const void*, size_t))vt[V_streamWrite / 8])(stream, both.data(), (size_t)count * 8)) return false;
    }
    // (juce::Range<unsigned> goes in one register, start low; Range<long long> in two)
    at<void (*)(void*, unsigned long long)>(A_addSamples)(sep + O_sepSamples, (unsigned long long)q.a | (unsigned long long)q.b << 32);
    at<void (*)(void*, long long, long long)>(A_addBytes)(sep + O_sepBytes, q.a * 8, q.b * 8);
    // update() takes both sets by value: copies, as the caller makes them (juce::Array: data, allocated, used).
    struct Array { void* data; int allocated, used; };
    auto copy = [](const Array& from, size_t each) {
        Array to{nullptr, from.used, from.used};
        if (from.used > 0 && (to.data = malloc(from.used * each))) memcpy(to.data, from.data, from.used * each);
        if (!to.data) to.allocated = to.used = 0;
        return to;
    };
    Array bytes = copy(*(Array*)(sep + O_sepBytes), 16), samples = copy(*(Array*)(sep + O_sepSamples), 8);
    const JStr& uri = *(const JStr*)(sep + O_sepUri);
    int model = *(int*)(sep + O_sepModel);
    at<void (*)(const JStr&, int, Array*, Array*)>(A_dataUpdate)(uri, model, &bytes, &samples);
    free(bytes.data), free(samples.data);
    at<void (*)(const JStr&, int, void*)>(A_separatorUpdate)(uri, model, sep);
    return true;
}

// The deck's side. Its audio source (StretchableAudioSource around a MultiPagingAudioSource: the mix and one child
// source per stem) keeps pages of a second buffered around the playhead. For a stem a page is read only when the
// track's data has all of it (AudioPartReader::isReadyToRead), and which pages to read is looked at when the playhead
// enters a new page, a second apart: stems that arrive in between would wait for that, and a page that wasn't whole
// then is played as the mix (measured with tools/dev/stemtest.mm's "reads": silent 0.1 to 1 s after the window was
// there, and a quarter second of the mix further on, at the page across a chunk's end).
// So the deck is told: IStereoAudioSource::ensureBufferedRange(range), which buffers the stems for the range now when
// they are all there, and else keeps the range until the track's data is updated (MultiPagingAudioSource's own
// handler of that update, on the message thread, as this call). Made for the stretch around the playhead whenever
// what is there of it has grown: after a window was published, and from the tick after a chunk.
static const Addr A_stateVt{"__ZTVN6rbxfrm17DjPlayerUnitStateE", 0x18};
static const Addr A_refAudioSource{"__ZNK6rbxfrm17DjPlayerUnitState14refAudioSourceEv"};
static const Addr A_dropReference{"__ZN6rbxfrm15RefCountedObjEx32decReferenceCountWithoutDeletingEv"};
static const Offset V_controlState{0x2b0, "__ZNK8djengine10DjEngineIF14getPlayingTimeENS_15EnPlayerChannelE", "ldr x8", 0};  // IDjPlayerControl: its state
static const Offset V_sourceEnsure{0xb8, "__ZN6rbxfrm22StretchableAudioSource19ensureBufferedRangeEN4juce5RangeIiEE", "ldr x8", 2};  // IStereoAudioSource
static const Offset V_sourceRate{0x78, "__ZNK6rbxfrm22StretchableAudioSource15getSamplingRateEv", "ldr x8", 2};
namespace {
// rbxfrm::RefCountedObjExPtr<IStereoAudioSource>, as refAudioSource returns it (a class with a destructor: through
// the hidden pointer, as rekordbox's).
struct SourceRef {
    void* source;
    SourceRef(const SourceRef&) = delete;
    ~SourceRef() {
        if (source && at<bool (*)(void*)>(A_dropReference)(source)) ((void (*)(void*))(*(void***)source)[1])(source);
    }
};
}  // namespace
// Main thread: every deck that needs its track's stems is asked to buffer what there is of them around its playhead,
// when that is more than at the last time.
static void buffersAtPlayhead() {
    static long long askedFrom[4], askedTo[4];
    static std::string askedUri[4];
    void* e = engine();
    void* sys = e ? at<void* (*)(void*)>(A_djSystem)(e) : nullptr;
    for (int d = 0; sys && d < deckCount() && d < 4; d++) {
        if (!g_deckNeeds[d]) {
            askedUri[d].clear();
            continue;
        }
        std::string uri;
        {
            std::lock_guard<std::mutex> l(g_wantLock);
            uri = g_deckUri[d];
        }
        long long playhead = (long long)playingMs(d) * 441 / 10, from = -1, to = -1, total = 0;
        {
            // (the track's separation: the latest one of it)
            std::lock_guard<std::mutex> l(g_sepLock);
            const Separation* s = nullptr;
            for (const auto& kv : g_seps)
                if (kv.second.uri == uri && kv.second.chunkLen > 0 && (!s || kv.second.serial > s->serial)) s = &kv.second;
            if (s && playhead < s->total && firstWithout(*s, s->chunkLen, playhead, playhead + 1) < 0) {
                total = s->total;
                from = firstWith(*s, s->chunkLen, playhead);
                to = firstWithout(*s, s->chunkLen, playhead, total);
                if (to < 0) to = total;
            }
        }
        if (from < 0) continue;
        // Whole pages only: a page of margin where the stems end short of the track's.
        long long lo = std::max(playhead - 2205, from > 0 ? from + kQuickPage : 0), hi = std::min(playhead + kBufferAhead, to < total ? to - kQuickPage : total);
        if (hi - lo < 22050) continue;
        if (uri == askedUri[d] && from == askedFrom[d] && to == askedTo[d]) continue;
        askedUri[d] = uri, askedFrom[d] = from, askedTo[d] = to;
        void* control = at<void* (*)(void*, int)>(A_playerControl)(sys, d);
        void* state = control ? ((void* (*)(void*))(*(void***)control)[V_controlState / 8])(control) : nullptr;
        if (!state || *(void**)state != at<void*>(A_stateVt)) continue;
        SourceRef ref = at<SourceRef (*)(void*)>(A_refAudioSource)(state);
        if (!ref.source) continue;
        void** vt = *(void***)ref.source;
        long long rate = ((int (*)(void*))vt[V_sourceRate / 8])(ref.source);
        if (rate <= 0) continue;
        unsigned long long range = (unsigned long long)(unsigned)(lo * rate / 44100) | (unsigned long long)(unsigned)(hi * rate / 44100) << 32;
        ((void (*)(void*, unsigned long long))vt[V_sourceEnsure / 8])(ref.source, range);
        if (g_logChunks) logf("stemcache: [%.3f] deck %d asked to buffer its stems for %.1f-%.1f s (there for %.1f-%.1f s)", wallClock(), d + 1, lo / 44100.0, hi / 44100.0, from / 44100.0, to / 44100.0);
    }
}

// What a deck's stem readers wait for. With the Spleeter models (the ones rekordbox calls random-accessible: the
// speed setting, its default) a page isn't looked at before it is read: AudioPartReader::isReadyToRead asks for the
// page's chunk and says yes, and fileRead then waits inside the call until the data is there. Each stem's buffering
// thread reads the pages around the playhead one after the other, so after a jump it is still inside the read of a
// page of the old place (one whose chunk hasn't been separated), and the new place's stems, there or not, aren't
// read until that returns (measured with stemtest's "reads": a window out 0.4 s after a jump, the deck silent
// 2.2 s after it, when the old page's chunk had had its turn). So the reads that are under way are kept here
// (AudioPartReader::fileRead's slot of its vtable), and a deck that needs stems gets a window for the page its
// readers were still waiting in from before its last jump as well (quickAtPlayhead). Only those: a read begun after the
// jump is of the new place, the readers working outwards from the playhead, and waits for chunks that are on their
// way (served like the first kind, they made the separation one window after another, as far as 20 s from the
// playhead).
static const Addr A_readerVt{"__ZTVN6rbxfrm9audiopart15AudioPartReaderE", 0x10};
static const Offset V_readerFileRead{0x20, "__ZTVN6rbxfrm9audiopart15AudioPartReaderE", "vt:__ZN6rbxfrm9audiopart15AudioPartReader8fileReadEiPNS_6Float2Ei", 0};
static const Offset O_readerSource{0x20, "__ZN6rbxfrm9audiopart15AudioPartReader13isReadyToReadEii", "ldr x20", 0};  // AudioPartReader: what it reads from
static const Offset O_sourceData{0x8, "__ZN6rbxfrm9audiopart15AudioPartReader13isReadyToReadEii", "ldr x8", 0};     // ... whose AudioPartData
static const Offset O_dataUri{0x10, "__ZNK6rbxfrm9audiopart13AudioPartData11numContainsExi", "add x0", 0};           // AudioPartData: its track (juce::String)
namespace {
struct Reading {  // a stem reader inside fileRead
    void* reader;
    std::string uri;
    long long pos;  // samples, as the separator's
    int count;
    double since;
};
}  // namespace
static std::mutex g_readingLock;
static std::vector<Reading> g_reading;
static int (*o_fileRead)(void*, int, void*, int);
static int fileRead(void* reader, int pos, void* to, int count) {  // (a stem's buffering thread)
    if (!g_quick || !stemsNeeded()) return o_fileRead(reader, pos, to, count);
    std::string uri;
    char* source = *(char**)((char*)reader + O_readerSource);
    char* data = source ? *(char**)(source + O_sourceData) : nullptr;
    if (data && ((const JStr*)(data + O_dataUri))->text) uri = ((const JStr*)(data + O_dataUri))->text;
    {
        std::lock_guard<std::mutex> l(g_readingLock);
        g_reading.push_back({reader, uri, pos, count, now()});
    }
    int got = o_fileRead(reader, pos, to, count);
    std::lock_guard<std::mutex> l(g_readingLock);
    for (size_t i = 0; i < g_reading.size(); i++)
        if (g_reading[i].reader == reader) {
            g_reading.erase(g_reading.begin() + i);
            break;
        }
    return got;
}
// Swapped in once (main thread, from the tick).
static void followStemReads() {
    static bool done;
    if (done) return;
    done = true;
    void** slot = at<void**>(A_readerVt) + V_readerFileRead / 8;
    o_fileRead = (int (*)(void*, int, void*, int))*slot;
    if (!o_fileRead || !patchSlot(slot, (void*)&fileRead)) logf("stemcache: the decks' stem reads can't be followed: after a jump the stems may take a second or two (speed setting)");
}
// The first sample without stems that a reader of this track is waiting for in a read it began before `before`
// (the deck's last jump), -1 if none (g_sepLock held).
static long long waitedFor(const Separation& s, const std::string& uri, double before) {
    std::lock_guard<std::mutex> l(g_readingLock);
    for (const Reading& r : g_reading) {
        if (r.uri != uri || r.since >= before) continue;
        long long at = firstWithout(s, s.chunkLen, std::max(0LL, r.pos), std::min(s.total, r.pos + r.count));
        if (at >= 0) return at;
    }
    return -1;
}

// On a separator's thread, inside its engine call: a deck that needs this track's stems where it plays, and has
// none there, gets one window of them now.
RBM_NOINLINE static void quickAtPlayhead() {
    OpContext& op = t_op;
    if (!g_quick || !op.handle || op.busy || !stemsNeeded() || !betterActive()) return;
    auto segment = service<int (*)()>("betterstems.segment");
    const Engine& e = g_engines[op.engine];
    const int len = segment ? segment() : 0;
    const long long chunkLen = e.outSamples, ctx = (e.inSamples - e.outSamples) / 2;
    if (len <= 4 * kQuickMargin || len > e.inSamples) return;
    void* thread = at<void* (*)()>(A_currentThread)();
    int skipped = 0;
    for (int round = 0; round < 3; round++) {  // (the playhead near a chunk's end: the next chunk's start as well; and where a reader waits)
        std::string uri;
        char* sep;
        long long total;
        {
            std::lock_guard<std::mutex> l(g_sepLock);
            auto it = g_seps.find(op.handle);
            if (it == g_seps.end() || !it->second.separator || now() - it->second.quickFailedAt < 2) return;
            uri = it->second.uri, sep = it->second.separator, total = it->second.total;
        }
        if (uri.empty() || *(int*)(sep + O_sepStems) != op.stems) return;
        // Where: the first sample without stems around the playhead of a deck that needs them; all there: the first
        // one a stem reader of the track waits for (see fileRead above).
        long long want = -1, playhead = 0;
        int deck = -1;
        bool forReader = false;
        for (int d = 0; d < 4 && want < 0; d++) {
            if (!g_deckNeeds[d]) continue;
            {
                std::lock_guard<std::mutex> l(g_wantLock);
                if (g_deckUri[d] != uri) continue;
            }
            playhead = (long long)playingMs(d) * 441 / 10;
            std::lock_guard<std::mutex> l(g_sepLock);
            auto it = g_seps.find(op.handle);
            if (it == g_seps.end()) return;
            want = firstWithout(it->second, chunkLen, std::max(0LL, playhead - kQuickBack), std::min(total, playhead + kQuickAhead));
            deck = d;
        }
        if (deck < 0) return;
        if (want < 0) {
            std::lock_guard<std::mutex> l(g_sepLock);
            auto it = g_seps.find(op.handle);
            if (it == g_seps.end()) return;
            double jumpedAt = g_jumpAt[deck];
            if (now() - jumpedAt > 10) return;
            want = waitedFor(it->second, uri, jumpedAt);
            forReader = true;
        }
        if (want < 0) return;
        // The window: half a second before that sample, kept inside the chunk's own input.
        auto q = std::make_shared<Quick>();
        long long chunk = want / chunkLen, lo = chunk * chunkLen - ctx, hi = (chunk + 1) * chunkLen + ctx - len;
        q->len = len;
        q->w0 = std::min(std::max(want - kQuickMargin, lo), hi);
        q->a = q->w0 == lo ? chunk * chunkLen : q->w0 + kQuickMargin;
        q->chunkEnd = std::min((chunk + 1) * chunkLen, total);
        q->b = std::min(q->w0 == hi ? (chunk + 1) * chunkLen : q->w0 + len - kQuickMargin, total);
        // (the chunk this call is separating, past its first segment: its own stems are as near as a window's)
        if (chunk == op.chunk && op.segments > 1) return;
        // Not what another window of this chunk has published already (a jump inside the chunk): this one ends or
        // starts where that one does, and is faded into it there before it goes out (below).
        std::shared_ptr<Quick> left, right;
        {
            std::lock_guard<std::mutex> l(g_sepLock);
            auto it = g_seps.find(op.handle);
            if (it == g_seps.end()) return;
            for (const auto& o : it->second.quicks) {
                if (o->a >= q->b || o->b <= q->a) continue;
                if (o->b <= want && o->b > q->a) q->a = o->b, left = o;
                else if (o->a > want && o->a < q->b) q->b = o->a, right = o;
            }
        }
        if (q->a > want || q->b <= want) return;
        auto failed = [&](const char* what) {
            std::lock_guard<std::mutex> l(g_sepLock);
            auto it = g_seps.find(op.handle);
            if (it != g_seps.end()) it->second.quickFailedAt = now();
            if (g_logChunks) logf("stemcache: no stems ahead of chunk %lld for deck %d (%s)", chunk, deck + 1, what);
        };
        // Its audio: this engine call's input, or read as the separator reads a chunk's.
        double began = now(), readTime = 0;
        std::vector<float> readL, readR;
        const float *inL, *inR;
        long long opLo = (long long)op.chunk * chunkLen - ctx;
        if (op.inL && q->w0 >= opLo && q->w0 + len <= opLo + e.inSamples) {
            inL = op.inL + (q->w0 - opLo), inR = op.inR + (q->w0 - opLo);
        } else {
            readL.resize(len), readR.resize(len);
            if (!at<bool (*)(void*, int, float*, float*, int)>(A_getStreamAt)(sep, (int)q->w0, readL.data(), readR.data(), len)) return failed("its audio couldn't be read");
            inL = readL.data(), inR = readR.data();
            readTime = now() - began;
        }
        // (the deck may have jumped on while that was read: then not this window, the one where it plays now)
        if (llabs((long long)playingMs(deck) * 441 / 10 - playhead) > 2 * 44100) {
            round--;
            if (++skipped > 3) return;
            continue;
        }
        // One run of the model, and its four sources in the engine's order (as separateBetter).
        std::vector<float> sources((size_t)8 * len);
        float* src[8];
        for (int i = 0; i < 8; i++) src[i] = sources.data() + (size_t)i * len;
        op.busy = true;
        bool ok = betterSeparate(inL, inR, len, src);
        op.busy = false;
        if (!ok) return failed("HTDemucs didn't separate it");
        if (thread && at<bool (*)(void*)>(A_threadShouldExit)(thread)) return;
        q->planes.assign((size_t)8 * len, 0.f);
        for (int c = 0; c < 2; c++) {
            const float *drums = src[c], *bass = src[2 + c], *other = src[4 + c], *vocals = src[6 + c];
            float* out = q->planes.data();
            for (int i = 0; i < len; i++) {
                out[(size_t)c * len + i] = other[i] + (op.stems == 3 ? bass[i] : 0);
                out[(size_t)(2 + c) * len + i] = vocals[i];
                out[(size_t)(4 + c) * len + i] = drums[i];
                out[(size_t)(6 + c) * len + i] = bass[i];
            }
        }
        // The seams with the windows beside it: over a fade's length this window becomes the other one, whose
        // samples are what the deck has there.
        auto seam = [&](const Quick& other, long long from, bool rising) {  // rising: this window's share grows
            for (int k = 0; k < kQuickFade; k++) {
                long long at = from + k, mine = at - q->w0, theirs = at - other.w0;
                if (mine < 0 || mine >= len || theirs < 0 || theirs >= other.len) continue;
                float w = (float)(k + 1) / (kQuickFade + 1);
                if (!rising) w = 1 - w;
                for (int plane = 0; plane < 8; plane++) {
                    float& x = q->planes[(size_t)plane * len + mine];
                    x = other.planes[(size_t)plane * other.len + theirs] + w * (x - other.planes[(size_t)plane * other.len + theirs]);
                }
            }
        };
        if (left) seam(*left, q->a, true);
        if (right) seam(*right, q->b - kQuickFade, false);
        if (!publishQuick(sep, *q, op.stems)) return failed("rekordbox's streams didn't take it");
        {
            std::lock_guard<std::mutex> l(g_sepLock);
            auto it = g_seps.find(op.handle);
            if (it != g_seps.end()) it->second.quicks.push_back(q);
        }
        g_quicks++;
        plat::onMain(buffersAtPlayhead);
        if (g_logChunks)
            logf("stemcache: [%.3f] deck %d plays at %.1f s: stems for %.1f-%.1f s ahead of chunk %lld, in %.2f s%s%s", wallClock(), deck + 1, playhead / 44100.0,
                 q->a / 44100.0, q->b / 44100.0, chunk, now() - began, readTime > 0 ? (" (" + std::to_string((int)(readTime * 1000)) + " ms reading)").c_str() : "",
                 forReader ? ": its stem readers waited there" : "");
    }
}
#else
static char* separatorOf(void**) { return nullptr; }
static void quickAtPlayhead() {}
static void buffersAtPlayhead() {}
static void followStemReads() {}
#endif

// A chunk's stems are there (out: the engine's outputs, n planes of e.outSamples): from now on the chunk counts as
// done, and what was published of it ahead stays as it was (see above).
static void chunkDone(void* handle, const Engine& e, int chunk, float* const* out, int n) {
    std::vector<std::shared_ptr<Quick>> mine;
    const long long chunkLen = e.outSamples, first = chunk * chunkLen;
    {
        std::lock_guard<std::mutex> l(g_sepLock);
        auto it = g_seps.find(handle);
        if (it == g_seps.end()) return;
        it->second.done.insert(chunk);
        auto& all = it->second.quicks;
        for (size_t i = 0; i < all.size();)
            if (all[i]->a / chunkLen == chunk) mine.push_back(all[i]), all.erase(all.begin() + i);
            else i++;
    }
    auto meets = [&](long long at, bool starts) {  // another window of the chunk starts (or ends) at this sample
        for (const auto& o : mine)
            if ((starts ? o->a : o->b) == at) return true;
        return false;
    };
    for (const auto& q : mine) {
        long long a = q->a - first, b = q->b - first, shift = first - q->w0;  // (a sample i of the chunk is the window's i + shift)
        if (g_logChunks) {
            // How far the window's stems were from the chunk's own, per stem, against the level of all stems together:
            // two runs of the model over the same music, so far below it (-25 dB and less); near 0 dB would be the
            // wrong place or the wrong stem.
            double diff[4] = {0, 0, 0, 0}, level = 0;
            for (int plane = 0; plane < n; plane++) {
                const float* quick = q->planes.data() + (size_t)plane * q->len;
                for (long long i = std::max(a, 0LL); i < b && i < chunkLen; i++) {
                    double own = out[plane][i], d = quick[i + shift] - own;
                    diff[plane / 2] += d * d, level += own * own;
                }
            }
            std::string said;
            for (int stem = 0; stem < n / 2; stem++) {
                char one[32];
                snprintf(one, sizeof one, "%s%.0f", stem ? ", " : "", level > 0 && diff[stem] > 0 ? 10 * log10(diff[stem] / level) : -99.0);
                said += one;
            }
            logf("stemcache: chunk %d: the window at %.1f-%.1f s was %s dB from the chunk's own stems (per stem, against all of them)", chunk, q->a / 44100.0, q->b / 44100.0, said.c_str());
        }
        for (int plane = 0; plane < n; plane++) {
            const float* quick = q->planes.data() + (size_t)plane * q->len;
            float* to = out[plane];
            auto mix = [&](long long i, float w) {  // w of the window's, the rest the chunk's own
                if (i >= 0 && i < chunkLen && i + shift >= 0 && i + shift < q->len) to[i] += w * (quick[i + shift] - to[i]);
            };
            // (no fade where another window of the chunk carries on: the two were made to meet there)
            if (a > 0 && !meets(q->a, false))
                for (int k = 0; k < kQuickFade; k++) mix(a - kQuickFade + k, (float)(k + 1) / (kQuickFade + 1));
            for (long long i = a; i < b; i++) mix(i, 1);
            if (b < chunkLen && !meets(q->b, true))
                for (int k = 0; k < kQuickFade; k++) mix(b + k, 1 - (float)(k + 1) / (kQuickFade + 1));
        }
    }
}

template <int E, int K>
static bool opWrapper(void* handle, int chunk, float* inL, float* inR, float* o0L, float* o0R, float* o1L, float* o1R,
                      float* o2L, float* o2R, float* o3L, float* o3R) {
    Engine& e = g_engines[E];
    bool better = betterActive();
    if ((!g_on && !better) || !inL || !inR || !o0L) return engineOp(E, K, handle, chunk, inL, inR, o0L, o0R, o1L, o1R, o2L, o2R, o3L, o3R);
    g_chunksStarted++;
    g_lastOpAt = now();
    noteChunk(handle, false, true);
    // K 0: OperationAtIndexOtherWithBass (3 stems; o3L/o3R are stale stack, not arguments), 1: SeparatedBass (4).
    if (K == 0) o3L = o3R = nullptr;
    float* out[8] = {o0L, o0R, o1L, o1R, o2L, o2R, o3L, o3R};
    int n = K == 1 ? 8 : 6, stems = n / 2;
    // (for the windows at a deck's playhead, which are separated from inside this call: quickAtPlayhead)
    struct InCall {
        InCall(void* handle, int engine, int chunk, int stems, const float* l, const float* r) { t_op = OpContext{handle, engine, chunk, stems, l, r, false}; }
        ~InCall() { t_op = OpContext{}; }
    } inCall(handle, E, chunk, stems, inL, inR);
    std::string model = better ? betterVersion() : e.version;
    // A Spotify track's chunks (or a separation's whose track isn't known) are kept in memory, never on disk.
    bool inMemory = g_on && separationSpotify(handle) != 0;
    // And those of a track whose stems aren't kept (stemcache_keep).
    if (g_on && !inMemory && g_keep != kKeepAll) {
        std::string uri = separationUri(handle);
        inMemory = g_keep == kKeepNone || !(isRunTrack(uri) || wasSeparated(uri));
    }
    bool cached = g_on && dirReady();  // (a folder that isn't there or doesn't answer: separated as without the cache)
    std::string path = cached || inMemory ? chunkPath16(e, model, stems, inL, inR) : "";
    if ((inMemory && memLoad(relOf(path), e.outSamples, out, n)) ||
        (cached && (load(path, e.outSamples, out, n) || load(chunkPath(e, model, stems, inL, inR), e.outSamples, out, n)))) {
        g_hits++;
        noteChunk(handle, true, false);
        if (g_logChunks) logf("stemcache: chunk %d from the cache", chunk);
        chunkDone(handle, e, chunk, out, n);
        return true;
    }
    double began = now();
    bool viaBetter = better && !g_ownEngine && separateBetter(e, inL, inR, out, n);
    if (!viaBetter && !engineOp(E, K, handle, chunk, inL, inR, o0L, o0R, o1L, o1R, o2L, o2R, o3L, o3R)) return false;
    if (g_logChunks) logf("stemcache: [%.3f] chunk %d separated in %.2f s (%s)", wallClock(), chunk, now() - began, viaBetter ? "HTDemucs" : e.prefix);
    g_misses++;
    chunkDone(handle, e, chunk, out, n);
    if (viaBetter || !better) {  // (not rekordbox's stems under betterstems' key)
        if (inMemory) memStore(relOf(path), e.outSamples, out, n);
        else if (cached) store(path, e.outSamples, out, n);
    }
    waitUntilWanted(handle);
    quickAtPlayhead();  // (before the next chunk, whichever that is; and at once when a separation that waited is asked for)
    return true;
}

// rekordbox initialises engine E: whether that is held back (g_lazyLock held). wanted: it should be, if it isn't yet.
// The engine's row of the table is found by where this file's Initialize stands, with PreOperation three places on
// and the two OperationAtIndex after it, as wrap() put them there. rekordbox fills the table again before every
// Initialize (it initialises an engine twice when it starts, for its 3-stem and its 4-stem model), so the other
// four are swapped every time; the engine's own are the ones that aren't this file's.
RBM_NOINLINE static bool holdEngine(int E, bool wanted, const char* modelDir, int a, int b, bool bigMemory, int threads, int c) {
    LazyEngine& z = g_lazy[E];
    if (!z.held && !wanted) return false;
    void** table = at<void**>(A_libraries);
    static const int kPlace[4] = {1, 2, 4, 5};
    bool found = false;
    for (int i = 0; i < 24 && !found; i++) {
        if (table[i] != engineWrapper(E, 0) || table[i + 3] != engineWrapper(E, 1) || table[i + 6] != engineWrapper(E, 2) || table[i + 7] != engineWrapper(E, 3)) continue;
        if (!table[i + 1] || !table[i + 2] || !table[i + 4] || !table[i + 5]) break;
        void* own[4];
        for (int k = 0; k < 4; k++) own[k] = table[i + kPlace[k]];
        if (own[0] != engineWrapper(E, 4)) z.finalize = (uintptr_t (*)())own[0];
        if (own[1] != engineWrapper(E, 5)) z.isInitialized = (bool (*)())own[1];
        if (own[2] != engineWrapper(E, 6)) z.post = (uintptr_t (*)(void*))own[2];
        if (own[3] != engineWrapper(E, 7)) z.cancel = (uintptr_t (*)(void*))own[3];
        for (int k = 0; k < 4; k++) table[i + kPlace[k]] = engineWrapper(E, 4 + k);
        found = true;
    }
    if (found) {
        if (!z.held) {
            z.held = true, z.real = false;
            z.modelDir = modelDir ? modelDir : "", z.a = a, z.b = b, z.bigMemory = bigMemory, z.threads = threads, z.c = c;
            logf("stemcache: rekordbox's %s model isn't loaded while HTDemucs separates (stemcache_lazy_model)", g_engines[E].prefix);
        }
        return !z.real;  // (loaded since, and rekordbox initialises it once more: as it would have)
    }
    if (z.held) {  // (the row was there before: it can't be gone)
        logf("stemcache: rekordbox's table of engine functions changed: its %s model is loaded now", g_engines[E].prefix);
        z.held = z.real = false;
    } else {
        logf("stemcache: rekordbox's table of engine functions isn't as expected: its %s model is loaded as always", g_engines[E].prefix);
    }
    return false;
}

template <int E>
static int initWrapper(const char* modelDir, int a, int b, bool bigMemory, int threads, int c) {
    std::string v = readFile(std::string(modelDir ? modelDir : "") + "/separator_ver.txt");
    v.erase(std::remove_if(v.begin(), v.end(), [](char ch) { return !isalnum((unsigned char)ch); }), v.end());
    g_engines[E].version = v.empty() ? "0" : v;
    logf("stemcache: %s model %s", g_engines[E].prefix, g_engines[E].version.c_str());
    // Held back while HTDemucs is what separates (see "rekordbox's own model only when it is needed").
    std::lock_guard<std::recursive_mutex> l(g_lazyLock);
    if (holdEngine(E, lazyModelWanted() && betterExpected(), modelDir, a, b, bigMemory, threads, c)) return 0;
    return g_engines[E].init(modelDir, a, b, bigMemory, threads, c);
}

template <int E>
static bool preWrapper(void** handle, unsigned samples, bool parallel) {
    const Engine& e = g_engines[E];
    g_trackChunks = (int)((samples + e.outSamples - 1) / e.outSamples);
    bool ok, better = betterActive();  // (before the lock: this may be what loads HTDemucs)
    {
        // (the engine's model not loaded, and HTDemucs there to separate: a handle of our own; else the engine's)
        std::lock_guard<std::recursive_mutex> l(g_lazyLock);
        bool held = g_lazy[E].held && !g_lazy[E].real;
        if (held && handle && better) {
            *handle = new StandIn{E, samples, parallel};
            g_standIns.insert(*handle);
            ok = true;
        } else {
            if (held) engineNeeded(E);
            ok = e.pre(handle, samples, parallel);
        }
    }
    if (ok && handle && *handle) noteSeparation(*handle, g_trackChunks, handleUri(handle), separatorOf(handle), samples, e.outSamples);
    return ok;
}

static const PreFn kPres[2] = {preWrapper<0>, preWrapper<1>};
static const OpFn kOps[2][2] = {{opWrapper<0, 0>, opWrapper<0, 1>}, {opWrapper<1, 0>, opWrapper<1, 1>}};
static const InitFn kInits[2] = {initWrapper<0>, initWrapper<1>};
RBM_NOINLINE static void* engineWrapper(int E, int which) {
    static void* const kStandIns[2][4] = {
        {(void*)(uintptr_t (*)())&finalizeWrapper<0>, (void*)(bool (*)())&isInitializedWrapper<0>, (void*)(uintptr_t (*)(void*))&postWrapper<0>, (void*)(uintptr_t (*)(void*))&cancelWrapper<0>},
        {(void*)(uintptr_t (*)())&finalizeWrapper<1>, (void*)(bool (*)())&isInitializedWrapper<1>, (void*)(uintptr_t (*)(void*))&postWrapper<1>, (void*)(uintptr_t (*)(void*))&cancelWrapper<1>}};
    return which == 0 ? (void*)kInits[E] : which == 1 ? (void*)kPres[E] : which < 4 ? (void*)kOps[E][which - 2] : kStandIns[E][which - 4];
}

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
    double maxGb = opt::maxGb.number();
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
    bool hold = g_staging && (stagingFull() || (holding && g_stagedBytes > opt::stagingGb.number() * 0.8e9));
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
        {
            std::lock_guard<std::mutex> l(g_wantLock);
            g_runUri = it.uri;  // (before its separation starts: its chunks are the run's from the first one)
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
    std::string d = opt::dir.text();
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
        parkIfQuitting();
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
            std::string mode = opt::staging.text();
            bool staging = mode == "0" ? false : mode == "1" ? true : judged == d ? g_external.load() : g_staging.load();
            if (staging) {
                std::error_code ec;
                std::filesystem::create_directories(plat::fsPath(g_stage), ec);
                uint64_t stageFree = plat::diskFree(g_stage);
                g_stageFull = stageFree && (double)stageFree < opt::minFreeGb.number() * 1e9;
            }
            if (g_staging.exchange(staging) != staging)
                logf(staging ? "stemcache: new stems go to %s first and move to the cache folder in the background (%s)"
                             : "stemcache: new stems go straight into the cache folder%.0s (%s)",
                     g_stage.c_str(), mode == "auto" ? (staging ? "the cache folder is on an external drive" : "the cache folder is on an internal disk")
                                                     : ("stemcache_staging=" + mode).c_str());
        }
        syncSeparated(d, !gone && fi.isDir);
        bool full = gone || (freeBytes && (double)freeBytes < opt::minFreeGb.number() * 1e9);
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
    g_spotify = opt::spotify.integer() != 0;
    g_onDemand = opt::ondemand.integer();
    g_quick = opt::quick.integer() != 0;
    followStemControls();
    g_logChunks = opt::log.integer() != 0;
    {
        std::string keep = opt::keep.text();
        g_keep = keep == "chosen" ? kKeepChosen : keep == "none" ? kKeepNone : kKeepAll;
    }
    {
        // A track that left the decks has to be asked for again the next time; the run's track is always separated.
        std::string decks[4];
        for (int d = 0; d < 4; d++) decks[d] = deckPath(d);
        for (int d = 0; d < 4; d++)  // a control that is on already (left on across a load)
            if (g_onDemand && g_stemState[d] && !decks[d].empty()) wantStems(decks[d], "a stem control is on");
        // Keeps the playhead's chunks first while a deck needs its stems (see separateAtPlayhead): at once when the
        // playhead jumped (a seek, a hot cue, a loop going back), else twice a second.
        // Every deck with a track, whether its stems are in use or not: then they are there when a stem control is
        // used. (With stemcache_ondemand only a track that was asked for: the others aren't separated at all.)
        static double askedAt[4], lastAt[4];
        static int lastMs[4];
        for (int d = 0; d < 4; d++) {
            bool loaded = d < deckCount() && !decks[d].empty();
            bool needs = loaded && (g_stemState[d] || (g_onDemand && isWanted(decks[d])));
            g_deckNeeds[d] = needs;
            if (!loaded || (g_onDemand && !needs)) continue;
            int ms = playingMs(d);
            // (a deck plays at most 2x: 0.1 s apart that is 200 ms, never 1.5 s)
            bool jumped = lastAt[d] > 0 && std::abs(ms - lastMs[d]) > 1500 + (t - lastAt[d]) * 2000;
            if (jumped) g_jumpAt[d] = t;
            if (jumped || t - askedAt[d] >= 0.5) {
                askedAt[d] = t;
                separateAtPlayhead(d, decks[d], ms);
                if (jumped && g_logChunks) logf("stemcache: [%.3f] deck %d jumped to %.1f s: that chunk first", wallClock(), d + 1, ms / 1000.0);
            }
            lastMs[d] = ms, lastAt[d] = t;
        }
        {
            std::lock_guard<std::mutex> l(g_wantLock);
            for (int d = 0; d < 4; d++)
                if (g_deckUri[d] != decks[d]) g_deckUri[d] = decks[d];
        }
        if (g_quick) followStemReads(), buffersAtPlayhead();
        std::lock_guard<std::mutex> l(g_wantLock);
        g_runUri = g_prep.running && g_prep.cur >= 0 && g_prep.cur < (int)g_prep.items.size() ? g_prep.items[g_prep.cur].uri : "";
        for (auto it = g_wanted.begin(); it != g_wanted.end();)
            it = std::find(decks, decks + 4, *it) == decks + 4 ? g_wanted.erase(it) : std::next(it);
    }
    // "open [playlist id]" (the window, on that playlist or selection), "separate <playlist id>" (as its Separate stems button), "stop"
    std::string cmd = devCommand("stemcache");
    if (cmd.rfind("open", 0) == 0) stemcache::showWindowFor(cmd.size() > 5 ? cmd.substr(5) : "");
    else if (cmd.rfind("separate ", 0) == 0) stemcache::separatePlaylist(cmd.substr(9));
    else if (cmd == "stop") stemcache::stop();
    else if (cmd.rfind("want ", 0) == 0) wantStems(deckPath(atoi(cmd.c_str() + 5) - 1), "command");  // "want <deck 1-4>"
    else if (cmd.rfind("ownengine ", 0) == 0) g_ownEngine = atoi(cmd.c_str() + 10) != 0;  // "ownengine <0|1>"
    else if (cmd.rfind("separateat ", 0) == 0) {  // "separateat <deck 1-4> <seconds>": the chunk there next
        int d = atoi(cmd.c_str() + 11) - 1;
        size_t sp = cmd.find(' ', 11);
        double sec = sp == std::string::npos ? 0 : atof(cmd.c_str() + sp + 1);
        if (d >= 0 && d < 4) separateAtPlayhead(d, deckPath(d), (int)(sec * 1000)), logf("stemcache: deck %d: chunks at %.0f s asked for first", d + 1, sec);
    }
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
                waits += "; in memory: " + std::to_string(g_mem.size()) + " chunks, " + std::to_string(g_memBytes / 1000000) +
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
RBM_PROVIDE("stemcache", "stemcache.stemsNeeded", stemsNeeded, &g_on);
// betterstems' turn before each of its segments (not the window's own: that is one segment, inside quickAtPlayhead).
static void betweenSegments() {
    if (t_op.busy) return;
    t_op.segments++;
    quickAtPlayhead();
}
RBM_PROVIDE("stemcache", "stemcache.betweenSegments", betweenSegments, &g_on);
// ...the same for a Spotify track's chunk: in memory, never on disk.
static void provideStoreSpotify(const float* inL, const float* inR, int inSamples, float* const out[8]) {
    const Engine& e = g_engines[0];
    if (!g_on || !inL || !inR || inSamples != e.inSamples || !betterActive()) return;
    memStore(relOf(chunkPath16(e, betterVersion(), 4, inL, inR)), e.outSamples, out, 8);
}
RBM_PROVIDE("stemcache", "stemcache.storeSpotify", provideStoreSpotify, &g_on);

// The window's sidebar area this mod is shown in (core/settings.h: the first declaration of an area counts).
namespace { const rbm::settings::Area kArea{"stems", "Stems",
    "How stems are separated and which ones are kept.",
    "<path d=\"M3 13V8M6.3 13V3M9.7 13V6M13 13V10\"/>", 20}; }

RBM_MOD({.name = "stemcache", .init = init, .tick = tick, .playlistMenu = playlistMenu, .trackMenu = trackMenu, .enabled = &g_on, .menuOrder = 30,
         .title = "Stems that are kept",
         .about = "Keeps separated stems, so a track has them at once the next time. A playlist's menu has Open stems window to separate a whole playlist ahead.",
         .area = "stems", .order = 10});
