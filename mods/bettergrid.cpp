// bettergrid: better beat grids, from "Beat This!" (engines/beatthis.h) and rekordbox's own analysis combined.
//
// Listening tests showed each is better at something else: Beat This! finds the tempo,
// its octave and where the bar starts more reliably; rekordbox places the beats themselves more exactly (Beat This!
// works in 20 ms steps). So when both find the same tempo, rekordbox's beats stay exactly where they are and only
// the bar start comes from Beat This! (and when rekordbox's BPM is a hair off a whole one the onsets fit, its beats are
// redrawn at the whole BPM through its middle beat; and the beat at the very start of the track, which rekordbox
// leaves out when its grid puts it a few ms before the file begins, is added). One thing rekordbox gets wrong at
// the right tempo: its beats
// on the off-beats, half a beat away (about 3% of tracks). When Beat This! hears the beats there over the whole track
// and the kick band's onsets are clearly stronger there too, rekordbox's beats are moved half a beat (each to the
// middle of its two neighbours, so its exact placement stays). Either one alone isn't enough: on the test tracks the
// onsets backed Beat This! on some of those tracks and rekordbox on others, and a wrong move is worse than none.
// When the kick band itself is much louder halfway between rekordbox's beats (its level averaged over every beat,
// at least kHalfBeatLevel times its level at them: the kicks sit on rekordbox's off-beats), most of Beat This!'s
// beats between them are enough: a track whose intro Beat This! hears otherwise (0LBTxmhGhEXbQQtmse1t4r,
// 2026-11-19: 72% between, the kick band 2.7x louder between) stayed on the off-beats.
// rekordbox's beats can also sit a fixed distance from the kicks that is neither nothing nor half a beat: on a hi-hat
// or a clap, 60 to 170 ms late or early over the whole track (Robert Hood's Snakes and Politics, 2026-11-26: on the
// hats, 96 ms after every kick; about 4% of 1,700 logged tracks have such an offset between the two grids). When
// the combined grid (Beat This!'s tempo, placed on the onsets) lies that far from rekordbox's beats over the whole
// track, and the kick band's level rises most at the combined grid's beats and clearly more than at rekordbox's, the
// combined grid is used: rekordbox's exact placement is no use when it is the placement of something else.
// Both can follow something other than the kick: on Red Passion II - A (Robert Hood, 2026-11-26) a noisy percussion
// layer sits on the beat and the kick comes 71 ms after it, steadily, so rekordbox and Beat This! agree on a grid
// the kick never hits. Last of all, the kick alone is measured against the grid that came out (kickLine): when it
// sits a fixed distance off it on most beats and clearly, the grid is moved onto the kicks (bettergrid_kick).
// rekordbox's beats can also be spaced at another tempo than the one they are labelled with (rbDrift): then they
// aren't taken as the same tempo, and the combined grid is used.
// When rekordbox's and Beat This!'s bar starts differ, the music decides between them: the beat in the bar where the
// track changes (a drop, a breakdown, a new bass line: barphase.h), else the beat where the kick comes in (see
// barByMusic and kickStartBar).
// When rekordbox's tempo is wrong (half, double, 3:2, other), the combined grid
// takes Beat This!'s tempo and bar start, and puts each beat on the onsets in the audio (each beat's nearest clear
// onset, a robust line through those offsets over the track: shift and a slight tempo correction; a whole BPM when it
// fits as well). Measured against rekordbox's beats where the tempos agree, that onset placement is right on average
// but 20-60 ms off on about one track in ten, so it isn't used when rekordbox's beats are there to keep. When Beat
// This! is unsure (fit under bettergrid_min_fit), rekordbox's grid stays.
//
// The track's genre picks the tempo's octave (Beat This! counts every tempo it finds within 70-200 BPM, so a drum &
// bass track can come out at 83): from the genres mod (service "genres.forTrack": its stored result, rekordbox's
// Genre field, or its style model on the audio already decoded here), mapped to the BPMs DJs count that genre in
// (mods/genretempo.h: drum & bass 160-185, house 115-130, hip hop 80-115...). When Beat This!'s tempo is outside the
// range and twice or half of it is inside, the grid goes to that tempo (a beat added between each two, or every other
// beat kept, the downbeats among them), and from there on as above: rekordbox's beats when they now agree. When only
// rekordbox's tempo (an octave or 3:2 from Beat This!'s) is in the range, rekordbox's grid is kept. Otherwise, or with
// no genre, nothing changes; a genre never brings in a tempo neither analysis found. Without the genres mod, as before.
//
// How: just before rekordbox's beat job, the track's audio (decoded once, shared with betterkey: analysisAudio) goes
// through Beat This! and the combination on another thread, next to rekordbox's own beat detection. When the beat
// job is done, CBeatAnalyzeThread::NotifyResult ends in a jump to notifyBeat(its beats), which saves the grid (the
// ANLZ file's PQTZ, via the callback's notify_beat_info): there the combined grid is handed over instead. Then
// rekordbox copies the job's beats into the analysis (the only Array<DetailedBeatInfo>::operator= in
// CBeatAnalyzeThread::analyze): replaced again, so the phrase analysis and the key job use the combined grid too;
// and the analysis' own notifyBeat calls (after the phrase analysis renumbers the bars) get it even when rekordbox
// reloaded the beat list from its database in between (bars the phrase analysis renumbered on the combined grid
// are rekordbox's to keep). A track whose beat job doesn't run (a locked grid, a key-only analysis) is left alone:
// a grid applied to an analysis is dropped at that analysis' last step, when the next analysis or beat job starts
// on that thread, and after kAppliedFor at the latest. If anything fails or takes too long rekordbox's own grid is
// kept; so is a track longer than the 20 minutes of audio that are analysed (only its bars can change).
//
// Every analysed track is logged with both grids and appended to rekordbox-bettergrid.csv next to the log (with
// rekordbox's own grid, for listening to them side by side). The "BT BPM" column (id 57) shows Beat This!'s BPM.
//   config: bettergrid=0 to disable (rekordbox's analysis runs exactly as without it)
//           bettergrid_apply=0   compare only: log both grids, keep rekordbox's
//           bettergrid_min_fit=0.6   Beat This!'s fit below which rekordbox's grid is kept
//           bettergrid_halfbeat=1   rekordbox's beats half a beat off the beat: 1 moved when Beat This!'s beats and the kick band (its onsets, or its level clearly higher between) both say so, 2 when Beat This!'s beats alone say so, 0 never
//           bettergrid_offbeat=1 rekordbox's beats a fixed 60 ms or more off the kicks (not half a beat): 1 the combined grid when Beat This! and the kick band both say so; 0 never
//           bettergrid_kick=1    the kick sits a fixed 50 ms or more off the grid over the whole track, also off Beat This!'s
//                                beats (a percussion layer on the beat, the kick after it): 1 the grid moved onto the kicks; 0 never
//           bettergrid_bars=1    rekordbox's and Beat This!'s bar starts differ: the music's changes (barphase.h) pick one; 0 the kick-in rule alone
//           bettergrid_genre=1   the track's genre (genres mod) picks the tempo's octave; 0 never
//           bettergrid_warmup=1  0: no first run of the model at launch (Apple Silicon)
//           bettergrid_wait=20   seconds the analysis waits for the combined grid at most (60 on Windows and Intel Macs)
//           bettergrid_rb_first=1  0: while it waits, the deck doesn't show rekordbox's own grid meanwhile (quickdeck)
//           bettergrid_slow=0    Windows, Intel Macs: 1: also wait for Beat This! for a track on a deck where it is
//                                slow (it runs on the CPU, or on a GPU that needs over a second for 30 s of audio)
//           bettergrid_title=BT BPM   header name of the column
//           bettergrid_debug=0   1: log the first beats of both grids (development)
#include "../core/rbmod.h"
#include "../engines/beatthis.h"
#include "../engines/dsp.h"
#include "barphase.h"
#include "genretempo.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <thread>
#include <vector>

using namespace rbm;

// Its settings (core/settings.h): what the settings window and the config file's list show of them.
namespace opt {
const Setting apply{"bettergrid_apply", true, {.label = "Use the better grids", .help = "Off: both grids are only compared in the log, and rekordbox's is kept."}};
const Setting bars{"bettergrid_bars", true, {.label = "Let the music decide where the bar starts", .help = "When rekordbox and Beat This! disagree: the 1 goes on the beat where the track changes."}};
const Setting genre{"bettergrid_genre", true, {.label = "Let the genre pick between half and double tempo", .help = "A drum & bass track at 87 becomes 174."}};
const Setting halfbeat{"bettergrid_halfbeat", 1, {.label = "Move grids that sit on the off-beats"}};
const Setting offbeat{"bettergrid_offbeat", true, {.label = "Fix grids that sit on a hi-hat or a clap"}};
const Setting kick{"bettergrid_kick", true, {.label = "Move the grid onto the kicks", .help = "When the kick sits a steady 50 ms or more off the grid over the whole track."}};
const Setting title{"bettergrid_title", "BT BPM", {.label = "Title of the Beat This! BPM column"}};
const Setting minFit{"bettergrid_min_fit", 0.6, {.label = "Trust Beat This! from this fit on", .min = 0, .max = 1, .step = 0.05, .advanced = true}};
// (Beat This! on the CPU is several times slower than Core ML on Apple Silicon: more patience on Windows and Intel Macs)
#if defined(_WIN32) || RBM_MAC_X86
const Setting wait{"bettergrid_wait", 60, {.label = "Wait for the combined grid at most", .unit = "s", .min = 0, .max = 600, .step = 5, .advanced = true}};
#else
const Setting wait{"bettergrid_wait", 20, {.label = "Wait for the combined grid at most", .unit = "s", .min = 0, .max = 600, .step = 5, .advanced = true}};
#endif
const Setting rbFirst{"bettergrid_rb_first", true, {.label = "Show rekordbox's grid on the deck meanwhile", .advanced = true}};
const Setting slow{"bettergrid_slow", false, {.label = "Wait for Beat This! on a deck even where it is slow", .help = "Windows and Intel Macs.", .advanced = true}};
const Setting warmup{"bettergrid_warmup", true, {.label = "Run the model once at launch", .advanced = true}};
const Setting debug{"bettergrid_debug", 0, {.label = "Log the beats of both grids", .debug = true}};
}  // namespace opt

static const int kColumnId = 57;  // unused by rekordbox
static const Offset S_beatInfo{0xc, "__ZN8analyzer13AnalyzeJobKey6getKeyERN4juce5ArrayINS_16DetailedBeatInfoENS1_20DummyCriticalSectionELi0EEEfNSt3__110shared_ptrIfEEjbmm", "movz x14", 0};  // sizeof(DetailedBeatInfo)

static volatile bool g_on = true;

namespace {
// juce::Array<DetailedBeatInfo>: {elements (malloc'd), numAllocated, numUsed}. An entry: {tempo*100 << 16 | beat in
// bar (1-4), time in whole ms, the rest of the time in µs (0-999, in the low 10 bits: see beatTime)}.
struct BeatArray { char* data; int allocated; int used; };

struct Combined {
    bool ok = false;                // Beat This! found a grid
    bool use = false;               // and it's sure enough to replace rekordbox's
    beatthis::Grid bt;
    double btDownbeat = NAN;        // s
    double first = 0, bpm = 0, downbeat = NAN;  // the combined grid
    double duration = 0;            // s
    int clearBeats = 0;
    double shiftMs = 0, took = 0;
    double wholeFrom = 0;           // the fitted tempo when it was rounded to a whole BPM
    bool onsetFit = false;          // the beats were placed on the onsets (enough clear ones)
    std::vector<double> beats;      // Beat This!'s own beats, s (for halfBeatOff)
    std::shared_ptr<const std::vector<float>> kick;  // the kick band's onsets per ms, smoothed (for halfBeatOff)
    std::shared_ptr<const std::vector<float>> level; // the kick band's level (RMS) per ms (for halfBeatOff)
    std::shared_ptr<const barphase::Bands> bands;    // 12 band powers per ~23 ms (for barByMusic)
    double bandsMs = 0;                              // ms they took
    std::string genre;              // the track's genre ("": not known, or not asked)
    double genreTook = 0;           // s the genres mod took for it
};

// What the genre did to the tempo, for the log.
struct GenreNote {
    std::string genre;
    genretempo::Range range;
    genretempo::Choice choice;
    double from = 0;                // Beat This!'s own tempo
    double took = 0;
};

// rekordbox's grid, for the log.
struct RbGrid { double bpm = 0, firstBeat = -1, firstDownbeat = -1; };

// rekordbox's beats half a beat from Beat This!'s (halfBeatOff).
struct HalfBeat {
    bool seen = false;              // Beat This! hears the beats half a beat from rekordbox's, over the whole track
    bool moved = false;             // and rekordbox's beats were moved there
    int mode = 1;                   // bettergrid_halfbeat
    double share = 0;               // the share of Beat This!'s beats that lie between rekordbox's
    double on = NAN, between = NAN; // the kick band's onsets at rekordbox's beats and halfway between them
    double levelOn = NAN, levelBetween = NAN;  // the kick band's level there, averaged over the beats
    bool loud = false;              // that level is clearly higher between rekordbox's beats (kHalfBeatLevel)
};

// rekordbox's beats a fixed distance off the kicks (offTheKick).
struct OffKick {
    bool seen = false;              // the combined grid lies between rekordbox's beats over the whole track, Beat This!'s beats on it
    bool agree = false;             // (the same)
    bool moved = false;             // the combined grid was used for it
    int mode = 1;                   // bettergrid_offbeat
    double offset = 0, share = 0;   // the combined grid's beat minus rekordbox's, s (the median), and the share of Beat This!'s own beats on that grid
    double atRb = NAN, atNew = NAN; // how strongly the kick band's level rises at rekordbox's beats and at the combined grid's
    double kickAt = 0;              // where it rises most around the combined grid's beats, s
    bool audio = false;             // at them, and kOffKickRise times as strongly as at rekordbox's beats
};

// The grid a fixed distance off the kicks, Beat This! included (kickLine).
struct KickLine {
    bool seen = false;              // the kick sits off the grid, regularly and clearly, over the whole track
    bool moved = false;             // and the grid was moved onto it
    int mode = 1;                   // bettergrid_kick
    double off = 0, slope = 0;      // the kick's line: offset from a beat at time t is off + slope * t, s
    double atStart = 0, atEnd = 0;  // that offset at the first and the last beat, s
    double share = 0, strength = 0; // the share of the beats with their kick on the line, and how clear those kicks are
};

// One analysis' grid being worked out next to rekordbox's beat job.
struct Job {
    std::string uri;
    std::shared_future<Combined> result;
    std::atomic<bool> closed{false};  // no early grid any more: the analysis has its result, was cancelled or is gone
    std::atomic<bool> abandoned{false};  // nobody will take the result any more: the worker need not work it out
    std::mutex showing;               // held while the early grid is handed to quickdeck
};

// A combined grid in use by an analysis (the beats as written), for the later hooks of that analysis. rbBars: when
// only the bar start was changed, rekordbox's own bar number of each beat.
struct Applied { std::string uri; std::vector<char> beats, rbBars; double at = 0; };

// Per analysis thread (CBeatAnalyzeThread*). Never destroyed: a worker may still run when rekordbox quits.
struct State {
    std::mutex lock;
    std::map<const void*, std::shared_ptr<Job>> jobs;
    std::map<const void*, Applied> applied;
};
}  // namespace

static State& state() {
    static State* s = new State;
    return *s;
}
static const double kAppliedFor = 900;  // s an applied grid stays valid for its analysis' later hooks

static bool applying() { return opt::apply.integer() != 0; }

// An entry of rekordbox's beat array.
static size_t beatSize() { return (size_t)S_beatInfo; }
static uint32_t beatWord(const char* d, long i) { return *(const uint32_t*)(d + (size_t)i * beatSize()); }
// The time of a beat, s. Only the low 10 bits of the third field are the µs: rekordbox leaves whatever was in the
// memory in the bits above them (seen on about half the entries past the first 64 of its own beat job's result).
static double beatTime(const char* d, long i) {
    const char* e = d + (size_t)i * beatSize();
    int us = beatSize() >= 12 ? std::min(*(const int*)(e + 8) & 0x3ff, 999) : 0;
    return *(const int*)(e + 4) / 1000.0 + us / 1e6;
}
static void setBeatTime(char* d, long i, double t) {
    char* e = d + (size_t)i * beatSize();
    long long us = llround(t * 1e6);
    *(int*)(e + 4) = (int)(us / 1000);
    if (beatSize() >= 12) *(int*)(e + 8) = (int)(us % 1000);
}
static BeatArray view(std::vector<char>& beats) {
    int n = (int)(beats.size() / beatSize());
    return BeatArray{beats.data(), n, n};
}

static std::atomic<int> g_model{0};  // 1 loaded, -1 failed, 0 not tried yet (or loading)
static beatthis::Model& model() {
    static beatthis::Model& m = *new beatthis::Model;  // never destroyed: a worker may still run when rekordbox quits
    static std::once_flag once;
    std::call_once(once, [] {
#ifdef _WIN32
        const char* file = "beatthis.onnx";   // ONNX Runtime (engines/beatthis_ort.cpp)
#else
        const char* file = "beatthis.mlmodelc";
#endif
        std::string res = plat::resourcesDir() + "/" + file, err;
        bool there = std::filesystem::exists(plat::fsPath(res));
        double t0 = now();
        auto units = setting("gpu").integer() ? beatthis::ComputeUnits::CpuAndGpu : beatthis::ComputeUnits::CpuOnly;
        bool ok = there && m.load(res, &err, units);
        if (!ok) logf("bettergrid: model not loaded: %s", there ? err.c_str() : "model missing");
        else logf("bettergrid: model loaded (%.1fs, %s)", now() - t0, m.device().c_str());
        g_model = ok ? 1 : -1;
    });
    return m;
}

// Onset strength per millisecond (index i = the ms starting at i ms): how sharply the energy rises, mostly in the
// kick drum's band (< 150 Hz), partly over the whole spectrum. kick: the kick band's alone, smoothed over 5 ms.
// level: the kick band's RMS per ms.
// The low-pass runs block by block (its state carried over), so no second and third copy of the track is held.
static std::vector<float> onsets(const std::vector<float>& x, double rate, std::vector<float>* kick, std::vector<float>* level = nullptr) {
    size_t n = x.size();
    double w = 2 * M_PI * 150 / rate, alpha = sin(w) / (2 * 0.7071), a0 = 1 + alpha;
    float co[5] = {(float)((1 - cos(w)) / 2 / a0), (float)((1 - cos(w)) / a0), (float)((1 - cos(w)) / 2 / a0),
                   (float)(-2 * cos(w) / a0), (float)((1 - alpha) / a0)};  // 2nd-order Butterworth low-pass
    size_t frames = (size_t)(n / rate * 1000);
    auto edge = [&](size_t i) { return std::min(n, (size_t)(i * rate / 1000)); };  // first sample of ms i
    std::vector<float> eLow(frames), eAll(frames);
    if (level) level->assign(frames, 0.f);
    const size_t kBlock = 4096;  // ms filtered at a time
    std::vector<float> in, low;
    float sIn[2] = {0, 0}, sLow[2] = {0, 0};  // the filter's state: the last two samples in and out
    for (size_t f0 = 0; f0 < frames; f0 += kBlock) {
        size_t f1 = std::min(frames, f0 + kBlock), a = edge(f0), m = edge(f1) - a;
        in.assign(m + 2, 0.f), low.assign(m + 2, 0.f);
        in[0] = sIn[0], in[1] = sIn[1], low[0] = sLow[0], low[1] = sLow[1];
        std::copy(x.begin() + a, x.begin() + a + m, in.begin() + 2);
        if (m) dsp::biquad(in.data(), co, low.data(), m);
        for (size_t i = f0; i < f1; i++) {
            size_t p = edge(i) - a, q = edge(i + 1) - a;
            float e = dsp::sumSquares(in.data() + 2 + p, q - p), f = dsp::sumSquares(low.data() + 2 + p, q - p);
            eAll[i] = logf(1 + 100 * e), eLow[i] = logf(1 + 100 * f);
            if (level && q > p) (*level)[i] = sqrtf(f / (float)(q - p));
        }
        sIn[0] = in[m], sIn[1] = in[m + 1], sLow[0] = low[m], sLow[1] = low[m + 1];
    }
    std::vector<float> oLow(frames, 0.f), oAll(frames, 0.f);
    double sLowSum = 0, sAllSum = 0;
    for (size_t i = 4; i < frames; i++) {
        oLow[i] = std::max(0.f, eLow[i] - eLow[i - 4]), oAll[i] = std::max(0.f, eAll[i] - eAll[i - 4]);
        sLowSum += oLow[i], sAllSum += oAll[i];
    }
    float mLow = frames ? (float)std::max(1e-9, sLowSum / frames) : 1.f, mAll = frames ? (float)std::max(1e-9, sAllSum / frames) : 1.f;
    std::vector<float> o(frames);
    for (size_t i = 0; i < frames; i++) o[i] = 0.6f * oLow[i] / mLow + 0.4f * oAll[i] / mAll;
    if (kick) {
        kick->assign(frames, 0.f);
        for (size_t i = 2; i + 2 < frames; i++) (*kick)[i] = (oLow[i - 2] + oLow[i - 1] + oLow[i] + oLow[i + 1] + oLow[i + 2]) / (5 * mLow);
    }
    return o;
}

// How well a line through the onsets' offsets (ds at times ts) with slope b fits: its intercept (a median, then the
// mean of the offsets within 10 ms of it) and the mean distance of the offsets from it, each capped at 20 ms.
static double lineFit(const std::vector<double>& ts, const std::vector<double>& ds, double b, double* a) {
    *a = 0;
    if (ts.empty()) return 0;
    std::vector<double> r(ts.size());
    for (size_t i = 0; i < ts.size(); i++) r[i] = ds[i] - b * ts[i];
    std::vector<double> s = r;
    std::nth_element(s.begin(), s.begin() + s.size() / 2, s.end());
    *a = s[s.size() / 2];
    for (int it = 0; it < 2; it++) {
        double sum = 0, n = 0;
        for (double x : r)
            if (fabs(x - *a) < 0.010) sum += x, n++;
        if (n) *a = sum / n;
    }
    double m = 0;
    for (double x : r) m += std::min(fabs(x - *a), 0.020);
    return m / r.size();
}

// Beat This!'s grid moved onto the audio's onsets (see the top).
static void placeOnOnsets(Combined& c, const std::vector<float>& o) {
    double period = 60 / c.bt.bpm;
    c.first = c.bt.firstBeat, c.bpm = c.bt.bpm;
    if (!(period > 0.05) || !std::isfinite(period) || !std::isfinite(c.first)) return;
    std::vector<float> sm(o.size(), 0.f);
    for (size_t i = 2; i + 2 < o.size(); i++) sm[i] = (o[i - 2] + o[i - 1] + o[i] + o[i + 1] + o[i + 2]) / 5;
    std::vector<float> sorted = sm;
    std::nth_element(sorted.begin(), sorted.begin() + sorted.size() / 2, sorted.end());
    float level = sorted.empty() ? 0 : sorted[sorted.size() / 2];
    std::vector<double> ts, ds;
    for (long k = (long)ceil(-c.bt.firstBeat / period);; k++) {
        long m = lround((c.bt.firstBeat + k * period) * 1000);
        if (m + 40 >= (long)sm.size()) break;
        if (m < 40) continue;
        long j = m - 40;
        for (long i = m - 40; i <= m + 40; i++)
            if (sm[i] > sm[j]) j = i;
        if (sm[j] > 3 * level) ts.push_back(m / 1000.0), ds.push_back((j - m) / 1000.0);
    }
    c.clearBeats = (int)ts.size();
    if (ts.size() < 32) return;  // too few clear onsets: Beat This!'s grid as it is
    std::vector<double> d = ds;
    std::nth_element(d.begin(), d.begin() + d.size() / 2, d.end());
    double a = d[d.size() / 2], b = 0;
    for (double tol : {0.015, 0.008}) {  // robust line: keep offsets near it, least squares, twice
        double n = 0, mt = 0, md = 0;
        for (size_t i = 0; i < ts.size(); i++)
            if (fabs(ds[i] - (a + b * ts[i])) < tol) n++, mt += ts[i], md += ds[i];
        if (n < 16) break;
        mt /= n, md /= n;
        double sxx = 0, sxy = 0;
        for (size_t i = 0; i < ts.size(); i++)
            if (fabs(ds[i] - (a + b * ts[i])) < tol) sxx += (ts[i] - mt) * (ts[i] - mt), sxy += (ts[i] - mt) * (ds[i] - md);
        b = sxx > 0 ? std::max(-0.003, std::min(0.003, sxy / sxx)) : 0;
        a = md - b * mt;
    }
    // A whole BPM when it fits about as well: most music is made at one, and 127.99 looks wrong. Kept when it fits
    // the onsets as well (mean distance at most 0.5 ms more) or drifts at most 12 ms from the fitted tempo at the
    // ends of the track. Tested on 100 tracks: only the truly fractional ones (125.70, 127.98, 174.98) stay.
    double fitted = 60 / (period * (1 + b)), whole = round(fitted);
    if (fabs(fitted - whole) >= 0.005 && fabs(fitted - whole) < 0.1) {
        double bw = 60 / whole / period - 1, af, aw;
        double mf = lineFit(ts, ds, b, &af), mw = lineFit(ts, ds, bw, &aw);
        double drift = fabs(bw - b) * (ts.back() - ts.front()) / 2;
        if (mw <= mf + 0.0005 || drift <= 0.012) b = bw, a = aw, c.wholeFrom = fitted;
    }
    period *= 1 + b;
    c.first = c.bt.firstBeat * (1 + b) + a;
    c.first -= floor(c.first / period) * period;
    c.bpm = 60 / period;
    c.shiftMs = a * 1000;
    c.onsetFit = true;
}

// Worker thread: Beat This! and the combination for one track. gone: the analysis stopped waiting for it (its wait
// ran out, it was cancelled or went on to another track): a worker that hasn't started decoding or the model yet
// leaves it then, or a batch on a slow machine queues up workers that each decode a whole track for nothing.
static Combined combine(const std::string& uri, const std::atomic<bool>& gone) {
    Combined c;
    double t0 = now();
    if (!model().loaded()) return c;  // (waits here, not on the analysis thread, while the model still loads)
    if (gone) return c;
    int rate = 0;
    auto mono = analysisAudio(uri, &rate);
    if (!mono || rate <= 0 || mono->size() < (size_t)rate * 2) return c;
    if (gone) return c;
    c.duration = (double)mono->size() / rate;
    beatthis::Result r = model().analyze(mono->data(), mono->size(), rate);
    c.bt = beatthis::fitGrid(r);
    if (!c.bt.ok || !(c.bt.bpm > 0) || !std::isfinite(c.bt.bpm) || !std::isfinite(c.bt.firstBeat)) {
        logf("bettergrid: no grid for %s (%zu beats)", uri.c_str(), r.beats.size());
        return c;
    }
    c.ok = true;
    c.beats = std::move(r.beats);
    std::sort(c.beats.begin(), c.beats.end());
    c.btDownbeat = c.bt.firstBeat + 60 / c.bt.bpm * c.bt.downbeatIndex;
    c.use = c.bt.confidence >= opt::minFit.number();
    if (c.use) {
        auto kick = std::make_shared<std::vector<float>>(), level = std::make_shared<std::vector<float>>();
        placeOnOnsets(c, onsets(*mono, rate, kick.get(), level.get()));
        c.kick = kick, c.level = level;
        if (opt::bars.integer()) {
            double b0 = now();
            c.bands = std::make_shared<const barphase::Bands>(barphase::bands(mono->data(), mono->size(), rate));
            c.bandsMs = (now() - b0) * 1000;
        }
        double period = 60 / c.bpm;
        c.downbeat = c.first + round((c.btDownbeat - c.first) / period) * period;
    }
    // The genre, for the tempo's octave. On this worker, next to rekordbox's beat job: when no genre is known yet the
    // genres mod runs its style model on this audio and keeps the result. Also for a grid that can't be used: this
    // is the only audio the genres mod gets of a Spotify track during its analysis (its Genre first).
    if (applying() && opt::genre.integer() && !gone) {
        using ForTrack = bool (*)(const char* uri, const std::vector<float>* mono, int rate, std::string* genre);
        if (auto forTrack = service<ForTrack>("genres.forTrack")) {
            double g0 = now();
            std::string genre;
            if (forTrack(uri.c_str(), mono.get(), rate, &genre)) c.genre = genre;
            c.genreTook = now() - g0;
        }
    }
    c.took = now() - t0;
    return c;
}

// Beat This!'s grid at twice or half its tempo (the octave the genre picked). Twice: a beat between each two. Half:
// every other beat, those on the downbeats (Beat This!'s downbeats are the bars' starts at either tempo, so they stay
// downbeats). Its own beats (for halfBeatOff) the same way.
static Combined atTempo(const Combined& c, double factor) {
    Combined o = c;
    if (!(factor == 2 || factor == 0.5) || !(c.bpm > 0) || !(c.bt.bpm > 0)) return o;
    double p = 60 / c.bpm, q = p / factor;
    double anchor = std::isfinite(c.downbeat) ? c.downbeat : c.first;
    o.bpm = c.bpm * factor;
    o.first = anchor - floor(anchor / q) * q;
    if (c.wholeFrom > 0) o.wholeFrom = c.wholeFrom * factor;
    double bp = 60 / c.bt.bpm, bq = bp / factor;
    double bAnchor = std::isfinite(c.btDownbeat) ? c.btDownbeat : c.bt.firstBeat;
    o.bt.bpm = c.bt.bpm * factor;
    o.bt.firstBeat = bAnchor - floor(bAnchor / bq) * bq;
    o.bt.downbeatIndex = (int)(lround((bAnchor - o.bt.firstBeat) / bq) % 4);
    o.beats.clear();
    if (factor == 2) {
        for (size_t i = 0; i < c.beats.size(); i++) {
            o.beats.push_back(c.beats[i]);
            if (i + 1 < c.beats.size() && c.beats[i + 1] - c.beats[i] < 1.5 * bp) o.beats.push_back((c.beats[i] + c.beats[i + 1]) / 2);
        }
    } else {
        for (double b : c.beats)
            if (lround((b - bAnchor) / bp) % 2 == 0) o.beats.push_back(b);
    }
    return o;
}

// The octave the genre picks for this grid (see genretempo::choose). rb: rekordbox's tempo, 0 when not known yet.
static GenreNote genreOctave(const Combined& c, double rb) {
    GenreNote n;
    n.from = c.bt.bpm, n.took = c.genreTook;
    if (c.genre.empty() || !c.use) return n;  // (an unused grid's genre was asked for the genres mod only)
    n.genre = c.genre;
    n.range = genretempo::rangeOf(c.genre);
    n.choice = genretempo::choose(c.bt.bpm, rb, n.range);
    return n;
}

static std::string songPath(const void* thread) {
    std::string uri;
    return analysisSong(thread, &uri) ? uri : "";
}

// ---- one analysis' job and its applied grid ---------------------------------------------------

// No early grid for this job from here on (waits for one that is being handed over just now, so it can't be saved
// after rekordbox's own grid).
static void close(const std::shared_ptr<Job>& job) {
    if (!job) return;
    job->closed = true;
    std::lock_guard<std::mutex> l(job->showing);
}
// The job's result won't be taken any more (it is out of the jobs map, or its analysis gave up waiting).
static void abandon(const std::shared_ptr<Job>& job) {
    if (!job) return;
    job->abandoned = true;
    close(job);
}
static std::shared_ptr<Job> jobOf(const void* thread, bool take) {
    State& s = state();
    std::lock_guard<std::mutex> l(s.lock);
    auto it = s.jobs.find(thread);
    if (it == s.jobs.end()) return nullptr;
    std::shared_ptr<Job> job = it->second;
    if (take) s.jobs.erase(it);
    return job;
}
static void dropApplied(const void* thread) {
    State& s = state();
    std::lock_guard<std::mutex> l(s.lock);
    s.applied.erase(thread);
}
// The combined grid of the analysis this thread runs, if it has one: the same track still, and not one left by an
// analysis long ago.
static bool appliedTo(const void* thread, Applied* out) {
    std::string uri = songPath(thread);
    State& s = state();
    std::lock_guard<std::mutex> l(s.lock);
    auto it = s.applied.find(thread);
    if (it == s.applied.end()) return false;
    if (uri.empty() || it->second.uri != uri || fabs(now() - it->second.at) > kAppliedFor) {
        s.applied.erase(it);
        return false;
    }
    *out = it->second;
    return true;
}

static bool writeGrid(BeatArray& a, const Combined& c);
static void bpmAnalysisEnds(const void* thread);

// quickdeck shows a grid on the deck as soon as there is one: Beat This!'s, while rekordbox's beat job still runs (when
// Beat This! is sure enough to be used at all). rekordbox's beat job then decides the final grid, as before.
static void showEarly(const void* thread, const std::string& uri, const Combined& c) {
    if (!g_on || !c.use || !applying()) return;
    auto show = service<void (*)(const void*, const char*, const void*, const char*)>("quickdeck.earlyGrid");
    if (!show) return;
    // At the genre's octave already (rekordbox's tempo isn't known yet: Beat This!'s doubled or halved only).
    GenreNote g = genreOctave(c, 0);
    Combined octave;
    bool octaved = g.choice.source == genretempo::Source::BeatThis;
    if (octaved) octave = atTempo(c, g.choice.factor);
    BeatArray a{nullptr, 0, 0};
    if (writeGrid(a, octaved ? octave : c)) show(thread, uri.c_str(), &a, "Beat This!'s");
    free(a.data);
}

// Service "bettergrid.prefetch" (called by core's beat job hook, on the analysis thread): the grid is worked out
// next to rekordbox's beat job. Returns at once: the model, the audio and Beat This! are the worker's.
static void gridPrefetch(const void* thread) {
    if (!thread) return;
    abandon(jobOf(thread, true));  // (one left by an analysis that never reached its grid)
    dropApplied(thread);         // a new beat job: the grid of the analysis before is of no use to it
    if (!g_on || g_model < 0) return;
    std::string uri = songPath(thread);
    if (uri.empty()) return;
#if defined(_WIN32) || RBM_MAC_X86
    // Where Beat This! is slow (on the CPU 15-25 s a track, which the analysis would wait for; or a slow GPU), a
    // track that is on a deck keeps rekordbox's own grid, which shows as soon as rekordbox has it: a DJ is waiting
    // for it. Tracks analysed from the Analysis window or the collection wait for Beat This! as before.
    // bettergrid_slow=1: wait on a deck too. (Windows and Intel Macs: on Apple Silicon it is never slow.)
    if (!opt::slow.integer() && model().slow()) {
        bool onDeck = false;
        for (int ch = 0; ch < deckCount(); ch++) onDeck |= deckPath(ch) == uri;
        if (onDeck) {
            logf("bettergrid: Beat This! is slow on this %s (%s): rekordbox's grid for the track on the deck, at once"
                 "  %s", RBM_MAC_X86 ? "Mac" : "PC", model().device().c_str(), uri.c_str());
            return;
        }
    }
#endif
    auto job = std::make_shared<Job>();
    auto p = std::make_shared<std::promise<Combined>>();
    job->uri = uri;
    job->result = p->get_future().share();
    {
        State& s = state();
        std::lock_guard<std::mutex> l(s.lock);
        s.jobs[thread] = job;
    }
    try {
        std::thread([job, p, thread] {
            Combined c;
            try {
                c = combine(job->uri, job->abandoned);
            } catch (...) {
                logf("bettergrid: failed on %s, rekordbox's grid kept", job->uri.c_str());
                c = Combined();
            }
            if (!provided("betterkey.prefetch")) analysisAudioDone(job->uri);  // betterkey frees it at the key stage; else it would stay minutes
            try {
                // Before the result is handed over, so rekordbox's own grid isn't saved meanwhile; and not at all
                // once the analysis has stopped waiting, was cancelled or went on to another track.
                std::lock_guard<std::mutex> l(job->showing);
                if (!job->closed) showEarly(thread, job->uri, c);
            } catch (...) {
            }
            p->set_value(std::move(c));
        }).detach();
    } catch (...) {
        logf("bettergrid: no thread for %s, rekordbox's grid kept", uri.c_str());
        jobOf(thread, true);
    }
}
RBM_PROVIDE("bettergrid", "bettergrid.prefetch", gridPrefetch, &g_on);

// Service "bettergrid.stage" (core's analysis hooks, on the analysis thread): a stage of the analysis starts.
// From the phrase analysis on, rekordbox's grid is saved: no early grid any more. A new analysis ("start") and the
// last step of one ("seek table") end what the analysis before left on this thread, so a later analysis of the same
// track without a beat job (a locked grid, a key-only analysis) never gets an old grid.
static void gridStage(const void* thread, const char* stage) {
    if (!thread || !stage || !strcmp(stage, "beat job")) return;  // (beat job: the prefetch follows)
    bool ends = !strcmp(stage, "start") || !strcmp(stage, "seek table");
    if (ends) abandon(jobOf(thread, true));
    else close(jobOf(thread, false));
    if (ends) dropApplied(thread);
    if (ends) bpmAnalysisEnds(thread);
}
RBM_PROVIDE("bettergrid", "bettergrid.stage", gridStage, &g_on);

// ---- rekordbox's grid and the combination -----------------------------------------------------

// rekordbox's grid, for the log and the comparison: the tempo most of its beats have.
static RbGrid rekordboxGrid(const BeatArray& a) {
    RbGrid g;
    if (!a.data || a.used < 1) return g;
    std::map<uint32_t, int> tempos;
    for (int i = 0; i < a.used; i++) tempos[beatWord(a.data, i) >> 16]++;
    uint32_t tempo = beatWord(a.data, 0) >> 16;
    for (auto& t : tempos)
        if (t.second > tempos[tempo]) tempo = t.first;
    g.bpm = tempo / 100.0;
    g.firstBeat = beatTime(a.data, 0);
    for (int i = 0; i < std::min(a.used, 8); i++)
        if ((beatWord(a.data, i) & 0xffff) == 1) { g.firstDownbeat = beatTime(a.data, i); break; }
    return g;
}

// The bar start of beats put at Beat This!'s downbeat. False when nothing changed: the bars start there already
// (always: every bar is numbered again, for beats that were moved). period: Beat This!'s beat, s. Its downbeats are 4
// of its beats apart, and the one to go by is the first that lies in these beats: its first downbeat is often a beat
// before rekordbox's first beat (a track that starts on a kick, which rekordbox's grid leaves out), and the beat
// nearest to that one is a beat late.
static bool renumber(std::vector<char>& beats, double downbeat, double period, bool always) {
    long n = (long)(beats.size() / beatSize());
    if (n < 4) return false;
    long kd = 0;
    if (std::isfinite(downbeat) && period > 0 && std::isfinite(period)) {
        double from = beatTime(beats.data(), 0) - period / 2;
        downbeat += ceil((from - downbeat) / (4 * period)) * 4 * period;
        for (long i = 1; i < n; i++)
            if (fabs(beatTime(beats.data(), i) - downbeat) < fabs(beatTime(beats.data(), kd) - downbeat)) kd = i;
    } else if (!always) {
        return false;
    }
    if (!always && (beatWord(beats.data(), kd) & 0xffff) == 1) return false;
    bool changed = false;
    for (long k = 0; k < n; k++) {
        uint32_t* w = (uint32_t*)(beats.data() + (size_t)k * beatSize());
        uint32_t v = (*w & 0xffff0000u) | (uint32_t)(((k - kd) % 4 + 4) % 4 + 1);
        changed |= v != *w;
        *w = v;
    }
    return changed;
}

// rekordbox leaves out a beat that falls before the start of the file, so on a track that starts on a kick (its grid
// puts that beat 1-5 ms early on 28% of 1,600 logged tracks) the grid begins at the second beat. A beat that would
// fall at most kStartBeat before the start is added at 0 s, numbered as the beat before the first.
static const double kStartBeat = 0.010;
static bool addStartBeat(std::vector<char>& beats) {
    long n = (long)(beats.size() / beatSize());
    if (n < 2) return false;
    double t0 = beatTime(beats.data(), 0), before = t0 - (beatTime(beats.data(), 1) - t0);
    if (!(before < 0) || before < -kStartBeat) return false;
    std::vector<char> first(beats.begin(), beats.begin() + (long)beatSize());
    uint32_t* w = (uint32_t*)first.data();
    uint32_t inBar = *w & 0xffff;
    *w = (*w & 0xffff0000u) | (inBar >= 2 && inBar <= 4 ? inBar - 1 : 4);
    setBeatTime(first.data(), 0, 0);
    beats.insert(beats.begin(), first.begin(), first.end());
    return true;
}

// Are rekordbox's beats half a beat off? Two witnesses (see the top). Beat This!: its own beats (not its fitted
// grid, which may drift against rekordbox's tempo over the track) lie between rekordbox's beats nearly everywhere.
// The audio: the kick band's onsets, at rekordbox's beats and halfway between them.
static const double kHalfBeatOnsets = 1.25;  // how much stronger the onsets between rekordbox's beats must be
// How much louder the kick band must be halfway between rekordbox's beats for fewer of Beat This!'s beats (kLoudShare)
// to do. On the test tracks (2026-11-19) the two tracks sitting on the off-beats came out at 2.7x and 3.9x, tracks on
// the beat (also with rolling off-beat bass) at 0.6x-1.3x, among them 5jkx7DryrO1NhJIWkTqcKH (0.86x), where a
// listen had rekordbox right.
static const double kHalfBeatLevel = 2.0, kLoudShare = 0.6;
// The kick band's level averaged over rekordbox's beats, at each ms from half a beat before to half a beat after:
// its highest within 60 ms of the beats (*on) and within 60 ms of halfway between them (*between).
static bool levelAround(const BeatArray& a, const std::vector<float>& level, double* on, double* between) {
    int n = a.used;
    if (n < 16) return false;
    double period = (beatTime(a.data, n - 1) - beatTime(a.data, 0)) / (n - 1);
    long h = lround(period * 500);
    if (h < 100 || h > 1000) return false;
    std::vector<double> acc((size_t)(2 * h + 1), 0.0);
    long counted = 0;
    for (int j = 0; j < n; j++) {
        long m = lround(beatTime(a.data, j) * 1000);
        if (m - h < 0 || m + h >= (long)level.size()) continue;
        for (long o = -h; o <= h; o++) acc[(size_t)(o + h)] += level[(size_t)(m + o)];
        counted++;
    }
    if (counted < 16) return false;
    double sOn = 0, sBetween = 0;
    for (long o = -h; o <= h; o++) {
        double v = acc[(size_t)(o + h)] / counted;
        if (labs(o) <= 60) sOn = std::max(sOn, v);
        if (labs(o) >= h - 60) sBetween = std::max(sBetween, v);
    }
    *on = sOn, *between = sBetween;
    return sOn > 0;
}
// The bar start from where the kick comes in: the beat nearest to where the kick band's level first reaches a quarter
// of its usual level (90th percentile, over 20 ms) is a bar's first beat in nearly every DJ track. When rekordbox's
// numbering has a 1 there and Beat This!'s doesn't, rekordbox's bars are kept. Beat This! often hears a track
// start mid-bar; rekordbox puts its 1 where the music starts. On 30 test tracks where the two disagree
// (2026-11-19), the section changes (loudness jumps in three bands) said Beat This!'s bar start was right on 18
// and rekordbox's on 12. Deciding with this rule first, and with Beat This!'s otherwise, got 24 right. It fixed the
// bars on 5BCgHsiBGAd88inU7uSfox, 3DboHPRHcpgiMO3jYabfFW, 5yiaktMqddRQ20RoTeJt8m and 6xcXPXxNZyZrcLcJLhzBGL, which
// started on their 3 or 4. Returns the time of that beat when rekordbox's 1 is to be kept, else NAN.
static double kickStartBar(const std::vector<char>& beats, const std::vector<float>& level, double btDownbeat, double period) {
    long n = (long)(beats.size() / beatSize());
    if (n < 8 || level.size() < 2000 || !std::isfinite(btDownbeat) || !(period > 0)) return NAN;
    std::vector<float> sm(level.size(), 0.f);
    double run = 0;
    for (size_t i = 0; i < level.size(); i++) {  // mean over the 20 ms around i
        run += level[i];
        if (i >= 20) run -= level[i - 20];
        if (i >= 10) sm[i - 10] = (float)(run / 20);
    }
    std::vector<float> sorted(sm);
    std::nth_element(sorted.begin(), sorted.begin() + (long)(sorted.size() * 9 / 10), sorted.end());
    float threshold = 0.25f * sorted[sorted.size() * 9 / 10];
    if (!(threshold > 0)) return NAN;
    size_t at = 0;
    while (at < sm.size() && sm[at] <= threshold) at++;
    if (at >= sm.size()) return NAN;
    double t = at / 1000.0;
    long s = 0;
    for (long i = 1; i < n; i++)
        if (fabs(beatTime(beats.data(), i) - t) < fabs(beatTime(beats.data(), s) - t)) s = i;
    double ts = beatTime(beats.data(), s);
    bool rbOne = (beatWord(beats.data(), s) & 0xffff) == 1;
    long k = lround((ts - btDownbeat) / period);
    bool btOne = ((k % 4) + 4) % 4 == 0;
    return rbOne && !btOne ? ts : NAN;
}

// The bar start from where the music changes (mods/barphase.h; the idea from rbxport's downbeat docs), when
// rekordbox's and Beat This!'s bar starts differ: the one of the two whose beat position in the bar has more of the
// track's change points, if it leads by at least kBarMargin of all of them; otherwise kickStartBar decides as before.
// Measured on 597 tracks (2026-11-19): where rekordbox and Beat This! agree (330, taken as right), it puts the
// true 1 above a wrong one 96% of the time, 98.7% of those it decides with the margin (about 92%); the kick-in rule's
// premise (the beat where the kick comes in is a 1) held on 78.5% of them. It agrees with the user's hand cues (cues
// set before the mods, 30 tracks) as often as Beat This!, and with the bars of the 7 tracks found wrong on 11-19 and 3
// checked ones. Of the 55 tracks where the two disagree it decides 41, differently from the kick-in rule on 8. As a
// witness for a grid on the off-beats it isn't good enough: on those grids its best position was an off-beat on
// 51 of 597, with nothing in the kick band to back that, so halfBeatOff doesn't ask it.
static const double kBarMargin = 0.05;
struct BarVote {
    bool asked = false;            // the bar starts differ and the music was asked
    int pick = 0;                  // 1 rekordbox's, 2 Beat This!'s, 0 undecided
    double rb = 0, bt = 0;         // their scores, as shares of all
    double at = NAN;               // the picked bar's first beat, s
};
static BarVote barByMusic(const std::vector<char>& beats, const Combined& c, double btDownbeat, double period) {
    BarVote v;
    long n = (long)(beats.size() / beatSize());
    if (!c.bands || n < 34 || !std::isfinite(btDownbeat) || !(period > 0)) return v;
    long jr = -1, jb = 0;
    for (long i = 0; i < std::min(n, 8L) && jr < 0; i++)
        if ((beatWord(beats.data(), i) & 0xffff) == 1) jr = i;
    if (jr < 0) return v;
    double from = beatTime(beats.data(), 0) - period / 2;  // Beat This!'s first downbeat within the beats (as renumber)
    btDownbeat += ceil((from - btDownbeat) / (4 * period)) * 4 * period;
    for (long i = 1; i < n; i++)
        if (fabs(beatTime(beats.data(), i) - btDownbeat) < fabs(beatTime(beats.data(), jb) - btDownbeat)) jb = i;
    if ((jr - jb) % 4 == 0) return v;
    std::vector<double> times((size_t)n);
    for (long i = 0; i < n; i++) times[(size_t)i] = beatTime(beats.data(), i);
    std::array<double, 8> sc = barphase::score(*c.bands, times);
    double all = 0;
    for (double x : sc) all += x;
    if (!(all > 0)) return v;
    v.asked = true;
    v.rb = sc[(size_t)(2 * (jr % 4))] / all, v.bt = sc[(size_t)(2 * (jb % 4))] / all;
    if (v.rb >= v.bt + kBarMargin) v.pick = 1, v.at = times[(size_t)jr];
    else if (v.bt >= v.rb + kBarMargin) v.pick = 2, v.at = times[(size_t)jb];
    return v;
}

static HalfBeat halfBeatOff(const BeatArray& a, const Combined& c) {
    HalfBeat h;
    int n = a.used;
    if (!a.data || n < 16 || c.beats.size() < 32) return h;
    if (c.level && levelAround(a, *c.level, &h.levelOn, &h.levelBetween)) h.loud = h.levelBetween >= kHalfBeatLevel * h.levelOn;
    long on = 0, between = 0, all = 0;
    int i = 0;
    for (double b : c.beats) {
        while (i + 1 < n && beatTime(a.data, i + 1) <= b) i++;
        if (i + 1 >= n) break;
        double t0 = beatTime(a.data, i), t1 = beatTime(a.data, i + 1);
        if (b < t0 || !(t1 > t0)) continue;
        double f = (b - t0) / (t1 - t0);
        all++;
        if (f < 0.2 || f > 0.8) on++;
        else if (f > 0.3 && f < 0.7) between++;
    }
    if (opt::debug.number() > 0)
        logf("bettergrid: of Beat This!'s %zu beats (%.3f, %.3f, %.3f .. %.3f s) %ld lie in rekordbox's grid: %ld at its beats, %ld between; analysed %.1f s",
             c.beats.size(), c.beats[0], c.beats[1], c.beats[2], c.beats.back(), all, on, between, c.duration);
    if (all < 32) return h;
    h.share = (double)between / all;
    h.seen = (between >= 0.8 * all && on <= 0.1 * all) || (h.loud && between >= kLoudShare * all && on <= 0.25 * all);
    if (!h.seen || !c.kick) return h;
    const std::vector<float>& k = *c.kick;
    auto peak = [&](double t) {  // the strongest onset within 20 ms; -1 outside the audio
        long m = lround(t * 1000);
        return m < 20 || m + 20 >= (long)k.size() ? -1.0 : (double)*std::max_element(k.begin() + (m - 20), k.begin() + (m + 21));
    };
    double sOn = 0, sBetween = 0;
    long counted = 0;
    for (int j = 0; j + 1 < n; j++) {
        double t0 = beatTime(a.data, j), t1 = beatTime(a.data, j + 1);
        double p = peak(t0), q = peak((t0 + t1) / 2);
        if (p < 0 || q < 0) continue;
        sOn += p, sBetween += q, counted++;
    }
    if (counted >= 32) h.on = sOn / counted, h.between = sBetween / counted;
    return h;
}

// Are rekordbox's beats a fixed distance off the kicks (see the top)? Two witnesses, as for half a beat.
// Beat This!: the combined grid (its tempo, placed on the onsets) lies between rekordbox's beats over the whole track,
// at least kOffKickMin from them and from halfway between them (that case is halfBeatOff's; the 10th and 90th
// percentile at least kOffKickEdge and on the same side, so the two grids never cross), and most of Beat This!'s own
// beats are on that grid. The distance is measured against the combined grid and not as one number, because
// rekordbox's tempo is often a hair off on these tracks: on Side Effect (Robert Hood, 2026-11-26) its beats are 93 ms
// late at the start and 120 ms at the end, and no single distance holds 80% of Beat This!'s beats.
// The audio: the kick sets in at the combined grid's beats, and clearly more strongly than at rekordbox's.
static const double kOffKickMin = 0.060, kOffKickEdge = 0.040, kOffKickNear = 0.030, kOffKickShare = 0.7, kOffKickRise = 1.5;
static OffKick offTheKick(const BeatArray& a, const Combined& c) {
    OffKick k;
    int n = a.used;
    if (!a.data || n < 32 || c.beats.size() < 32 || !c.level || !(c.bpm > 0)) return k;
    double p = 60 / c.bpm, period = (beatTime(a.data, n - 1) - beatTime(a.data, 0)) / (n - 1);
    auto fromGrid = [&](double t) {  // t minus the combined grid's nearest beat
        double d = fmod(t - c.first, p);
        if (d > p / 2) d -= p;
        if (d < -p / 2) d += p;
        return d;
    };
    std::vector<double> d((size_t)n);
    for (int j = 0; j < n; j++) d[(size_t)j] = -fromGrid(beatTime(a.data, j));  // the combined grid's beat minus rekordbox's
    std::sort(d.begin(), d.end());
    double m = d[(size_t)n / 2], lo = d[(size_t)n / 10], hi = d[(size_t)(n - 1 - n / 10)];
    long near = 0;
    for (double b : c.beats) near += fabs(fromGrid(b)) <= kOffKickNear;
    k.offset = m, k.share = (double)near / (double)c.beats.size();
    auto between = [&](double x, double edge) { return fabs(x) >= edge && fabs(x) <= period / 2 - edge; };
    if (!between(m, kOffKickMin) || !between(lo, kOffKickEdge) || !between(hi, kOffKickEdge) || (lo < 0) != (hi < 0) || k.share < kOffKickShare) return k;
    k.seen = k.agree = true;
    // The audio: where the kick sets in. The kick band's level, averaged over 17 ms (a kick's own 50-100 Hz ripple is
    // not a new kick, and it is what made the onsets used for half a beat useless here: 100 ms after a kick the drum
    // still rings), and its rise over 16 ms, summed at each ms around every beat of the combined grid, and at
    // rekordbox's beats.
    const std::vector<float>& level = *c.level;
    long h = lround(period * 500), size = (long)level.size();
    if (h < 100 || h > 1000 || size < 4 * h) return k;
    std::vector<double> sum((size_t)size + 1, 0.0);
    for (long j = 0; j < size; j++) sum[(size_t)j + 1] = sum[(size_t)j] + level[(size_t)j];
    auto rise = [&](long ms) {  // of the mean level over 17 ms, from 8 ms before to 8 ms after
        return std::max(0.0, (sum[(size_t)(ms + 17)] - sum[(size_t)ms] - sum[(size_t)(ms + 1)] + sum[(size_t)(ms - 16)]) / 17);
    };
    std::vector<double> acc((size_t)(2 * h + 1), 0.0);
    long counted = 0;
    for (double t = c.first; t < c.duration; t += p) {
        long ms = lround(t * 1000);
        if (ms - h - 16 < 0 || ms + h + 17 >= size) continue;
        for (long o = -h; o <= h; o++) acc[(size_t)(o + h)] += rise(ms + o);
        counted++;
    }
    double atRb = 0;
    long countedRb = 0;
    std::vector<double> accRb(31, 0.0);
    for (int j = 0; j < n; j++) {
        long ms = lround(beatTime(a.data, j) * 1000);
        if (ms - 32 < 0 || ms + 33 >= size) continue;
        for (long o = -15; o <= 15; o++) accRb[(size_t)(o + 15)] += rise(ms + o);
        countedRb++;
    }
    if (counted < 32 || countedRb < 32) return k;
    for (double v : accRb) atRb = std::max(atRb, v / countedRb);
    long best = 0;
    double atBest = -1, atNew = 0;
    for (long o = -h; o <= h; o++) {
        double v = acc[(size_t)(o + h)] / counted;
        if (v > atBest) atBest = v, best = o;
        if (labs(o) <= 25) atNew = std::max(atNew, v);
    }
    k.atRb = atRb, k.atNew = atNew, k.kickAt = best / 1000.0;
    k.audio = labs(best) <= 25 && atNew >= kOffKickRise * atRb;
    return k;
}

// Is the grid as a whole a fixed distance off the kicks, where Beat This! doesn't hear them either (see the top)?
// The kick alone: for each beat of the grid, where the kick band's level (17 ms mean) rises most within
// kKickWindow of a beat, a robust line through those offsets over the track (a shift and a tempo a hair different),
// and how many beats have their rise within kKickNear of that line. The grid is moved onto the line when the kick is
// that regular (kKickShare of the beats), clear (the rise kKickStrength times the window's median, on the beats on
// the line), and the line lies kKickMin or more from the grid at both ends of the track, on the same side, and not
// near half a beat (halfBeatOff's case). Measured on 103 tracks (2026-11-26): the kick's line lies within 25 ms of
// the grid on every track with a clear kick; only Red Passion II - A (Robert Hood, 3EdePBazZYVabOGfKGma1D: a noisy
// percussion layer on the beat, which rekordbox and Beat This! both follow, the kick 71 ms after it) passes.
static const double kKickWindow = 0.45, kKickNear = 0.010, kKickShare = 0.65, kKickStrength = 5, kKickMin = 0.050, kKickMaxSlope = 0.0005;
static KickLine kickLine(const BeatArray& a, const Combined& c) {
    KickLine k;
    int n = a.used;
    if (!a.data || n < 64 || !c.level) return k;
    const std::vector<float>& level = *c.level;
    long size = (long)level.size();
    double period = (beatTime(a.data, n - 1) - beatTime(a.data, 0)) / (n - 1);
    long h = lround(period * 1000 * kKickWindow);
    if (h < 100 || h > 500) return k;
    std::vector<double> sum((size_t)size + 1, 0.0);
    for (long j = 0; j < size; j++) sum[(size_t)j + 1] = sum[(size_t)j] + level[(size_t)j];
    auto rise = [&](long ms) {  // the mean level over the 17 ms from ms on, minus that over the 17 ms up to ms
        return (sum[(size_t)(ms + 17)] - sum[(size_t)ms] - sum[(size_t)(ms + 1)] + sum[(size_t)(ms - 16)]) / 17;
    };
    std::vector<double> ts, ds, strength, w((size_t)(2 * h + 1));
    for (int j = 0; j < n; j++) {
        double t = beatTime(a.data, j);
        long ms = lround(t * 1000);
        if (ms - h - 16 < 0 || ms + h + 17 >= size) continue;
        long best = -h;
        for (long o = -h; o <= h; o++) {
            w[(size_t)(o + h)] = rise(ms + o);
            if (w[(size_t)(o + h)] > w[(size_t)(best + h)]) best = o;
        }
        double peak = w[(size_t)(best + h)];
        for (double& v : w) v = fabs(v);
        std::nth_element(w.begin(), w.begin() + h, w.end());
        ts.push_back(t), ds.push_back(best / 1000.0), strength.push_back(w[(size_t)h] > 0 ? peak / w[(size_t)h] : 0);
    }
    if (ts.size() < 64) return k;
    std::vector<double> m = ds;
    std::nth_element(m.begin(), m.begin() + m.size() / 2, m.end());
    double off = m[m.size() / 2], slope = 0;
    for (double tol : {0.030, 0.015, 0.008}) {  // robust line: keep offsets near it, least squares
        double cnt = 0, mt = 0, md = 0;
        for (size_t i = 0; i < ts.size(); i++)
            if (fabs(ds[i] - (off + slope * ts[i])) < tol) cnt++, mt += ts[i], md += ds[i];
        if (cnt < 16) break;
        mt /= cnt, md /= cnt;
        double sxx = 0, sxy = 0;
        for (size_t i = 0; i < ts.size(); i++)
            if (fabs(ds[i] - (off + slope * ts[i])) < tol) sxx += (ts[i] - mt) * (ts[i] - mt), sxy += (ts[i] - mt) * (ds[i] - md);
        slope = sxx > 0 ? sxy / sxx : 0;
        off = md - slope * mt;
    }
    std::vector<double> strong;
    for (size_t i = 0; i < ts.size(); i++)
        if (fabs(ds[i] - (off + slope * ts[i])) < kKickNear) strong.push_back(strength[i]);
    k.share = (double)strong.size() / (double)ts.size();
    if (!strong.empty()) {
        std::nth_element(strong.begin(), strong.begin() + strong.size() / 2, strong.end());
        k.strength = strong[strong.size() / 2];
    }
    k.off = off, k.slope = slope;
    k.atStart = off + slope * ts.front(), k.atEnd = off + slope * ts.back();
    auto clear = [&](double x) { return fabs(x) >= kKickMin && fabs(x) <= period / 2 - 0.060; };
    k.seen = clear(k.atStart) && clear(k.atEnd) && (k.atStart < 0) == (k.atEnd < 0) && fabs(slope) <= kKickMaxSlope &&
             k.share >= kKickShare && k.strength >= kKickStrength;
    return k;
}

// The beats moved onto the kick's line: t + off + slope * t, with their tempo to match; beats that end up before the
// start or after the end of the track are left out (rbBars, rekordbox's own bar numbers per beat, with them).
static std::vector<char> movedOntoKicks(const char* d, int n, const KickLine& k, double end, std::vector<char>* rbBars) {
    size_t size = beatSize();
    std::vector<char> out, bars;
    out.reserve((size_t)n * size);
    for (int i = 0; i < n; i++) {
        double t = beatTime(d, i), u = t + k.off + k.slope * t;
        if (u < 0 || (end > 0 && u > end)) continue;
        long at = (long)(out.size() / size);
        out.insert(out.end(), d + (size_t)i * size, d + (size_t)(i + 1) * size);
        uint32_t word = beatWord(d, i), tempo = (uint32_t)lround((word >> 16) / (1 + k.slope)) & 0xffff;
        *(uint32_t*)(out.data() + (size_t)at * size) = tempo << 16 | (word & 0xffff);
        setBeatTime(out.data(), at, u);
        if (rbBars && (size_t)i < rbBars->size()) bars.push_back((*rbBars)[(size_t)i]);
    }
    if (rbBars && !rbBars->empty()) *rbBars = std::move(bars);
    return out;
}

// rekordbox's beats spaced at another tempo than the one they're labelled with: on a few tracks its beat job labels
// every beat with one tempo (135.88) while their times run at another (135.95: Red Passion II - A, 2026-11-26; 5 of
// 2,246 saved grids drift 30 ms or more from their label over the track). The label is what bettergrid compares
// with Beat This!'s tempo, so such a grid passed as the same tempo and was kept, 200 ms off the music by the end.
// Returns how far rekordbox's beats drift from the combined grid's tempo over the track (s, last beat minus first),
// when they drift kRbDrift or more both from that and from their own label; 0 otherwise (or with tempo changes).
static const double kRbDrift = 0.040;
static double rbDrift(const BeatArray& a, const Combined& c) {
    int n = a.used;
    if (!a.data || n < 64 || !(c.bpm > 0)) return 0;
    uint32_t tempo = beatWord(a.data, 0) >> 16;
    double mi = (n - 1) / 2.0, mt = 0, sxx = 0, sxy = 0;
    for (int i = 0; i < n; i++) {
        if (beatWord(a.data, i) >> 16 != tempo) return 0;
        mt += beatTime(a.data, i);
    }
    mt /= n;
    for (int i = 0; i < n; i++) sxx += (i - mi) * (i - mi), sxy += (i - mi) * (beatTime(a.data, i) - mt);
    double spacing = sxy / sxx, label = tempo > 0 ? 6000.0 / tempo : 0;
    double fromLabel = (spacing - label) * (n - 1), fromCombined = (spacing - 60 / c.bpm) * (n - 1);
    return fabs(fromLabel) >= kRbDrift && fabs(fromCombined) >= kRbDrift ? fromCombined : 0;
}

// rekordbox's beats moved half a beat: each to the middle of its two neighbours (so its placement, and any tempo
// change, stay as they are), with one more before the first and after the last where the track has room for them.
// end: the track's length in s (0: unknown, no beat added after the last).
static std::vector<char> movedHalfBeat(const BeatArray& a, double end) {
    int n = a.used;
    if (!a.data || n < 4) return {};
    size_t size = beatSize();
    std::vector<char> out;
    out.reserve((size_t)(n + 1) * size);
    auto add = [&](int from, double t) {
        long at = (long)(out.size() / size);
        out.insert(out.end(), a.data + (size_t)from * size, a.data + (size_t)(from + 1) * size);
        setBeatTime(out.data(), at, t);
    };
    double lead = beatTime(a.data, 0) - (beatTime(a.data, 1) - beatTime(a.data, 0)) / 2;
    if (lead >= 0) add(0, lead);
    for (int i = 0; i + 1 < n; i++) add(i, (beatTime(a.data, i) + beatTime(a.data, i + 1)) / 2);
    double tail = beatTime(a.data, n - 1) + (beatTime(a.data, n - 1) - beatTime(a.data, n - 2)) / 2;
    if (tail < end) add(n - 1, tail);
    return out;
}

// rekordbox can land a little off a whole BPM (120.06 for a 120 track: 80 ms adrift by the end). When the onsets
// fit a whole BPM (placeOnOnsets rounds only when that fits as well; 127.98 stays; not when there were too few
// clear onsets to tell) and rekordbox's grid is one steady tempo, its beats are redrawn at the whole BPM through its
// own beat in the middle of the track: its beat placement is kept where it is most reliable, the drift goes. Bars
// from Beat This!, as for rekordbox's beats.
static bool wholeThroughRekordbox(const BeatArray& a, const RbGrid& g, const Combined& c, double downbeat, Combined* w) {
    double whole = round(c.bpm);
    if (!c.onsetFit || !a.data || a.used < 16 || !(g.bpm > 0)) return false;
    if (fabs(c.bpm - whole) >= 0.005 || fabs(g.bpm - whole) < 0.005 || fabs(whole / g.bpm - 1) > 0.002) return false;
    uint32_t tempo0 = beatWord(a.data, 0) >> 16;
    for (int i = 1; i < a.used; i++)
        if (beatWord(a.data, i) >> 16 != tempo0) return false;  // tempo changes: leave them
    double mid = beatTime(a.data, a.used / 2);
    double period = 60 / whole;
    *w = c;
    w->bpm = whole;
    w->first = mid - floor(mid / period) * period;
    w->downbeat = std::isfinite(downbeat) ? w->first + round((downbeat - w->first) / period) * period : w->first;
    w->wholeFrom = g.bpm;
    return true;
}

// Writes the combined grid into rekordbox's beat array: every beat from 0 s to the end of the track (and at 0 s the
// beat that falls just before it, as addStartBeat).
static bool writeGrid(BeatArray& a, const Combined& c) {
    if (!(c.bpm > 0) || !std::isfinite(c.bpm) || !std::isfinite(c.first) || !std::isfinite(c.duration)) return false;
    double period = 60 / c.bpm;
    double count = floor((c.duration - c.first) / period) + 1;
    if (!(count >= 4) || count > 100000) return false;
    long lead = c.first - period < 0 && c.first - period >= -kStartBeat ? 1 : 0;
    long n = (long)count + lead;
    size_t size = beatSize();
    if (n > a.allocated) {  // juce's HeapBlock: malloc/realloc/free
        char* d = (char*)realloc(a.data, (size_t)n * size);
        if (!d) return false;
        a.data = d, a.allocated = (int)n;
    }
    uint32_t tempo = (uint32_t)lround(c.bpm * 100) & 0xffff;
    long kDown = std::isfinite(c.downbeat) ? lround((c.downbeat - c.first) / period) : 0;  // index of a downbeat (may be < 0)
    for (long j = 0; j < n; j++) {
        long k = j - lead;  // (-1: the beat at the start)
        uint32_t inBar = (uint32_t)(((k - kDown) % 4 + 4) % 4 + 1);
        memset(a.data + (size_t)j * size, 0, size);
        *(uint32_t*)(a.data + (size_t)j * size) = tempo << 16 | inBar;
        setBeatTime(a.data, j, std::max(0.0, c.first + k * period));
    }
    a.used = (int)n;
    return true;
}

// The column's sort calls compare O(n log n) times: numeric BPM per track path, kept next to the note so
// a comparison is one hash lookup per side (no string copies, no note-map walk). Only this mod writes bt_bpm.
namespace {
struct SvHash { using is_transparent = void; size_t operator()(std::string_view s) const { return std::hash<std::string_view>()(s); } };
}  // namespace
static std::mutex g_btLock;
static std::unordered_map<std::string, double, SvHash, std::equal_to<>> g_btBpm;
// A value that changes during a sort makes the comparator inconsistent (rekordbox's sort can then run past the list).
// rekordbox sorts on the main thread, so new values go into the map there, between sorts; the note (text, other mods)
// is set at once.
static void putBtBpm(std::vector<std::pair<std::string, double>> v) {
    auto put = [v = std::move(v)] {
        std::lock_guard<std::mutex> l(g_btLock);
        for (auto& [uri, bpm] : v) g_btBpm[uri] = bpm;
    };
    if (plat::isMainThread()) put();
    else plat::onMain(std::move(put));
}
static void setBtBpm(const std::string& uri, const char* text, std::vector<std::pair<std::string, double>>* batch = nullptr) {
    setTrackNote(uri, "bt_bpm", text);
    if (batch) batch->emplace_back(uri, atof(text));
    else putBtBpm({{uri, atof(text)}});
}

// how: 0 rekordbox's grid kept, 1 the combined grid, 2 rekordbox's beats with Beat This!'s bar start, 3 rekordbox's
// beats and bars (Beat This! agrees), 4 rekordbox's beats redrawn at a whole BPM (wholeThroughRekordbox), 5
// rekordbox's beats and bars with beats moved or added. hb: rekordbox's beats moved half a beat first. covered: the
// analysed audio was the whole track. startBeat: the beat at the start was added to rekordbox's beats. 6: rekordbox's
// grid kept for the genre. c: Beat This!'s grid at the genre's octave (the column and the CSV show that one; the log
// line names the tempo it came from).
static void logResult(const std::string& uri, const RbGrid& rb, const Combined& c, int how, double waited, const HalfBeat& hb, bool covered,
                      bool startBeat, const GenreNote& gn, bool rbBars = false, const BarVote& vote = BarVote(), const OffKick& ok = OffKick(),
                      const KickLine& kl = KickLine(), double drift = 0) {
    double period = 60 / c.bt.bpm;
    auto phase = [](double x, double t0, double p) { double d = fmod(x - t0, p); if (d < 0) d += p; return d > p / 2 ? d - p : d; };
    double r = rb.bpm > 0 ? c.bt.bpm / rb.bpm : 0;
    const char* oct = rb.bpm <= 0 ? "n/a" : fabs(r - 1) < 0.01 ? "same" : fabs(r - 2) < 0.02 ? "double" : fabs(r - 0.5) < 0.01 ? "half"
                    : (fabs(r - 1.5) < 0.02 || fabs(r - 2.0 / 3) < 0.01) ? "3:2" : "other";
    double beatOff = NAN, barOff = NAN;
    if (!strcmp(oct, "same") && rb.firstBeat >= 0) {
        beatOff = phase(rb.firstBeat, c.bt.firstBeat, period) * 1000;
        if (rb.firstDownbeat >= 0) barOff = phase(rb.firstDownbeat, c.btDownbeat, 4 * period) / period;
    }
    char bpms[32];
    snprintf(bpms, sizeof bpms, "%.2f", c.bt.bpm);
    setBtBpm(uri, bpms);
    setTrackNote(uri, "bt_rel", oct);
    snprintf(bpms, sizeof bpms, "%.3f", c.bt.confidence);
    setTrackNote(uri, "bt_fit", bpms);  // (the analyzer's "re-analyse old grids" looks for unsure ones)
    setTrackNote(uri, "bt_fv", "2");    // fit from the fixed fitGrid (2026-11-12): unsure stays unsure on a re-analysis
    plat::onMain([] { refreshColumn(kColumnId); });
    char what[320];
    char whole[48] = "";
    if (c.wholeFrom > 0) snprintf(whole, sizeof whole, ", whole BPM instead of %.3f", c.wholeFrom);
    if (how == 1)
        snprintf(what, sizeof what, "combined grid used (%.2f BPM%s, beats moved %+.0f ms onto %d clear onsets)", c.bpm, whole, c.shiftMs, c.clearBeats);
    else if (how == 2)
        snprintf(what, sizeof what, "rekordbox's beats, Beat This!'s bar start");
    else if (how == 4)
        snprintf(what, sizeof what, "rekordbox's beats at a whole BPM (%.0f instead of %.2f), %s bar start", round(c.bpm), rb.bpm, rbBars ? "rekordbox's" : "Beat This!'s");
    else if (how == 3)
        snprintf(what, sizeof what, "rekordbox's grid kept (same tempo and bars)");
    else if (how == 5)
        snprintf(what, sizeof what, "rekordbox's beats and bars");
    else if (how == 6)
        snprintf(what, sizeof what, "rekordbox's grid kept (its tempo fits the genre)");
    else if (!c.use)
        snprintf(what, sizeof what, "rekordbox's grid kept (Beat This! unsure)");
    else if (!covered && applying())
        snprintf(what, sizeof what, "rekordbox's grid kept (only the first %.0f min of the track were analysed)", c.duration / 60);
    else
        snprintf(what, sizeof what, "rekordbox's grid kept");
    std::string half;
    if (hb.seen) {
        char b[300];
        if (hb.moved)
            snprintf(b, sizeof b, "; rekordbox's beats were half a beat off and were moved (%.0f%% of Beat This!'s beats between them, kick onsets %.2f at them and %.2f between, kick band %.1fx as loud between)",
                     100 * hb.share, hb.on, hb.between, hb.levelBetween / hb.levelOn);
        else
            snprintf(b, sizeof b, "; half a beat off? %.0f%% of Beat This!'s beats lie between rekordbox's, %s (%.2f at rekordbox's beats, %.2f between): not moved",
                     100 * hb.share, hb.mode <= 0 ? "bettergrid_halfbeat=0" : "but the kick onsets don't settle it", hb.on, hb.between);
        half = b;
    }
    if (ok.seen) {
        char b[300];
        snprintf(b, sizeof b, "; rekordbox's beats %.0f ms %s the beat (%.0f%% of Beat This!'s beats on the combined grid; the kick sets in %+.0f ms from that grid, %.1fx as strongly as at rekordbox's beats): %s",
                 fabs(ok.offset) * 1000, ok.offset < 0 ? "after" : "before", 100 * ok.share, ok.kickAt * 1000, ok.atRb > 0 ? ok.atNew / ok.atRb : 0.0,
                 ok.moved ? "the combined grid is used" : !ok.mode ? "not changed (bettergrid_offbeat=0)" : "not changed (the kick doesn't settle it)");
        half += b;
    }
    if (drift != 0) {
        char b[200];
        snprintf(b, sizeof b, "; rekordbox's beats are spaced at another tempo than their label, %+.0f ms from Beat This!'s by the end", drift * 1000);
        half += b;
    }
    if (kl.seen) {
        char b[300];
        snprintf(b, sizeof b, "; the kick sets in %.0f ms %s the grid's beats (%+.0f at the start, %+.0f at the end; %.0f%% of the beats, %.1fx clear): %s",
                 fabs(kl.off + kl.slope * c.duration / 2) * 1000, kl.atStart > 0 ? "after" : "before", kl.atStart * 1000, kl.atEnd * 1000,
                 100 * kl.share, kl.strength, kl.moved ? "the grid was moved onto the kicks" : !kl.mode ? "not changed (bettergrid_kick=0)" : "not changed");
        half += b;
    }
    if (vote.asked) {
        // "; bar start by the music's changes: rekordbox's (34% of the change points on its 1, 12% on Beat This!'s)"
        char b[200];
        snprintf(b, sizeof b, "; bar start by the music's changes: %s (%.0f%% of the change points on rekordbox's 1, %.0f%% on Beat This!'s, %.0f ms)",
                 vote.pick == 1 ? "rekordbox's" : vote.pick == 2 ? "Beat This!'s" : "undecided", 100 * vote.rb, 100 * vote.bt, c.bandsMs);
        half += b;
    }
    if (rbBars && !vote.pick) half += "; rekordbox's bar start kept: its 1 is where the kick comes in, Beat This!'s isn't";
    if (startBeat) half += "; the beat at the start of the track added";
    if (!gn.genre.empty()) {
        // "genre Drum n Bass (160-185): 83.00 -> 166.00", greppable as "; genre ".
        char b[320], range[32] = "no tempo range";
        if (gn.range.constrains()) snprintf(range, sizeof range, "%.0f-%.0f", gn.range.lo, gn.range.hi);
        using genretempo::Source;
        if (gn.choice.source == Source::BeatThis)
            snprintf(b, sizeof b, "; genre %s (%s): %.2f -> %.2f", gn.genre.c_str(), range, gn.from, gn.choice.bpm);
        else if (gn.choice.source == Source::Rekordbox)
            snprintf(b, sizeof b, "; genre %s (%s): %.2f -> rekordbox's %.2f", gn.genre.c_str(), range, gn.from, gn.choice.bpm);
        else
            snprintf(b, sizeof b, "; genre %s (%s)%s", gn.genre.c_str(), range,
                     !gn.range.constrains() ? "" : gn.range.has(gn.from) ? ": fits" : ": no octave fits, unchanged");
        half += b;
        if (gn.took >= 0.5) {
            snprintf(b, sizeof b, " (%.1fs)", gn.took);
            half += b;
        }
    }
    logf("bettergrid: rekordbox %.2f BPM, Beat This! %.2f BPM (%s, fit %.0f%%), beat offset %+.0f ms, bar offset %+.0f beats, %.1fs (waited %.1fs): %s%s  %s",
         rb.bpm, c.bt.bpm, oct, 100 * c.bt.confidence, beatOff, barOff, c.took, waited, what, half.c_str(), uri.c_str());
    static std::mutex csvLock;
    std::lock_guard<std::mutex> l(csvLock);
    std::string csv = plat::utf8(plat::fsPath(plat::logFile()).parent_path()) + "/rekordbox-bettergrid.csv";
    std::error_code ec;
    bool fresh = !std::filesystem::exists(plat::fsPath(csv), ec);
    if (FILE* f = plat::openFile(csv, "a")) {
        if (fresh) fprintf(f, "time,rb_bpm,bt_bpm,octave,bt_fit,rb_first_beat,bt_first_beat,beat_offset_ms,rb_first_downbeat,bt_first_downbeat,bar_offset_beats,path\n");
        fprintf(f, "%.0f,%.2f,%.2f,%s,%.3f,%.3f,%.3f,%.0f,%.3f,%.3f,%.0f,\"%s\"\n", now(), rb.bpm, c.bt.bpm, oct, c.bt.confidence,
                rb.firstBeat, c.bt.firstBeat, beatOff, rb.firstDownbeat, c.btDownbeat, barOff, uri.c_str());
        fclose(f);
    }
}

namespace {
struct Outcome {
    std::string uri;
    std::vector<char> beats;   // empty: keep rekordbox's
    std::vector<char> rbBars;  // see Applied
};
}  // namespace

// The combined grid for an analysis, once: waits for it, logs both grids, and returns its beats (empty: keep
// rekordbox's). rb = rekordbox's own beats.
static Outcome combinedBeats(const void* thread, const BeatArray& rb) {
    Outcome out;
    std::shared_ptr<Job> job = jobOf(thread, true);
    if (!job) return out;
    out.uri = job->uri;
    if (job->uri != songPath(thread)) {  // (the thread went on to another track)
        abandon(job);
        return out;
    }
    // Beat This! on the CPU (ONNX Runtime) is several times slower than Core ML on Apple Silicon: in a batch of
    // analyses on a 6-core PC it needs 30-50 s past rekordbox's own grid (an Intel Mac's CPU: over 20 s under
    // Rosetta), which is why the setting's default is higher there. Waiting holds only this analysis thread.
    double wait = opt::wait.number();
    wait = std::isfinite(wait) ? std::max(0.0, std::min(wait, 600.0)) : 20;
    double waitFrom = now();
    if (job->result.wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
        // rekordbox's grid is there and Beat This! isn't yet (it takes about a second per minute of track on a
        // Windows GPU; on a Mac it is nearly always done first): the deck shows rekordbox's grid meanwhile, as
        // plain rekordbox would have by now, and the combined one replaces it when it is ready. Beat This!'s own
        // early grid would only come a moment before the final one now: not shown.
        close(job);
        if (opt::rbFirst.integer() && rb.data && rb.used > 0)
            if (auto show = service<void (*)(const void*, const char*, const void*, const char*)>("quickdeck.earlyGrid"))
                show(thread, job->uri.c_str(), &rb, "rekordbox's");
    }
    bool ready = job->result.wait_for(std::chrono::milliseconds((long long)(wait * 1000))) == std::future_status::ready;
    close(job);  // from here on rekordbox saves its grid: the early one must not come after it
    if (!ready) {
        job->abandoned = true;
        logf("bettergrid: no result after %.0f s, rekordbox's grid kept  %s", wait, job->uri.c_str());
        return out;
    }
    const Combined* result = nullptr;
    try {
        result = &job->result.get();
    } catch (...) {
        return out;
    }
    const std::string& uri = job->uri;
    double waited = now() - waitFrom;  // how long rekordbox's analysis was held up
    if (!result->ok) return out;
    RbGrid g = rekordboxGrid(rb);
    // The genre's octave: Beat This!'s grid doubled or halved, and from there on as without it; or rekordbox's grid.
    GenreNote gn = genreOctave(*result, g.bpm);
    Combined octave;
    if (gn.choice.source == genretempo::Source::BeatThis) octave = atTempo(*result, gn.choice.factor);
    const Combined& c = gn.choice.source == genretempo::Source::BeatThis ? octave : *result;
    bool keepRb = gn.choice.source == genretempo::Source::Rekordbox;
    bool sameTempo = g.bpm > 0 && fabs(c.bt.bpm / g.bpm - 1) < 0.002;  // within 0.25 BPM at 128
    // The analysed audio stops at 20 minutes: a grid drawn from it would end there, so a longer track keeps
    // rekordbox's beats (its bars can still come from Beat This!).
    bool covered = !rb.data || rb.used < 1 || beatTime(rb.data, rb.used - 1) <= c.duration + 1;
    double drift = sameTempo && covered && c.use ? rbDrift(rb, c) : 0;
    if (opt::debug.number() > 0) logf("bettergrid: drift %.0f ms, onset fit %d (%d clear), combined %.3f BPM", drift * 1000, (int)c.onsetFit, c.clearBeats, c.bpm);
    if (drift != 0) sameTempo = false;  // rekordbox's beats aren't at the tempo they say: the combined grid
    int how = 0;
    bool startBeat = false, rbBars = false;
    HalfBeat hb;
    OffKick ok;
    KickLine kl;
    BarVote vote;
    auto draw = [&](const Combined& grid, int as) {
        BeatArray a{nullptr, 0, 0};
        if (writeGrid(a, grid)) out.beats.assign(a.data, a.data + (size_t)a.used * beatSize()), how = as;
        free(a.data);
    };
    if (keepRb) {
        how = 6;
    } else if (c.use && applying()) {
        if (sameTempo) {
            std::vector<char> base(rb.data, rb.data + (size_t)rb.used * beatSize());
            hb = halfBeatOff(rb, c);
            int mode = hb.mode = opt::halfbeat.integer();
            bool onsetsAgree = hb.on == hb.on && hb.between > 0 && hb.between >= kHalfBeatOnsets * hb.on;
            if (hb.seen && (mode >= 2 || (mode == 1 && (onsetsAgree || hb.loud)))) {
                std::vector<char> moved = movedHalfBeat(rb, covered ? c.duration : 0);
                if (!moved.empty()) base = std::move(moved), hb.moved = true;
            }
            if (!hb.moved && covered) {
                ok = offTheKick(rb, c);
                ok.mode = opt::offbeat.integer();
                if (ok.seen && ok.agree && ok.audio && ok.mode) {
                    draw(c, 1);
                    ok.moved = how == 1;
                }
            }
            if (!ok.moved) {
                startBeat = addStartBeat(base);
                double barAt = c.btDownbeat;  // the bars go by this downbeat: Beat This!'s, or rekordbox's where the kick comes in
                if (!hb.moved && opt::bars.integer()) vote = barByMusic(base, c, c.btDownbeat, 60 / c.bt.bpm);
                if (vote.pick) {
                    barAt = vote.at, rbBars = vote.pick == 1;
                } else if (!hb.moved && c.level) {
                    double k = kickStartBar(base, *c.level, c.btDownbeat, 60 / c.bt.bpm);
                    if (std::isfinite(k)) barAt = k, rbBars = true;
                }
                Combined w;
                if (covered && wholeThroughRekordbox(view(base), g, c, barAt, &w)) draw(w, 4);
                if (how != 4) {
                    bool bars = renumber(base, barAt, 60 / c.bt.bpm, hb.moved);
                    if (bars && !hb.moved && !startBeat) {  // rekordbox's own numbering, for notifyBeat
                        out.rbBars.resize((size_t)rb.used);
                        for (int i = 0; i < rb.used; i++) out.rbBars[(size_t)i] = (char)(beatWord(rb.data, i) & 0xff);
                    }
                    if (bars) how = 2;
                    else how = hb.moved || startBeat ? 5 : 3;
                    if (how != 3) out.beats = std::move(base);
                }
            }
        } else if (covered) {
            draw(c, 1);
            if (drift != 0 && how == 1 && opt::bars.integer()) {
                // rekordbox's tempo was right, only its beats' spacing wasn't: its bar start may still be the one
                // to keep. The music's changes decide, as for rekordbox's own beats, on the combined beats.
                long jr = -1;
                for (int i = 0; i < std::min(rb.used, 8) && jr < 0; i++)
                    if ((beatWord(rb.data, i) & 0xffff) == 1) jr = i;
                long n = (long)(out.beats.size() / beatSize()), j0 = 0;
                if (jr >= 0 && n > 0) {
                    double at = beatTime(rb.data, jr);
                    for (long i = 1; i < n; i++)
                        if (fabs(beatTime(out.beats.data(), i) - at) < fabs(beatTime(out.beats.data(), j0) - at)) j0 = i;
                    std::vector<char> alt = out.beats;
                    for (long i = 0; i < n; i++) {
                        uint32_t& w = *(uint32_t*)(alt.data() + (size_t)i * beatSize());
                        w = (w & 0xffff0000u) | (uint32_t)(((i - j0) % 4 + 4) % 4 + 1);
                    }
                    vote = barByMusic(alt, c, c.btDownbeat, 60 / c.bt.bpm);
                    if (vote.pick == 1) out.beats = std::move(alt), rbBars = true;
                }
            }
        }
    }
    // Last, on whichever grid came out of the above: does the kick sit a fixed distance off it?
    if (c.use && applying() && !keepRb && covered && rb.data && rb.used > 0) {
        const char* d = out.beats.empty() ? rb.data : out.beats.data();
        int n = out.beats.empty() ? rb.used : (int)(out.beats.size() / beatSize());
        kl = kickLine(BeatArray{(char*)d, n, n}, c);
        kl.mode = opt::kick.integer();
        if (opt::debug.number() > 0)
            logf("bettergrid: kick line %+.1f..%+.1f ms (slope %.5f), %.0f%% of the beats on it, %.1fx clear: %s", kl.atStart * 1000, kl.atEnd * 1000,
                 kl.slope, 100 * kl.share, kl.strength, kl.seen ? "off the grid" : "no");
        if (kl.seen && kl.mode) {
            std::vector<char> moved = movedOntoKicks(d, n, kl, c.duration, &out.rbBars);
            if (moved.size() / beatSize() >= 16) out.beats = std::move(moved), kl.moved = true;
        }
    }
    if (opt::debug.number() > 0)
        for (int i = 0; i < 2; i++) {
            const char* d = i ? out.beats.data() : rb.data;
            int n = i ? (int)(out.beats.size() / beatSize()) : rb.used;
            for (int k = 0; d && k < std::min(n, 4); k++)
                logf("bettergrid: %s beat %d: %08x %.3f s", i ? "combined" : "rekordbox", k, beatWord(d, k), beatTime(d, k));
        }
    logResult(uri, g, c, how, waited, hb, covered, startBeat && how != 4, gn, rbBars, vote, ok, kl, drift);
    if (c.use && applying() && !keepRb && provided("quickdeck.earlyGrid")) {  // how far the grid quickdeck showed early is from the final one
        const char* d = out.beats.empty() ? rb.data : out.beats.data();
        int n = out.beats.empty() ? rb.used : (int)(out.beats.size() / beatSize());
        if (d && n > 8 && c.bpm > 0) {
            double t = beatTime(d, n / 2), p = 60 / c.bpm;
            double off = fmod(t - c.first, p);
            if (off < 0) off += p;
            if (off > p / 2) off -= p;
            logf("bettergrid: early grid %.2f BPM, %+.0f ms from the final grid (%.2f BPM) mid-track", c.bpm, off * 1000, (beatWord(d, n / 2) >> 16) / 100.0);
        }
    }
    return out;
}

// A copy of beats in rekordbox's array (juce's HeapBlock: malloc/realloc/free).
static void assign(BeatArray& a, const std::vector<char>& beats) {
    int n = (int)(beats.size() / beatSize());
    if (n < 1) return;
    if (n > a.allocated || !a.data) {
        char* d = (char*)realloc(a.data, beats.size());
        if (!d) return;
        a.data = d, a.allocated = n;
    }
    memcpy(a.data, beats.data(), beats.size());
    a.used = n;
}

// ---- the collection's BPM ---------------------------------------------------------------------

// rekordbox stores a track's BPM (the BPM column, the deck's title) from its own beat detection, not from the grid
// that is saved: with a grid at another tempo (Beat This!'s at half of rekordbox's, say) the list said 140 and the
// deck played 70. So when the saved grid has one tempo and it isn't rekordbox's, that tempo is written as the
// track's BPM (rekordboxDBController::modTrackBpm, as rekordbox's XML import does). rekordbox may store its own at
// any point of the analysis: the track is watched from when the grid is handed over until a while after the
// analysis' last step, and corrected when its BPM is another. (macOS: the function isn't in the Windows table yet.)
#ifndef _WIN32
namespace {
const Addr A_dbHolder{"SingletonHolder<DatabaseIF>", [] { return singleton("__ZN11DetailColorC2Ev", "__ZN4juce15SingletonHolderIN2db10DatabaseIFENS_15CriticalSectionELb0EE3getEv"); }};
const Addr A_dbGet{"__ZN4juce15SingletonHolderIN2db10DatabaseIFENS_15CriticalSectionELb0EE3getEv"};
const Addr A_mainDBController{"__ZN2db16DatabaseMediator16mainDBControllerEv"};  // DatabaseMediator::mainDBController() -> rekordboxDBController*
const Addr A_modTrackBpm{"__ZN2db21rekordboxDBController11modTrackBpmEjjb"};  // rekordboxDBController::modTrackBpm(content id, BPM * 100, also in its cached rows)
struct BpmFix { unsigned bpm100 = 0; const void* thread = nullptr; double until = 0; int written = 0; };
std::mutex g_bpmLock;
std::map<std::string, BpmFix> g_bpmFixes;  // by track (g_bpmLock)
const double kBpmWatchAfter = 30;  // s after the analysis' last step
const int kBpmWrites = 3;          // at most, per analysis (never a tug of war with rekordbox)
}  // namespace

// The grid handed over for this track: its tempo if all its beats have one and it isn't rekordbox's, else 0.
static void bpmOfGrid(const void* thread, const std::string& uri, const BeatArray& mine, const BeatArray& rb) {
    unsigned tempo = mine.used > 0 ? beatWord(mine.data, 0) >> 16 : 0;
    for (int i = 1; tempo && i < mine.used; i++)
        if (beatWord(mine.data, i) >> 16 != tempo) tempo = 0;  // tempo changes: rekordbox's average stays
    if (tempo && lround(rekordboxGrid(rb).bpm * 100) == (long)tempo) tempo = 0;
    std::lock_guard<std::mutex> l(g_bpmLock);
    if (tempo) g_bpmFixes[uri] = BpmFix{tempo, thread, now() + kAppliedFor, 0};
    else g_bpmFixes.erase(uri);
}
static void bpmAnalysisEnds(const void* thread) {
    std::lock_guard<std::mutex> l(g_bpmLock);
    for (auto& kv : g_bpmFixes)
        if (kv.second.thread == thread) kv.second.thread = nullptr, kv.second.until = now() + kBpmWatchAfter;
}
static void tick() {  // main thread
    static double next;
    if (now() < next) return;
    next = now() + 1;
    std::vector<std::pair<std::string, BpmFix>> fixes;
    {
        std::lock_guard<std::mutex> l(g_bpmLock);
        for (auto it = g_bpmFixes.begin(); it != g_bpmFixes.end();) {
            if (now() > it->second.until || it->second.written >= kBpmWrites) it = g_bpmFixes.erase(it);
            else fixes.push_back(*it), ++it;
        }
    }
    for (auto& f : fixes) {
        CollectionTrack t;
        if (!collectionTrack(f.first, &t) || !t.id) continue;
        showTrackBpm(f.first, f.second.bpm100 / 100.0);  // (the lists' rows: rekordbox refreshes them with its own BPM when the analysis is done)
        if (lround(t.bpm * 100) == (long)f.second.bpm100) continue;
        void* db = at<void* (*)(uintptr_t)>(A_dbGet)(A_dbHolder + g_slide);
        void* ctrl = db ? at<void* (*)(void*)>(A_mainDBController)(*(void**)db) : nullptr;  // (DatabaseIF's first word: its mediator)
        if (!ctrl) continue;
        at<int (*)(void*, unsigned, unsigned, bool)>(A_modTrackBpm)(ctrl, t.id, f.second.bpm100, true);
        logf("bettergrid: the track's BPM %.2f -> %.2f, the saved grid's  %s", t.bpm, f.second.bpm100 / 100.0, f.first.c_str());
        std::lock_guard<std::mutex> l(g_bpmLock);
        auto it = g_bpmFixes.find(f.first);
        if (it != g_bpmFixes.end() && it->second.bpm100 == f.second.bpm100) it->second.written++;
    }
}
#else
static void bpmOfGrid(const void*, const std::string&, const BeatArray&, const BeatArray&) {}
static void bpmAnalysisEnds(const void*) {}
#endif

// A grid whose beats go back in time somewhere is never saved: rekordbox's beat job handed over two grids joined end to
// end on two hardcore tracks (11-27: 215.12 BPM to the end of the track, then the same track again from 0.4 s at
// 53.78, a quarter of it), which bettergrid kept because Beat This! was unsure there. Of such a grid only its longest
// stretch of beats in order is kept (there: the 215 BPM grid over the whole track). Empty: the beats are in order.
static std::vector<char> inOrder(const char* d, int n, const std::string& uri) {
    if (!d || n < 2) return {};
    int best = 0, bestLen = 0, from = 0, backs = 0;
    for (int i = 1; i <= n; i++) {
        if (i < n && beatTime(d, i) > beatTime(d, i - 1)) continue;
        if (i < n) backs++;
        if (i - from > bestLen) best = from, bestLen = i - from;
        from = i;
    }
    if (!backs) return {};
    int back = best + bestLen < n ? best + bestLen : best;  // where the kept stretch ends (or, kept from there: begins)
    logf("bettergrid: the grid's beats go back in time %d time%s (beat %d of %d at %.3f s, after %.3f s): only beats %d-%d kept (%.2f BPM)  %s",
         backs, backs == 1 ? "" : "s", back, n, beatTime(d, back), beatTime(d, back - 1), best, best + bestLen - 1,
         (beatWord(d, best) >> 16) / 100.0, uri.c_str());
    return std::vector<char>(d + (size_t)best * beatSize(), d + (size_t)(best + bestLen) * beatSize());
}

// ---- hooks ------------------------------------------------------------------------------------

// CBeatAnalyzeThread::NotifyResult(beat job) -> b notifyBeat(this, job's beats): the grid to save.
extern "C" void rbm_hook_grid_save(void* thread, const BeatArray* beats) {
    auto orig = RBM_ORIG(grid_save, void (*)(void*, const BeatArray*));
    if (!g_on || !beats) {
        abandon(jobOf(thread, true));
        dropApplied(thread);
        return orig(thread, beats);
    }
    Outcome mine = combinedBeats(thread, *beats);
    bool cut = false;
    if (!mine.uri.empty() && applying()) {  // (beats in order, whichever grid is saved)
        bool own = mine.beats.empty();
        std::vector<char> kept = own ? inOrder(beats->data, beats->used, mine.uri)
                                     : inOrder(mine.beats.data(), (int)(mine.beats.size() / beatSize()), mine.uri);
        if (!kept.empty()) {
            mine.beats = std::move(kept);
            mine.rbBars.clear();  // (rekordbox's numbering per beat, for the whole grid: not for a part of it)
            cut = true;
        }
    }
    // (A cut grid: rekordbox's BPM came from both grids (107.56 for 215.12 and 53.78), so the collection's is
    // compared with the kept grid's tempo whatever rekordbox's beats said.)
    if (!mine.uri.empty()) bpmOfGrid(thread, mine.uri, view(mine.beats), cut ? BeatArray{nullptr, 0, 0} : *beats);
    if (mine.beats.empty()) {
        dropApplied(thread);
        return orig(thread, beats);
    }
    {
        State& s = state();
        std::lock_guard<std::mutex> l(s.lock);
        s.applied[thread] = Applied{mine.uri, mine.beats, mine.rbBars, now()};
    }
    BeatArray a = view(mine.beats);
    orig(thread, &a);
}

// CBeatAnalyzeThread::analyze: its beat list = the beat job's beats (thread from x20; Intel Macs: see below).
#ifndef _WIN32
extern "C" void* rbm_grid_assign(void* dst, const void* src, const void* thread) {
    void* ret = RBM_ORIG(grid_assign, void* (*)(void*, const void*))(dst, src);
    if (!g_on || !dst) return ret;
    Applied mine;
    if (appliedTo(thread, &mine)) assign(*(BeatArray*)dst, mine.beats);
    return ret;
}
#endif
#if defined(__aarch64__)
asm(R"(
    .text
    .p2align 2
    .globl _rbm_hook_grid_assign
_rbm_hook_grid_assign:
    mov  x2, x20
    b    _rbm_grid_assign
)");
#elif defined(_WIN32)
// Windows: Array<DetailedBeatInfo>::operator= is inlined into analyze(), so the table's grid_assign site is the call
// just before that copy, AnalyzeJobController::GetResult(controller, 3 = beat job) (tools/win/verify/win2.out.json):
// the beats go into the result's array (+8), and the inlined copy takes them. The thread comes from r15 (hookreg).
// (assign() may realloc rekordbox's array: both CRTs are the static UCRT on the process heap.)
extern "C" void* rbm_grid_assign(void* controller, int job, void* thread) {
    void* result = RBM_ORIG(grid_assign, void* (*)(void*, int))(controller, job);
    if (!g_on || !result) return result;
    Applied mine;
    if (appliedTo(thread, &mine)) assign(*(BeatArray*)((char*)result + 8), mine.beats);
    return result;
}
#else
// Intel Macs: Array<DetailedBeatInfo>::operator=(this = analyze()'s beat list (rdi), source = the beat job's result
// + 8 (rsi)) -> this (rax), one call in analyze() (7.2.19 and 7.2.20). No register holds the thread there: the
// current juce thread is the analysis (core/analysis.cpp); without it the copy is rekordbox's own.
extern "C" void* rbm_hook_grid_assign(void* dst, const void* src) {
    if (const void* thread = analysisThreadHere()) return rbm_grid_assign(dst, src, thread);
    return RBM_ORIG(grid_assign, void* (*)(void*, const void*))(dst, src);
}
#endif

// Is a the grid this analysis was given (perhaps with rekordbox's own adjustments, e.g. a small shift)? The same
// number of beats and tempo, and its first, middle and last beat within 50 ms.
static bool sameBeats(const BeatArray& a, const std::vector<char>& mine) {
    int n = (int)(mine.size() / beatSize());
    if (!a.data || n < 1 || a.used != n) return false;
    if (beatWord(a.data, 0) >> 16 != beatWord(mine.data(), 0) >> 16) return false;
    for (int i : {0, n / 2, n - 1})
        if (fabs(beatTime(a.data, i) - beatTime(mine.data(), i)) > 0.050) return false;
    return true;
}

// CBeatAnalyzeThread::analyze -> notifyBeat(beats): the grid to save; the combined one if this track has one.
extern "C" void rbm_hook_grid_notify1(void* thread, const BeatArray* beats);
extern "C" void rbm_hook_grid_notify2(void* thread, const BeatArray* beats);
static void notifyBeat(void (*orig)(void*, const BeatArray*), void* thread, const BeatArray* beats) {
    Applied mine;
    bool have = g_on && beats && beats->data && beats->used > 0 && appliedTo(thread, &mine);
    if (opt::debug.number() > 0)
        logf("bettergrid: notifyBeat %d beats (first %.3f s), combined %zu beats  %s", beats ? beats->used : -1,
             beats && beats->data && beats->used > 0 ? beatTime(beats->data, 0) : -1.0, have ? mine.beats.size() / beatSize() : (size_t)0,
             songPath(thread).c_str());
    if (!have) {
        std::vector<char> kept = g_on && applying() && beats ? inOrder(beats->data, beats->used, songPath(thread)) : std::vector<char>();
        if (kept.empty()) return orig(thread, beats);
        BeatArray a = view(kept);
        return orig(thread, &a);
    }
    if (sameBeats(*beats, mine.beats)) {
        // Still the combined grid: left as it is, with the bars the phrase analysis may have renumbered. Unless
        // only the bars were Beat This!'s and these are exactly rekordbox's own again: its list from before (it
        // reloads the beat list from its database when the track has cues), not a renumbering.
        bool rbBars = mine.rbBars.size() == (size_t)beats->used;
        for (int i = 0; rbBars && i < beats->used; i++) rbBars = (char)(beatWord(beats->data, i) & 0xff) == mine.rbBars[(size_t)i];
        if (!rbBars) return orig(thread, beats);
    }
    BeatArray a = view(mine.beats);
    orig(thread, &a);
}
extern "C" void rbm_hook_grid_notify1(void* thread, const BeatArray* beats) {
    notifyBeat(RBM_ORIG(grid_notify1, void (*)(void*, const BeatArray*)), thread, beats);
}
extern "C" void rbm_hook_grid_notify2(void* thread, const BeatArray* beats) {
    notifyBeat(RBM_ORIG(grid_notify2, void (*)(void*, const BeatArray*)), thread, beats);
}

// ---- the column ---------------------------------------------------------------------------------

static std::string columnText(const char* track) {
    const char* uri = trackPath(track);
    std::string b = uri ? trackNote(uri, "bt_bpm") : "";
    if (b.empty()) return "";
    std::string rel = trackNote(uri, "bt_rel");
    return rel == "same" || rel.empty() ? b : b + "  (" + rel + ")";
}

static int columnCompare(const char* a, const char* b) {
    // Missing counts as 0 (sorts first), as before.
    const char* ua = trackPath(a);
    const char* ub = trackPath(b);
    double x = 0, y = 0;
    static std::atomic<bool> offMain{false};  // (putBtBpm counts on sorts running on the main thread)
    if (!plat::isMainThread() && !offMain.exchange(true)) logf("bettergrid: BT BPM sorted off the main thread");
    {
        std::lock_guard<std::mutex> l(g_btLock);
        if (ua) if (auto it = g_btBpm.find(std::string_view(ua)); it != g_btBpm.end()) x = it->second;
        if (ub) if (auto it = g_btBpm.find(std::string_view(ub)); it != g_btBpm.end()) y = it->second;
    }
    return x < y ? -1 : x > y;
}

static std::string columnTitle() { return opt::title.text(); }

// Earlier results from the CSV.
static void init() {
    FILE* f = plat::openFile(plat::utf8(plat::fsPath(plat::logFile()).parent_path()) + "/rekordbox-bettergrid.csv", "r");
    int n = 0;
    std::vector<std::pair<std::string, double>> bpms;  // (one trip to the main thread)
    if (f) {
        char line[4096];
        while (fgets(line, sizeof line, f)) {
            double at = 0, rb = 0, bt = 0, fit = 0;
            char oct[16] = "";
            const char* q = strchr(line, '"');
            if (!q || sscanf(line, "%lf,%lf,%lf,%15[^,],%lf", &at, &rb, &bt, oct, &fit) < 4) continue;
            std::string uri(q + 1);
            size_t end = uri.rfind('"');
            if (end == std::string::npos) continue;  // a line cut short (the write was interrupted)
            uri.resize(end);
            if (uri.empty()) continue;
            char b[32];
            snprintf(b, sizeof b, "%.2f", bt);
            setBtBpm(uri, b, &bpms);
            setTrackNote(uri, "bt_rel", oct);
            snprintf(b, sizeof b, "%.3f", fit);
            setTrackNote(uri, "bt_fit", b);
            if (at >= 816206400) setTrackNote(uri, "bt_fv", "2");  // after 2026-11-12 21:00: the fixed fitter's fit
            n++;
        }
        fclose(f);
    }
    putBtBpm(std::move(bpms));
    logf("bettergrid: %d earlier results", n);
    if (g_on) {
        try {
            // load (and compile for the GPU) in the background; then one run on a little noise, so the first track
            // of the session doesn't pay for the model's first run (macOS: the GPU pipeline is built then, and
            // meets the stems model's first run when the track is loaded on a deck)
            startupTask([] {
                if (!model().loaded()) return;
#if !defined(_WIN32) && !RBM_MAC_X86  // (an Intel Mac ran a chunk on the GPU when it loaded the model, to time it)
                if (!opt::warmup.integer()) return;
                double t0 = now();
                const int rate = 22050;
                std::vector<float> x((size_t)rate * 15);
                unsigned seed = 1;
                for (float& v : x) seed = seed * 1664525u + 1013904223u, v = ((int)(seed >> 16) - 32768) / 3276800.f;  // -40 dB noise
                model().analyze(x.data(), x.size(), rate);
                logf("bettergrid: model warmed up (%.1fs)", now() - t0);
#endif
            });
        } catch (...) {  // (no thread: the first analysis' worker loads it)
        }
    }
}

RBM_COLUMN({.id = kColumnId, .title = columnTitle, .text = columnText, .compare = columnCompare, .after = 29, .enabled = &g_on});
#ifndef _WIN32
RBM_MOD({.name = "bettergrid", .init = init, .tick = tick, .enabled = &g_on,
         .title = "Beat grids",
         .about = "Better beat grids: the Beat This! beat tracker's tempo and bar starts, with every beat on the hits in the audio.",
         .area = "analysis", .order = 20});
#else
// The window's sidebar area this mod is shown in (core/settings.h: the first declaration of an area counts).
namespace { const rbm::settings::Area kArea{"analysis", "Grids, keys & cues",
    "Beat grids, keys, genres and cues, and how tracks get analysed.",
    "<path d=\"M1.5 8h2l1.5-4 2.5 9 2-7 1.5 4h3.5\"/>", 30}; }

RBM_MOD({.name = "bettergrid", .init = init, .enabled = &g_on,
         .title = "Beat grids",
         .about = "Better beat grids: the Beat This! beat tracker's tempo and bar starts, with every beat on the hits in the audio.",
         .area = "analysis", .order = 20});
#endif
