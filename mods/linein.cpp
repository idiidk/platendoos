// linein: a turntable, a CD player or any other audio input, played through a deck. The deck's channel on the mixer
// (trim, EQ, filter, channel fader, crossfader, headphone cue, the FX) then works on it like on a track, with any
// controller: rekordbox itself only offers this on hardware that has its own inputs (DVS units, a DDJ's LINE /
// PHONO), and a controller like the DDJ-FLX4 has none.
//
// rekordbox's engine has the mode built in, per deck: "input through" (DjSystem::enableInputThru, what the DVS deck
// menu's THROUGH does). Every deck's player unit (rbxfrm::RekordboxDjPlayerUnit, a juce::AudioProcessor in the
// engine's graph) gets a buffer whose first two channels are its audio input; with the unit's "thru" flag on it
// copies them, runs its input BPM detection on the copy and adds it to the deck's output, faded in and out, and the
// deck's display says INPUT THROUGH (PlayerPanelL::updateDVSThroughMode). The input is whatever the audio device's
// input channels carry, routed there by rekordbox: silence, with a controller that has none.
// Here the unit's processBlock is replaced (a vtable slot): for a deck that has an input of its own, the buffer's
// first two channels are filled with that device's audio just before rekordbox's code runs, and the thru flag is
// held on (rekordbox switches every deck's off again at each mixer or controller event: DeckMuteManager's
// updateMuteState). Everything after that is rekordbox's own.
//
// The device is opened here (macOS, mods/linein_mac.mm: its own CoreAudio IO callback, the output streams left alone)
// and runs on its own clock: a USB turntable's crystal and the controller's are never the same, 44,100 against
// 44,103 samples a second is ordinary, and its rate may be another altogether (48 kHz into a 44.1 kHz engine). So its
// audio goes through a ring and is read back with a windowed-sinc interpolator (64 taps, 256 phases) at a ratio that
// a slow control loop keeps trimming so that the distance to the writer stays the same: what the input delivers is
// what the deck plays, no sample dropped or repeated. That distance is the latency this adds: half the input's and the
// engine's IO blocks, one engine block and 2 ms, about 12 ms with 128-frame blocks (the input is asked for 128
// frames); an underrun (the device stalled) adds 2 ms for the rest of the session.
//
// Which deck plays what is in the config: a device per deck, by its name or its UID (two turntables of the same
// make have the same name: "USB AUDIO CODEC #2" is the second, or use the UIDs the log lists), and a switch per
// deck. With linein_empty=1 (the default) a deck plays its input only while no track is loaded on it: loading a
// track gives the deck back to the track, unloading it brings the input back. (In input through rekordbox mutes the
// deck's own player, RekordboxDjPlayerUnit -> mutePlayer: with linein_empty=0 a loaded track stays silent.)
// macOS asks once whether rekordbox may use the microphone: that permission covers every audio input.
//
// Windows (mods/linein_win.cpp): the input is a WASAPI capture stream in shared mode, in the format Windows mixes the
// device at (32-bit float, or 16, 24 or 32-bit integers), event-driven on a thread of its own at MMCSS "Pro Audio"
// priority; the device's UID is its endpoint ID. Its block is the device's period (usually 10 ms: 480 frames at
// 48 kHz), so the deck plays about 5 ms later than on macOS. A device unplugged ends its stream: the deck lets go of
// it and takes it again once it is back, as on macOS. Windows' privacy switch "Let desktop apps access your
// microphone" (Settings > Privacy & security > Microphone) must be on, or no input opens: the log says so.
//   config: linein=0 to disable
//           linein_1 .. linein_4      deck 1-4's audio input: the device's name, "name #2" or its UID (empty: none)
//           linein_1_on .. linein_4_on   0: that deck plays tracks only, its input stays off
//           linein_1_ch .. linein_4_ch   the device's channels to take, "1-2" by default ("3-4", or "1" for a mono input)
//           linein_1_gain .. linein_4_gain   gain in dB on top of the mixer's trim (-24 .. +24)
//           linein_empty=1            a deck plays its input only while no track is loaded on it (0: always, and a loaded track stays silent)
//           linein_extra_ms=0         latency to add, for an input device whose audio arrives unevenly
//           linein_log=0              1: every 10 s per deck, the input's clock against the engine's, the distance kept and the level
#include "../core/rbmod.h"
#include "linein.h"
#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using namespace rbm;
using namespace linein;

// Its settings (core/settings.h): what the settings window and the config file's list show of them.
namespace opt {
const Setting deck1{"linein_1", "", {.label = "Deck 1: input", .help = "The audio input this deck plays: the device's name, \"name #2\" for a second one of the same name, or its UID.", .list = "linein.inputs", .line = "Deck 1", .column = "Plays this input"}};
const Setting deck2{"linein_2", "", {.label = "Deck 2: input", .help = "The audio input this deck plays: the device's name, \"name #2\" for a second one of the same name, or its UID.", .list = "linein.inputs", .line = "Deck 2", .column = "Plays this input"}};
const Setting deck3{"linein_3", "", {.label = "Deck 3: input", .help = "The audio input this deck plays: the device's name, \"name #2\" for a second one of the same name, or its UID.", .list = "linein.inputs", .line = "Deck 3", .column = "Plays this input"}};
const Setting deck4{"linein_4", "", {.label = "Deck 4: input", .help = "The audio input this deck plays: the device's name, \"name #2\" for a second one of the same name, or its UID.", .list = "linein.inputs", .line = "Deck 4", .column = "Plays this input"}};
const Setting deck1Ch{"linein_1_ch", "1-2", {.label = "Deck 1: channels", .help = "The device's channels to take: 1-2, 3-4, or 1 for a mono input.", .line = "Deck 1", .column = "Channels"}};
const Setting deck2Ch{"linein_2_ch", "1-2", {.label = "Deck 2: channels", .help = "The device's channels to take: 1-2, 3-4, or 1 for a mono input.", .line = "Deck 2", .column = "Channels"}};
const Setting deck3Ch{"linein_3_ch", "1-2", {.label = "Deck 3: channels", .help = "The device's channels to take: 1-2, 3-4, or 1 for a mono input.", .line = "Deck 3", .column = "Channels"}};
const Setting deck4Ch{"linein_4_ch", "1-2", {.label = "Deck 4: channels", .help = "The device's channels to take: 1-2, 3-4, or 1 for a mono input.", .line = "Deck 4", .column = "Channels"}};
const Setting deck1Gain{"linein_1_gain", 0, {.label = "Deck 1: gain", .help = "Gain on the input, on top of the mixer's trim.", .unit = "dB", .min = -24, .max = 24, .step = 1, .line = "Deck 1", .column = "Gain"}};
const Setting deck2Gain{"linein_2_gain", 0, {.label = "Deck 2: gain", .help = "Gain on the input, on top of the mixer's trim.", .unit = "dB", .min = -24, .max = 24, .step = 1, .line = "Deck 2", .column = "Gain"}};
const Setting deck3Gain{"linein_3_gain", 0, {.label = "Deck 3: gain", .help = "Gain on the input, on top of the mixer's trim.", .unit = "dB", .min = -24, .max = 24, .step = 1, .line = "Deck 3", .column = "Gain"}};
const Setting deck4Gain{"linein_4_gain", 0, {.label = "Deck 4: gain", .help = "Gain on the input, on top of the mixer's trim.", .unit = "dB", .min = -24, .max = 24, .step = 1, .line = "Deck 4", .column = "Gain"}};
const Setting deck1On{"linein_1_on", true, {.label = "Deck 1: on", .help = "Off: this deck plays tracks only.", .line = "Deck 1", .column = "On"}};
const Setting deck2On{"linein_2_on", true, {.label = "Deck 2: on", .help = "Off: this deck plays tracks only.", .line = "Deck 2", .column = "On"}};
const Setting deck3On{"linein_3_on", true, {.label = "Deck 3: on", .help = "Off: this deck plays tracks only.", .line = "Deck 3", .column = "On"}};
const Setting deck4On{"linein_4_on", true, {.label = "Deck 4: on", .help = "Off: this deck plays tracks only.", .line = "Deck 4", .column = "On"}};
const Setting empty{"linein_empty", true, {.label = "Only while the deck is empty", .help = "Loading a track gives the deck back to the track; unloading it brings the input back. Off: the deck always plays its input, and a loaded track stays silent."}};
const Setting extraMs{"linein_extra_ms", 0, {.label = "Extra latency", .help = "For an input whose audio arrives unevenly. Normally the deck plays about 12 ms behind the input.", .unit = "ms", .min = 0, .max = 200, .step = 1, .advanced = true}};
const Setting log{"linein_log", false, {.label = "Log the inputs every 10 seconds", .help = "Per deck: the input's clock against the engine's, the distance kept and the level.", .debug = true}};
}  // namespace opt

static volatile bool g_on = true;

// ---- rekordbox's side ------------------------------------------------------------------------------------------------

static const Addr A_djSystem{"__ZNK8djengine10DjEngineIF11getDjSystemEv"};
static const Addr A_enableThru{"__ZN8DjSystem15enableInputThruEib"};                  // DjSystem::enableInputThru(int deck, bool): main thread
static const Addr A_unitVtable{"__ZTVN6rbxfrm21RekordboxDjPlayerUnitE"};
// (the class has a virtual base: its vtable symbol starts with one more word, so the slot is 0x50 from the objects' vptr)
static const Offset V_unitProcess{0x58, "__ZTVN6rbxfrm21RekordboxDjPlayerUnitE", "vt:__ZN6rbxfrm21RekordboxDjPlayerUnit12processBlockERN4juce11AudioBufferIfEERNS1_10MidiBufferE", 0};
static const Offset O_sysGraph{0x50, "__ZN8DjSystem15enableInputThruEib", "ldr x0", 0};          // DjSystem: its rbxfrm::DjUnitAudioGraph*
static const Offset O_graphCount{0x53c, "__ZN6rbxfrm16DjUnitAudioGraph15enableInputThruEib", "ldr x0", 0};     // DjUnitAudioGraph: int, how many player units
static const Offset O_graphUnits{0x530, "__ZN6rbxfrm16DjUnitAudioGraph15enableInputThruEib", "ldr x0", 1};     // RekordboxDjPlayerUnit**, by deck
static const Offset O_unitThru{0x220, "__ZN6rbxfrm21RekordboxDjPlayerUnit15enableInputThruEb", "str x19", 0};       // RekordboxDjPlayerUnit: bool, input thru is on
static const Offset O_bufChannels{0x10, "__ZN6rbxfrm21RekordboxDjPlayerUnit12processBlockERN4juce11AudioBufferIfEERNS1_10MidiBufferE", "ldr x19", 0};  // juce::AudioBuffer<float>: float** channels (numChannels, numSamples: its first two ints)
static const Offset O_bufClear{0x120, "__ZN6rbxfrm21RekordboxDjPlayerUnit12processBlockERN4juce11AudioBufferIfEERNS1_10MidiBufferE", "ldr x19", 2};    // bool isClear
static const Offset O_procRate{0x30, "__ZN4juce14AudioProcessor20setPlayConfigDetailsEiidi", "str x20", 0};  // juce::AudioProcessor: double, its sample rate
// The decks' display: MainComponent -> its panels' holder -> PlayerComponent -> PlayerPanelL.
static const Addr A_mainComponent{"__ZN13MainComponent11getInstanceEv"};
static const Offset O_mainHolder{0x568, "__ZN13MainComponent16callInputThroughEib", "ldr x0", 0};
static const Offset O_holderPlayers{0xe0, "__ZN13MainComponent16callInputThroughEib", "ldr x8", 0};
static const Offset O_playersPanel{0x148, "__ZN6djplay15PlayerComponent16setExternalInputEbbb", "ldr x19", 0};
static const Addr A_showThru{"__ZN6djplay15PlayerComponent12PlayerPanelL20updateDVSThroughModeEib"};  // (int deck, bool): INPUT THROUGH on the deck

namespace {

const int kDecks = 4;
const int kTaps = 64, kPhases = 256;
const int kMaxBlock = 2048;         // engine frames in one processBlock (128 in practice)
const int kScratch = kMaxBlock * 5 + kTaps + 8;  // input frames one block can need (an input at up to 4.4x the engine's rate)
const double kSafety = 0.002;       // s of distance on top of the blocks'
const double kUnderrunStep = 0.002, kMaxExtra = 0.050;
const double kMaxAdjust = 0.0015;   // the read ratio's trim: 1500 ppm, under 3 cents

// ---- the interpolator --------------------------------------------------------------------------------------------------

// A Kaiser-windowed sinc, kTaps long, at kPhases + 1 positions between two input frames (the reader blends the two
// nearest). cutoff: of the input's Nyquist; 0.96 of the lower of the two rates' leaves the top of the band to the
// filter's slope (64 taps: about 3.6 kHz wide at 44.1 kHz, -80 dB beyond), which only reaches down to 19.4 kHz.
struct Kernel {
    float h[kPhases + 1][kTaps];
    double cutoff = 0;
};
double besselI0(double x) {
    double sum = 1, term = 1;
    for (int k = 1; k < 40; k++) {
        term *= (x / (2 * k)) * (x / (2 * k));
        sum += term;
        if (term < 1e-12 * sum) break;
    }
    return sum;
}
void makeKernel(Kernel& k, double cutoff) {
    const double beta = 8.0, i0 = besselI0(beta);
    k.cutoff = cutoff;
    for (int p = 0; p <= kPhases; p++) {
        double frac = (double)p / kPhases, sum = 0, v[kTaps];
        for (int t = 0; t < kTaps; t++) {
            double x = (double)(t - (kTaps / 2 - 1)) - frac;  // from the point being read to tap t's frame
            double w = x / (kTaps / 2);
            double win = fabs(w) >= 1 ? 0 : besselI0(beta * sqrt(1 - w * w)) / i0;
            double s = fabs(x) < 1e-9 ? cutoff : sin(M_PI * cutoff * x) / (M_PI * x);
            v[t] = s * win, sum += v[t];
        }
        for (int t = 0; t < kTaps; t++) k.h[p][t] = (float)(v[t] / sum);  // exactly 1 at DC in every phase
    }
}
// One output sample: kTaps input frames against one phase of the kernel. Eight partial sums, so that the compiler
// vectorizes it (NEON / SSE) without reordering a float sum on its own; the same result as vDSP_dotpr's to the last
// bits of a float.
inline float dot(const float* x, const float* h) {
    float s[8] = {0, 0, 0, 0, 0, 0, 0, 0};
    for (int t = 0; t < kTaps; t += 8)
        for (int j = 0; j < 8; j++) s[j] += x[t + j] * h[t + j];
    return ((s[0] + s[4]) + (s[1] + s[5])) + ((s[2] + s[6]) + (s[3] + s[7]));
}

// ---- one deck's reader -------------------------------------------------------------------------------------------------

// Its settings are the main thread's, changed only while it holds the deck's lock; the rest is the audio thread's.
struct Reader {
    Capture* cap = nullptr;
    std::shared_ptr<Kernel> kernel;
    int chL = 0, chR = 1;
    std::atomic<float> gain{1};
    double extra = 0;            // s added to the distance (linein_extra_ms, and what underruns added)
    // audio thread
    bool synced = false;
    double pos = 0;              // the input frame the next output frame is read at
    double played = 0;           // the input frame the output got to: a restart never goes back before it
    double adjust = 0, integral = 0, fillLp = 0;
    float lastGain = 1;
    bool fadeIn = false;
    double burstFrames = 0, burstMax = 0;   // engine frames since the last pause between calls; the largest lately
    int64_t lastCall = 0;                   // ns (steady clock), 0: none yet
    // for the log (written by the audio thread)
    std::atomic<int> underruns{0}, resyncs{0};
    std::atomic<float> ppm{0}, fillMs{0}, lowMs{1e9f}, peak{0};
    std::atomic<double> addedExtra{0};
    float sl[kScratch], sr[kScratch];
};

struct Deck {
    std::atomic<void*> unit{nullptr};      // its RekordboxDjPlayerUnit
    std::atomic<bool> feeding{false};      // the audio thread fills this deck's input and holds its thru flag
    std::atomic<bool> busy{false};         // the lock: the audio thread only tries it, the main thread waits for it
    std::atomic<bool> switchedOff{false};  // rekordbox switched the deck's thru off since (the audio thread put it back)
    Reader reader;
    // main thread
    bool thruOurs = false;                 // we switched the deck's input thru on
    std::string showing;                   // the device it plays, for the log
    std::string why;                       // why it doesn't, as last logged
};
Deck g_deck[kDecks];

bool tryLock(Deck& d) {
    bool e = false;
    return d.busy.compare_exchange_strong(e, true, std::memory_order_acquire);
}
void unlock(Deck& d) { d.busy.store(false, std::memory_order_release); }
void lockWait(Deck& d) {  // main thread: the audio thread holds it for one block at most
    while (!tryLock(d)) std::this_thread::sleep_for(std::chrono::microseconds(200));
}

int64_t nowNs() {  // (the audio thread: mach_absolute_time / QueryPerformanceCounter underneath)
    return (int64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

// One engine block of the deck's input into l and r. False: nothing to give (not enough written yet, or an
// underrun: the block stays as it is, silent). Audio thread, the deck's lock held.
bool render(Reader& r, float* l, float* rr, int n, double engineRate) {
    Capture* c = r.cap;
    if (!c || !r.kernel || !(c->rate > 0) || !(engineRate > 0) || n < 1 || n > kMaxBlock) return false;
    // How many frames the engine asks for in one go: its calls come in bursts (one per output device block).
    int64_t t = nowNs();
    double sinceLast = r.lastCall ? (double)(t - r.lastCall) * 1e-9 : 1;
    r.lastCall = t;
    if (sinceLast > 0.5 * n / engineRate) r.burstFrames = 0;
    r.burstFrames += n;
    r.burstMax = std::max(r.burstFrames, r.burstMax * 0.9999);

    double ratio = c->rate / engineRate;                 // input frames per engine frame
    double step = ratio * (1 + r.adjust);
    uint64_t written = c->written.load(std::memory_order_acquire);
    double inBlock = std::max<double>(c->block, c->lastBlock.load(std::memory_order_relaxed));
    double target = (inBlock + r.burstMax * ratio) / 2 + n * ratio + kTaps / 2 + (kSafety + r.extra) * c->rate;
    double need = (n - 1) * step + kTaps / 2 + 2;         // frames past pos this block reads
    double fill = (double)written - r.pos;
    int m = n;  // the output frames there is input for: all of them, unless the input runs out in this block
    if (r.synced && (fill < need || fill > kRing - kTaps - inBlock)) {
        // Underrun (the input stalled, or its callbacks came later than the distance allows), or the reader fell
        // a whole ring behind (the engine stood still): it starts again below, a little further back.
        if (fill < need) {
            r.underruns.fetch_add(1, std::memory_order_relaxed);
            if (r.extra < kMaxExtra) r.extra += kUnderrunStep, r.addedExtra.store(r.extra, std::memory_order_relaxed);
            target += kUnderrunStep * c->rate;
            // What is still there is played, faded to nothing: an input that stops doesn't click.
            m = (int)std::max(0.0, std::min((double)n, floor((fill - kTaps / 2 - 2) / step) + 1));
            if (m < 8) m = 0;
        }
        if (m == n || m == 0) r.synced = false, m = n;
    }
    if (!r.synced) {
        // A (re)start comes on the first block after the input wrote, with the ring at its fullest: the distance
        // is taken half an input block longer, so that it is the distance kept on average. And it waits for input
        // that wasn't played yet: an input that stalls gives silence, never what was heard again.
        double start = target + inBlock / 2;
        if ((double)written < start + kTaps || (double)written - start < r.played) return false;
        r.pos = (double)written - start;
        r.adjust = 0, r.integral = 0, r.fillLp = target;
        r.synced = true, r.fadeIn = true;
        r.resyncs.fetch_add(1, std::memory_order_relaxed);
        step = ratio, fill = start;
    }
    // The input frames this block reads, the two channels side by side.
    int64_t base = (int64_t)floor(r.pos);
    int count = (int)((int64_t)floor(r.pos + (m - 1) * step) - base) + kTaps;
    int64_t first = base - (kTaps / 2 - 1);
    if (count > kScratch || first < 0) { r.synced = false; return false; }
    const float* ring = c->ring.data();
    int ch = c->channels, cl = std::min(r.chL, ch - 1), cr = std::min(r.chR, ch - 1);
    for (int i = 0; i < count; i++) {
        const float* f = ring + (size_t)((first + i) & (kRing - 1)) * (size_t)ch;
        r.sl[i] = f[cl], r.sr[i] = f[cr];
    }
    const Kernel& k = *r.kernel;
    float peak = 0;
    for (int i = 0; i < m; i++) {
        double p = r.pos + i * step;
        int64_t at = (int64_t)floor(p);
        double ph = (p - (double)at) * kPhases;
        int pi = (int)ph;
        float pf = (float)(ph - pi);
        int o = (int)(at - base);
        float a0 = dot(r.sl + o, k.h[pi]), a1 = dot(r.sl + o, k.h[pi + 1]);
        float b0 = dot(r.sr + o, k.h[pi]), b1 = dot(r.sr + o, k.h[pi + 1]);
        l[i] = a0 + (a1 - a0) * pf, rr[i] = b0 + (b1 - b0) * pf;
        peak = std::max(peak, std::max(fabsf(l[i]), fabsf(rr[i])));
    }
    r.pos += m * step;
    r.played = r.pos;
    // Gain, ramped over the block when it changed (both sides from g0 to g1); a fade-in after a (re)start.
    float g1 = r.gain.load(std::memory_order_relaxed), g0 = r.fadeIn ? 0 : r.lastGain;
    if (m < n) {
        // The input ran out: the rest of what there was, from the gain it had down to nothing, then silence. The
        // next block finds the reader without input and starts it again once there is new input.
        for (int i = 0; i < m; i++) {
            float x = 1 - (float)(i + 1) / (float)m, g = g0 * x * x;  // (squared: it ends level, however few frames there are)
            l[i] *= g, rr[i] *= g;
        }
        memset(l + m, 0, sizeof(float) * (size_t)(n - m)), memset(rr + m, 0, sizeof(float) * (size_t)(n - m));
        r.synced = false, r.lastGain = g1;
        if (peak * g0 > r.peak.load(std::memory_order_relaxed)) r.peak.store(peak * g0, std::memory_order_relaxed);
        return true;
    }
    if (g0 != g1) {
        float dg = (g1 - g0) / n;
        for (int i = 0; i < n; i++) {
            float g = g0 + dg * (float)i;
            l[i] *= g, rr[i] *= g;
        }
    } else if (g1 != 1) {
        for (int i = 0; i < n; i++) l[i] *= g1, rr[i] *= g1;
    }
    r.lastGain = g1, r.fadeIn = false;
    if (peak * g1 > r.peak.load(std::memory_order_relaxed)) r.peak.store(peak * g1, std::memory_order_relaxed);
    // The loop that keeps the distance: the fill, smoothed over a second (it steps with every block of either side),
    // against the target; proportional and integral, critically damped at about a 25 s period.
    double dt = n / engineRate;
    r.fillLp += (fill - r.fillLp) * std::min(1.0, dt / 1.0);
    double err = (r.fillLp - target) / c->rate;           // s: positive = too far behind the writer, read faster
    r.integral = std::max(-kMaxAdjust, std::min(kMaxAdjust, r.integral + 0.0625 * err * dt));
    r.adjust = std::max(-kMaxAdjust, std::min(kMaxAdjust, 0.5 * err + r.integral));
    r.ppm.store((float)(r.adjust * 1e6), std::memory_order_relaxed);
    r.fillMs.store((float)(r.fillLp / c->rate * 1000), std::memory_order_relaxed);
    float low = (float)((fill - need) / c->rate * 1000);
    if (low < r.lowMs.load(std::memory_order_relaxed)) r.lowMs.store(low, std::memory_order_relaxed);
    return true;
}

// ---- rekordbox's player units ------------------------------------------------------------------------------------------

void (*g_origProcess)(void*, void*, void*);

// RekordboxDjPlayerUnit::processBlock(juce::AudioBuffer<float>&, juce::MidiBuffer&). rekordbox's audio thread: no
// lock waited for, no allocation, no logging. (The same three pointers on both systems: x0-x2, Windows rcx, rdx, r8.)
void unitProcess(void* unit, void* buffer, void* midi) {
    for (Deck& d : g_deck) {
        if (d.unit.load(std::memory_order_relaxed) != unit) continue;
        if (!d.feeding.load(std::memory_order_acquire) || !tryLock(d)) break;
        if (d.feeding.load(std::memory_order_acquire)) {
            int channels = *(int*)buffer, n = *(int*)((char*)buffer + 4);
            float** ch = *(float***)((char*)buffer + O_bufChannels);
            if (channels >= 2 && n > 0 && ch && ch[0] && ch[1]) {
                if (render(d.reader, ch[0], ch[1], n, *(double*)((char*)unit + O_procRate))) *((uint8_t*)buffer + O_bufClear) = 0;
                else memset(ch[0], 0, sizeof(float) * (size_t)n), memset(ch[1], 0, sizeof(float) * (size_t)n);
            }
            uint8_t& thru = *((uint8_t*)unit + O_unitThru);
            if (!thru) thru = 1, d.switchedOff.store(true, std::memory_order_relaxed);
        }
        unlock(d);
        break;
    }
    g_origProcess(unit, buffer, midi);
}

// ---- the devices -------------------------------------------------------------------------------------------------------

// Every device with input channels, in a steady order (by UID): "name #2" counts in it.
std::vector<Input> inputs() {
    std::vector<Input> v = sys::inputs();
    std::sort(v.begin(), v.end(), [](const Input& x, const Input& y) { return x.uid < y.uid; });
    return v;
}
std::string lower(std::string s) {
    for (char& c : s) c = (char)tolower((unsigned char)c);
    return s;
}
std::string trimmed(std::string s) {
    s.erase(0, s.find_first_not_of(" \t"));
    s.erase(s.find_last_not_of(" \t") + 1);
    return s;
}
// The device a deck's option names: its UID, its name, or "name #n" for the nth of that name.
const Input* findInput(const std::vector<Input>& all, const std::string& spec) {
    std::string s = trimmed(spec);
    if (s.empty()) return nullptr;
    for (const Input& i : all)
        if (i.uid == s) return &i;
    int nth = 1;
    size_t hash = s.rfind('#');
    if (hash != std::string::npos && hash + 1 < s.size() && s.find_first_not_of("0123456789", hash + 1) == std::string::npos) {
        nth = std::max(1, atoi(s.c_str() + hash + 1));
        s = trimmed(s.substr(0, hash));
    }
    std::string want = lower(s);
    for (const Input& i : all)
        if (lower(i.name) == want && --nth == 0) return &i;
    return nullptr;
}

std::map<std::string, std::unique_ptr<Capture>> g_captures;   // by UID: the ones running
std::vector<std::unique_ptr<Capture>> g_retired;              // stopped; freed once no audio block can still be reading them
std::map<double, std::weak_ptr<Kernel>> g_kernels;            // by cutoff

std::shared_ptr<Kernel> kernelFor(double inRate, double engineRate) {
    double cutoff = 0.96 * std::min(1.0, engineRate / inRate);
    cutoff = round(cutoff * 1000) / 1000;
    if (auto k = g_kernels[cutoff].lock()) return k;
    auto k = std::make_shared<Kernel>();
    makeKernel(*k, cutoff);
    g_kernels[cutoff] = k;
    return k;
}

// Seconds on a clock that only goes forward (now() is the wall clock: it can be set back).
double mono() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

// A device that could not be opened is tried again after 10 s, then 20, 40 ... up to two minutes: an open can take
// seconds (Windows waits for its audio service), on the main thread.
struct Failed {
    double at = 0;
    int times = 0;
    std::string err;
};
std::map<std::string, Failed> g_failed;

Capture* startCapture(const Input& in, std::string* err) {
    auto it = g_captures.find(in.uid);
    if (it != g_captures.end()) return it->second.get();
    auto f = g_failed.find(in.uid);
    if (f != g_failed.end() && mono() - f->second.at < std::min(120.0, 10.0 * (1 << std::min(f->second.times - 1, 4)))) {
        *err = f->second.err;
        return nullptr;
    }
    std::unique_ptr<Capture> c = sys::open(in, err);
    if (!c) {
        Failed& fail = g_failed[in.uid];
        fail.at = mono(), fail.times++, fail.err = *err;
        return nullptr;
    }
    g_failed.erase(in.uid);
    c->listedRate = in.rate;
    logf("linein: listening to '%s' (%d input channel%s, %.0f Hz, %u-frame blocks)  %s", c->name.c_str(), in.channels, in.channels == 1 ? "" : "s",
         c->rate, (unsigned)c->block, c->uid.c_str());
    Capture* raw = c.get();
    g_captures[in.uid] = std::move(c);
    return raw;
}
// Stops a device nobody reads any more. Its ring stays for a few seconds: a reader that was detached under its
// deck's lock can't be in it, but that costs nothing to be sure of.
void retire(const std::string& uid) {
    auto it = g_captures.find(uid);
    if (it == g_captures.end()) return;
    Capture* c = it->second.get();
    sys::stop(c);
    c->stoppedAt = mono();
    logf("linein: stopped listening to '%s'", c->name.c_str());
    g_retired.push_back(std::move(it->second));
    g_captures.erase(it);
}

// ---- the options -------------------------------------------------------------------------------------------------------

struct Wanted {
    std::string device, channels;
    bool on;
    double gainDb;
};
Wanted wanted(int deck) {
    switch (deck) {
        case 0: return {opt::deck1.text(), opt::deck1Ch.text(), opt::deck1On.integer() != 0, opt::deck1Gain.number()};
        case 1: return {opt::deck2.text(), opt::deck2Ch.text(), opt::deck2On.integer() != 0, opt::deck2Gain.number()};
        case 2: return {opt::deck3.text(), opt::deck3Ch.text(), opt::deck3On.integer() != 0, opt::deck3Gain.number()};
        default: return {opt::deck4.text(), opt::deck4Ch.text(), opt::deck4On.integer() != 0, opt::deck4Gain.number()};
    }
}
// "3-4" -> 2, 3; "1" -> 0, 0 (a mono input on both sides); anything else, or past the device's channels: its first two.
void channelsOf(const std::string& s, int have, int* l, int* r) {
    int a = atoi(s.c_str()), b = a;
    size_t dash = s.find_first_of("-,/");
    if (dash != std::string::npos) b = atoi(s.c_str() + dash + 1);
    if (a < 1 || b < 1 || a > have || b > have) a = 1, b = std::min(2, have);
    *l = a - 1, *r = b - 1;
}

// ---- rekordbox's thru, per deck ----------------------------------------------------------------------------------------

void* djSystem() {
    void* e = engine();
    return e ? at<void* (*)(void*)>(A_djSystem)(e) : nullptr;
}
// The decks' player units, as the engine has them now (they are made anew when the audio device changes).
void findUnits() {
    void* sys = djSystem();
    char* graph = sys ? *(char**)((char*)sys + O_sysGraph) : nullptr;
    void** units = graph ? *(void***)(graph + O_graphUnits) : nullptr;
    int n = graph ? *(int*)(graph + O_graphCount) : 0;
    for (int d = 0; d < kDecks; d++) g_deck[d].unit.store(units && d < n ? units[d] : nullptr, std::memory_order_relaxed);
}
void* playerPanel() {
    char* mc = at<char* (*)()>(A_mainComponent)();
    char* holder = mc ? *(char**)(mc + O_mainHolder) : nullptr;
    char* players = holder ? *(char**)(holder + O_holderPlayers) : nullptr;
    return players ? *(void**)(players + O_playersPanel) : nullptr;
}
void setThru(int deck, bool on) {
    if (void* sys = djSystem()) at<void (*)(void*, int, bool)>(A_enableThru)(sys, deck, on);
    if (void* panel = playerPanel()) at<void (*)(void*, int, bool)>(A_showThru)(panel, deck, on);
}

bool g_patched;

void detach(Deck& d, int deck, const char* why) {
    if (d.feeding.load()) {
        lockWait(d);
        d.feeding.store(false, std::memory_order_release);
        d.reader.cap = nullptr, d.reader.kernel.reset();
        unlock(d);
    }
    if (d.thruOurs) {
        d.thruOurs = false;
        setThru(deck, false);
        logf("linein: deck %d plays tracks again (%s)", deck + 1, why);
    }
    d.showing.clear();
}

void manage() {  // main thread, once a second
    if (!g_patched) return;
    findUnits();
    bool emptyOnly = opt::empty.integer() != 0;
    double extra = std::max(0.0, std::min(opt::extraMs.number(), 200.0)) / 1000;
    Wanted want[kDecks];
    bool any = false;
    for (int i = 0; i < kDecks; i++) want[i] = wanted(i), any |= want[i].on && !trimmed(want[i].device).empty();
    std::vector<Input> all;
    if (any || !g_captures.empty()) all = inputs();
    // (a device that could not be opened and was unplugged since gets a fresh try when it is back)
    for (auto f = g_failed.begin(); (any || !g_captures.empty()) && f != g_failed.end();)
        f = std::any_of(all.begin(), all.end(), [&](const Input& i) { return i.uid == f->first; }) ? std::next(f) : g_failed.erase(f);
    // A device that went away (or stopped delivering), or runs at another rate now: its decks let go of it, and it is
    // started anew below.
    std::vector<std::string> gone;
    for (auto& [uid, c] : g_captures) {
        const Input* in = nullptr;
        for (const Input& x : all)
            if (x.uid == uid) in = &x;
        if (!in || in->id != c->id || in->rate != c->listedRate || c->lost.load()) gone.push_back(uid);
    }
    for (const std::string& uid : gone) {
        Capture* c = g_captures[uid].get();
        for (int i = 0; i < kDecks; i++)
            if (g_deck[i].reader.cap == c) detach(g_deck[i], i, ("'" + c->name + "' went away or changed").c_str());
        retire(uid);
    }
    std::map<std::string, bool> used;
    for (int i = 0; i < kDecks; i++) {
        Deck& d = g_deck[i];
        const Wanted& w = want[i];
        void* unit = d.unit.load();
        std::string why, err;
        const Input* in = nullptr;
        Capture* cap = nullptr;
        if (trimmed(w.device).empty()) why = "";   // (nothing asked for: nothing to say)
        else if (!w.on) why = "switched off";
        else if (!unit) why = "rekordbox has no such deck now";
        else if (emptyOnly && (deckLoaded(i) || deckLoading(i))) why = "a track is loaded";
        else if (!sys::allowed(&why)) {}  // (macOS: waiting for its permission to use audio inputs)
        else if (!(in = findInput(all, w.device))) why = "'" + trimmed(w.device) + "' isn't connected";
        else if (!(cap = startCapture(*in, &err))) why = "'" + in->name + "': " + err;
        if (!cap) {
            bool was = d.thruOurs || d.feeding.load();
            detach(d, i, why.empty() ? "no input set" : why.c_str());
            if (!was && !why.empty() && why != d.why) logf("linein: deck %d's input is off: %s", i + 1, why.c_str());
            d.why = why;
            continue;
        }
        d.why.clear();
        used[cap->uid] = true;
        double engineRate = *(double*)((char*)unit + O_procRate);
        int l, r;
        channelsOf(w.channels, cap->channels, &l, &r);
        float gain = (float)pow(10, std::max(-24.0, std::min(w.gainDb, 24.0)) / 20);
        Reader& rd = d.reader;
        std::shared_ptr<Kernel> kernel = kernelFor(cap->rate, engineRate > 0 ? engineRate : 44100);
        if (rd.cap != cap || rd.kernel != kernel || rd.chL != l || rd.chR != r || !d.feeding.load()) {
            lockWait(d);
            bool fresh = rd.cap != cap;
            rd.cap = cap, rd.kernel = kernel, rd.chL = l, rd.chR = r;
            if (fresh) {
                rd.synced = false, rd.pos = rd.played = 0, rd.extra = extra, rd.burstMax = 0, rd.lastCall = 0;
                rd.underruns = 0, rd.resyncs = 0, rd.addedExtra = 0, rd.peak = 0, rd.lowMs = 1e9f;
            }
            d.feeding.store(true, std::memory_order_release);
            unlock(d);
        }
        rd.gain.store(gain, std::memory_order_relaxed);
        if (rd.extra < extra) { lockWait(d); rd.extra = extra; unlock(d); }
        // rekordbox's own thru for the deck, and its display. rekordbox switches thru off again by itself now and
        // then (the audio thread holds the flag meanwhile): switched on again here, which also puts the deck's BPM
        // back to "from the input".
        bool again = d.switchedOff.exchange(false);
        if (!d.thruOurs || again) {
            setThru(i, true);
            if (!d.thruOurs)
                logf("linein: deck %d plays '%s' (channels %d-%d, %+.1f dB, %.0f Hz into the engine's %.0f Hz)", i + 1, cap->name.c_str(), l + 1, r + 1,
                     20 * log10(gain), cap->rate, engineRate);
            d.thruOurs = true;
        } else if (void* panel = playerPanel()) {
            at<void (*)(void*, int, bool)>(A_showThru)(panel, i, true);  // (does nothing when the deck shows it already)
        }
        d.showing = cap->name;
    }
    // Devices no deck reads any more, and the rings of those stopped a while ago.
    std::vector<std::string> idle;
    for (auto& [uid, c] : g_captures)
        if (!used.count(uid)) idle.push_back(uid);
    for (const std::string& uid : idle) retire(uid);
    g_retired.erase(std::remove_if(g_retired.begin(), g_retired.end(), [](const std::unique_ptr<Capture>& c) { return mono() - c->stoppedAt > 5; }), g_retired.end());
}

void logDecks() {
    for (int i = 0; i < kDecks; i++) {
        Deck& d = g_deck[i];
        Reader& r = d.reader;
        if (!d.feeding.load() || !r.cap) continue;
        float peak = r.peak.exchange(0), low = r.lowMs.exchange(1e9f);
        logf("linein: deck %d '%s': read at %+.0f ppm, %.1f ms behind the input (least to spare %.1f ms), peak %.1f dBFS, %d underrun%s, %d start%s%s", i + 1,
             r.cap->name.c_str(), r.ppm.load(), r.fillMs.load(), low > 1e8f ? 0 : low, peak > 0 ? 20 * log10(peak) : -120.0, r.underruns.load(),
             r.underruns.load() == 1 ? "" : "s", r.resyncs.load(), r.resyncs.load() == 1 ? "" : "s",
             r.addedExtra.load() > 0 ? (", " + std::to_string((int)lround(r.addedExtra.load() * 1000)) + " ms added after underruns").c_str() : "");
    }
}

// Test commands (command-linein in the config folder): "list" logs the audio inputs; "state" each deck.
void command() {
    std::string cmd = devCommand("linein");
    if (cmd == "list") {
        std::vector<Input> all = inputs();
        logf("linein: %d audio input%s", (int)all.size(), all.size() == 1 ? "" : "s");
        std::map<std::string, int> seen;
        for (const Input& i : all) {
            int nth = ++seen[lower(i.name)];
            logf("linein:   '%s'%s  %d channel%s, %.0f Hz  UID %s", i.name.c_str(), nth > 1 ? (" #" + std::to_string(nth)).c_str() : "", i.channels,
                 i.channels == 1 ? "" : "s", i.rate, i.uid.c_str());
        }
    } else if (cmd == "state") {
        for (int i = 0; i < kDecks; i++) {
            Deck& d = g_deck[i];
            void* unit = d.unit.load();
            logf("linein: deck %d: %s%s; rekordbox's thru flag %d", i + 1, d.feeding.load() ? ("plays '" + d.showing + "'").c_str() : "no input",
                 d.why.empty() ? "" : (" (" + d.why + ")").c_str(), unit ? *((uint8_t*)unit + O_unitThru) : -1);
        }
        logDecks();
    }
}

void tick() {  // main thread, every 0.1 s
    command();
    static double managedAt, loggedAt, listedAt = -1e9;
    if (mono() - managedAt >= 1) managedAt = mono(), manage();
    if (opt::log.integer() && mono() - loggedAt >= 10) loggedAt = mono(), logDecks();
    // Once, when a deck first asks for an input: what there is to choose from.
    if (listedAt < 0)
        for (int i = 0; i < kDecks; i++)
            if (!trimmed(wanted(i).device).empty()) {
                listedAt = now();
                std::string names;
                for (const Input& in : inputs()) names += (names.empty() ? "'" : ", '") + in.name + "'";
                logf("linein: audio inputs here: %s", names.empty() ? "none" : names.c_str());
                break;
            }
}

// The list "linein.inputs" (the settings window's drop-downs): the audio inputs there are now, as what goes into a
// deck's setting for each ("USB AUDIO CODEC", "USB AUDIO CODEC #2"). Main thread.
void listInputs(std::vector<std::string>& out) {
    std::map<std::string, int> seen;
    for (const Input& i : inputs()) {
        int nth = ++seen[lower(i.name)];
        out.push_back(i.name + (nth > 1 ? " #" + std::to_string(nth) : ""));
    }
}

void init() {
    // (Windows: the table maps the vtable symbol to the vtable - 0x10, so this lands on the same slot there)
    void** slot = (void**)(at<char*>(A_unitVtable) + 0x10 + V_unitProcess);
    g_origProcess = (void (*)(void*, void*, void*))*slot;
    g_patched = patchSlot(slot, (void*)&unitProcess) != nullptr;
    if (!g_patched) logf("linein: the decks' audio could not be reached (a vtable that can't be changed): no inputs on decks");
}

}  // namespace

// The window's sidebar area this mod is shown in (core/settings.h: the first declaration of an area counts).
namespace { const rbm::settings::Area kArea{"decks", "Decks & sound",
    "What you hear and mix: inputs, outputs, the mixer's effects.",
    "<circle cx=\"8\" cy=\"8\" r=\"6\"/><circle cx=\"8\" cy=\"8\" r=\"1.6\"/>", 10}; }

RBM_MOD({.name = "linein", .init = init, .tick = tick, .enabled = &g_on,
         .title = "Turntables and other inputs",
         .about = "Play a turntable, a CD player or any audio input through a deck, with the mixer's EQ, filter, faders and headphone cue on it.",
         .area = "decks", .order = 10, .rekordboxPage = "Audio/Input/Output"});
static int g_listed = (settings::list("linein.inputs", &listInputs), 0);
