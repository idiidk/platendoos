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
//
// A reload that only brings a new grid (2 and 3, once 1 has shown the waveforms) redraws just the grid (macOS). In
// rekordbox's own reload, UiPlayer::handleDBMSG_subRoutine1_2 compares the new beats with the deck's, and when they
// differ it empties both waveform views and builds them again (WidgetWaveView::initializeWave without and then with
// the data): the waveform went blank and came back for every grid, three times per load. The grid editor has the
// other way, WidgetWaveView::updateBeatPosition(BeatInfo*), which swaps the beats and redraws the lines; here the
// two initializeWave calls of such a reload become that one call. Not when rekordbox finds more than the beats
// changed (another track, another waveform scale), and not for the final grid with the blue waveform style, whose
// zoomed waveform is written together with that grid.
//
// The overview waveform streams in during the audio pass (macOS). rekordbox builds the overview only when the whole
// track has been read: 2-5 s after the load in which the deck shows nothing. But its waveform builder
// (WaveCreator::readWaveData) fills arrays sized for the whole track block by block, zeros where the audio hasn't
// come yet, and the overview is a plain function of those arrays (createWholeWaveData, create3BandsWholeWaveData:
// 1200 columns). So four times a second, between two blocks, that function runs on a copy of the builder (the
// colour one deletes its input array when done, so it gets a copy of that too), the result is put in the deck's own
// form (DjplayDB::getWholeWavePoint / get3BandsWholeWavePoint: six bytes a column) and handed to the deck the way
// its reload does (UiPlayer::handleDBMSG_subRoutine1_1: the player's array, then operateLoudViewUpdate2 on its
// widgets). The overview grows from the left as the track downloads. No reload, no analysis file: the real ones
// follow when the pass is done, as before.
//
// A Spotify track on a deck is analysed while it downloads (macOS). rekordbox's analysis opens a streaming track only
// when the whole file is there: DownloadManager::syncDownload waits for the service's isDownloaded() on an analysis
// thread and for isInitialized() (the first piece) on any other, and StreamingReader::fileOpen marks an analysis
// thread's reader so that it ignores the download's progress. So nothing of the analysis starts before the last
// byte: with a slow download (10-20 s isn't rare) the deck plays for that long without a waveform. A reader waits
// for data that hasn't come yet, though (StreamingReader::fileRead polls isReadyToRead): the deck's does it all the
// time. So for a track that is on a deck, the analysis thread's open is told "downloaded" as soon as the track is
// initialized, and its reader is made an ordinary one that follows the download (the mark cleared, then updateData
// and, if the download finished meanwhile, completeData, as the download's callbacks would have). The audio pass then
// runs as far as the download has come, the overview above grows with it, and waveforms and grid are there when the
// download ends instead of a few seconds later. If the stream fails on the way, the reader hands out silence from
// there on (as it does for a deck): the analysis is cancelled then, so nothing of it is kept.
//   config: quickdeck=0 to disable
//           quickdeck_progressive=0  macOS, an experiment that is off: 1: a Spotify track's analysis starts reading
//                                    while the track downloads (see above). analyze() has a wait of its own for
//                                    the download (DataPath::IsDownloaded), which the analyzer mod lets it past
//                                    for such a track (service "quickdeck.readsEarly"). Seen working once
//                                    (2026-11-26): the overview grew during a 3 s download and the grid, tempo
//                                    and key came out as from the finished file. Not proven: that the audio it
//                                    read had no gap (two overview columns differed from the final ones), and a
//                                    failed stream or a cancelled analysis in the middle. And five seconds after
//                                    that one run Spotify answered rekordbox with 403 permission_denied and kept
//                                    it signed out for 80 minutes; whether reading during the download caused
//                                    that isn't known. Leave it off.
//           quickdeck_gridonly=1  macOS: 0: every reload builds the waveforms again, as rekordbox does
//           quickdeck_stream=1    macOS: 0: the overview waveform appears when the whole track has been read
//           quickdeck_log=0       1 logs each partial overview (columns filled, ms it took)
//           quickdeck_merge=0.25  Windows: seconds the waveforms wait for rekordbox's own grid, so that one redraw of the deck shows both (0: no wait)
#include "../core/rbmod.h"
#include "../core/plat.h"
#include <string.h>
#ifndef _WIN32
#include <pthread.h>
#endif
#include <algorithm>
#include <map>
#include <memory>
#include <mutex>
#include <vector>

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

#ifndef _WIN32
// ---- a reload that only brings a new grid -----------------------------------------------------------------------------
static const char* const kApplyBeats = "__ZN6djplay8UiPlayer25handleDBMSG_subRoutine1_2EPNS_8BeatInfoEPNS_8SongInfoENS_17WaveNormalizeCoefE";
static const Offset O_playerSong{0x4e0, kApplyBeats, "ldr x19", 0};  // UiPlayer: SongInfo* of the loaded track
static const Offset O_playerCoef{0x4f0, kApplyBeats, "ldr x19", 3};  // WaveNormalizeCoef: three floats
static const Offset O_songId{0x8, kApplyBeats, "ldr x20", 0};      // SongInfo: what rekordbox compares to tell a new song (8 bytes
static const Offset O_songName{0x40, kApplyBeats, "add x20", 0};  // and a juce::String)
static const Addr A_stringDiffers{"__ZN4juceneERKNS_6StringES2_"};   // juce::operator!=(String const&, String const&)
static const Addr A_updateBeats{"__ZN6djplay14WidgetWaveView18updateBeatPositionEPNS_8BeatInfoE"};
static const Addr A_settings{"__ZN6djplay9SettingIF11getInstanceEv"};
static const Addr A_waveColour{"__ZN6djplay9SettingIF19getCurrentWaveColorEb"};  // 1 RGB, 3 3Band; else a blue style

namespace {
struct WaveCoef { float a, b, c; };  // djplay::WaveNormalizeCoef
}
static thread_local int t_stage;       // main thread, in reload(): 1 the waveforms, 2 an early grid, 3 the final grid (0: rekordbox's own reload)
static thread_local bool t_beatsOnly;  // in the deck's apply step: only the beats are new, the waveforms stay
static int g_gridRedraws, g_waveBuilds;  // (main thread; for the log)
namespace {
struct Shown { std::string uri; int stage = 0; };  // per deck: the track whose waveforms were built, and at which stage last
}
static Shown g_shown[8];  // main thread

// UiPlayer::handleDBMSG_subRoutine1 -> bl handleDBMSG_subRoutine1_2(new BeatInfo, new SongInfo, coef), in every reload:
// ours and rekordbox's own at the end of the analysis (stage 4 here). Only the beats are new when this deck was given
// this track's waveforms by a reload before and rekordbox's own comparisons of the song and the scale say "same"
// (the beats it compares too: when they are the same it builds nothing anyway). The blue waveform styles draw the
// zoomed waveform that is written with the final grid, so they need the waveforms built at stage 3 once.
// windows-optional: qd_apply qd_initWaveDJ qd_initWave   (macOS only: the Windows deck keeps rekordbox's own reload)
extern "C" void rbm_hook_qd_apply(char* player, char* beat, char* song, WaveCoef coef) {
    int ch = -1, stage = t_stage ? t_stage : 4;
    for (int c = 0; c < deckCount() && c < 8 && ch < 0; c++)
        if ((char*)at<void* (*)(int)>(A_getUiPlayer)(c) == player) ch = c;
    bool only = false;
    if (g_on && ch >= 0 && stage > 1 && beat && song && cfgInt("quickdeck_gridonly", 1) && !g_shown[ch].uri.empty() && g_shown[ch].uri == deckPath(ch)) {
        int colour = at<int (*)(void*, bool)>(A_waveColour)(at<void* (*)()>(A_settings)(), false);
        bool wavesThere = colour == 1 || colour == 3 || stage < 3 || g_shown[ch].stage >= 3;
        char* oldSong = *(char**)(player + O_playerSong);
        const float* old = (const float*)(player + O_playerCoef);
        bool sameScale = coef.a == 0 || coef.b == 0 || coef.c == 0 || (coef.a == old[0] && coef.b == old[1] && coef.c == old[2]);
        // (also when the deck had no beats yet, a track's first grid: the views take their first beats the same way)
        only = wavesThere && oldSong && sameScale && *(uint64_t*)(oldSong + O_songId) == *(uint64_t*)(song + O_songId) &&
               !at<bool (*)(const void*, const void*)>(A_stringDiffers)(oldSong + O_songName, song + O_songName);
    }
    int builds = g_waveBuilds;
    t_beatsOnly = only;
    RBM_ORIG(qd_apply, void (*)(char*, char*, char*, WaveCoef))(player, beat, song, coef);
    t_beatsOnly = false;
    if (ch >= 0 && g_waveBuilds != builds) g_shown[ch] = {deckPath(ch), stage};
}

typedef int (*InitWave)(void*, long, long, void*, void*, int, int, int, void*, bool, int, WaveCoef);
// WidgetPlayer(DJ)::operateWaveViewInitialize -> bl WidgetWaveView::initializeWave(scale, mode, SongInfo*, WaveData*,
// type, colour, parts, BeatInfo*, bool, deck, coef): during a grid-only reload the emptying call (no song, no beats)
// is left out and the call with the new data only hands the view its new beats.
static int initWave(InitWave orig, void* view, long scale, long mode, void* song, void* wave, int type, int colour, int parts, void* beat, bool flag,
                    int deck, WaveCoef coef) {
    if (t_beatsOnly) {
        if (!song && !beat) return 0;
        if (beat) {
            at<void (*)(void*, void*)>(A_updateBeats)(view, beat);
            g_gridRedraws++;
            return 0;
        }
    }
    if (song) g_waveBuilds++;
    return orig(view, scale, mode, song, wave, type, colour, parts, beat, flag, deck, coef);
}
extern "C" int rbm_hook_qd_initWaveDJ(void* view, long scale, long mode, void* song, void* wave, int type, int colour, int parts, void* beat, bool flag,
                                      int deck, WaveCoef coef) {
    return initWave(RBM_ORIG(qd_initWaveDJ, InitWave), view, scale, mode, song, wave, type, colour, parts, beat, flag, deck, coef);
}
extern "C" int rbm_hook_qd_initWave(void* view, long scale, long mode, void* song, void* wave, int type, int colour, int parts, void* beat, bool flag,
                                    int deck, WaveCoef coef) {
    return initWave(RBM_ORIG(qd_initWave, InitWave), view, scale, mode, song, wave, type, colour, parts, beat, flag, deck, coef);
}
#endif

#ifndef _WIN32
// ---- the overview waveform during the audio pass ----------------------------------------------------------------------
static const char* const kApplyWaves = "__ZN6djplay8UiPlayer25handleDBMSG_subRoutine1_1Ev";
static const Offset O_playerBlue{0x4d0, kApplyWaves, "ldr x20", 2};     // UiPlayer: the blue overview (new[], 0xc80 bytes)
static const Offset O_playerWhole{0x4d8, kApplyWaves, "ldr x20", 4};    // the colour overview: sWholeWavePoint[1200] (new[], 0x1c20 bytes)
static const Offset O_player3Bands{0x500, kApplyWaves, "ldr x20", 10};  // the 3-band overview, the same size
static const Offset O_playerWidgets{0x5b0, kApplyWaves, "ldr x20", 3};  // juce::Array<deck widget*>*
static const Offset V_overviewUpdate{0x130, "__ZTVN6djplay12WidgetPlayerE", "vt:__ZN6djplay12WidgetPlayer22operateLoudViewUpdate2EPKNS_15sWholeWavePointE", 0};
static const char* const kCreateWhole = "__ZN8analyzer11WaveCreator19createWholeWaveDataEv";
static const char* const kCreate3Bands = "__ZN8analyzer11WaveCreator25create3BandsWholeWaveDataEv";
static const Addr A_createWhole{kCreateWhole};    // the colour overview from the builder's 12-byte points; deletes them
static const Addr A_create3Bands{kCreate3Bands};  // the 3-band overview from its 6-byte points
static const Offset O_wcColourPoints{0x6a0, kCreateWhole, "ldr x0", 0};   // WaveCreator: its colour points (new[], 12 bytes each)
static const Offset O_wcPoints{0x688, kCreateWhole, "ldr x8", 0};         // how many points the whole track has
static const Offset O_wc3BandPoints{0x6b0, kCreate3Bands, "ldr x0", 0};   // its 3-band points (6 bytes each)
static const size_t kCreatorSize = 0x8560;   // sizeof(WaveCreator) (operator new in CBeatAnalyzeThread::analyze)
static const size_t kOverviewBytes = 0x1c20; // 1200 columns of 6 bytes
static const int kColumns = 1200;

static thread_local std::string t_analysing;  // analysis thread: the track in its audio pass ("": none)
static thread_local const void* t_analysisThread;
static thread_local double t_partialAt;

// Service "quickdeck.waveDue" (parallel's block hook, on the analysis thread, for every block of audio): is it time
// for another partial overview of the track this thread reads? Then its URI and the thread.
static bool waveDue(const char** uri, const void** thread) {
    if (!g_on || t_analysing.empty()) return false;
    double t = now();
    if (t - t_partialAt < 0.25) return false;
    t_partialAt = t;
    if (!cfgInt("quickdeck_stream", 1)) return false;
    bool onDeck = false;
    for (int ch = 0; ch < deckCount(); ch++) onDeck |= deckPath(ch) == t_analysing;
    if (!onDeck) return false;
    *uri = t_analysing.c_str(), *thread = t_analysisThread;
    return true;
}
RBM_PROVIDE("quickdeck", "quickdeck.waveDue", waveDue, &g_on);

namespace {
struct Partial { std::string uri; bool threeBands = false; std::shared_ptr<std::vector<unsigned char>> data; int shown = 0; };
}
static Partial g_partial[8];  // main thread, per deck: the last partial overview it was given (for quickdeck_log's check)

// Main thread: the deck takes the overview as its reload would.
static void showOverview(const std::string& uri, bool threeBands, const std::shared_ptr<std::vector<unsigned char>>& data) {
    for (int ch = 0; ch < deckCount() && ch < 8; ch++) {
        if (deckPath(ch) != uri) continue;
        char* player = (char*)at<void* (*)(int)>(A_getUiPlayer)(ch);
        char* widgets = player ? *(char**)(player + O_playerWidgets) : nullptr;
        if (!widgets) continue;
        unsigned char* points = new unsigned char[kOverviewBytes];  // (the player's from here on: it delete[]s it)
        memcpy(points, data->data(), kOverviewBytes);
        unsigned char** mine = (unsigned char**)(player + (threeBands ? O_player3Bands : O_playerWhole));
        unsigned char* before = *mine;
        *mine = points;
        if (!before) {  // (as rekordbox: the blue overview goes when a colour one comes)
            unsigned char** blue = (unsigned char**)(player + O_playerBlue);
            delete[] *blue;
            *blue = nullptr;
        }
        void*** list = *(void****)widgets;
        for (int i = 0, n = *(int*)(widgets + 0xc); i < n; i++)
            ((void (*)(void*, const void*))(*(void***)list[i])[V_overviewUpdate / 8])(list[i], points);
        delete[] before;
        int shown = g_partial[ch].uri == uri ? g_partial[ch].shown + 1 : 1;
        g_partial[ch] = {uri, threeBands, data, shown};
    }
}
// quickdeck_log: how a partial overview compares with the one the reload brought.
static void compareOverview(int ch, const char* player, const Partial& p, const char* what) {
    const unsigned char* final = *(const unsigned char* const*)(player + (p.threeBands ? O_player3Bands : O_playerWhole));
    if (!final || !p.data) return;
    int filled = 0, same = 0, last = -1, worst = 0, off = 0;
    for (int k = 0; k < kColumns; k++) {
        const unsigned char* a = p.data->data() + 6 * k;
        if (!(a[0] | a[1] | a[2] | a[3] | a[4] | a[5])) continue;
        filled++, last = k;
        int d = 0;
        for (int j = 0; j < 6; j++) d = std::max(d, abs((int)a[j] - (int)final[6 * k + j]));
        same += d == 0, off += d > 2, worst = std::max(worst, d);
    }
    logf("quickdeck: deck %d, %s (%s): the last partial overview had %d columns (up to %d of %d): %d exactly as in the final one, %d more than 2 off, the furthest %d",
         ch + 1, what, p.threeBands ? "3-band" : "colour", filled, last + 1, kColumns, same, off, worst);
}
static Partial g_otherStyle;  // (g_lock) quickdeck_log=2: the last partial overview in the style that isn't shown
static void checkOverview(int ch, const char* player, const std::string& uri) {
    Partial p = std::move(g_partial[ch]);
    g_partial[ch] = Partial();
    if (p.uri != uri || !cfgInt("quickdeck_log", 0)) return;
    logf("quickdeck: deck %d was given %d partial overviews", ch + 1, p.shown);
    compareOverview(ch, player, p, "shown");
    Partial other;
    {
        std::lock_guard<std::mutex> l(g_lock);
        other = std::move(g_otherStyle), g_otherStyle = Partial();
    }
    if (other.uri == uri) compareOverview(ch, player, other, "not shown");
}


// The overview of what the builder has read so far, in the deck's form (6 bytes a column); null when it has no points
// of that style. filled: how many columns have something in them.
static std::shared_ptr<std::vector<unsigned char>> overviewOf(const char* creator, bool threeBands, int* filled) {
    long points = *(long*)(creator + O_wcPoints);
    const char* source = *(char* const*)(creator + (threeBands ? O_wc3BandPoints : O_wcColourPoints));
    if (!source || points < kColumns || points > (1L << 26)) return nullptr;
    static thread_local std::vector<char> copyOf;  // a WaveCreator to run rekordbox's own function on: the real one stays as it is
    copyOf.assign(creator, creator + kCreatorSize);
    char* copy = copyOf.data();
    auto out = std::make_shared<std::vector<unsigned char>>(kOverviewBytes, (unsigned char)0);
    unsigned char* o = out->data();
    if (threeBands) {
        at<void (*)(void*)>(A_create3Bands)(copy);
        const unsigned char* b = at<const unsigned char* (*)(void*)>(A_3bandsWholeWave)(copy);  // three bytes a column
        for (int k = 0; k < kColumns; k++)
            o[6 * k + 3] = b[3 * k], o[6 * k + 4] = b[3 * k + 1], o[6 * k + 5] = b[3 * k + 2];  // (get3BandsWholeWavePoint)
    } else {
        char* own = new char[(size_t)points * 12];  // (createWholeWaveData delete[]s the points it read)
        memcpy(own, source, (size_t)points * 12);
        *(char**)(copy + O_wcColourPoints) = own;
        at<void (*)(void*)>(A_createWhole)(copy);
        const float* f = at<const float* (*)(void*)>(A_wholeWave)(copy);  // six floats a column
        for (int k = 0; k < kColumns; k++)
            for (int j = 0; j < 6; j++) o[6 * k + j] = (unsigned char)(int)(f[6 * k + j] * 128.0f * (1.0f / 32768.0f));  // (notifyWhole, getWholeWavePoint)
    }
    if (filled) {
        *filled = 0;
        for (int k = 0; k < kColumns; k++) *filled += (o[6 * k] | o[6 * k + 1] | o[6 * k + 2] | o[6 * k + 3] | o[6 * k + 4] | o[6 * k + 5]) != 0;
    }
    return out;
}

// Service "quickdeck.partialWave" (parallel: on its waveform queue between two blocks, or on the analysis thread
// when it doesn't queue): the overview of what has been read so far, for the decks that hold the track.
static void partialWave(const char* creator, const char* uri, const char* thread) {
    if (!g_on || !creator || !uri) return;
    double t0 = plat::monotonic();
    bool threeBands = at<int (*)(void*, bool)>(A_waveColour)(at<void* (*)()>(A_settings)(), false) == 3;
    if (thread && !thread[threeBands ? O_want3BandsWhole : O_wantWhole]) return;  // (this analysis doesn't make that one)
    int filled = 0;
    auto out = overviewOf(creator, threeBands, &filled);
    if (!out) return;
    int log = cfgInt("quickdeck_log", 0);
    if (log >= 2) {  // the other style's too, not shown: checked against the final one like the shown one
        auto other = overviewOf(creator, !threeBands, nullptr);
        std::lock_guard<std::mutex> l(g_lock);
        g_otherStyle = {uri, !threeBands, other, 0};
    }
    if (log) logf("quickdeck: partial %s overview, %d of %d columns, %.1f ms  %s", threeBands ? "3-band" : "colour", filled, kColumns, (plat::monotonic() - t0) * 1000, uri);
    std::string u = uri;
    plat::onMain([u, threeBands, out] { showOverview(u, threeBands, out); });
}
RBM_PROVIDE("quickdeck", "quickdeck.partialWave", partialWave, &g_on);
#endif

#ifndef _WIN32
// ---- a Spotify track's analysis while it downloads ---------------------------------------------------------------------
static const char* const kDataVt = "__ZTVN9streaming7spotify19CompressDataManagerE";  // (DownloadManager's "is it there" object)
static const Addr A_dataVt{kDataVt};
static const Offset V_isInitialized{0x40, kDataVt, "vt:__ZN9streaming19CompressDataManager13isInitializedEN4juce6StringE", 0};
static const Offset V_isDownloaded{0x48, kDataVt, "vt:__ZN9streaming19CompressDataManager12isDownloadedEN4juce6StringE", 0};
static const Addr A_syncDownload{"__ZN9streaming15DownloadManager12syncDownloadERKN4juce6StringE"};
static const Addr A_readerVt{"__ZTVN6rbxfrm13SpotifyReaderE"};
static const Offset V_readerOpen{0x10, "__ZTVN6rbxfrm13SpotifyReaderE", "vt:__ZN6rbxfrm13SpotifyReader8fileOpenERKN4juce6StringE", 0};
static const Offset O_readerComplete{0x80, "__ZN6rbxfrm15StreamingReader13isReadyToReadEii", "ldr x22", 1};  // StreamingReader: bool, the whole track is there
static const Offset O_readerError{0xc8, "__ZN6rbxfrm15StreamingReaderC2Ev", "str x19", 2};                  // bool: no stream (fileRead hands out zeros)
static const Offset O_readerStatic{0xc9, "__ZN6rbxfrm15StreamingReader10updateDataERKN4juce6StringE", "ldr x20", 2};  // bool: an analysis thread's (ignores the download)
static const Addr A_updateData{"__ZN6rbxfrm15StreamingReader10updateDataERKN4juce6StringE"};
static const Addr A_completeData{"__ZN6rbxfrm15StreamingReader12completeDataERKN4juce6StringEb"};
static const Addr A_cancelAnalysis{"__ZN8analyzer11CAnalyzerIF14cancel_analyzeEl"};

static const Offset V_readerReady{0x28, "__ZTVN6rbxfrm13SpotifyReaderE", "vt:__ZN6rbxfrm15StreamingReader13isReadyToReadEii", 0};
static const Addr A_currentThread{"__ZN4juce6Thread16getCurrentThreadEv"};
static const Addr A_shouldExit{"__ZNK4juce6Thread16threadShouldExitEv"};
namespace {
struct Early { std::string uri; void* thread = nullptr; double updatedAt = 0; };  // thread: the analysis' juce::Thread
}
static std::mutex g_earlyLock;
static std::map<void*, Early> g_early;  // readers opened before their download was done, until their audio pass is over
static bool (*g_readerReady)(void*, int, int);
static bool (*g_isDownloaded)(void*, const JStr*);
static bool (*g_isInitialized)(void*, const JStr*);
static int (*g_readerOpen)(void*, const JStr*);
static void* volatile g_data;                  // streaming::spotify::CompressDataManager (seen in its isDownloaded)
static thread_local bool t_openEarly;          // analysis thread, inside its reader's fileOpen
static thread_local char* t_earlyReader;       // analysis thread: the reader it opened before the download was done
static thread_local std::string t_earlyUri;

static bool analysisThread() {
    char name[64] = "";
    pthread_getname_np(pthread_self(), name, sizeof name);
    return !strncmp(name, "CBeatAnalyzeThread:", 19);
}
// CompressDataManager::isDownloaded(path). DownloadManager::syncDownload asks it on an analysis thread (isInitialized
// on any other) to know when to stop waiting: there, for a reader being opened early, "initialized" will do. Everyone
// else who asks (the reader itself, to know that nothing more will come) gets the real answer.
__attribute__((noinline)) static bool downloadedOrStarted(void* data, const JStr* path) {
    g_data = data;
    if (t_openEarly) {
        uintptr_t from = (uintptr_t)__builtin_return_address(0), sync = (uintptr_t)at<void*>(A_syncDownload);
        if (from >= sync && from < sync + 0x800) return g_isInitialized(data, path);
    }
    return g_isDownloaded(data, path);
}
// StreamingReader::isReadyToRead(frame, frames), which fileRead polls every 50 ms while the audio isn't there. A reader
// follows its download through the download's callbacks (updateData: its frame table grows); an analysis thread's
// reader isn't given those, so one opened early asks for the news itself whenever it finds it has to wait. And when
// its analysis has been cancelled meanwhile (rekordbox starts a track's analysis over when the track joins the
// collection), it stops waiting: marked failed, it hands out silence at once and the analysis thread sees its stop.
static bool readerReady(char* reader, int pos, int frames) {
    if (g_readerReady(reader, pos, frames)) return true;
    Early e;
    {
        std::lock_guard<std::mutex> l(g_earlyLock);
        auto it = g_early.find(reader);
        if (it == g_early.end()) return false;
        if (now() - it->second.updatedAt < 0.04) return false;
        it->second.updatedAt = now();
        e = it->second;
    }
    if (e.thread && at<bool (*)(void*)>(A_shouldExit)(e.thread)) {
        reader[O_readerError] = 1;
        return true;
    }
    JStr uri(e.uri.c_str());
    at<void (*)(void*, const JStr*)>(A_updateData)(reader, &uri);
    return g_readerReady(reader, pos, frames);
}
// A Spotify track on a deck, with the experiment on: its analysis may read it while it downloads. The analyzer mod
// asks this at the start of an analysis and then lets analyze() past its own wait for the download; without that mod
// the reader is still opened early here, and analyze() waits as before.
static bool g_progressive;  // the vtables are changed
static bool readsEarly(const char* uri) {
    if (!g_on || !g_progressive || !uri || strncmp(uri, "spotify:", 8) || !cfgInt("quickdeck_progressive", 0)) return false;
    for (int ch = 0; ch < deckCount(); ch++)
        if (deckPath(ch) == uri) return true;
    return false;
}
RBM_PROVIDE("quickdeck", "quickdeck.readsEarly", readsEarly, &g_on);
// SpotifyReader::fileOpen(path).
static int readerOpen(char* reader, const JStr* path) {
    {
        std::lock_guard<std::mutex> l(g_earlyLock);
        g_early.erase(reader);  // (a reader that went without its audio pass ending: this is a new one at its address)
    }
    bool early = path && analysisThread() && readsEarly(path->text);
    if (!early) return g_readerOpen(reader, path);
    t_openEarly = true;
    int r = g_readerOpen(reader, path);
    t_openEarly = false;
    if (!r || reader[O_readerError] || reader[O_readerComplete]) return r;  // (failed, or everything was there already)
    // An ordinary reader from here on: what the download brought while it was marked, and its end if that came.
    reader[O_readerStatic] = 0;
    at<void (*)(void*, const JStr*)>(A_updateData)(reader, path);
    if (void* data = g_data)
        if (g_isDownloaded(data, path)) at<void (*)(void*, const JStr*, bool)>(A_completeData)(reader, path, true);
    t_earlyReader = reader, t_earlyUri = path->text;
    {
        std::lock_guard<std::mutex> l(g_earlyLock);
        g_early[reader] = {path->text, at<void* (*)()>(A_currentThread)(), 0};
    }
    logf("quickdeck: the analysis of %s reads it while it downloads", path->text);
    return r;
}
// The audio pass is over (analysis thread): did the stream hold? If not the reader filled in silence and what was
// analysed isn't the track: the analysis is cancelled (on the main thread, as rekordbox's own cancels).
static void earlyReaderDone(const char* thread, const char* uri) {
    char* reader = t_earlyReader;
    t_earlyReader = nullptr;
    if (!reader) return;
    {
        std::lock_guard<std::mutex> l(g_earlyLock);
        g_early.erase(reader);
    }
    if (t_earlyUri != uri) return;
    bool failed = reader[O_readerError], complete = reader[O_readerComplete];
    if (!failed && complete) return;
    void* analyzer = thread ? *(void* const*)(thread + O_listener) : nullptr;
    CollectionTrack t;
    bool known = collectionTrack(uri, &t);
    logf("quickdeck: the stream of %s %s during its analysis: the analysis is cancelled%s", uri, failed ? "failed" : "wasn't complete",
         analyzer && known && t.id ? "" : " (not found: could not)");
    if (!analyzer || !known || !t.id) return;
    long id = (long)t.id;
    plat::onMain([analyzer, id] { at<void (*)(void*, long)>(A_cancelAnalysis)(analyzer, id); });
}
static void installProgressive() {
    void** service = at<void**>(A_dataVt) + 2;  // (the object's vptr: the symbol + 0x10)
    g_isInitialized = (bool (*)(void*, const JStr*))service[V_isInitialized / 8];
    g_isDownloaded = (bool (*)(void*, const JStr*))service[V_isDownloaded / 8];
    void** reader = at<void**>(A_readerVt) + 2;
    g_readerOpen = (int (*)(void*, const JStr*))reader[V_readerOpen / 8];
    g_readerReady = (bool (*)(void*, int, int))reader[V_readerReady / 8];
    if (!patchSlot(&reader[V_readerReady / 8], (void*)&readerReady) || !patchSlot(&reader[V_readerOpen / 8], (void*)&readerOpen) || !patchSlot(&service[V_isDownloaded / 8], (void*)&downloadedOrStarted))
        logf("quickdeck: could not change the Spotify reader's vtables: analyses wait for the whole download as before");
    else g_progressive = spotifyReadersKeepUp();
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
#ifndef _WIN32
    int builds = g_waveBuilds, redraws = g_gridRedraws;
    t_stage = strstr(why, "with the waveforms") ? 1 : strstr(why, "the beat grid") ? 3 : 2;
#endif
    at<void (*)(void*, unsigned, long, void*)>(A_completed)(player + thisDelta(A_completed), id, 2, &param);
#ifndef _WIN32
    t_stage = 0;
    if (ch < 8) checkOverview(ch, player, uri);
    const char* drew = g_gridRedraws != redraws ? ", only the grid redrawn" : g_waveBuilds != builds ? ", waveforms built" : "";
#else
    const char* drew = "";
#endif
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
    logf("quickdeck: deck %d shows %s %.1f s after the analysis started (%.0f ms, %.2f BPM%s)", ch + 1, why, since, (now() - t) * 1000,
         deckBpm(ch, false), drew);
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
#ifndef _WIN32
    if (!strcmp(stage, "start")) t_analysing = uri, t_analysisThread = thread, t_partialAt = now();
    else if (strcmp(stage, "poll")) t_analysing.clear();  // (the audio pass is over at the beat job)
    if (!strcmp(stage, "beat job")) {
        earlyReaderDone((const char*)thread, uri);
        if (cfgInt("quickdeck_log", 0))  // what the pass read, to compare a progressive analysis with one from the finished file
            if (auto pass = service<bool (*)(const void**, const int16_t**, const int16_t**, size_t*)>("streamio.passAudio")) {
                const void* reader = nullptr;
                const int16_t *l = nullptr, *r = nullptr;
                size_t n = 0;
                if (pass(&reader, &l, &r, &n)) {
                    uint64_t h = 1469598103934665603ull;
                    for (size_t i = 0; i < n; i++) h = (h ^ (uint16_t)l[i]) * 1099511628211ull, h = (h ^ (uint16_t)r[i]) * 1099511628211ull;
                    logf("quickdeck: the audio pass read %zu frames, hash %016llx  %s", n, (unsigned long long)h, uri);
                }
            }
    }
#endif
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

#ifndef _WIN32
RBM_MOD({.name = "quickdeck", .init = [] { installProgressive(); }, .enabled = &g_on});
#else
RBM_MOD({.name = "quickdeck", .enabled = &g_on});
#endif
