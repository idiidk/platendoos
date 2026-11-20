// analyzer: analyse a whole playlist from its context menu ("Analyze this playlist").
//
// The collection analyser refuses Spotify tracks (RowDataTrack::isAnalyzeRestrictionFile), but the
// function a deck load uses to queue an analysis, analyzeCollectionLoad, handles them itself. So by
// default this queues rekordbox's analysis of each row directly, without a deck. A Spotify track is streamed by the
// analysis itself, in memory.
// Rules:
//  - analyze_direct=0 falls back to loading rows onto spare decks from analyze_decks, a few in
//    parallel; a deck the DJ loads or plays is left alone, and Spotify's loaded-track limit applies.
//  - "Re-analyze track" / "Re-analyze N tracks" in the track list's context menu runs on just the selected
//    tracks, and "Open analysis window for N tracks" opens the window on them: a selection playlist (core's
//    selectionPlaylist), which the window lists and analyses like any other playlist.
//  - Already analysed tracks are skipped, unless "Re-analyze this playlist" was chosen: then rekordbox
//    is told the existing analysis is outdated (hook on isAnalyzedOldVer) and analyses it again.
//  - Deck loads don't count as played: their play-history entry is dropped (hook on
//    addTrackToPlayHistory), so they don't turn green.
//  - When Spotify says its streaming limit is reached (DOWNLOAD_ERROR_RATE_LIMITED) no new
//    Spotify track is started until its remainingSec is over; the refused tracks are queued again.
//  - If another list is opened meanwhile the run pauses, and resumes when the playlist is shown again.
//  - After a restart the playlist analysed last is opened again (switching the browser to Spotify etc.).
//   config: analyzer=0 to disable
//           analyze_direct=1       analyse without decks (0: load onto the decks below)
//           analyze_parallel=6     tracks analysed at once in direct mode (6 is fastest on a 10-core Mac)
//           analyze_live=1         tracks analysed at once while a deck is playing (0: the run waits for the decks to stop)
//           analyze_min_free_gb=5  stop a run when the disk has less free space than this (at most 1 unless
//                                  another mod writes more during a run: "analyzer.diskUse")
//           analyze_import=1       direct mode: import a Spotify playlist track that isn't in the collection
//                                  yet (like playing it does), since rekordbox only analyses collection
//                                  tracks; 0: such tracks fail
//           analyze_import_timeout=20  seconds to wait for an imported track to show up in the collection
//           analyze_decks=3,4      decks to use when analyze_direct=0 (1-4)
//           analyze_streams=3      max Spotify tracks loaded at once, counting all decks
//           analyze_gap=4          min seconds between two loads (spares the streaming service)
//           analyze_timeout=90     seconds before a track that never finishes is given up (180 on Windows)
//           analyze_start_timeout=15  seconds before a track whose analysis never starts is retried
//           analyze_window=1       open the analysis window when a run starts
//           analyze_reopen=1       open the playlist analysed last after a restart (analyze_playlist,
//                                  saved when a run starts)
//           analyze_fast=0         1: tracks this mod loads skip rekordbox's optional analyses (phrase,
//                                  vocal detection, similarity embedding); rekordbox adds those later,
//                                  when the track is loaded for playing
#include "../core/rbmod.h"
#include "../core/library.h"
#include "analyzer.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <algorithm>
#include <mutex>
#include <map>
#include <set>
#include <system_error>
#include <atomic>
#include <thread>

using namespace rbm;

static const Addr A_dbHolder{"SingletonHolder<DatabaseIF>", [] { return singleton("__ZN11DetailColorC2Ev", "__ZN4juce15SingletonHolderIN2db10DatabaseIFENS_15CriticalSectionELb0EE3getEv"); }};
static const Addr A_dbGet{"__ZN4juce15SingletonHolderIN2db10DatabaseIFENS_15CriticalSectionELb0EE3getEv"};  // SingletonHolder<db::DatabaseIF>::get()
static const Addr A_isCollectionSong{"__ZN2db10DatabaseIF16isCollectionSongERKN4juce6StringERNS_12RowDataTrackE"};  // DatabaseIF::isCollectionSong(String const&, RowDataTrack&)
static const Addr A_rowCtor{"__ZN2db12RowDataTrackC2Ev"};  // db::RowDataTrack::RowDataTrack()
static const Addr A_rowDtor{"__ZN2db12RowDataTrackD2Ev"};  // db::RowDataTrack::~RowDataTrack()
static const Addr A_analyzeLoad{"__ZN2db16DatabaseMediator21analyzeCollectionLoadERKNS_12RowDataTrackEb"};  // DatabaseMediator::analyzeCollectionLoad(RowDataTrack const&, bool)
static const Addr A_lvImportFromBridge{"__ZN6browse10ListViewer21importTrackFromBridgeERN2db12RowDataTrackE"};  // browse::ListViewer::importTrackFromBridge(RowDataTrack&)
static const Addr A_analyzerIF{"SingletonHolder<CAnalyzerIF>", [] {
    return singleton("__ZN2db16DatabaseMediatorC2Ev", "__ZN4juce15SingletonHolderIN8analyzer11CAnalyzerIFENS_15CriticalSectionELb0EE3getEv");
}};
static const Addr A_analyzerGet{"__ZN4juce15SingletonHolderIN8analyzer11CAnalyzerIFENS_15CriticalSectionELb0EE3getEv"};  // SingletonHolder<analyzer::CAnalyzerIF>::get()
static const Addr A_isRunning{"__ZN8analyzer11CAnalyzerIF9isRunningEl"};  // analyzer::CAnalyzerIF::isRunning(long)
static const Addr A_dataPathVptr{"__ZTV8DataPath", 0x10};  // DataPath's vtable pointer
static const Addr A_isDownloaded{"__ZNK8DataPath12IsDownloadedEv"};  // DataPath::IsDownloaded()
static const size_t    S_rowDataTrack    = 0x4e0;
static const Offset O_rowId{0x8, "__ZN2db16DatabaseMediator21analyzeCollectionLoadERKNS_12RowDataTrackEb", "ldr x20", 1};  // RowDataTrack: database id
static const Offset O_rowStatusLow{0x3fc, "__ZN2db21rekordboxDBController16getAnalyzeStatusEjRNS_14sAnalyzeStatusEPNS_12RowDataTrackE", "ldr x20", 2};  // RowDataTrack: analyse status, low byte (signed: < 0 = streaming data)
static const Offset O_dataPathString{0x10, "__ZNK8DataPath12IsDownloadedEv", "add x19", 0};  // DataPath: the path (juce::String)
static const Offset V_dataPathIsDownloaded{0xb8, "__ZTV8DataPath", "vt:__ZNK8DataPath12IsDownloadedEv", 0};  // DataPath vtable: IsDownloaded
static const Offset V_sourcePath{0x28, "__ZTVN8analyzer10DataSourceE", "vt:__ZNK8analyzer10DataSource7GetPathEv", 0};  // DataSource vtable: path (juce::String const&)
static const Offset O_threadId{0x258, "__ZN8analyzer18CBeatAnalyzeThread7analyzeEv", "ldr x20", 12};  // CBeatAnalyzeThread: track id
static const Offset O_threadInfo{0x2a0, "__ZN8analyzer18CBeatAnalyzeThread7analyzeEv", "ldr x20", 3};  // CBeatAnalyzeThread: track-info interface
static const Offset O_historyDeck{0x28, "__ZN6djplay8UiPlayer18PlayHistoryManagerC1Ei", "str x0", 1};  // UiPlayer::PlayHistoryManager: deck (ePlayerID)
static const Offset O_uiPlayerDeck{0x450, "__ZN6djplay8UiPlayer17onBeatGridChangedEPN6rbxfrm14IControlClientERKNS1_14IDjPlayerStateE", "ldr x0", 1};  // djplay::UiPlayer: deck (ePlayerID)

struct Slot {
    int ch;                  // deck, or -1 when analysing directly (no deck)
    int row = -1;            // row being analysed, -1 = idle
    unsigned id = 0;         // its database id (direct mode)
    std::string uri;         // its track URI
    bool force = false;      // re-analysing
    bool importing = false;  // waiting for the track to show up in the collection (direct mode)
    double importStartedAt = 0;
    bool seenRunning = false;
    double startedAt = 0, stableSince = 0, lastBpm = 0, checkedAt = 0;
    double clockFrom = 0;    // direct mode: where its start and analysis timeouts count from (see directFinished)
    double timingFrom = 0;   // when it was queued, by the clock: what the timing mod is asked from. Unlike startedAt
                             // it is not moved on after the Mac slept (the analysis' marks are from before the sleep:
                             // asked from a later time, a running analysis looked like one that never started)
    bool sawLoading = false;
    bool streams = false;    // streams the track from Spotify (direct mode: no local file of it)
    bool retrying = false;   // the row came from the retry list (stopped before its result: it goes back there)
};

using analyzer::State;

// Another mod may hold tracks back from a run for a while (rbmod.h, "analyzer.holdBack"; none of this tree does): the
// run goes on with the others and takes the held ones once it lets them go. gb: how much it waits for.
static bool holdingBack(double* gb) {
    auto f = service<bool (*)(const char*, double*)>("analyzer.holdBack");
    return f && f(nullptr, gb);
}
// Whether it holds this track back while it does.
static bool holdsBack(const std::string& uri) {
    auto f = service<bool (*)(const char*, double*)>("analyzer.holdBack");
    double gb = 0;
    return f && f(uri.c_str(), &gb);
}
// How much still has to go before it lets them go, what the run waits for, and what happens right now.
static void holdProgress(double* toGoMb, std::string* what, std::string* step) {
    auto f = service<void (*)(double*, std::string*, std::string*)>("analyzer.holdProgress");
    if (f) f(toGoMb, what, step);
}

// What the window shows per row.
struct TrackInfo {
    State state = State::Waiting;
    int deck = -1;
    double startedAt = 0, took = 0;
    std::string title, artist;
};

namespace {  // file-local: another mod has a type of the same name
struct Run {
    bool exists = false, running = false, waitingForList = false, paused = false, force = false, stopping = false;
    bool direct = false;       // analyse without decks (a track with a local audio file needs no stream)
    bool oldOnly = false;      // re-analyse only grids bettergrid never made (see oldGrid)
    bool selection = false;    // the playlist is a selection of tracks (core's selectionPlaylist): they are the run's
                               // rows, whatever else the list shows
    bool userPaused = false;   // "Pause" in the window: no new loads
    double holdWaitGb = 0;     // only held tracks are left, waiting for the mod holding them to let go (0: no wait)
    double holdToGoMb = 0, holdTotalMb = 0;  // of this wait: what has to go before the run continues, and what had to in all
    std::string holdWhat, holdStep;  // what the run waits for, and what happens about it right now
    std::string finishedWhy;
    std::string stopWhy;             // why the run stopped by itself, for the window ("" = the user stopped it)
    std::vector<TrackInfo> info;     // per row
    int lastRows = -1, next = 0, done = 0, skipped = 0, failed = 0, rows = 0;
    int limit = 0;                   // rows to go through (0: all)
    double lastActivity = 0;
    double startedAt = 0, stableSince = 0, lastLoadAt = 0, idleSince = 0, blockedLogAt = 0, stoppingSince = 0;
    std::vector<Slot> slots;
    std::vector<std::string> paths;  // row -> track URI, to notice when another list is shown
    std::vector<int> retry;          // rows to try once more at the end (e.g. hit the stream limit)
    std::set<int> retried;
    std::set<std::string> pathSet;   // paths, to add the tracks of rows as they load
    double collectedAt = 0;
    std::string playlistId;          // tree identifier of the playlist
    std::vector<int> needList;       // rows to import once the playlist is shown again
    bool shown = true;               // the browser shows the playlist
    std::vector<int> held;           // rows another mod holds back for now (holdsBack)
    bool retryOnly = false;          // "Retry failed" on a finished or stopped run: only the retry list, not the rest
    std::string resumeWhy;           // how that run had ended (what it ends as again, if rows are still left)
};
}  // namespace
static Run g_run;

static TrackInfo& info(int row) {
    if (row >= (int)g_run.info.size()) g_run.info.resize(row + 1);
    return g_run.info[row];
}
static void forgetKnown(int row);
static void setState(int row, State st) {
    if (row < 0) return;
    if (info(row).state != st) forgetKnown(row);  // its BPM, key and "analysed" are read again
    info(row).state = st;
}

// What this mod loaded per deck (deck path as the engine reports it), so its play history can be
// dropped. Read by the history hook, which runs on the main thread like everything else here.
static std::string g_owned[8];

// Track URIs to analyse again even though rekordbox has a current analysis.
static std::mutex g_forceLock;
static std::set<std::string> g_force;

// When rekordbox last reported a finished analysis per deck.
static double g_completedAt[8];

static volatile bool g_on = true;

// ---- helpers ------------------------------------------------------------------------------

// True if the track is in the collection and rekordbox has a current analysis for it.
static bool isAnalyzed(const std::string& uri) {
    CollectionTrack c;
    return collectionTrack(uri, &c) && c.analysed;
}

// The collection's view of tracks for the window (a database query each, on the main thread): looked up again every
// few seconds only for a track that is being analysed, and at once when its row's state changes; any other track
// once a minute (the window shows playlists of thousands, twice a second).
struct Known { CollectionTrack c; bool inCollection = false; double at = 0, freshUntil = 0; std::string genre; bool genreHeard = false; };
static std::map<std::string, Known> g_known;
static const Known& known(const std::string& uri, bool active = false);
static void forgetKnown(int row);
static const Known& known(const std::string& uri, bool active) {
    Known& k = g_known[uri];
    double t = now();
    if (t - k.at > (active || t < k.freshUntil ? 3 : 60)) {
        k.c = CollectionTrack();
        k.inCollection = collectionTrack(uri, &k.c);
        k.genre.clear(), k.genreHeard = false;
        bool written = false;
        auto genre = service<bool (*)(const char*, std::string*, bool*)>("genres.known");
        if (genre && genre(uri.c_str(), &k.genre, &written)) k.genreHeard = !written;
        k.at = t;
    }
    return k;
}
static void forgetKnown(int row) {
    if (row < 0 || row >= (int)g_run.paths.size()) return;
    Known& k = g_known[g_run.paths[row]];
    k.at = 0, k.freshUntil = now() + 25;  // (rekordbox saves a finished analysis a moment after its thread ends; the genres mod fills the Genre after that)
}

// Ids this mod queued directly: only those skip rekordbox's download wait (a deck load still waits).
static std::mutex g_directIdsLock;
static std::set<unsigned> g_directIds;
// Of those, ids a slot let go of before rekordbox had finished them (stopped, not started in time, timed out):
// rekordbox may still begin them from its queue, and without the skip a Spotify one waits for a download that never
// comes, holding one of its analysis threads. They stay direct until seen running and then ended, or for a while
// (main thread only).
namespace {
struct Leftover { double until; bool seen; };
}
static std::map<unsigned, Leftover> g_leftoverIds;
static bool isDirectId(unsigned id) {
    std::lock_guard<std::mutex> l(g_directIdsLock);
    return g_directIds.count(id) > 0;
}
static void setDirectId(unsigned id, bool on) {
    if (!id) return;
    std::lock_guard<std::mutex> l(g_directIdsLock);
    if (on) g_directIds.insert(id); else g_directIds.erase(id);
}

// Queue rekordbox's own analysis of a track, exactly like loading it onto a deck does, but without a
// deck: analyzeCollectionLoad is the function a deck load calls, and it handles the Spotify branch (and
// this mod's force hooks) itself. (A track with a local file, rbmod.h "trackfiles", is read from it, not the stream.)
// analyzeCollectionLoad only analyses tracks in the collection (execAnalyzeCollection looks the id up in
// refAllCollection). A Spotify playlist row comes from the streaming bridge instead, with a bridge id, so
// it is imported first, like "Import To Collection" (or loading it onto a deck) does; the caller then
// waits for the collection row and queues that.
enum class Queue { Queued, Importing, Failed };

// ---- Spotify's streaming limit ------------------------------------------------------------
// After ~30 new tracks in a minute Spotify refuses more for a while. DownloadConnectionJob::run then logs
//   Connection: DOWNLOAD_ERROR_RATE_LIMITED: <track id> remainingSec=44
// to Spotify.log, and an analysis started meanwhile gets no audio. The run starts no streamed track until the time
// is up (one with a local file still goes), and queues the refused tracks again without counting that as their one retry.
// The SP_LOG call is hooked (download thread); Spotify.log is the fallback when a build lacks the hook's site.
static std::mutex g_limitLock;
static std::atomic<time_t> g_limitUntil{0};  // Spotify refuses new tracks until then
static std::set<std::string> g_limitedUris;  // refused with the limit and not retried since (g_limitLock)

static void noteLimit(const std::string& id, time_t until) {
    time_t t = time(nullptr);
    if (until <= t || id.empty()) return;  // over already
    std::lock_guard<std::mutex> l(g_limitLock);
    g_limitedUris.insert("spotify:track:" + id);
    if (until > g_limitUntil) {
        if (g_limitUntil < t) logf("analyze: Spotify's streaming limit reached, no new Spotify track for %lds", (long)(until - t));
        g_limitUntil = until;
    }
}

// Forgets a refusal of uri; true if there was one.
static bool takeLimited(const std::string& uri) {
    std::lock_guard<std::mutex> l(g_limitLock);
    return g_limitedUris.erase(uri) != 0;
}

static void clearLimited() {
    std::lock_guard<std::mutex> l(g_limitLock);
    g_limitedUris.clear();
}

// Spotify.log is read only when the hook has no site in this build (a Windows build without its table entry).
static long g_logPos = -1;

static std::string spotifyLogPath() {
#ifdef _WIN32
    return plat::utf8(plat::fsPath(plat::configDir()).parent_path() / "Pioneer" / "rekordbox6" / "Spotify.log");
#else
    return plat::homeDir() + "/Library/Logs/Pioneer/rekordbox6/Spotify.log";
#endif
}

// "17 Nov 2026 5:21:20pm" (local time), 0 if the line doesn't start like that.
static time_t logLineTime(const char* s) {
    int d, y, h, m, sec;
    char mon[4] = "", ap[3] = "";
    if (sscanf(s, "%d %3s %d %d:%d:%d%2s", &d, mon, &y, &h, &m, &sec, ap) != 7) return 0;
    static const char months[] = "JanFebMarAprMayJunJulAugSepOctNovDec";
    const char* p = strstr(months, mon);
    if (!p || strlen(mon) != 3) return 0;
    struct tm tm = {};
    tm.tm_mday = d, tm.tm_mon = (int)(p - months) / 3, tm.tm_year = y - 1900;
    tm.tm_hour = h % 12 + (ap[0] == 'p' || ap[0] == 'P' ? 12 : 0), tm.tm_min = m, tm.tm_sec = sec, tm.tm_isdst = -1;
    return mktime(&tm);
}

// Reads what Spotify.log gained since the last call (at most once a second unless now).
static void readSpotifyLog(bool now_ = false) {
    if (hookPatched(HK_sp_rateLimited0)) return;  // the hook sees the refusals itself
    static double readAt;
    if (!now_ && now() - readAt < 1) return;
    readAt = now();
    FILE* f = plat::openFile(spotifyLogPath(), "rb");
    if (!f) return;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    // rekordbox trims the file by writing its end out again: start over a little before the end (lines seen
    // twice are harmless, their time is in them)
    if (g_logPos < 0 || size < g_logPos) g_logPos = std::max(0L, size - 16384);
    if (size - g_logPos > (1 << 20)) g_logPos = size - (1 << 20);
    std::string text;
    if (size > g_logPos) {
        text.resize(size - g_logPos);
        fseek(f, g_logPos, SEEK_SET);
        text.resize(fread(&text[0], 1, text.size(), f));
    }
    fclose(f);
    size_t end = text.rfind('\n');
    if (end == std::string::npos) return;  // no complete line yet
    g_logPos += (long)end + 1;
    for (size_t at = 0; at < end;) {
        size_t nl = text.find('\n', at);
        std::string line = text.substr(at, nl - at);
        at = nl + 1;
        static const char kTag[] = "DOWNLOAD_ERROR_RATE_LIMITED: ";
        size_t k = line.find(kTag), r = line.find("remainingSec=");
        if (k == std::string::npos || r == std::string::npos) continue;
        std::string id = line.substr(k + sizeof kTag - 1);
        id = id.substr(0, id.find(' '));
        time_t when = logLineTime(line.c_str());
        noteLimit(id, (when ? when : time(nullptr)) + atoi(line.c_str() + r + 13));
    }
}

// SP_LOG("Connection: DOWNLOAD_ERROR_RATE_LIMITED: ", trackId, " remainingSec=", seconds) in
// DownloadConnectionJob::run. The juce::String is passed by reference (non-trivial type). macOS has two call sites
// in run; MSVC moved them into one helper (called twice from run) whose SP_LOG copy has the strings built in and
// takes (?, ?, String*, ?, int seconds): one site, hook 0.
static void onRateLimited(const char* const* id, int sec) {
    if (g_on && id && *id) noteLimit(*id, time(nullptr) + sec);
}
#ifdef _WIN32
// windows-optional: sp_rateLimited0 sp_rateLimited1   (the Spotify.log reader stands in without them)
extern "C" void rbm_hook_sp_rateLimited0(void* a, void* b, const char* const* id, void* d, int sec) {
    onRateLimited(id, sec);
    RBM_ORIG(sp_rateLimited0, void (*)(void*, void*, const char* const*, void*, int))(a, b, id, d, sec);
}
#else
extern "C" void rbm_hook_sp_rateLimited0(const char* a, const char* const* id, const char* b, int sec) {
    onRateLimited(id, sec);
    RBM_ORIG(sp_rateLimited0, void (*)(const char*, const char* const*, const char*, int))(a, id, b, sec);
}
extern "C" void rbm_hook_sp_rateLimited1(const char* a, const char* const* id, const char* b, int sec) {
    onRateLimited(id, sec);
    RBM_ORIG(sp_rateLimited1, void (*)(const char*, const char* const*, const char*, int))(a, id, b, sec);
}
#endif

// Seconds until Spotify takes new tracks again (0: it does), with a few to spare.
static int spotifyLimitLeft() {
    long left = (long)(g_limitUntil + 3 - time(nullptr));
    return left > 0 ? (int)left : 0;
}

// A row that got no result because Spotify refused its stream: queued again after the limit, not counted as a try.
static bool requeueIfLimited(int row, const std::string& uri, bool streamed);

// Set while this mod queues a track itself (see rbm_hook_load_execAnalyze).
static thread_local bool t_batchQueue;

extern "C" void rbm_hook_load_execAnalyze(void* self, void* thread, unsigned* const* ids, int type, bool notAnalysed,
                                          bool oldVer, bool b6, bool b7);
static thread_local bool t_forcing;

static bool isLocalFile(const std::string& uri) {
    return !uri.empty() && (uri[0] == '/' || (uri.size() > 2 && uri[1] == ':' && (uri[2] == '/' || uri[2] == '\\')));
}

// A local file: queue execAnalyzeCollection like analyzeCollectionLoad's streaming branch does, not through its
// local-file branch. That branch skips an analysed track unless isAnalyzedOldVer (not hooked there), and runs
// analyzeCollection, a modal progress window on the main thread (it froze the Windows VM), whose thread then
// pauses the analyses already running. Here: a full analysis ("not analysed"), nothing paused.
static void queueLocalTrack(void* db, const std::string& uri, unsigned trackId) {
    bool force;
    {
        std::lock_guard<std::mutex> l(g_forceLock);
        force = g_force.erase(uri) > 0;
    }
    if (!force && isAnalyzed(uri)) return;  // nothing to do: the run sees it as already analysed
    if (force) logf("analyze: re-analysing %s", uri.c_str());
    struct { unsigned* data; int allocated, used; } ids = {&trackId, 1, 1};  // juce::Array<unsigned>
    t_forcing = force;
    // (thread none, BeatAnalyzeType 0, notAnalysed, oldVer, pause the others, b7) as the streaming branch passes
    rbm_hook_load_execAnalyze(db, nullptr, (unsigned* const*)&ids, 0, true, false, true, false);
    t_forcing = false;
}

static Queue queueCollectionTrack(const std::string& uri, unsigned* id) {
    void* dbif = at<void* (*)(uintptr_t)>(A_dbGet)(A_dbHolder + g_slide);
    // DatabaseIF holds the mediator in its first word (isCollectionSong is a thunk that loads it);
    // analyzeCollectionLoad wants the mediator itself.
    void* db = dbif ? *(void**)dbif : nullptr;
    if (!db || uri.empty()) return Queue::Failed;
    alignas(16) static char row[S_rowDataTrack + 0x100];
    at<void (*)(void*)>(A_rowCtor)(row);
    JStr path(uri.c_str());
    bool found = at<bool (*)(void*, const void*, void*)>(A_isCollectionSong)(dbif, &path, row);
    if (found) {
        unsigned trackId = *(const unsigned*)(row + O_rowId);
        if (id) *id = trackId;
        setDirectId(trackId, true);  // before queueing: the thread may start right away
        g_leftoverIds.erase(trackId);  // a slot holds it again
        t_batchQueue = true;
        if (isLocalFile(uri)) queueLocalTrack(db, uri, trackId);
        else at<void (*)(void*, const void*, bool)>(A_analyzeLoad)(db, row, false);
        t_batchQueue = false;
    }
    at<void (*)(void*)>(A_rowDtor)(row);
    return found ? Queue::Queued : Queue::Failed;
}

static Queue analyzeWithoutDeck(const std::string& uri, void* lv, void* listRowData, unsigned* id) {
    Queue q = queueCollectionTrack(uri, id);
    if (q != Queue::Failed || !isSpotify(uri.c_str()) || !cfgInt("analyze_import", 1)) return q;
    if (importTrack(uri)) {  // from the row core kept: works whatever list is shown
        logf("analyze: %s is not in the collection, importing it", uri.c_str());
        return Queue::Importing;
    }
    if (!lv || !listRowData) return q;
    // Returns without importing unless the list shows a streaming (or iTunes/rekordbox xml/cloud) browse.
    at<void (*)(void*, void*)>(A_lvImportFromBridge)(lv, listRowData);
    logf("analyze: %s is not in the collection, importing it", uri.c_str());
    return Queue::Importing;
}

// True while rekordbox's analyser has a running thread for that track id.
static bool analysisRunning(unsigned id) {
    void* a = at<void* (*)(uintptr_t)>(A_analyzerGet)(A_analyzerIF + g_slide);
    return a && id && at<bool (*)(void*, long)>(A_isRunning)(a, id);
}

// Content ids of the analyser's threads, for diagnostics (CAnalyzerIF: vector at +0x98, size at +0xa4).
static std::string runningIds() {
#ifdef _WIN32
    return "-";  // CAnalyzerIF's thread list: mac layout only (+0x98 / +0xa4), not verified for Windows
#else
    void* a = at<void* (*)(uintptr_t)>(A_analyzerGet)(A_analyzerIF + g_slide);
    if (!a || !testedBuild()) return "-";  // (a diagnostic only: its two offsets are 7.2.19's)
    void** arr = *(void***)((char*)a + 0x98);
    int n = *(int*)((char*)a + 0xa4);
    std::string s;
    for (int i = 0; i < n && i < 64; i++) {
        void* th = arr ? arr[i] : nullptr;
        if (!th) continue;
        if (!s.empty()) s += ",";
        s += std::to_string(*(unsigned*)((char*)th + O_threadId));
    }
    return s.empty() ? "-" : s;
#endif
}

// rekordbox's analysis waits for DataPath::IsDownloaded() before reading a streaming track, which
// never becomes true without a deck loading it. For a track this mod queued directly, skip that wait:
// the reader streams the track itself (or reads a local file of it, rbmod.h "trackfiles").
static bool spotifyIsDownloaded(void* self) {
    const JStr* p = (const JStr*)((const char*)self + O_dataPathString);
    if (p->text && strncmp(p->text, "spotify:", 8) == 0) return true;
    return at<bool (*)(void*)>(A_isDownloaded)(self);
}

static void skipDownloadWait(void* source) {
    if (!source || *(void**)source != at<void*>(A_dataPathVptr)) return;
    const JStr* p = (const JStr*)((const char*)source + O_dataPathString);
    if (!p->text || strncmp(p->text, "spotify:", 8) != 0) return;
    static void** copy;
    static std::once_flag once;
    if (V_dataPathIsDownloaded / 8 + 2 >= 0x100 / 8) return;  // (a build whose vtable outgrew the copy: wait for the stream as rekordbox does)
    std::call_once(once, [] {
        void** vt = (void**)at<void*>(A_dataPathVptr);
        copy = (void**)malloc(0x100);
#ifdef _WIN32
        const int hdr = 1;  // MSVC: the complete object locator before the vtable
#else
        const int hdr = 2;  // Itanium: offset to top, typeinfo
#endif
        memcpy(copy, vt - hdr, 0x100);
        copy[hdr + V_dataPathIsDownloaded / 8] = (void*)&spotifyIsDownloaded;
    });
#ifdef _WIN32
    *(void***)source = copy + 1;
#else
    *(void***)source = copy + 2;
#endif
    logf("analyze: %s %s", p->text, trackFile(p->text, nullptr) ? "has a local file, not waiting for the stream" : "is streamed");
}

static void setForce(const std::string& uri, bool on) {
    std::lock_guard<std::mutex> l(g_forceLock);
    if (on) g_force.insert(uri); else g_force.erase(uri);
}

// Tracks loaded by this mod (URIs), for hooks on analysis threads.
static std::mutex g_oursLock;
static std::set<std::string> g_ours;
static std::atomic<bool> g_fast{false};
static void setOurs(const std::string& uri, bool on) {
    std::lock_guard<std::mutex> l(g_oursLock);
    if (on) g_ours.insert(uri); else g_ours.erase(uri);
}

// Decks ejected recently, until when to check that they stay empty: an eject issued while rekordbox is
// still finishing the load can be overtaken by it, leaving the track on the deck.
static double g_ejectUntil[8], g_ejectedAt[8];

static void ejectSlot(Slot& s) {
    if (s.ch >= 0) {
        if (!deckPlaying(s.ch) && (deckLoaded(s.ch) || deckLoading(s.ch) || deckHasSong(s.ch))) deckUnload(s.ch);
        if (s.ch < 8) g_ejectUntil[s.ch] = now() + 60, g_ejectedAt[s.ch] = now();
        logf("analyze: ejecting deck %d (loaded %d, loading %d, playing %d, shown %d)", s.ch + 1, deckLoaded(s.ch), deckLoading(s.ch), deckPlaying(s.ch), deckHasSong(s.ch));
    }
    if (s.force) setForce(s.uri, false);
    setOurs(s.uri, false);
    if (s.id) {
        if (s.seenRunning && !analysisRunning(s.id)) setDirectId(s.id, false);  // finished
        else g_leftoverIds[s.id] = Leftover{now() + 900, false};  // may still begin, or is still running
    }
    s.row = -1;
    s.id = 0;
    s.seenRunning = false;
}

// Drops the leftover ids whose analysis has run and ended, or that rekordbox never began.
static void pruneLeftoverIds(double t) {
    static double at;
    if (g_leftoverIds.empty() || t - at < 1) return;
    at = t;
    for (auto it = g_leftoverIds.begin(); it != g_leftoverIds.end();) {
        bool running = analysisRunning(it->first);
        if (running) it->second.seen = true;
        if ((it->second.seen && !running) || t > it->second.until) {
            setDirectId(it->first, false);
            it = g_leftoverIds.erase(it);
        } else {
            ++it;
        }
    }
}

// A row in flight when the run stops: waiting again, and back on the retry list if it came from there.
static void putBack(Slot& s) {
    setState(s.row, State::Waiting);
    if (s.retrying) g_run.retry.push_back(s.row);
    ejectSlot(s);
}

// ---- hooks --------------------------------------------------------------------------------

// analyzeCollectionLoad shows rekordbox's "Analysis Setting" dialog (modal; true = OK) before it queues a track that
// was never analysed, unless "don't ask" was saved. For the tracks this mod queues it answers OK without asking: the
// current analysis settings apply, as they would after OK, and a run of hundreds doesn't stop at every new track.
static bool skipSettingsDialog() {
    if (!t_batchQueue || !g_on) return false;
    static int logged;
    if (logged++ < 3) logf("analyze: not asking for the analysis settings (rekordbox's dialog), using the current ones");
    return true;
}
// AnalysisSettingWindow::show returns an int (nonzero: OK); both builds test the whole register.
extern "C" int rbm_hook_load_askSettings0(bool a, bool b) {
    if (skipSettingsDialog()) return 1;
    return RBM_ORIG(load_askSettings0, int (*)(bool, bool))(a, b);
}
extern "C" int rbm_hook_load_askSettings1(bool a, bool b) {
    if (skipSettingsDialog()) return 1;
    return RBM_ORIG(load_askSettings1, int (*)(bool, bool))(a, b);
}

// t_forcing (declared above): set when isAnalyzedOldVer was forced; the same call then queues the analysis.

// DatabaseMediator::analyzeCollectionLoad asks whether a deck-loaded track's analysis is outdated.
extern "C" bool rbm_hook_load_isAnalyzedOld(const char* track, bool arg) {
    bool r = RBM_ORIG(load_isAnalyzedOld, bool (*)(const char*, bool))(track, arg);
    if (r || !g_on) return r;
    const char* p = trackPath(track);
    std::lock_guard<std::mutex> l(g_forceLock);
    // Only once per load: rekordbox asks again while loading, and every "outdated" answer queues a new
    // analysis that cancels the one already running.
    if (!p || !g_force.erase(p)) return false;
    logf("analyze: re-analysing %s", p);
    t_forcing = true;
    return true;
}

// Track ids being force re-analysed (DB ids, as queued by execAnalyzeCollection).
static std::mutex g_forcedIdsLock;
static std::set<unsigned> g_forcedIds;
static std::map<unsigned, double> g_forceKeyUntil;
static std::set<unsigned> g_runIds;               // ids the current run queued
static std::set<unsigned> g_runStarted;           // of those, the ones rekordbox has started since we queued them
static std::map<unsigned, double> g_activeUntil;  // ids the analyzer loaded (analysis may be running)  // ids whose stored key the analysis must not reuse

// The analysis reuses a track's stored key (via its track-info interface, vtable slots 0xb0 / 0x190)
// instead of running key detection. While a forced track is analysed on this thread, those report "no key".
static thread_local bool t_forceKey;
static std::mutex g_vtLock;
static std::map<void* const*, std::pair<void*, void*>> g_vtOrig;  // patched vtable -> original slots

static std::pair<void*, void*> vtOrig(const void* obj) {
    std::lock_guard<std::mutex> l(g_vtLock);
    return g_vtOrig[*(void* const* const*)obj];
}
static int keyById(void* obj, unsigned id) {
    int r = ((int (*)(void*, unsigned))vtOrig(obj).first)(obj, id);
    return t_forceKey ? 24 : r;
}
static int keyFromInfo(void* obj, void* info) {
    int r = ((int (*)(void*, void*))vtOrig(obj).second)(obj, info);
    return t_forceKey ? 24 : r;
}
// The track-info interface's slots and the thread's job flags. Windows' values were read from the same code in
// CBeatAnalyzeThread::analyze() of 7.2.19 (MSVC: one destructor slot, so the slots are 8 lower; the thread's own
// members are laid out differently). On Windows they're table fields (tools/win/verify/analyzer.out.json), so a
// table the patcher makes for another build has them where that build reads them (tools/win/sigfields.txt).
#ifdef _WIN32
#define AN_FN "__ZN8analyzer18CBeatAnalyzeThread7analyzeEv"
static const Offset kSlotKeyById{0xa8, AN_FN, "win:info slot keyById", 0};
static const Offset kSlotKeyFromInfo{0x188, AN_FN, "win:info slot keyFromInfo", 0};
static const Offset kParamKey{0x2c68, AN_FN, "win:param key", 0};      // AnalyzeParametorManager flags
static const Offset kParamPhrase{0x2c69, AN_FN, "win:param phrase", 0};
static const Offset kParamVocal{0x2c6a, AN_FN, "win:param vocal", 0};
static const Offset kJobKey{0x2c89, AN_FN, "win:job key", 0};          // AnalyzeJobManager (+0x2c80) key flag
static const Offset kEmbedding{0x29a, AN_FN, "win:embedding", 0};
#undef AN_FN
#else
// mac: read from rekordbox's own code like every other field, so a build that moves them is followed, or (when a recipe
// no longer resolves) switches the mod off instead of writing into the wrong place of the thread.
#define AN_FN "__ZN8analyzer18CBeatAnalyzeThread7analyzeEv"
static const Offset kSlotKeyById{0xb0, AN_FN, "ldr x8", 10};       // track-info vtable: key by track id
static const Offset kSlotKeyFromInfo{0x190, AN_FN, "ldr x8", 92};  // track-info vtable: key from the info
static const Offset kEmbedding{0x272, AN_FN, "ldr x20", 61};       // CBeatAnalyzeThread: similarity embedding asked for
static const Offset O_threadSource2{0x3948, AN_FN, "ldr x20", 0};  // CBeatAnalyzeThread: its shared_ptr<DataSource>'s pointer
static const Offset O_paramManager{0x3860, "__ZN8analyzer18CBeatAnalyzeThreadC2Ev", "movz x8", 2};  // CBeatAnalyzeThread: AnalyzeParametorManager
static const Offset O_jobManager{0x38f8, AN_FN, "movz x8", 15};                                     // CBeatAnalyzeThread: AnalyzeJobManager
static const Offset O_paramKeyFlag{0x80, "__ZN8analyzer23AnalyzeParametorManager6SetKeyEbNS_29AnalyzeParametorManagerReasonE", "str x0", 0};
static const Offset O_paramPhraseFlag{0x81, "__ZN8analyzer23AnalyzeParametorManager9SetPhraseEbNS_29AnalyzeParametorManagerReasonE", "str x0", 0};
static const Offset O_paramVocalFlag{0x82, "__ZN8analyzer23AnalyzeParametorManager8SetVocalEbNS_29AnalyzeParametorManagerReasonE", "str x0", 0};
static const Offset O_jobKeyFlag{0x9, "__ZN8analyzer17AnalyzeJobManager6SetKeyEb", "str x0", 0};
#undef AN_FN
#define kParamKey (O_paramManager + O_paramKeyFlag)
#define kParamPhrase (O_paramManager + O_paramPhraseFlag)
#define kParamVocal (O_paramManager + O_paramVocalFlag)
#define kJobKey (O_jobManager + O_jobKeyFlag)
#endif

static void patchVtable(void* obj) {
    void** vt = *(void***)obj;
    std::lock_guard<std::mutex> l(g_vtLock);
    if (g_vtOrig.count(vt)) return;
    const int a = (int)(kSlotKeyById / 8), b = (int)(kSlotKeyFromInfo / 8);
    void* oldA = rbm::patchSlot(&vt[a], (void*)&keyById);
    if (!oldA) {
        logf("analyze: could not patch the track-info vtable");
        g_vtOrig[vt] = {nullptr, nullptr};
        return;
    }
    void* oldB = rbm::patchSlot(&vt[b], (void*)&keyFromInfo);
    g_vtOrig[vt] = {oldA, oldB};
}

// the "columns" / "sortclick" / "sortset" test commands
static const Offset O_viewerHeader{0x2a8, "__ZNK6browse10ListViewer14getTableHeaderEv", "ldr x0", 0};  // ListViewer: BrowseListHeader*
static const Addr A_columnClicked{"__ZN6browse16BrowseListHeader13columnClickedEiRKN4juce12ModifierKeysE"};  // BrowseListHeader::columnClicked(id, ModifierKeys const&)
static const Addr A_columnIdOfIndex{"__ZNK4juce20TableHeaderComponent18getColumnIdOfIndexEib"};
static const Addr A_columnName{"__ZNK4juce20TableHeaderComponent13getColumnNameEi"};
static const Addr A_setSortColumnId{"__ZN4juce20TableHeaderComponent15setSortColumnIdEib"};
static const Offset O_threadSource{0x260, "__ZN8analyzer18CBeatAnalyzeThread15getSongFilePathEv", "ldr x21", 0};  // CBeatAnalyzeThread: analyzer::DataSource* (vtable +0x28: path)

// Start of an analysis on a CBeatAnalyzeThread (+0x258 track id, +0x2a0 track-info interface).
extern "C" void* rbm_an_begin(void* holder, const char* thread) {
    analysisMark(thread, "start");
    if (g_on) {
        void* source = *(void* const*)(thread + O_threadSource);
        const JStr* path = source ? (*reinterpret_cast<const JStr* (***)(void*)>(source))[V_sourcePath / 8](source) : nullptr;
        bool ours = false;
        if (path && path->text) {
            std::lock_guard<std::mutex> l(g_oursLock);
            ours = g_ours.count(path->text) > 0;
        }
        if (ours)  // a full analysis: its key stage will come
            if (auto f = service<void (*)(const void*)>("betterkey.prefetch")) f(thread);
        if (ours && g_fast) {
            // Requested jobs, read by analyze() into its AnalyzeJobManager: phrase, vocal; and the embedding flag.
            *(volatile char*)(thread + kParamPhrase) = 0;
            *(volatile char*)(thread + kParamVocal) = 0;
            *(volatile char*)(thread + kEmbedding) = 0;
            logf("analyze: fast analysis of %s", path->text);
        }
    }
    unsigned id = *(const unsigned*)(thread + O_threadId);
    void* info = *(void* const*)(thread + O_threadInfo);
    bool force = false;
    {
        std::lock_guard<std::mutex> l(g_forcedIdsLock);
        auto it = g_forceKeyUntil.find(id);
        if (it != g_forceKeyUntil.end()) {
            force = now() < it->second;
            if (!force) g_forceKeyUntil.erase(it);
        }
    }
    if (force && info) patchVtable(info);
    t_forceKey = force && info && vtOrig(info).first;
    if (force) {
        // Ask for the key job: AnalyzeParametorManager's key flag (copied into the job manager by analyze()) and
        // AnalyzeJobManager's.
        *(volatile char*)(thread + kParamKey) = 1;
        *(volatile char*)(thread + kJobKey) = 1;
    }
    if (force) logf("analyze: re-detecting the key of track %u%s", id, t_forceKey ? "" : " (not possible)");
    // A track this mod queued without a deck has no deck downloading it: don't wait for one. (Same
    // object in both places, but guard against two.)
    if (isDirectId(id)) {
        void* source = *(void* const*)(thread + O_threadSource);
        skipDownloadWait(source);
#ifndef _WIN32  // (Windows: no second source pointer verified)
        void* source2 = *(void* const*)(thread + O_threadSource2);
        if (source2 != source) skipDownloadWait(source2);
#endif
    }
    return RBM_ORIG(an_begin, void* (*)(void*))(holder);
}
#if defined(__aarch64__)
asm(R"(
    .text
    .p2align 2
    .globl _rbm_hook_an_begin
_rbm_hook_an_begin:
    mov  x1, x20
    b    _rbm_an_begin
)");
#else
// Windows: no trampoline; the table's "hookreg an_begin r15 1" line makes the thunk pass the thread (r15) as argument 1 of rbm_an_begin.
#endif

// A run without decks is going and no deck is being loaded (a DJ's load still pauses analyses to go first).
static bool batchRunning() {
    if (!g_on || !g_run.running || !g_run.direct) return false;
    for (int ch = 0; ch < deckCount(); ch++)
        if (deckLoading(ch)) return false;
    return true;
}

// For an analysed track rekordbox's "outdated" path only re-requests the extras, keeping "beats done", so
// beat and key detection never run. Passing notAnalysed makes it queue a full analysis instead.
extern "C" void rbm_hook_load_execAnalyze(void* self, void* thread, unsigned* const* ids, int type, bool notAnalysed,
                                          bool oldVer, bool b6, bool b7) {
    if (g_run.running && ids && *ids) {
        std::lock_guard<std::mutex> l(g_forcedIdsLock);
        g_activeUntil[**ids] = now() + 120;
        g_runIds.insert(**ids);
        g_runStarted.erase(**ids);  // may start (again)
    }
    // analyzeCollectionLoad asks execAnalyzeCollection to pause every running analysis (so a deck's track
    // goes first), and a paused analysis starts over from the beginning. During a run without decks that
    // restarted the running tracks every time another was queued, by this mod or by rekordbox for a track
    // it imports; let them run on. A deck being loaded (by the DJ) still goes first.
    if (t_batchQueue || batchRunning()) b6 = false;
    if (t_forcing) {
        t_forcing = false;
        notAnalysed = true;
        if (ids && *ids) {
            std::lock_guard<std::mutex> l(g_forcedIdsLock);
            g_forcedIds.insert(**ids);
            g_forceKeyUntil[**ids] = now() + 300;
        }
    }
    RBM_ORIG(load_execAnalyze, void (*)(void*, void*, unsigned* const*, int, bool, bool, bool, bool))(
        self, thread, ids, type, notAnalysed, oldVer, b6, b7);
}

// DatabaseMediator::execAnalyze reads the queued track's status. For streaming tracks with analysis data
// it can short-cut to "already analysed"; the re-analyse bit (0x2000) keeps it from doing so for ours.
extern "C" const char* rbm_hook_exec_analyzeStatus(void* db, unsigned id, unsigned* status, const char* hint) {
    const char* row = RBM_ORIG(exec_analyzeStatus, const char* (*)(void*, unsigned, unsigned*, const char*))(db, id, status, hint);
    bool forced;
    {
        std::lock_guard<std::mutex> l(g_forcedIdsLock);
        forced = g_forcedIds.erase(id) > 0;
    }
    if (forced && status) {
        logf("analyze: queued track %u status %#x%s", id, *status, row && *(const signed char*)(row + O_rowStatusLow) < 0 ? " (streaming data)" : "");
        *status |= 0x1000 | 0x2000;
        *status &= ~0x11u;
    }
    return row;
}

// The Spotify bridge pauses and restarts collection analysis (e.g. after importing a track), and every
// running analysis starts over; rekordbox's auto analysis queues tracks asking to pause the running ones.
// Neither during a run without decks.
extern "C" void rbm_hook_bridge_pause(void* dbif) {
    if (batchRunning()) { logf("analyze: not pausing analysis for the Spotify bridge"); return; }
    RBM_ORIG(bridge_pause, void (*)(void*))(dbif);
}
// Auto analysis also queues the run's tracks again right after they were analysed (their analysis isn't
// saved yet, e.g. after streaming them), each time it scans the collection: leave those to the run.
struct IdArray { unsigned* data; int allocated, used; };  // juce::Array<unsigned>
extern "C" void rbm_hook_auto_execAnalyze(void* self, void* thread, IdArray* ids, int type, bool notAnalysed,
                                          bool oldVer, bool b6, bool b7) {
    if (batchRunning()) {
        b6 = false;
        int kept = 0, n = ids ? ids->used : 0;
        {
            std::lock_guard<std::mutex> l(g_forcedIdsLock);
            for (int i = 0; i < n; i++)
                if (!g_runIds.count(ids->data[i])) ids->data[kept++] = ids->data[i];
        }
        if (kept < n) {
            logf("analyze: auto analysis: leaving %d of %d tracks to this run", n - kept, n);
            ids->used = kept;
        }
        if (!kept) return;
    }
    RBM_ORIG(auto_execAnalyze, void (*)(void*, void*, IdArray*, int, bool, bool, bool, bool))(
        self, thread, ids, type, notAnalysed, oldVer, b6, b7);
}

// For a track without artwork, rekordbox's analysis asks whether the Music app's library can be read
// ([ITLibrary libraryWithAPIVersion:], once per launch). When Apple's library agent doesn't answer, that
// blocks rekordbox's database thread for good: no analysis (or anything else it does) runs again. Ask
// once on a thread of its own and answer "no" until it has answered (it only matters for tracks from Music).
extern "C" bool rbm_hook_itunes_reading() {
    auto orig = RBM_ORIG(itunes_reading, bool (*)());
    static std::atomic<int> state{0};  // 0 not asked, 1 asking, 2 answered
    if (state == 2) return orig();
    int expect = 0;
    if (state.compare_exchange_strong(expect, 1)) {
        std::thread([orig] {
            double t0 = now();
            bool r = orig();
            state = 2;
            logf("analyze: the Music library answered after %.1fs (%s)", now() - t0, r ? "readable" : "not readable");
        }).detach();
    }
    static double loggedAt;
    if (now() - loggedAt > 30) logf("analyze: waiting for the Music library, not checking it for artwork"), loggedAt = now();
    return false;
}

// rekordbox keeps every track it imports pending until its analysis is saved, and starts all pending tracks
// again each time it processes them (after every import, for one). For a track that is being analysed
// that restarts its analysis from the beginning (CAnalyzerIF::start_analyze reuses its thread), so during a
// run without decks streaming tracks were analysed up to 18 times. Each time this mod queues a track it may
// start once; a later request fails (-1), which only drops it from rekordbox's pending tracks: the running
// analysis still reports its result. ("No free thread", -11, would keep it pending and have rekordbox's
// database thread retry it in a loop, holding up everything else.)
extern "C" int rbm_hook_exec_start(void* analyzerIf, const void* req) {
    unsigned id = req ? (unsigned)*(const long*)req : 0;
    if (id && batchRunning()) {
        std::lock_guard<std::mutex> l(g_forcedIdsLock);
        if (g_runIds.count(id) && !g_runStarted.insert(id).second) {
            static double loggedAt;
            if (now() - loggedAt > 10) logf("analyze: not restarting the analysis of track %u", id), loggedAt = now();
            return -1;
        }
    }
    return RBM_ORIG(exec_start, int (*)(void*, const void*))(analyzerIf, req);
}

// Queuing the next track's analysis cancels the running ones whose stored status says they still need
// analysing (true for every track until its analysis is saved), and rekordbox then starts them over:
// ~half of all tracks were analysed 2-3 times. Not for the tracks the analyzer loaded.
static bool keepRunning(long id, const char* who) {
    std::lock_guard<std::mutex> l(g_forcedIdsLock);
    auto it = g_activeUntil.find((unsigned)id);
    if (it == g_activeUntil.end() || now() >= it->second) return false;
    logf("analyze: not cancelling the running analysis of track %ld (%s)", id, who);
    return true;
}
extern "C" void rbm_hook_rm_cancel1(void* analyzer, long id) {
    if (!keepRunning(id, "modifyRemove")) RBM_ORIG(rm_cancel1, void (*)(void*, long))(analyzer, id);
}
extern "C" void rbm_hook_rm_cancel2(void* analyzer, long id) {
    if (!keepRunning(id, "modifyRemove")) RBM_ORIG(rm_cancel2, void (*)(void*, long))(analyzer, id);
}
extern "C" void rbm_hook_lock_cancel(void* analyzer, long id) {
    if (!keepRunning(id, "analysis lock")) RBM_ORIG(lock_cancel, void (*)(void*, long))(analyzer, id);
}
// Importing a track into the collection cancels every running analysis (the id-less cancel_analyze in
// DatabaseMediator::importTrack). In a run of Spotify tracks that aren't in the collection yet, importing the next
// one ended the ones still running before they had read any audio: each "finished" without a BPM.
extern "C" void rbm_hook_import_cancel(void* analyzer) {
    if (batchRunning()) {
        static double loggedAt;
        if (now() - loggedAt > 10) logf("analyze: not cancelling the running analyses for an import"), loggedAt = now();
        return;
    }
    RBM_ORIG(import_cancel, void (*)(void*))(analyzer);
}
extern "C" void rbm_hook_coll_cancel(void* analyzer, long id) {
    {
        std::lock_guard<std::mutex> l(g_forcedIdsLock);
        auto it = g_activeUntil.find((unsigned)id);
        if (it != g_activeUntil.end() && now() < it->second) {
            logf("analyze: not cancelling the running analysis of track %ld", id);
            return;
        }
    }
    RBM_ORIG(coll_cancel, void (*)(void*, long))(analyzer, id);
}

// UiPlayer::handleDBMessageTrackStatusWhenAnalyzeCompleted: rekordbox finished analysing the deck's
// track. Not passed on for an empty deck (rekordbox would crash updating its beat grid); this guard
// stays on even with the analyzer mod off, since a manual eject during analysis hits it too.
extern "C" void rbm_hook_analyze_completed(const char* player, unsigned id, long long arg, void* param) {
    int ch = *(const int*)(player + O_uiPlayerDeck);
    if (ch >= 0 && ch < 8) g_completedAt[ch] = now();
    if (ch >= 0 && ch < deckCount() && !deckLoaded(ch) && !deckLoading(ch)) {
        logf("analyze: analysis finished for empty deck %d, ignored", ch + 1);
        return;
    }
    RBM_ORIG(analyze_completed, void (*)(const char*, unsigned, long long, void*))(player, id, arg, param);
}

// PlayHistoryManager::timerCallback -> DatabaseIF::addTrackToPlayHistory(id); x19 is the manager.
extern "C" void rbm_history_add(void* db, unsigned id, const char* manager) {
    int ch = *(const int*)(manager + O_historyDeck);
    if (g_on && ch >= 0 && ch < 8 && !g_owned[ch].empty()) {
        std::string p = deckPath(ch);
        if (p.empty() || p == g_owned[ch]) {
            logf("analyze: not adding track %u (deck %d) to play history", id, ch + 1);
            return;
        }
    }
    RBM_ORIG(history_add, void (*)(void*, unsigned))(db, id);
}
#if defined(__aarch64__)
asm(R"(
    .text
    .p2align 2
    .globl _rbm_hook_history_add
_rbm_hook_history_add:
    mov  x2, x19
    b    _rbm_history_add
)");
#else
// Windows: no trampoline; the table's "hookreg history_add rbx 2" line passes the manager (rbx) as argument 2 of rbm_history_add.
#endif

// ---- run ----------------------------------------------------------------------------------

static std::vector<int> configuredDecks() {
    std::vector<int> d;
    std::string s = cfgStr("analyze_decks", "3,4");
    for (char* t = strtok(&s[0], ", \t"); t; t = strtok(nullptr, ", \t"))
        if (atoi(t) >= 1 && atoi(t) <= deckCount() && std::find(d.begin(), d.end(), atoi(t) - 1) == d.end())
            d.push_back(atoi(t) - 1);
    return d;
}

static void finishRun(const char* why) {
    if (g_run.running)
        logf("analyze: stopped (%s): %d analysed, %d already done, %d failed, %d rows", why, g_run.done,
             g_run.skipped, g_run.failed, g_run.rows);
    for (Slot& s : g_run.slots)
        if (s.row >= 0) putBack(s);
    for (int r : g_run.held)  // (one from the retry list goes back there; the others wait like the rest)
        if (info(r).state == State::Retry) g_run.retry.push_back(r);
    g_run.held.clear();
    for (TrackInfo& i : g_run.info)
        if (i.state == State::Retry) i.state = State::Waiting;
    g_run.running = g_run.stopping = false;
    g_run.finishedWhy = why;
    g_run.lastActivity = now();
    g_run.slots.clear();
}

// How a run ends once nothing is left to start: "done", but a "Retry failed" of a stopped run still leaves the rows that
// stop left waiting, so it ends as that run had.
static const char* retryEnd() {
    if (!g_run.retryOnly || g_run.resumeWhy.empty() || g_run.resumeWhy == "done") return "done";
    int n = (int)g_run.paths.size();
    if (g_run.limit > 0) n = std::min(n, g_run.limit);
    for (int r = 0; r < n && r < (int)g_run.info.size(); r++)
        if (g_run.info[r].state == State::Waiting) return g_run.resumeWhy.c_str();
    return "done";
}

// Stops loading new tracks; the tracks being analysed are ejected once they are finished.
static void stopRun(const char* why, const std::string& explain = "") {
    if (!g_run.running || g_run.stopping) return;
    g_run.stopping = true;
    g_run.stopWhy = explain;
    g_run.stoppingSince = now();
    logf("analyze: stopping (%s)", why);
}

// Analyses the playlist the browser shows (or is about to show: id).
static void startRun(bool force, const std::string& id = "") {
    bool direct = cfgInt("analyze_direct", 1) != 0;
    std::vector<int> decks = configuredDecks();
    if (!direct && decks.empty()) { logf("analyze: no usable deck in analyze_decks"); return; }
    g_run = Run();
    g_run.direct = direct;
    clearLimited();
    {
        std::lock_guard<std::mutex> l(g_forcedIdsLock);
        g_runIds.clear();
        g_runStarted.clear();
    }
    if (direct) {
        int n = std::max(1, cfgInt("analyze_parallel", 6));
        for (int i = 0; i < n; i++) g_run.slots.push_back(Slot{-1});
    } else {
        for (int ch : decks) g_run.slots.push_back(Slot{ch});
    }
    g_run.exists = g_run.running = g_run.waitingForList = true;
    g_run.force = force;
    g_run.startedAt = now();
    g_run.playlistId = id.empty() ? shownPlaylist() : id;
    g_run.selection = isSelection(g_run.playlistId);
    if (direct)
        logf("analyze: start%s without decks, %d at a time", force ? " (re-analyse)" : "", (int)g_run.slots.size());
    else
        logf("analyze: start%s on deck(s) %s, max %d Spotify tracks loaded, gap %.1fs", force ? " (re-analyse)" : "",
             cfgStr("analyze_decks", "3,4").c_str(), cfgInt("analyze_streams", 3), cfgNum("analyze_gap", 4));
    if (cfgInt("analyze_window", 1)) analyzer::showWindow();
}

// Saved for reopening it after a restart, once the browser shows it.
static void savePlaylist() {
    std::string where = selectedPlaylist();
    if (!where.empty() && where != cfgStr("analyze_playlist")) setCfg("analyze_playlist", where);
}

// After a restart rekordbox shows the collection; open the playlist analysed last again (analyze_reopen=0
// to not). Its tree items fill in as their parents open (the Spotify ones from the network), so this
// retries for a while.
static double g_reopenUntil;
#ifdef _WIN32
// Windows: a Spotify playlist opened right after the launch can end up as a name over an empty list on a spinner:
// rekordbox's own first selection (in the collection's tree) comes after it, the playlist is no longer its tree's
// selection, and its tracks are never asked for. Opening it once more fills it. Done a few times, some seconds
// apart, while the list is empty and the selection is still that first one of rekordbox's (anything else selected
// since is the user's: left alone).
static double g_refillAt;
static int g_refills;
static std::string g_refillBase;
static void refillReopened() {
    if (!g_refillAt || now() < g_refillAt) return;
    std::string where = cfgStr("analyze_playlist"), sel = selectedPlaylist();
    auto id = [](const std::string& w) { return w.substr(w.find('|') + 1); };
    void* lv = mainListViewer();
    int rows = lv ? listRows(lv) : -1;
    if (!g_refills) g_refillBase = sel;
    bool otherTree = sel.empty() || atoi(sel.c_str()) != atoi(where.c_str());
    if (g_refills >= 4 || where.empty() || rows != 0 || id(sel) == id(where) || !otherTree || sel != g_refillBase) {
        if (rows == 0 && id(sel) != id(where)) logf("analyze: the reopened playlist's list is empty (selection %s): left alone", sel.c_str());
        g_refillAt = 0;
        return;
    }
    g_refills++;
    g_refillAt = now() + 6;
    if (openPlaylist(where) == 1) logf("analyze: the reopened playlist was still empty, opened it again");
}
#endif
static void reopenPlaylist() {
#ifdef _WIN32
    refillReopened();
#endif
    static double triedAt;
    static std::string left;  // the selection the last try left; another one means the user clicked: stop
    double t = now();
    if (!g_reopenUntil || t - triedAt < 1) return;
    std::string where = cfgStr("analyze_playlist");
    // Switching the browser to the playlist's source selects that source's first item (Spotify: the library
    // folder): a selection on the way to the playlist is this one's, not the user's.
    auto onTheWay = [&](const std::string& sel) {
        size_t a = sel.find('|'), b = where.find('|');
        if (a == std::string::npos || b == std::string::npos) return false;
        std::string s = sel.substr(a + 1), w = where.substr(b + 1);
        return !s.empty() && w.compare(0, s.size() + 1, s + "/") == 0;
    };
    std::string sel = selectedPlaylist();
    // (and while rekordbox is still starting, nothing is selected yet: its own first selection isn't a click)
    if (triedAt && sel != left && !left.empty() && !onTheWay(sel)) {
        g_reopenUntil = triedAt = 0;
        logf("analyze: not reopening the last analysed playlist: another one was selected");
        return;
    }
    triedAt = t;
    int r = where.empty() ? -1 : openPlaylist(where);
    left = selectedPlaylist();
    if (r == 0 && t < g_reopenUntil) return;
    g_reopenUntil = triedAt = 0;
    if (r == 1) logf("analyze: opened the last analysed playlist (%s)", where.c_str());
    else if (!where.empty()) logf("analyze: could not open the last analysed playlist (%s)", where.c_str());
#ifdef _WIN32
    if (r == 1) g_refillAt = now() + 5, g_refills = 0;
#endif
}

// Seconds an analysis may run (analyze_timeout). A slower PC with bettergrid waiting for Beat This! on the CPU
// (up to 60 s past rekordbox's own grid) needs more than a Mac.
#ifdef _WIN32
static const double kAnalyzeTimeout = 180;
#else
static const double kAnalyzeTimeout = 90;
#endif

// Watches a direct analysis; true once rekordbox's analyser has finished the track (or given up).
static bool directFinished(Slot& s, double t) {
    if (s.importing) {
        info(s.row).took = t - s.startedAt;
        if (g_run.stopping) {  // queue nothing new once stopping (it would be cut, its analysis left in rekordbox's queue)
            putBack(s);
            return true;
        }
        if (t - s.checkedAt < 0.5) return false;
        s.checkedAt = t;
        unsigned id = 0;
        if (queueCollectionTrack(s.uri, &id) == Queue::Queued) {
            logf("analyze: %s imported into the collection after %.1fs (id %u), analysing it", s.uri.c_str(),
                 t - s.importStartedAt, id);
            s.importing = false;
            s.id = id;
            s.startedAt = s.timingFrom = t;
            s.seenRunning = false;
            s.stableSince = s.checkedAt = 0;
            return false;
        }
        if (t - s.importStartedAt <= cfgNum("analyze_import_timeout", 20)) return false;
        logf("analyze: row %d: %s did not show up in the collection after %.0fs", s.row, s.uri.c_str(),
             t - s.importStartedAt);
        g_run.failed++;
        setState(s.row, State::Failed);
        ejectSlot(s);
        return true;
    }
    bool runningNow = analysisRunning(s.id);
    if (runningNow && !s.seenRunning) s.clockFrom = t;  // its analysis began: the timeout counts from here
    if (s.id) {  // still ours: queueing the next tracks must not cancel it, however long it waits and runs
        std::lock_guard<std::mutex> l(g_forcedIdsLock);
        g_activeUntil[s.id] = t + 120;
    }
    if (runningNow) s.seenRunning = true;
    // rekordbox runs only a few analyses at once (3 on a 6-core PC) and queues the rest. A track waiting behind
    // others of this run that are still analysing hasn't failed to start: on a slow computer each takes 30-50 s,
    // longer than the start timeout. Its clock starts once nothing of ours runs ahead of it.
    if (!s.seenRunning) {
        bool queued = false;
        for (const Slot& o : g_run.slots)
            if (&o != &s && o.seenRunning && o.stableSince == 0 && !o.importing) queued = true;
        if (queued || s.clockFrom < s.startedAt) s.clockFrom = t;
    }
    if (s.seenRunning && info(s.row).state == State::Loading) info(s.row).state = State::Analysing;
    info(s.row).took = t - s.startedAt;

    // rekordbox didn't queue it (already analysed): the collection says so.
    bool skipped = false;
    if (!s.seenRunning && !s.force && t - s.checkedAt >= 1) {
        s.checkedAt = t;
        skipped = isAnalyzed(s.uri);
    }

    // A thread that was running and no longer is has finished; give the database a moment to catch up.
    bool done = s.seenRunning && !runningNow;
    if (done) {
        if (s.stableSince == 0) s.stableSince = t;
    } else {
        s.stableSince = 0;
    }
    bool finished = done && t - s.stableSince >= 1;
    bool timedOut = t - s.clockFrom > cfgNum("analyze_timeout", kAnalyzeTimeout);
    // A thread that holds the track but never begins its analysis (left paused) counts as not started.
    // (Only with the timing mod running: off, e.g. on Windows, it records nothing and every analysis looked unstarted.)
    bool timing = provided("timing.report") != nullptr;
    bool neverStarted = (!s.seenRunning || (timing && analysisTiming(s.uri, s.timingFrom).empty())) &&
                        t - s.clockFrom > cfgNum("analyze_start_timeout", 15);
    if (!finished && !skipped && !timedOut && !neverStarted) return false;

    CollectionTrack c;
    double bpm = collectionTrack(s.uri, &c) ? c.bpm : 0;
    // A Spotify track analysed the moment it was imported into the collection can "finish" within a couple of
    // seconds without ever reading its audio (no BPM). That's no result: it's tried again at the end of the run.
    // (Streaming only: a local file analysed to no BPM, a one-shot sample say, is a result.)
    bool empty = finished && !skipped && bpm <= 0 && !isLocalFile(s.uri);
    logf("analyze: row %d %s bpm=%.2f after %.1fs [%s] threads %s", s.row,
         skipped ? "skipped" : empty ? "EMPTY" : finished ? "ok" : neverStarted ? "NOT STARTED" : "TIMEOUT", bpm, t - s.startedAt,
         analysisTiming(s.uri, s.timingFrom).c_str(), runningIds().c_str());
    if (skipped) {
        g_run.skipped++;
        setState(s.row, State::Skipped);
    } else if (finished && !empty) {
        g_run.done++;
        setState(s.row, State::Done);
    } else if (g_run.stopping) {
        g_run.failed++;
        setState(s.row, State::Failed);
    } else if (requeueIfLimited(s.row, s.uri, s.streams)) {
    } else if (!g_run.retried.count(s.row)) {  // didn't start, or no result: try again at the end
        g_run.retried.insert(s.row);
        g_run.retry.push_back(s.row);
        setState(s.row, State::Retry);
    } else {
        g_run.failed++;
        setState(s.row, State::Failed);
    }
    ejectSlot(s);
    return true;
}

static bool requeueIfLimited(int row, const std::string& uri, bool streamed) {
    if (!isSpotify(uri.c_str())) return false;
    readSpotifyLog(true);  // fallback only: the refusal may have been written just now
    // (a track read from a local file never met the limit: it failed for another reason, and would be tried over and
    // over, uncounted, for as long as the limit lasts)
    if (!takeLimited(uri) && !(streamed && spotifyLimitLeft())) return false;
    logf("analyze: row %d: Spotify's streaming limit, trying it again after", row);
    g_run.retry.push_back(row);
    setState(row, State::Retry);
    return true;
}

// Watches one deck's load; true once it is finished (or given up).
static bool slotFinished(Slot& s, double t) {
    if (g_run.direct) return directFinished(s, t);
    bool loaded = deckLoaded(s.ch), loading = deckLoading(s.ch);
    double bpm = deckBpm(s.ch, false);
    if (loading) s.sawLoading = true;
    std::string p = deckPath(s.ch);
    if (!p.empty() && g_owned[s.ch].empty()) g_owned[s.ch] = p;
    if (!p.empty() && p != g_owned[s.ch]) {  // the DJ loaded something else onto this deck
        logf("analyze: deck %d was taken over, row %d will be retried", s.ch + 1, s.row);
        g_run.retry.push_back(s.row);
        setState(s.row, State::Retry);
        g_owned[s.ch].clear();
        if (s.force) setForce(s.uri, false);
        setOurs(s.uri, false);
        s.row = -1;
        return false;
    }

    if (loaded && !loading && info(s.row).state == State::Loading) info(s.row).state = State::Analysing;
    info(s.row).took = t - s.startedAt;

    bool settledNow = t - s.startedAt > 2 && (s.sawLoading || t - s.startedAt > 8) && loaded && !loading && bpm > 0;
    if (settledNow && fabs(bpm - s.lastBpm) < 0.01) {
        if (s.stableSince == 0) s.stableSince = t;
    } else {
        s.stableSince = 0;
    }
    s.lastBpm = bpm;

    // Done when rekordbox reports the deck's analysis finished. Fallback for a track it didn't need to
    // analyse: the collection says it is analysed and the deck has been stable for a while.
    bool done = g_completedAt[s.ch] > s.startedAt && t - g_completedAt[s.ch] >= 1 && loaded && !loading;
    if (!done && !s.force && s.stableSince && t - s.stableSince >= 5 && t - s.checkedAt >= 1) {
        s.checkedAt = t;
        done = isAnalyzed(s.uri);
    }
    bool timedOut = t - s.startedAt > cfgNum("analyze_timeout", kAnalyzeTimeout);
    if (!done && !timedOut) return false;

    logf("analyze: row %d on deck %d %s bpm=%.2f after %.1fs [%s]", s.row, s.ch + 1, done ? "ok" : "TIMEOUT", bpm,
         t - s.startedAt, analysisTiming(s.uri, s.timingFrom).c_str());
    if (done) {
        g_run.done++;
        setState(s.row, State::Done);
    } else if (g_run.stopping) {
        g_run.failed++;
        setState(s.row, State::Failed);
    } else if (requeueIfLimited(s.row, s.uri, s.streams)) {
    } else if (!g_run.retried.count(s.row)) {  // e.g. Spotify refused another stream: try again at the end
        g_run.retried.insert(s.row);
        g_run.retry.push_back(s.row);
        setState(s.row, State::Retry);
    } else {
        g_run.failed++;
        setState(s.row, State::Failed);
    }
    ejectSlot(s);
    return true;
}

// Is the list showing the playlist we started on? Extends the snapshot as streaming rows page in.
// The run goes through the playlist's tracks by URI, in the order they turned up. A big streaming
// playlist loads in pages that slot in by the list's sort order, shifting the rows, so the list's row
// numbers aren't stable; this adds the tracks of rows not seen yet.
static void collectUris(void* lv, int rows) {
    for (int i = 0; i < rows; i++) {
        const char* track = listRow(lv, i);
        const char* p = track ? trackPath(track) : nullptr;
        if (!p || !*p || !g_run.pathSet.insert(p).second) continue;
        int r = (int)g_run.paths.size();
        g_run.paths.push_back(p);
        TrackInfo& in = info(r);
        in.title = trackTitle(track), in.artist = trackArtist(track);
        setTrackNote(p, "title", in.title), setTrackNote(p, "artist", in.artist);
    }
}

// A selection's tracks (core's selectionPlaylist) are the run's rows, in their order. They are known, so nothing waits
// for the list, and what else the list shows never joins the run; lv: the list while it shows them, for their titles.
static void collectSelection(void* lv, int rows) {
    for (const std::string& u : playlistTracks(g_run.playlistId).uris)
        if (!u.empty() && g_run.pathSet.insert(u).second) g_run.paths.push_back(u);
    std::map<std::string, int> at;
    for (int r = 0; r < (int)g_run.paths.size(); r++) at[g_run.paths[r]] = r;
    for (int i = 0; lv && i < rows; i++) {
        const char* track = listRow(lv, i);
        const char* p = track ? trackPath(track) : nullptr;
        auto it = p ? at.find(p) : at.end();
        if (it == at.end()) continue;
        TrackInfo& in = info(it->second);
        in.title = trackTitle(track), in.artist = trackArtist(track);
        setTrackNote(p, "title", in.title), setTrackNote(p, "artist", in.artist);
    }
}

// The list row showing a track, -1 if none.
static int findRow(void* lv, const std::string& uri) {
    for (int i = 0, n = listRows(lv); i < n; i++) {
        const char* p = trackPath(listRow(lv, i));
        if (p && uri == p) return i;
    }
    return -1;
}

// Spotify tracks loaded or loading on any deck (including in-flight analysis loads that don't show a
// path yet).
static int spotifyStreams() {
    int n = 0;
    for (int ch = 0; ch < deckCount(); ch++) {
        bool ours = false;
        for (Slot& s : g_run.slots) ours |= s.ch == ch && s.row >= 0;
        if (ours || isSpotify(deckPath(ch).c_str())) n++;
    }
    return n;
}

static std::string runUri(void*, int row) { return row < (int)g_run.paths.size() ? g_run.paths[row] : ""; }

// A grid bettergrid didn't make, or made while Beat This! was unsure: analysed before bettergrid existed (rekordbox's
// own grid, ~3% half a beat off and more with the bars wrong), or while Beat This!'s fit counted off-beats as beats
// (fixed 2026-11-12). bettergrid notes every track it saw (bt_bpm) with its fit (bt_fit), also from its CSV, and
// bt_fv=2 when the fit came from the fixed fitter: Beat This! stays unsure on those, so analysing them again would
// only repeat the same result on every run.
static bool oldGrid(const std::string& uri) {
    if (uri.find("/rekordbox/Sampler/") != std::string::npos) return false;  // rekordbox's own sampler sounds: left alone
    if (trackNote(uri, "bt_bpm").empty()) return true;
    std::string fit = trackNote(uri, "bt_fit");
    return !fit.empty() && atof(fit.c_str()) < cfgNum("bettergrid_min_fit", 0.6) && trackNote(uri, "bt_fv").empty();
}

// Whether a row can only be imported from its playlist row, so waits while another list is shown.
static bool needsList(const std::string& uri) {
    return !g_run.shown && !(!uri.empty() && collectionTrack(uri, nullptr)) && !canImportTrack(uri);
}

// Next row to analyse, or -1. Skips rows that are already analysed (unless re-analysing). While another
// list is shown, rows that would have to be imported first (that needs their playlist row) wait. Rows another mod held
// back go first once it lets them go (behind: it doesn't yet).
static int nextRow(void* lv, int rows, bool behind) {
    while (!behind && !g_run.held.empty()) {
        int r = g_run.held.front();
        if (!needsList(runUri(lv, r))) return r;
        g_run.held.erase(g_run.held.begin());
        (info(r).state == State::Retry ? g_run.retry : g_run.needList).push_back(r);  // back where it came from
    }
    if (g_run.retryOnly) return g_run.retry.empty() ? -1 : g_run.retry.front();
    if (g_run.limit > 0) rows = std::min(rows, g_run.limit);
    while (g_run.next < rows) {
        int r = g_run.next;
        std::string uri = runUri(lv, r);
        bool listed = !uri.empty() && collectionTrack(uri, nullptr);
        if ((!g_run.force || (g_run.oldOnly && !oldGrid(uri))) && listed && isAnalyzed(uri)) { g_run.skipped++; setState(r, State::Skipped); g_run.next++; continue; }
        if (!g_run.shown && !listed && !canImportTrack(uri)) { g_run.needList.push_back(r); g_run.next++; continue; }
        if (!uri.empty() && (uri[0] == '/' || (uri.size() > 2 && uri[1] == ':')) && !plat::fileInfo(uri, nullptr)) {  // moved, or on a drive that isn't there
            logf("analyze: row %d skipped, the file is missing: %s", r, uri.c_str());
            g_run.failed++, setState(r, State::Failed), g_run.next++;
            continue;
        }
        return r;
    }
    if (g_run.shown && !g_run.needList.empty()) return g_run.needList.front();
    if (!g_run.retry.empty()) return g_run.retry.front();
    return -1;
}

// Takes the row nextRow gave off the list it came from.
static void takeRow(int row) {
    if (!g_run.held.empty() && g_run.held.front() == row) g_run.held.erase(g_run.held.begin());
    else if (g_run.shown && !g_run.needList.empty() && g_run.needList.front() == row) g_run.needList.erase(g_run.needList.begin());
    else if (!g_run.retry.empty() && g_run.retry.front() == row) g_run.retry.erase(g_run.retry.begin());
    else g_run.next++;
}

// Whether rows are left to start (without asking the database, as nextRow does).
static bool rowsLeft(int rows) {
    if (g_run.limit > 0) rows = std::min(rows, g_run.limit);
    if (!g_run.held.empty() || !g_run.retry.empty()) return true;
    return !g_run.retryOnly && (g_run.next < rows || (g_run.shown && !g_run.needList.empty()));
}

// The row to start next (nextRow's), except that while another mod holds tracks back (holdingBack), a track it holds
// is put aside (g_run.held) and the run goes on with the rows after it that it doesn't hold; the held ones go first
// again once it lets them go. -1 if none: then gb is what it still waits for when only held rows are left (0
// otherwise), and more is true when this tick stopped looking.
static int pickRow(void* lv, int rows, double* gb, bool* more) {
    *gb = 0, *more = false;
    double staged = 0;
    int asked = 0;  // 0 not asked yet, 1 behind, 2 not (asked only when it matters: the asking may take a while)
    auto behind = [&] {
        if (!asked) asked = holdingBack(&staged) ? 1 : 2;
        return asked == 1;
    };
    for (int n = 0; n < 10; n++) {  // (each row looked at queries the database: a few per tick)
        int r = nextRow(lv, rows, !g_run.held.empty() && behind());
        if (r < 0) {
            if (!g_run.held.empty() && behind()) *gb = std::max(staged, 0.01);
            return -1;
        }
        if (!holdsBack(runUri(lv, r)) || !behind()) return r;
        takeRow(r);
        if (g_run.held.empty()) logf("analyze: %s holds tracks back (%.1f GB to go), going on with the others", providerOf("analyzer.holdBack"), staged);
        g_run.held.push_back(r);
    }
    *more = true;
    return -1;
}

// Commands from a file, for scripted test runs on the list the browser shows: "reanalyze [rows]", "reanalyze old [rows]",
// "analyze [rows]", "analyze <playlist id>" / "reanalyze <playlist id>" (that playlist or selection, as the window's buttons),
// "selected" (re-analyse the selected tracks, as the track list's menu) or "selected <tracks>" (those of the list instead:
// "3" its first three rows, "rows 0 4", or URIs; see devTracks in rbmod.h), "window [playlist id]" (the analysis window,
// on that playlist or selection; none: the run's), "stop", "retry" (the window's Retry failed), "playlist" (log the selected one), "open" (the last analysed one) or
// "rows [n]" (log the list's first rows), "load <row> [deck]", "columns" (log the list's columns) or
// "sortclick <column id>" (as a click on its header) or "sortset <column id>" (sorted by it, no click) in <configDir>/command (removed when read).
static void readCommand() {
    static double lastRead;
    double t = now();
    if (t - lastRead < 1) return;
    lastRead = t;
    std::string path = plat::configDir() + "/command";
    FILE* f = plat::openFile(path, "r");
    if (!f) return;
    char line[2200] = "", cmd[32] = "", arg[1024] = "", arg2[32] = "";
    if (!fgets(line, sizeof line, f)) line[0] = 0;
    fclose(f);
    sscanf(line, "%31s %1023s %31s", cmd, arg, arg2);
    int rows = atoi(arg);
    std::string rest = line + std::min(strlen(line), strspn(line, " \t") + strlen(cmd));  // what follows the command word (ids and paths have spaces)
    rest.erase(0, rest.find_first_not_of(" \t"));
    rest.erase(rest.find_last_not_of(" \t\r\n") + 1);
    std::error_code ec;
    std::filesystem::remove(plat::fsPath(path), ec);
    logf("analyze: command '%s %s'", cmd, rest.c_str());
    if (!strcmp(cmd, "stop")) { if (g_run.running) stopRun("stopped"); return; }
    if (!strcmp(cmd, "retry")) { analyzer::retryFailed(); return; }
    if (!strcmp(cmd, "playlist")) { logf("analyze: selected playlist %s", selectedPlaylist().c_str()); return; }
    if (!strcmp(cmd, "rows")) {
        void* lv = mainListViewer();
        int n = listRows(lv);
        logf("analyze: %d rows", n);
        for (int i = 0; i < std::min(rows ? rows : 5, n); i++) {
            const char* t = listRow(lv, i);
            logf("analyze: row %d %s  %.2f %s", i, trackPath(t) ? trackPath(t) : "-", trackBpm(t), trackKey(t) ? trackKey(t) : "-");
        }
        return;
    }
    if (!strcmp(cmd, "columns") || !strcmp(cmd, "sortclick") || !strcmp(cmd, "sortset")) {  // the list's columns (id name), a click on
        // one's header, or its sort set directly (as a playlist's saved sort does: no click)
        void* lv = mainListViewer();
        void* header = lv ? *(void* const*)((const char*)lv + O_viewerHeader) : nullptr;
        if (!header) return;
        if (!strcmp(cmd, "sortset")) {
            at<void (*)(void*, int, bool)>(A_setSortColumnId)(header, rows, true);
            return;
        }
        if (!strcmp(cmd, "sortclick")) {
            int mods = 0x10;  // juce::ModifierKeys: left button
            at<void (*)(void*, int, const int*)>(A_columnClicked)(header, rows, &mods);
            return;
        }
        for (int i = 0; i < 100; i++) {
            int id = at<int (*)(void*, int, bool)>(A_columnIdOfIndex)(header, i, false);
            if (id) logf("analyze: column %d %s", id, memberString(A_columnName, header, id).c_str());
        }
        return;
    }
    if (!strcmp(cmd, "open")) { g_reopenUntil = now() + 120; return; }
    if (!strcmp(cmd, "selected")) {  // as "Re-analyze track(s)" in the track list's menu
        std::vector<std::string> sel = rest.empty() ? selectedTracks(browserListViewer()) : devTracks(rest);
        std::string id = selectionPlaylist(sel);
        if (id.empty()) logf("analyze: no tracks selected");
        else logf("analyze: re-analysing %d selected track(s) (%s)", (int)sel.size(), id.c_str());
        analyzer::analyzePlaylist(id, true);
        return;
    }
    if (!strcmp(cmd, "window")) { analyzer::showWindowFor(rest); return; }
    if (!strcmp(cmd, "load")) {  // "load <row or uri> [deck 1-4]": the shown list's row onto a deck, like a double click
        void* lv = mainListViewer();
        int deck = arg2[0] ? atoi(arg2) : 1;
        if (lv && !isdigit((unsigned char)arg[0])) {  // a track's path or URI: its row in the list, which pages in
            rows = -1;
            for (int i = 0, n = listRows(lv); i < n; i++) {
                const char* q = trackPath(listRow(lv, i));
                if (q && !strcmp(q, arg)) { rows = i; break; }
            }
        }
        const char* p = lv ? trackPath(listRow(lv, rows)) : nullptr;
        logf("analyze: loading row %d (%s) onto deck %d", rows, p ? p : "-", deck);
        if (p && deck >= 1 && deck <= deckCount()) listLoadTrack(lv, deck - 1, rows);
        return;
    }
    if (g_run.running || (strcmp(cmd, "reanalyze") && strcmp(cmd, "analyze"))) return;
    if (rest[0] == '/' || isSelection(rest)) { analyzer::analyzePlaylist(rest, !strcmp(cmd, "reanalyze")); return; }  // by id
    bool old = !strcmp(arg, "old");  // "reanalyze old [rows]": only the grids bettergrid didn't make (oldGrid)
    if (old) rows = atoi(arg2);
    startRun(!strcmp(cmd, "reanalyze"));
    if (!g_run.running) return;
    g_run.oldOnly = old;
    g_run.limit = rows;
    if (old) logf("analyze: only grids bettergrid didn't make");
}

// Every deck load (the DJ's too) in the log: when the deck could play and when rekordbox's analysis
// (grid, waveform) was in, with the analysis stages, so a slow load can be traced afterwards.
static void watchDeckLoads() {
    struct Load { std::string path; double start = 0, playable = 0; bool done = true; };
    static Load loads[8];
    double t = now();
    for (int ch = 0; ch < deckCount() && ch < 8; ch++) {
        Load& d = loads[ch];
        std::string p = deckPath(ch);
        if (p != d.path) {
            d = Load{p, t, 0, p.empty()};
            if (!p.empty()) logf("deck %d: loading %s", ch + 1, p.c_str());
            continue;
        }
        if (d.done) continue;
        if (!d.playable && deckLoaded(ch) && !deckLoading(ch)) {
            d.playable = t;
            logf("deck %d: playable after %.1fs", ch + 1, t - d.start);
        }
        // The analysis can start a moment before the deck reports the new path.
        std::string timing = analysisTiming(p, d.start - 5, d.start);
        if (g_completedAt[ch] > d.start) {
            d.done = true;
            logf("deck %d: analysed after %.1fs [%s]", ch + 1, t - d.start, timing.c_str());
        } else if ((d.playable && timing.empty() && t - d.playable > 60) || t - d.start > 300) {
            d.done = true;
            logf("deck %d: %s after %.0fs [%s]", ch + 1, timing.empty() ? "no analysis" : "analysis not finished", t - d.start,
                 timing.c_str());
        }
    }
}

static void tick() {
    watchDeckLoads();
    static bool started;
    if (!started) {
        started = true;
        if (cfgInt("analyze_reopen", 1)) g_reopenUntil = now() + 120;
    }
    reopenPlaylist();
    readCommand();
    g_fast = cfgInt("analyze_fast", 0) != 0;
    if (g_run.running) readSpotifyLog();  // fallback only
    // Forget decks this mod loaded once they hold something else; re-eject ours if they came back.
    double t0 = now();
    for (int ch = 0; ch < deckCount() && ch < 8; ch++) {
        if (g_owned[ch].empty()) continue;
        std::string p = deckPath(ch);
        if (!p.empty() && p != g_owned[ch]) { g_owned[ch].clear(); g_ejectUntil[ch] = 0; continue; }
        bool busy = false;
        for (Slot& s : g_run.slots) busy |= s.ch == ch && s.row >= 0;
        if (!busy && t0 < g_ejectUntil[ch] && t0 - g_ejectedAt[ch] >= 2 && !deckPlaying(ch) &&
            (!p.empty() || deckLoaded(ch) || deckLoading(ch) || deckHasSong(ch))) {
            g_ejectedAt[ch] = t0;
            logf("analyze: deck %d still holds the analysed track (loaded %d, loading %d), ejecting again", ch + 1,
                 deckLoaded(ch), deckLoading(ch));
            deckUnload(ch);
        }
    }
    static double uiAt;
    // (a wait for held tracks counts down live: more often then)
    if (analyzer::windowVisible() && t0 - uiAt >= (g_run.holdWaitGb > 0 ? 0.2 : 0.5)) { uiAt = t0; analyzer::refreshWindow(); }
    // A long gap between ticks means the Mac slept: don't count it against the tracks being analysed.
    static double lastTick;
    double slept = lastTick ? t0 - lastTick : 0;
    lastTick = t0;
    if (slept > 10)
        for (auto& l : g_leftoverIds) l.second.until += slept;
    pruneLeftoverIds(t0);
    if (!g_run.running) return;
    if (slept > 10) {
        logf("analyze: %.0fs without a tick (the Mac slept?), not counting it", slept);
        for (Slot& s : g_run.slots) s.startedAt += slept, s.importStartedAt += slept, s.clockFrom += slept;
    }
    // A run writes analysis files for every track: stop it before the disk fills up (a full disk
    // made the Mac fail to wake from hibernation on 2026-11-12).
    static double diskAt;
    if (!g_run.stopping && t0 - diskAt > 10) {
        diskAt = t0;
        uint64_t free = plat::diskFree(plat::configDir());
        // A run writes only rekordbox's analysis files: 1 GB is room enough, unless another mod writes more meanwhile.
        double need = cfgNum("analyze_min_free_gb", 5);
        std::string hint;
        auto diskUse = service<bool (*)(std::string*)>("analyzer.diskUse");
        if (!(diskUse && diskUse(&hint))) need = std::min(need, 1.0), hint.clear();
        if (free > 0 && free < (uint64_t)(need * 1e9)) {
            logf("analyze: only %.1f GB free on the disk", free / 1e9);
            char why[240];
            snprintf(why, sizeof why, "The disk is nearly full: %.1f GB free, an analysis needs %g GB (analyze_min_free_gb%s). "
                     "Free some space, then analyse again.", free / 1e9, need, hint.c_str());
            stopRun("disk nearly full", why);
        }
    }
    void* lv = browserListViewer();
    if (!lv) lv = mainListViewer();
    double t = now();

    // In-flight loads finish independently of what the browser shows.
    int busy = 0;
    for (Slot& s : g_run.slots) {
        if (s.row < 0) continue;
        if (g_run.stopping && t - g_run.stoppingSince > 30) { putBack(s); continue; }
        if (deckPlaying(s.ch)) {  // the DJ is using this deck now: leave it alone
            logf("analyze: deck %d started playing, not using it any more (row %d will be retried)", s.ch + 1, s.row);
            g_run.retry.push_back(s.row);
            setState(s.row, State::Retry);
            if (s.force) setForce(s.uri, false);
            setOurs(s.uri, false);  // (the DJ's own load of it is analysed in full)
            g_owned[s.ch].clear();
            s.row = -1;
            continue;
        }
        if (!slotFinished(s, t) && s.row >= 0) busy++;
    }

    if (g_run.stopping) {
        if (!busy) finishRun("stopped");
        return;
    }

    int rows = listRows(lv);
    // (a selection: the list that shows its tracks is the one of the playlist it was taken from)
    bool onPlaylist = g_run.playlistId.empty() || shownPlaylist() == playlistParent(g_run.playlistId);
    if (g_run.waitingForList && g_run.selection) {
        g_run.waitingForList = false;
        collectSelection(onPlaylist ? lv : nullptr, rows);
        if (g_run.paths.empty()) {
            g_run.stopWhy = "The selection's tracks aren't known any more (it is from an earlier session). Select them in rekordbox again.";
            finishRun("empty list");
            return;
        }
        logf("analyze: %d selected track(s): %s", (int)g_run.paths.size(), playlistName(g_run.playlistId).c_str());
        if (onPlaylist) savePlaylist();
    } else if (g_run.waitingForList) {
        // Let the newly opened playlist replace the old list, then wait for its row count to settle.
        if (rows != g_run.lastRows || !onPlaylist) { g_run.lastRows = rows; g_run.stableSince = t; }
        bool settled = t - g_run.startedAt > 2.5 && rows > 0 && t - g_run.stableSince > 1.5;
        if (!settled && t - g_run.startedAt < 30) return;
        g_run.waitingForList = false;
        if (!onPlaylist) {  // another list is shown: go on with the playlist's tracks as recorded
            PlaylistTracks pl = playlistTracks(g_run.playlistId);
            if (pl.uris.empty()) {
                g_run.stopWhy = "rekordbox didn't show the playlist, and its tracks aren't known yet. Open it in rekordbox, then analyse again.";
                finishRun("playlist not shown");
                return;
            }
            for (auto& u : pl.uris)
                if (!u.empty() && g_run.pathSet.insert(u).second) g_run.paths.push_back(u);
            logf("analyze: the playlist isn't shown, using its %d recorded tracks", (int)g_run.paths.size());
        } else {
            if (rows <= 0) {
                g_run.stopWhy = "The list is empty.";
                finishRun("empty list");
                return;
            }
            collectUris(lv, rows);
            g_run.collectedAt = t;
            logf("analyze: list has %d rows", rows);
            savePlaylist();
        }
    }

    // Without decks the run goes on while another list is shown (tracks are queued by URI); only tracks
    // that can't be imported yet (no list has shown their row) wait for the playlist.
    bool same = lv && onPlaylist;
    if (same != g_run.shown) {
        g_run.shown = same;
        logf("analyze: %s", same ? "the playlist is shown again" : g_run.direct ? "another list is shown, going on" : "paused (another list is shown)");
    }
    if (!same && !g_run.direct) { g_run.paused = true; return; }
    if (same && !g_run.selection && t - g_run.collectedAt >= 1) g_run.collectedAt = t, collectUris(lv, rows);
    rows = (int)g_run.paths.size();
    if (!same) lv = nullptr;
    g_run.rows = rows;
    g_run.paused = false;
    if (g_run.userPaused) { g_run.idleSince = 0; return; }

    // Start one new analysis per tick. With decks that needs a free empty deck, a free Spotify stream and
    // the gap elapsed; directly it needs a free slot (rekordbox queues it and reads it).
    // (nextRow queries the database, so only once a load could actually start.)
    Slot* free = nullptr;
    int active = 0;
    for (Slot& s : g_run.slots) {
        if (s.row >= 0) {
            active++;
            continue;
        }
        if (!g_run.direct && (deckPlaying(s.ch) || deckLoaded(s.ch) || deckLoading(s.ch))) continue;
        if (!free) free = &s;
    }
    // While a deck plays the run analyses one track at a time (analyze_live; 0: none until the decks have stopped):
    // rekordbox's beat detection takes every core for seconds, and one analysis alone already made the audio thread
    // miss cycles (2-6 CoreAudio overloads) and held the main thread up for 0.3 s; six at once is for when nobody plays.
    static bool liveSaid;
    bool live = slowdrive::deckPlays();
    if (live && active >= std::max(0, cfgInt("analyze_live", 1))) free = nullptr;
    if (live != liveSaid) {
        liveSaid = live;
        if (live) logf("analyze: a deck plays: %d track(s) at a time until the decks have stopped", std::max(0, cfgInt("analyze_live", 1)));
    }
    double gap = g_run.direct ? 0.2 : cfgNum("analyze_gap", 4);
    bool canLoad = free && t - g_run.lastLoadAt >= gap;
    // A track another mod holds back waits while it does; the window says the run waits for it only once nothing else
    // can start (pickRow).
    double waitGb = 0;
    bool looking = false;
    int row = canLoad ? pickRow(lv, rows, &waitGb, &looking) : rowsLeft(rows) ? 0 : -1;
    std::string uri;
    if (!free) g_run.holdWaitGb = 0;  // waiting for a slot or a deck
    else if (canLoad) g_run.holdWaitGb = waitGb;
    if (g_run.holdWaitGb > 0) {  // for the window: how far the wait is
        double toGo = 0;
        holdProgress(&toGo, &g_run.holdWhat, &g_run.holdStep);
        if (g_run.holdTotalMb <= 0) g_run.holdTotalMb = toGo;
        else if (toGo > g_run.holdToGoMb + 1) g_run.holdTotalMb += toGo - g_run.holdToGoMb;  // more came meanwhile
        g_run.holdToGoMb = toGo;
    } else {
        g_run.holdTotalMb = g_run.holdToGoMb = 0;
        g_run.holdWhat.clear(), g_run.holdStep.clear();
    }
    if (waitGb > 0 && t - g_run.blockedLogAt > 60)
        g_run.blockedLogAt = t, logf("analyze: waiting for %s to let held tracks go (%.1f GB to go, %d track%s held back)",
                                     providerOf("analyzer.holdBack"), waitGb, (int)g_run.held.size(), g_run.held.size() == 1 ? "" : "s");
    if (canLoad && row >= 0) {
        uri = runUri(lv, row);
        // Spotify serves only a few streams at once; more and the analysis gets no audio.
        bool needsStream = isSpotify(uri.c_str()) && (!g_run.direct || !trackFile(uri.c_str(), nullptr));
        int streams = 0;
        if (needsStream && !g_run.direct) streams = spotifyStreams();
        if (needsStream && g_run.direct)
            for (Slot& s : g_run.slots) streams += s.row >= 0 && s.streams;
        int limitLeft = needsStream ? spotifyLimitLeft() : 0;
        if (free && streams < cfgInt("analyze_streams", 3) && !limitLeft) {
            takeRow(row);
            bool retrying = info(row).state == State::Retry;
            *free = Slot{free->ch};
            free->row = row;
            free->retrying = retrying;
            free->uri = uri;
            free->force = g_run.force;
            free->streams = needsStream;
            free->startedAt = free->timingFrom = g_run.lastLoadAt = t;
            setOurs(uri, true);
            info(row).state = State::Loading;
            info(row).deck = free->ch;
            info(row).startedAt = t;
            info(row).took = 0;
            if (free->force) setForce(uri, true);
            if (g_run.direct) {
                unsigned id = 0;
                int at = lv ? findRow(lv, uri) : -1;
                Queue q = analyzeWithoutDeck(uri, lv, at >= 0 ? (void*)listRow(lv, at) : nullptr, &id);
                if (q == Queue::Queued) {
                    free->id = id;
                    logf("analyze: analysing %s directly (id %u)", uri.c_str(), id);
                    busy++;
                } else if (q == Queue::Importing) {
                    free->importing = true;
                    free->importStartedAt = t;
                    busy++;
                } else {
                    logf("analyze: %s is not in the collection, cannot analyse it without a deck", uri.c_str());
                    g_run.failed++;
                    setState(row, State::Failed);
                    ejectSlot(*free);
                }
            } else {
                g_owned[free->ch].clear();
                int at = findRow(lv, uri);
                if (at >= 0) {
                    listLoadTrack(lv, free->ch, at);
                    busy++;
                } else {  // (it was loaded as row 0: the list's first track got analysed and this row counted as done)
                    logf("analyze: %s is no longer in the list shown, cannot load it onto a deck", uri.c_str());
                    g_run.failed++;
                    setState(row, State::Failed);
                    ejectSlot(*free);
                }
            }
        } else if (limitLeft) {
            if (t - g_run.blockedLogAt > 30) g_run.blockedLogAt = t, logf("analyze: waiting %ds for Spotify's streaming limit", limitLeft);
        } else if (!busy && t - g_run.blockedLogAt > 30) {
            g_run.blockedLogAt = t;
            logf("analyze: waiting (Spotify track limit reached on the other decks)");
        }
    } else if (!g_run.direct && !free && !busy && row >= 0 && t - g_run.blockedLogAt > 30) {
        g_run.blockedLogAt = t;
        logf("analyze: waiting (no empty analysis deck)");
    }

    // Finished when every row is handled and nothing is in flight; streaming lists page in as they
    // scroll, so give a possible next page a moment first.
    if (row >= 0 || busy || looking || !g_run.held.empty()) { g_run.idleSince = 0; return; }
    if (!g_run.retryOnly && !g_run.needList.empty()) {  // the rest waits for the playlist to be shown
        g_run.paused = true;
        g_run.idleSince = 0;
        return;
    }
    if (g_run.idleSince == 0) g_run.idleSince = t;
    if (t - g_run.idleSince >= 5) {
        const char* why = retryEnd();
        if (!strcmp(why, "done")) g_run.stopWhy.clear();  // (a stopped run's reason, kept by retryFailed)
        finishRun(why);
    }
}

// ---- window API ---------------------------------------------------------------------------

namespace analyzer {

static std::string cnnText(const std::string& uri, const char* ownKey) {
    std::string k = trackNote(uri, "cnn_key");
    if (k.empty()) return "";
    int i = atoi(k.c_str()), own;
    bool minor, camelot = true;
    parseKey(ownKey, &own, &minor, &camelot);
    char buf[48];
    snprintf(buf, sizeof buf, "%s  %.0f%%", formatKey((9 + i % 12) % 12, i >= 12, camelot).c_str(),
             100 * atof(trackNote(uri, "cnn_prob").c_str()));
    return buf;
}

View snapshot() {
    View v;
    v.exists = g_run.exists;
    if (!v.exists) return v;
    v.playlistId = g_run.playlistId;
    v.playlistName = playlistName(g_run.playlistId);
    v.isRun = true;
    v.shown = g_run.shown;
    v.running = g_run.running;
    v.paused = g_run.paused;
    v.userPaused = g_run.userPaused;
    v.stopping = g_run.stopping;
    v.force = g_run.force;
    v.limitLeft = g_run.running ? spotifyLimitLeft() : 0;
    v.holdWaitGb = g_run.running ? g_run.holdWaitGb : 0;
    v.holdToGoMb = g_run.holdToGoMb, v.holdTotalMb = g_run.holdTotalMb, v.holdWhat = g_run.holdWhat, v.holdStep = g_run.holdStep;
    v.finishedWhy = g_run.finishedWhy;
    v.stopWhy = g_run.stopWhy;
    v.done = g_run.done;
    v.skipped = g_run.skipped;
    v.failed = g_run.failed;
    double t = now();
    v.elapsed = (g_run.running ? t : g_run.lastActivity) - g_run.startedAt;
    int n = std::max((int)g_run.paths.size(), (int)g_run.info.size());
    if (g_run.limit > 0) n = std::min(n, g_run.limit);
    v.rows = n;
    for (int r = 0; r < n; r++) {
        TrackInfo i = r < (int)g_run.info.size() ? g_run.info[r] : TrackInfo();
        TrackView tv{r, i.state, i.deck, i.took, i.title, i.artist, r < (int)g_run.paths.size() ? g_run.paths[r] : ""};
        tv.bpm = 0;
        if (!tv.uri.empty()) {
            const Known& k = known(tv.uri, i.state == State::Loading || i.state == State::Analysing);
            if (k.inCollection) {
                tv.bpm = k.c.bpm;
                tv.key = k.c.key;
                if (k.c.analysed) v.analysed++;
                if (tv.title.empty()) tv.title = k.c.title, tv.artist = k.c.artist;
            }
            tv.genre = k.genre, tv.genreHeard = k.genreHeard;
            if (tv.title.empty()) {
                library::Track lt;
                if (library::track(tv.uri, &lt)) tv.title = lt.title, tv.artist = lt.artist;
            }
        }
        tv.cnnKey = tv.uri.empty() ? "" : cnnText(tv.uri, tv.key.c_str());
        if (i.state == State::Loading || i.state == State::Analysing) v.active++;
        // (a "Retry failed" run leaves the waiting rows waiting: only its retries are to go)
        if (i.state == State::Retry || (i.state == State::Waiting && !(g_run.running && g_run.retryOnly))) v.waiting++;
        v.tracks.push_back(tv);
    }
    int handled = v.done + v.failed;
    if (g_run.running && !g_run.paused && handled >= 2 && v.waiting + v.active > 0)
        v.eta = v.elapsed / handled * (v.waiting + v.active);
    return v;
}

// A playlist outside the run: its recorded tracks and what the collection knows about them.
View playlistView(const std::string& id) {
    if (id.empty() || (g_run.exists && id == g_run.playlistId)) return snapshot();
    View v;
    v.playlistId = id;
    PlaylistTracks pl = playlistTracks(id);
    v.playlistName = pl.name;
    v.known = pl.known;
    v.shown = pl.shown;
    v.rows = (int)pl.uris.size();
    for (int r = 0; r < v.rows; r++) {
        const std::string& uri = pl.uris[r];
        TrackView tv{r, State::NotAnalysed, -1, 0, "", "", uri, 0, "", ""};
        if (!uri.empty()) {
            const Known& k = known(uri);
            tv.genre = k.genre, tv.genreHeard = k.genreHeard;
            if (k.inCollection) {
                tv.title = k.c.title, tv.artist = k.c.artist, tv.bpm = k.c.bpm, tv.key = k.c.key;
                if (k.c.analysed) tv.state = State::Analysed, v.analysed++;
            }
            if (tv.title.empty()) {
                library::Track lt;
                if (library::track(uri, &lt)) tv.title = lt.title, tv.artist = lt.artist;
            }
            tv.cnnKey = cnnText(uri, tv.key.c_str());
        }
        if (tv.state == State::NotAnalysed) v.waiting++;
        v.tracks.push_back(tv);
    }
    return v;
}

static std::vector<std::string> g_windowLists;  // opened in the window this session

void addWindowPlaylist(const std::string& id) {
    if (!id.empty() && std::find(g_windowLists.begin(), g_windowLists.end(), id) == g_windowLists.end())
        g_windowLists.push_back(id);
}

std::vector<PlaylistEntry> playlistEntries() {
    std::vector<PlaylistEntry> out;
    auto add = [&](const std::string& id) {
        for (auto& e : out) if (e.id == id) return;
        View v = playlistView(id);  // cheap enough: the collection lookups are cached
        PlaylistEntry e;
        e.id = id;
        e.name = v.playlistName;
        e.running = v.isRun && v.running;
        e.tracks = v.rows;
        e.analysed = v.analysed;
        out.push_back(e);
    };
    if (g_run.exists && !g_run.playlistId.empty()) add(g_run.playlistId);
    for (auto& id : g_windowLists) add(id);
    return out;
}

std::string runPlaylist() { return g_run.exists ? g_run.playlistId : ""; }

void analyzePlaylist(const std::string& id, bool force) {
    if (g_run.running || id.empty()) return;
    showPlaylistId(id);
    startRun(force, id);
}

void setUserPaused(bool paused) {
    if (g_run.userPaused != paused) logf("analyze: %s by the user", paused ? "paused" : "resumed");
    g_run.userPaused = paused;
}

void stop() { stopRun("stopped"); }

bool fastMode() { return cfgInt("analyze_fast", 0) != 0; }
void setFastMode(bool on) {
    setCfg("analyze_fast", on ? "1" : "0");
    g_fast = on;
    logf("analyze: fast mode %s", on ? "on" : "off");
}

// "Genre first": the genres mod's switch (its window has it too). The model listens to a track when bettergrid asks
// for its genre, before the grid's tempo is picked; with the switch on, what it heard also fills an empty Genre field.
bool hasGenres() { return service<void*>("genres.forTrack") != nullptr; }
bool genreFirst() { return cfgInt("genres_on_analyse", 0) != 0; }
void setGenreFirst(bool on) {
    setCfg("genres_on_analyse", on ? "1" : "0");
    logf("analyze: genre first %s", on ? "on" : "off");
}

void retryFailed() {
    int n = 0;
    for (int r = 0; r < (int)g_run.info.size(); r++)
        if (g_run.info[r].state == State::Failed) {
            g_run.info[r].state = State::Retry;
            g_run.retry.push_back(r);
            g_run.retried.erase(r);
            n++;
        }
    if (!n) return;
    g_run.failed -= n;
    logf("analyze: retrying %d failed track%s", n, n == 1 ? "" : "s");
    if (!g_run.running) {
        // Retry only what failed (and what was waiting for its retry): the rows a stop left are left as they are
        // (Analyze goes on with those). stopWhy stays for when it ends stopped again (hidden while running).
        g_run.retryOnly = true;
        g_run.resumeWhy = g_run.finishedWhy;
        for (int r : g_run.retry) info(r).state = State::Retry;
        g_run.slots.clear();
        if (g_run.direct) {
            for (int i = 0, n = std::max(1, cfgInt("analyze_parallel", 6)); i < n; i++) g_run.slots.push_back(Slot{-1});
        } else {
            for (int ch : configuredDecks()) g_run.slots.push_back(Slot{ch});
        }
        g_run.running = true;
        g_run.stopping = g_run.userPaused = false;
        g_run.idleSince = 0;
        g_run.startedAt += now() - g_run.lastActivity;  // elapsed time and the ETA leave out the time it stood still
        g_run.finishedWhy.clear();
    }
}

}  // namespace analyzer

// The window has the actions (analyse, re-analyse, pause, stop).
static void playlistMenu(Menu* menu, void* treeItem) {
    menuAddItem(menu, "Open analysis window", [treeItem] { analyzer::showWindow(treeItem); });
}

// The track list's menu: analyse the selected tracks again (Spotify ones too, which rekordbox's own "Analyze Track"
// refuses), as a run on the selection, and the window on them (analyse the ones not analysed yet, retry, ...).
static void trackMenu(Menu* menu, const std::vector<std::string>& uris, const std::string& selection) {
    if (!g_run.running) {  // one run at a time; the window shows the one going on
        std::string text = uris.size() == 1 ? "Re-analyze track" : "Re-analyze " + tracksText(uris.size());
        menuAddMainItem(menu, text.c_str(), [selection] { analyzer::analyzePlaylist(selection, true); });
    }
    menuAddItem(menu, menuWindowItem("analysis window", uris.size()).c_str(), [selection] { analyzer::showWindowFor(selection); });
}

// For other mods (see rbmod.h, "between mods"): starting and following a run.
static bool provideStatus(std::string* runPlaylist, int* finished, int* rows) {
    analyzer::View v = analyzer::snapshot();
    if (runPlaylist) *runPlaylist = analyzer::runPlaylist();
    if (finished) *finished = v.done + v.skipped + v.failed;
    if (rows) *rows = v.rows;
    return v.running;
}
static void provideAnalyzePlaylist(const char* id, bool force) { analyzer::analyzePlaylist(id ? id : "", force); }
static void provideStop() { analyzer::stop(); }
// Whether a run of this mod is analysing the track (any thread).
static bool provideAnalysing(const char* uri) {
    if (!uri) return false;
    std::lock_guard<std::mutex> l(g_oursLock);
    return g_ours.count(uri) != 0;
}
RBM_PROVIDE("analyzer", "analyzer.analysing", provideAnalysing, &g_on);
RBM_PROVIDE("analyzer", "analyzer.status", provideStatus, &g_on);
RBM_PROVIDE("analyzer", "analyzer.analyzePlaylist", provideAnalyzePlaylist, &g_on);
RBM_PROVIDE("analyzer", "analyzer.stop", provideStop, &g_on);

// The tracks this mod queues are read while Spotify still downloads them (skipDownloadWait): their readers have to
// keep up with the download, or the end of the track is analysed as silence.
static void init() {
    if (!spotifyReadersKeepUp())
        logf("analyzer: in this rekordbox build a Spotify track analysed from the stream can lose its end (its readers can't follow the download)");
}

RBM_MOD({.name = "analyzer", .init = init, .tick = tick, .playlistMenu = playlistMenu, .trackMenu = trackMenu, .enabled = &g_on, .menuOrder = 10});
