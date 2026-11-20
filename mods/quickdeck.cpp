// quickdeck: a Spotify (or any unanalysed) track on a deck shows its waveforms and a beat grid as soon as the analysis
// has them, instead of only when the whole analysis is done.
//
// rekordbox's analysis of a loaded track runs: the audio pass (it builds every waveform while the stream downloads),
// the beat job (madmom; 0.5-1 s per minute of audio), phrase, vocal and key analysis; only then does it write the
// colour and 3-band waveforms and tell the deck (UiPlayer::handleDBMessageTrackStatusWhenAnalyzeCompleted, which
// reloads the track's analysis files). Measured on Spotify loads: audio pass ~2-3 s, beat job ~1.5-4 s, the rest ~0.6 s.
// This makes the deck reload at three earlier points (the same call rekordbox makes at the end):
//  1. when the beat job starts: the colour and 3-band waveforms are handed over then (as analyze() does at the end,
//     with the same data), so the deck shows every waveform right after the audio pass;
//  2. when bettergrid's Beat This! grid is ready (it runs next to the beat job and is usually done 1-3 s sooner):
//     saved the way the beat job saves its grid, as a first grid (only when Beat This! is sure of it);
//  3. when rekordbox's beat job has saved its grid (bettergrid's final one), before phrase and key analysis.
// A track's first analysis has no analysis file path in the database until it has finished, so during these reloads the
// deck is pointed at the file the analysis created. The reloads only happen for a track that is on a deck.
//   config: quickdeck=0 to disable
//           quickdeck_merge=0.25  Windows: seconds the waveforms wait for rekordbox's own grid, so that one redraw of the deck shows both (0: no wait)
#include "../core/rbmod.h"
#include "../core/plat.h"
#include <string.h>
#include <map>
#include <mutex>

using namespace rbm;

static volatile bool g_on = true;

static const Addr A_getUiPlayer{"__ZN6browse14PlayerAccessor11getUiPlayerEN6djplay9ePlayerIDE"};
static const Addr A_completed{"__ZN6djplay8UiPlayer46handleDBMessageTrackStatusWhenAnalyzeCompletedEjlN2db26DBMessageToPlayerParameterE"};
static const Addr A_bigInt{"__ZN4juce10BigIntegerC1Ev"};
static const Addr A_bigIntFree{"__ZN4juce10BigIntegerD1Ev"};
// UiPlayer: content id of the loaded track (handleDBMessageTrackStatus compares the message's id with it first)
static const Offset O_playerTrack{0x580, "__ZN6djplay8UiPlayer26handleDBMessageTrackStatusEjlN2db26DBMessageToPlayerParameterE", "ldr x0", 0};
// CBeatAnalyzeThread and its WaveCreator, as analyze() uses them when it hands over the colour waveforms at the end
static const Addr A_wholeWave{"__ZN8analyzer11WaveCreator16getWholeWaveDataEv"};
static const Addr A_colorZoomWave{"__ZN8analyzer11WaveCreator20getColorZoomWaveDataEv"};
static const Addr A_3bandsWholeWave{"__ZN8analyzer11WaveCreator22get3BandsWholeWaveDataEv"};
static const Addr A_3bandsZoomWave{"__ZN8analyzer11WaveCreator21get3BandsZoomWaveDataEv"};
static const Addr A_normalizeCoef{"__ZN8analyzer11WaveCreator20getWaveNormalizeCoefEv"};
static const Addr A_notifyWhole{"__ZN8analyzer18CBeatAnalyzeThread11notifyWholeEPNS0_17BA_WholeWaveInfo_Eiiif"};
#ifndef _WIN32
static const Addr A_notifyColorZoom{"__ZN8analyzer18CBeatAnalyzeThread15notifyColorZoomEPNS_13ColorZoomWaveEi"};
#endif
static const Addr A_notifyBeat{"__ZN8analyzer18CBeatAnalyzeThread10notifyBeatERKN4juce5ArrayINS_16DetailedBeatInfoENS1_20DummyCriticalSectionELi0EEE"};
#ifndef _WIN32
static const Addr A_notify3BandsZoom{"__ZN8analyzer18CBeatAnalyzeThread16notify3BandsZoomEPNS_18ThreeBandsWaveInfoEi"};
#endif
static const char* const kAN = "__ZN8analyzer18CBeatAnalyzeThread7analyzeEv";
static const Offset O_waveCreator{0x3970, kAN, "ldr x20", 94};    // CBeatAnalyzeThread: WaveCreator*
static const Offset O_wantWhole{0x27c, kAN, "ldr x20", 90};       // bools: colour whole, colour zoom, 3-band whole, 3-band zoom
static const Offset O_wantColorZoom{0x27d, kAN, "ldr x20", 91};
static const Offset O_want3BandsWhole{0x27e, kAN, "ldr x20", 92};
static const Offset O_want3BandsZoom{0x27f, kAN, "ldr x20", 93};
static const Offset O_listener{0x2a0, "__ZN8analyzer18CBeatAnalyzeThread10notifyDotsEP11BA_WaveInfoiii", "ldr x0", 0};  // its listener (CAnalyzerIF)
static const Offset O_listenerCtx{0x2a8, "__ZN8analyzer18CBeatAnalyzeThread10notifyLoudEP11BA_WaveInfoiii", "ldr x22", 1};  // and the listener's context
static const Offset O_colorZoomCount{0x670, kAN, "ldr x8", 172};   // WaveCreator: colour zoom points
static const Offset O_3bandsZoomCount{0x8550, kAN, "movz x9", 27};  // WaveCreator: 3-band zoom points
static const Offset V_3bandsWhole{0x70, kAN, "ldr x8", 174};        // listener vtable: notify_3bands_whole_wave(ctx, data, n)
static const Offset V_normalizeCoef{0x80, kAN, "ldr x8", 176};      // listener vtable: notify_3bands_normalize_coef(ctx, coef*)
#ifdef _WIN32
// MSVC inlined notifyColorZoom and notify3BandsZoom into analyze(): call the listener's slots as they do
// (tools/win/verify/quickdeck.out.json)
static const Offset V_colorZoom{0x50, kAN, "win:listener colorZoom slot", 0};    // notify_color_zoom_wave(ctx, data, n)
static const Offset V_3bandsZoom{0x70, kAN, "win:listener 3bandsZoom slot", 0};  // notify_3bands_zoom_wave(ctx, data, n)
#endif

namespace {
struct BigInt { alignas(16) unsigned char b[64]; };  // juce::BigInteger (40 bytes)
}

// A track's first analysis: the database gets the analysis file's path only when the analysis has finished, so until then
// the deck's reload would look for "USBANLZ///ANLZ0000.DAT". The analysis thread notes the file it creates, and the reload
// reads that one instead.
static thread_local std::string t_created;  // analysis thread: the ANLZ0000.DAT it created
static thread_local double t_createdAt;
static thread_local const char* t_reloadPath;  // main thread, during a reload: the file to read
static std::mutex g_lock;
static std::map<std::string, std::string> g_files;  // track URI -> its analysis file, while analysed
static std::map<std::string, int> g_stage;  // track URI -> 1 waveforms handed over, 2 rekordbox's grid saved
static std::map<std::string, double> g_started;  // track URI -> when its analysis started (for the log)

extern "C" int rbm_hook_qd_create(void* self, const JStr* path, int* handle) {
    int r = RBM_ORIG(qd_create, int (*)(void*, const JStr*, int*))(self, path, handle);
    if (path && path->text) t_created = path->text, t_createdAt = now();
    return r;
}

extern "C" void* rbm_hook_qd_anlzPath(void* self, const JStr* path, unsigned flags) {
    // (the row's path is the full one, e.g. …/share/PIONEER/USBANLZ/9ed/442c6-2a23-…/ANLZ0000.DAT; "" before the first
    // analysis has finished, which DataPath turns into …/USBANLZ///ANLZ0000.DAT)
    if (t_reloadPath && path && (!path->text || !*path->text)) {
        JStr use(t_reloadPath);
        return RBM_ORIG(qd_anlzPath, void* (*)(void*, const JStr*, unsigned))(self, &use, flags);
    }
    return RBM_ORIG(qd_anlzPath, void* (*)(void*, const JStr*, unsigned))(self, path, flags);
}

#ifdef _WIN32
// Tracks whose waveforms wait for a grid's reload (see stage()), and when the wait began.
static std::map<std::string, double> g_waveWaits;  // (g_lock)
static bool takeWaveWait(const std::string& uri) {
    std::lock_guard<std::mutex> l(g_lock);
    return g_waveWaits.erase(uri) != 0;
}
#endif

static void reload(int ch, const std::string& uri, const char* why) {
    if (deckPath(ch) != uri) return;
    char* player = (char*)at<void* (*)(int)>(A_getUiPlayer)(ch);
    if (!player) return;
    unsigned id = *(unsigned*)(player + O_playerTrack);
    if (!id) return;
    double t = now();
    std::string file;
    {
        std::lock_guard<std::mutex> l(g_lock);
        auto it = g_files.find(uri);
        if (it != g_files.end()) file = it->second;
    }
    BigInt param;
    at<void (*)(void*)>(A_bigInt)(&param);
    t_reloadPath = file.empty() ? nullptr : file.c_str();
    at<void (*)(void*, unsigned, long, void*)>(A_completed)(player + thisDelta(A_completed), id, 2, &param);
    t_reloadPath = nullptr;
#ifndef _WIN32
    at<void (*)(void*)>(A_bigIntFree)(&param);  // (MSVC: the callee destroys a by-value argument itself)
#endif
    double since = -1;
    {
        std::lock_guard<std::mutex> l(g_lock);
        auto it = g_started.find(uri);
        if (it != g_started.end()) since = now() - it->second;
        if (strstr(why, "with the beat grid") || strstr(why, "and the beat grid")) g_files.erase(uri), g_started.erase(uri);
    }
    logf("quickdeck: deck %d shows %s %.1f s after the analysis started (%.0f ms, %.2f BPM)", ch + 1, why, since, (now() - t) * 1000,
         deckBpm(ch, false));
}

// The audio pass has built every waveform (WaveCreator::createWave), and rekordbox has written the blue ones; the colour
// and 3-band ones it only hands over at the very end of the analysis. Hand them over now, on the analysis thread, as
// analyze() does then (it does so again at the end, with the same data).
static void colourWavesNow(const char* thread) {
    char* wc = *(char* const*)(thread + O_waveCreator);
    if (!wc) return;
    if (thread[O_wantWhole])
        if (void* d = at<void* (*)(void*)>(A_wholeWave)(wc))
            at<void (*)(const void*, void*, int, int, int, float)>(A_notifyWhole)(thread, d, 0, 1200, 1200, 32768.0f);
    void* listener = *(void* const*)(thread + O_listener);
    void* ctx = *(void* const*)(thread + O_listenerCtx);
#ifdef _WIN32
    auto slot = [&](size_t off) { return (void (*)(void*, void*, void*, int))(*(void***)listener)[off / 8]; };
#endif
    if (thread[O_wantColorZoom])
        if (void* d = at<void* (*)(void*)>(A_colorZoomWave)(wc)) {
#ifdef _WIN32
            if (listener) slot(V_colorZoom)(listener, ctx, d, *(int*)(wc + O_colorZoomCount));
#else
            at<void (*)(const void*, void*, int)>(A_notifyColorZoom)(thread, d, *(int*)(wc + O_colorZoomCount));
#endif
        }
    if (thread[O_want3BandsWhole])
        if (void* d = at<void* (*)(void*)>(A_3bandsWholeWave)(wc)) {
            if (listener) {
                void** vt = *(void***)listener;
                ((void (*)(void*, void*, void*, int))vt[V_3bandsWhole / 8])(listener, ctx, d, 1200);
                unsigned char buf[8] = {};  // WaveNormalizeCoef {u32, u16}
#ifdef _WIN32
                at<void* (*)(void*, void*)>(A_normalizeCoef)(wc, buf);  // MSVC returns it through a pointer
#else
                uint64_t coef = at<uint64_t (*)(void*)>(A_normalizeCoef)(wc);
                memcpy(buf, &coef, 6);
#endif
                ((void (*)(void*, void*, void*))vt[V_normalizeCoef / 8])(listener, ctx, buf);
            }
        }
    if (thread[O_want3BandsZoom])
        if (void* d = at<void* (*)(void*)>(A_3bandsZoomWave)(wc)) {
#ifdef _WIN32
            int n = *(int*)(wc + O_3bandsZoomCount);
            if (listener && n) slot(V_3bandsZoom)(listener, ctx, d, n);
#else
            at<void (*)(const void*, void*, int)>(A_notify3BandsZoom)(thread, d, *(int*)(wc + O_3bandsZoomCount));
#endif
        }
}

// timing's analysis stages: "beat job" starts once the waveforms are written, "phrase" once the grid is.
static void stage(const void* thread, const char* uri, const char* stage) {
    if (!g_on || !uri || !*uri) return;
    if (!strcmp(stage, "start")) {
        std::lock_guard<std::mutex> l(g_lock);
        for (auto it = g_started.begin(); it != g_started.end();) it = now() - it->second > 600 ? g_started.erase(it) : std::next(it);
        g_started[uri] = now();
        return;
    }
    const char* why = !strcmp(stage, "beat job") ? "with the waveforms" : !strcmp(stage, "phrase") ? "with the beat grid" : nullptr;
    if (!why) return;
    std::string u = uri;
    if (!t_created.empty() && now() - t_createdAt < 600) {  // (created by this analysis, not one before on this thread)
        std::lock_guard<std::mutex> l(g_lock);
        g_files[u] = t_created;
    }
    if (!strcmp(stage, "phrase")) t_created.clear();
    {
        std::lock_guard<std::mutex> l(g_lock);
        if (!strcmp(stage, "phrase")) g_stage.erase(u);
        else if (!strcmp(stage, "beat job")) g_stage[u] = 1;
    }
    bool onDeck = false;
    for (int ch = 0; ch < deckCount(); ch++) onDeck |= deckPath(ch) == u;
    if (!onDeck) {
        if (!strcmp(stage, "phrase")) {  // no reload will come for it: nothing to keep
            std::lock_guard<std::mutex> l(g_lock);
            g_files.erase(u), g_started.erase(u);
        }
        return;
    }
    if (!strcmp(stage, "beat job") && thread) colourWavesNow((const char*)thread);
#ifdef _WIN32
    // Windows: rekordbox's own grid is saved a fraction of a second after the waveforms (bettergrid shows it while
    // Beat This! still runs), and every reload keeps rekordbox's window busy for about 0.15 s: the two in a row were
    // one freeze of 0.25-0.4 s where plain rekordbox has one of 0.15 s. So the waveforms wait a moment
    // (quickdeck_merge seconds, 0: no wait) and are shown by the grid's reload when that comes first.
    double wait = cfgNum("quickdeck_merge", 0.25);
    if (!strcmp(stage, "beat job") && wait > 0) {
        {
            std::lock_guard<std::mutex> l(g_lock);
            g_waveWaits[u] = now();
        }
        plat::onMainAfter(wait, [=] {
            {
                std::lock_guard<std::mutex> l(g_lock);
                if (!g_waveWaits.erase(u)) return;  // a grid's reload has shown them
            }
            for (int ch = 0; ch < deckCount(); ch++)
                if (deckPath(ch) == u) reload(ch, u, why);
        });
        return;
    }
    if (takeWaveWait(u)) why = "with the waveforms and the beat grid";
#endif
    for (int ch = 0; ch < deckCount(); ch++)
        if (deckPath(ch) == u) plat::onMain([=] { reload(ch, u, why); });
}
RBM_PROVIDE("quickdeck", "analysis.stage", stage, &g_on);

// bettergrid: Beat This!'s grid for the track (a juce::Array<DetailedBeatInfo>), ready while rekordbox's beat job still
// runs (on bettergrid's thread; rekordbox's grid isn't saved until this returns). Saved the way the beat job saves its
// grid (notifyBeat: the analysis files' PQTZ and PQT2), then shown; rekordbox's grid replaces it when the job is done.
// what: whose grid it is, for the log ("Beat This!'s"; "rekordbox's" while the analysis waits for a late Beat This!).
static void earlyGrid(const void* thread, const char* uri, const void* beats, const char* what) {
    if (!g_on || !thread || !uri || !beats) return;
    std::string u = uri;
    {
        std::lock_guard<std::mutex> l(g_lock);
        auto it = g_stage.find(u);
        if (it == g_stage.end() || it->second != 1) return;  // not waiting for rekordbox's grid (any more)
    }
    bool onDeck = false;
    for (int ch = 0; ch < deckCount(); ch++) onDeck |= deckPath(ch) == u;
    if (!onDeck) return;
    at<void (*)(const void*, const void*)>(A_notifyBeat)(thread, beats);
    std::string why = std::string("with ") + (what ? what : "a first") + " grid";
#ifdef _WIN32
    if (takeWaveWait(u)) why = "with the waveforms and " + why.substr(5);
#endif
    for (int ch = 0; ch < deckCount(); ch++)
        if (deckPath(ch) == u) plat::onMain([=] { reload(ch, u, why.c_str()); });
}
RBM_PROVIDE("quickdeck", "quickdeck.earlyGrid", earlyGrid, &g_on);

RBM_MOD({.name = "quickdeck", .enabled = &g_on});
