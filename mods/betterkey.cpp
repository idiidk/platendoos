// betterkey: musical key detection with a CNN instead of rekordbox's built-in (DSP) key detector.
//
// When rekordbox analyses a track, this decodes the track again at full quality with rekordbox's own
// reader (so it works for streaming tracks too), off the analysis thread (from the start of the analysis
// for tracks the analyzer queued, else from the key stage), runs the key CNN of
// Korzeniowski & Widmer (ISMIR 2018, from madmom; see engines/keycnn.h) on it, and then, when rekordbox's
// key job asks for the key, either reports rekordbox's answer (compare mode) or the CNN's (replace).
// Every analysed track is logged with both keys, and appended to rekordbox-betterkey.csv next to the log.
// The "CNN Key" column (id 56) shows the CNN's key and its probability for every track it has seen
// (results are reloaded from the CSV at launch); other mods read them as track notes cnn_key/cnn_prob.
//   config: betterkey=0 to disable
//           betterkey_mode=compare   compare: keep rekordbox's key, log both; replace: use the CNN's key
//           betterkey_min_conf=0     replace only when the CNN's probability is at least this (0..1)
//           betterkey_title=CNN Key  header name of the column
//           betterkey_wait=120       seconds the key stage waits for the CNN's result before it keeps rekordbox's key
//           betterkey_passcheck=0    1: log how the audio pass's samples differ from a decode (development; read by core/analysis.cpp)
#include "../core/rbmod.h"
#include "../engines/keycnn.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <atomic>
#include <chrono>
#include <cmath>
#include <thread>
#include <map>
#include <functional>
#include <future>
#include <mutex>
#include <string_view>
#include <unordered_map>
#include <thread>
#include <vector>

using namespace rbm;

// Its settings (core/settings.h): what the settings window and the config file's list show of them.
namespace opt {
const Setting mode{"betterkey_mode", "compare", {.label = "Key to use", .choices = {{"compare", "rekordbox's (compare only)"}, {"replace", "The network's"}}}};
const Setting minConf{"betterkey_min_conf", 0, {.label = "Only when the network is at least this sure", .help = "0 to 1. 0: always.", .min = 0, .max = 1, .step = 0.05, .needs = "betterkey_mode=replace"}};
const Setting title{"betterkey_title", "CNN Key", {.label = "Title of the key column"}};
const Setting wait{"betterkey_wait", 120, {.label = "Wait for the network at most", .unit = "s", .min = 0, .max = 600, .step = 5, .advanced = true}};
}  // namespace opt

static const Offset O_jobSpecialInfo{0x60, "__ZN8analyzer13AnalyzeJobKey6DetectEv", "ldr x19", 2};  // AnalyzeJobKey: AnalyzeJobKeySpecialInfo*

static volatile bool g_on = true;
static std::atomic<bool> g_replace{false};
static std::atomic<float> g_minConf{0.f};

namespace {
struct Result { int key = -1; float prob = 0; std::string path; double seconds = 0; };
}  // namespace
static std::mutex g_lock;
static std::map<const void*, std::shared_future<Result>> g_results;  // key job's special info -> CNN result

static const int kColumnId = 56;  // unused by rekordbox

static std::string csvPath() {
    return plat::utf8(plat::fsPath(plat::logFile()).parent_path()) + "/rekordbox-betterkey.csv";
}

// Camelot sort rank per track path, kept next to the note: the column's sort calls compare O(n log n) times,
// so a comparison is one hash lookup per side (no string copies, no note-map walk). Only this mod writes cnn_key.
namespace {
struct SvHash { using is_transparent = void; size_t operator()(std::string_view s) const { return std::hash<std::string_view>()(s); } };
}  // namespace
static std::mutex g_rankLock;
static std::unordered_map<std::string, int, SvHash, std::equal_to<>> g_rank;

// A rank that changes during a sort makes the comparator inconsistent (rekordbox's sort can then run past the list).
// rekordbox sorts on the main thread, so new ranks go into the map there, between sorts; the notes (text, other mods)
// are set at once.
static void putRanks(std::vector<std::pair<std::string, int>> v) {
    auto put = [v = std::move(v)] {
        std::lock_guard<std::mutex> l(g_rankLock);
        for (auto& [uri, rank] : v) g_rank[uri] = rank;
    };
    if (plat::isMainThread()) put();
    else plat::onMain(std::move(put));
}

static void note(const std::string& uri, int cnn, float prob, const std::string& rb, std::vector<std::pair<std::string, int>>* batch = nullptr) {
    setTrackNote(uri, "cnn_key", std::to_string(cnn));
    int rank = camelotRank((9 + cnn % 12) % 12, cnn >= 12);
    if (batch) batch->emplace_back(uri, rank);
    else putRanks({{uri, rank}});
    char p[16];
    snprintf(p, sizeof p, "%.3f", prob);
    setTrackNote(uri, "cnn_prob", p);
    setTrackNote(uri, "rb_key", rb);
}

static int keyIndex(const char* name) {
    for (int k = 0; k < 24; k++)
        if (!strcmp(name, keycnn::kKeyNames[k])) return k;
    return -1;
}

// Earlier results, from the CSV (later lines win).
static void loadResults() {
    FILE* f = plat::openFile(csvPath(), "r");
    if (!f) return;
    char line[4096];
    int n = 0;
    std::vector<std::pair<std::string, int>> ranks;  // (one trip to the main thread)
    while (fgets(line, sizeof line, f)) {
        char rb[16], cnn[16];
        float prob;
        const char* q = strchr(line, '"');
        if (!q || sscanf(line, "%*[^,],%15[^,],%15[^,],%f,", rb, cnn, &prob) != 3) continue;
        std::string uri(q + 1);
        uri = uri.substr(0, uri.find('"'));
        int k = keyIndex(cnn);
        if (k >= 0 && !uri.empty()) note(uri, k, prob, rb, &ranks), n++;
    }
    fclose(f);
    putRanks(std::move(ranks));
    logf("betterkey: %d earlier results", n);
}

// CNN key in the notation of the track's own key ("8A" or "Am"), with its probability.
static std::string columnText(const char* track) {
    const char* uri = trackPath(track);
    std::string k = uri ? trackNote(uri, "cnn_key") : "";
    if (k.empty()) return "";
    int i = atoi(k.c_str());
    int pc = (9 + i % 12) % 12, own;  // index 0 = A
    bool minor = i >= 12, camelot = true, m;
    parseKey(trackKey(track), &own, &m, &camelot);
    char buf[48];
    snprintf(buf, sizeof buf, "%s  %.0f%%", formatKey(pc, minor, camelot).c_str(), 100 * atof(trackNote(uri, "cnn_prob").c_str()));
    return buf;
}

static int columnCompare(const char* a, const char* b) {
    const char* ua = trackPath(a);
    const char* ub = trackPath(b);
    int x = -1, y = -1;  // no result sorts first
    static std::atomic<bool> offMain{false};  // (putRanks counts on sorts running on the main thread)
    if (!plat::isMainThread() && !offMain.exchange(true)) logf("betterkey: CNN Key sorted off the main thread");
    {
        std::lock_guard<std::mutex> l(g_rankLock);
        if (ua) if (auto it = g_rank.find(std::string_view(ua)); it != g_rank.end()) x = it->second;
        if (ub) if (auto it = g_rank.find(std::string_view(ub)); it != g_rank.end()) y = it->second;
    }
    return x - y;
}

static std::string columnTitle() { return opt::title.text(); }

static keycnn::Model& model() {
    static keycnn::Model m;
    static std::once_flag once;
    std::call_once(once, [] {
        std::string res = plat::resourcesDir() + "/keycnn.bin", err;
        bool there = std::filesystem::exists(plat::fsPath(res));
        if (!there || !m.load(res, &err)) logf("betterkey: model not loaded: %s", there ? err.c_str() : "keycnn.bin missing");
        else logf("betterkey: model loaded");
    });
    return m;
}

typedef bool (*PassAudioFn)(const void** reader, const int16_t** l, const int16_t** r, size_t* n);

// Whether the beat job will get the audio pass's samples (then nothing needs decoding before it).
static bool passAudioComing() { return service<PassAudioFn>("streamio.passAudio") && setting("streamio_phrase").integer() != 0; }

// Decodes the song (analysisAudio) and runs the CNN. Takes its own copy of the path: the song source may go away if
// rekordbox cancels the analysis.
static std::function<Result()> cnnJob(const std::string& uri) {
    return [uri]() {
        double t0 = now();
        int rate = 0;
        auto mono = analysisAudio(uri, &rate);
        analysisAudioDone(uri);  // the key stage is the last to need it
        Result res;
        res.path = uri;
        if (!mono || !g_on || !model().loaded()) return res;
        std::vector<float> p = model().predict(mono->data(), mono->size(), rate);
        res.seconds = now() - t0;
        for (int k = 0; k < (int)p.size(); k++)
            if (p[k] > res.prob) { res.prob = p[k]; res.key = k; }
        if (res.key < 0) logf("betterkey: no result for %s (%zu samples at %d Hz)", res.path.c_str(), mono->size(), rate);
        return res;
    };
}

// Runs a job on its own thread. (Not std::async: its future blocks when dropped while still running.)
static std::shared_future<Result> runAsync(std::function<Result()> job) {
    auto p = std::make_shared<std::promise<Result>>();
    std::shared_future<Result> f = p->get_future().share();
    // (anything thrown on a thread of ours, or by starting it on rekordbox's analysis thread, would end rekordbox:
    // no CNN result then, and rekordbox's key stays)
    try {
        std::thread([p, job] {
            Result r;
            try {
                r = job();
            } catch (...) {
                logf("betterkey: the key detection failed, rekordbox's key kept");
                r = Result();
            }
            p->set_value(std::move(r));
        }).detach();
    } catch (...) {
        logf("betterkey: no thread for the key detection, rekordbox's key kept");
        p->set_value(Result());
    }
    return f;
}

// CNN runs started at the start of an analysis: analysis thread -> (song, result).
static std::map<const void*, std::pair<std::string, std::shared_future<Result>>> g_prefetched;

// Service "betterkey.prefetch" (called by the analyzer when an analysis it queued starts, on the analysis thread):
// the key CNN starts right away, next to rekordbox's audio and beat passes, instead of at the key stage.
static void keyPrefetch(const void* thread) {
    if (!g_on || !model().loaded()) return;
    if (passAudioComing()) return;  // the beat job brings the audio; the CNN (~0.1 s) then runs next to the key job
    std::string uri;
    if (!analysisSong(thread, &uri)) return;
    auto f = runAsync(cnnJob(uri));
    std::lock_guard<std::mutex> l(g_lock);
    g_prefetched[thread] = {uri, f};
}

// Service "betterkey.keyJob" (called by core's hook on the analysis thread, right before rekordbox's key job):
// starts (or takes over the prefetched) CNN run for it; rbm_hook_key_getKey waits for the result.
static void keyJob(const void* thread, const std::string& uri, void* info) {
    if (!model().loaded()) return;
    std::shared_future<Result> f;
    {
        std::lock_guard<std::mutex> l(g_lock);
        auto it = g_prefetched.find(thread);
        if (it != g_prefetched.end() && it->second.first == uri) f = it->second.second;
        if (it != g_prefetched.end()) g_prefetched.erase(it);
    }
    if (!f.valid()) f = runAsync(cnnJob(uri));  // runs next to rekordbox's key job
    std::lock_guard<std::mutex> l(g_lock);
    g_results[info] = f;
}
RBM_PROVIDE("betterkey", "betterkey.prefetch", keyPrefetch, &g_on);
RBM_PROVIDE("betterkey", "betterkey.keyJob", keyJob, &g_on);

static const char* relation(int a, int b) {
    if (a == b) return "same";
    if (a < 0 || b < 0) return "n/a";
    bool ma = a >= 12, mb = b >= 12;
    int ra = a % 12, rb = b % 12;
    if (ma != mb) {
        int major = ma ? rb : ra, minor = ma ? ra : rb;
        if ((major + 9) % 12 == minor) return "relative";
        if (major == minor) return "parallel";
    } else if ((ra + 7) % 12 == rb || (rb + 7) % 12 == ra) {
        return "fifth";
    }
    return "other";
}

// AnalyzeJobKey::getKey(beats, bpm, pcm, n, flag, a, b) -> key code.
extern "C" int rbm_hook_key_getKey(void* job, void* beats, float bpm, void* pcm, unsigned n, bool flag, unsigned a, unsigned b) {
    int rb = RBM_ORIG(key_getKey, int (*)(void*, void*, float, void*, unsigned, bool, unsigned, unsigned))(job, beats, bpm, pcm, n, flag, a, b);
    const void* info = *(void* const*)((const char*)job + O_jobSpecialInfo);
    std::shared_future<Result> f;
    {
        std::lock_guard<std::mutex> l(g_lock);
        auto it = g_results.find(info);
        if (it == g_results.end()) {
            logf("betterkey: key job finished without a CNN result (rekordbox=%d)", rb);
            return rb;
        }
        f = it->second;
        g_results.erase(it);
    }
    double waitFrom = now();
    // (the CNN takes a second or two; one that has to decode the track itself from a stream that stalled would hold
    // this analysis thread for good)
    double wait = opt::wait.number();
    wait = std::isfinite(wait) ? std::max(0.0, std::min(wait, 600.0)) : 120;
    if (f.wait_for(std::chrono::milliseconds((long long)(wait * 1000))) != std::future_status::ready) {
        logf("betterkey: no CNN result after %.0f s, rekordbox's key kept", wait);
        return rb;
    }
    Result res = f.get();
    double waited = now() - waitFrom;
    if (res.key < 0 || !g_on) return rb;  // (no result, or switched off meanwhile)
    bool use = g_replace && res.prob >= g_minConf;
    const char* rbName = rb >= 0 && rb < 24 ? keycnn::kKeyNames[rb] : "?";
    const char* rel = relation(rb, res.key);
    logf("betterkey: rekordbox=%s cnn=%s (%.2f) %s, %.1fs (waited %.1fs)%s  %s", rbName, keycnn::kKeyNames[res.key], res.prob,
         rel, res.seconds, waited, use ? ", using cnn" : "", res.path.c_str());
    note(res.path, res.key, res.prob, rbName);
    plat::onMain([] { refreshColumn(kColumnId); });
    static std::mutex csvLock;
    std::lock_guard<std::mutex> l(csvLock);
    std::string csv = csvPath();
    bool fresh = !std::filesystem::exists(plat::fsPath(csv));
    if (FILE* f = plat::openFile(csv, "a")) {
        // title/artist: noted by the analyzer for tracks it loads (Spotify may relink a track to another URI).
        auto clean = [](std::string s) { for (char& c : s) if (c == '"') c = '\''; return s; };
        if (fresh) fprintf(f, "time,rekordbox,cnn,cnn_prob,relation,used,path,title,artist\n");
        fprintf(f, "%.0f,%s,%s,%.3f,%s,%d,\"%s\",\"%s\",\"%s\"\n", now(), rbName, keycnn::kKeyNames[res.key], res.prob, rel, use,
                res.path.c_str(), clean(trackNote(res.path, "title")).c_str(), clean(trackNote(res.path, "artist")).c_str());
        fclose(f);
    }
    return use ? res.key : rb;
}

static void tick() {
    g_replace = opt::mode.text() == "replace";
    g_minConf = (float)opt::minConf.number();
}

static void init() {
    tick();
    loadResults();
    model();
}

RBM_COLUMN({.id = kColumnId, .title = columnTitle, .text = columnText, .compare = columnCompare, .after = 55, .enabled = &g_on});
RBM_MOD({.name = "betterkey", .init = init, .tick = tick, .enabled = &g_on,
         .title = "Keys",
         .about = "Detects keys with a neural network trained for it, which is right more often than rekordbox.",
         .area = "analysis", .order = 30});
