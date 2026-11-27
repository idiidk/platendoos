// mac-only: two call sites of the mac binary and a vtable of rekordbox's mixer; leave it out of a Windows build.
// idlewake: three things rekordbox keeps doing while nothing happens (measured idle, no deck loaded: 20% of a core,
// 4000 context switches a second).
//
// 1. The MIDI output thread (one per MIDI output, djplay::MidiOutputHelper::run) waits 1 ms on its event, over and
//    over: 1000 wake-ups a second to find nothing to send. Messages with a time (the ring) signal the event; "priority"
//    messages don't, they count on the 1 ms poll, and a turn of the loop sends one of them. rekordbox has the other
//    way built in: sendPriorityMessage signals the event when the thread's wait (a field) differs from its first value.
//    Here the wait field is set off that value, so every sender wakes the thread, and the wait itself is 1 ms while
//    priority messages are queued (the same pace as before: one a millisecond) and up to 20 ms when there are none,
//    never past the moment the controller's keep-alive message is due (every 201 ms).
//
// 2. A level meter restarts a timer on every frame it is drawn (LevelMeter::drawMetalTextures, on rekordbox's display
//    thread): "if I'm not drawn for a while, set my level to 0". Each restart wakes JUCE's timer thread: per meter, per
//    frame (four meters at 120 fps: 480 a second). While the meter's level is 0 the timer has nothing to do, so the
//    restart is left out; a timer still running then fires once, sets 0 to 0 and stops, as it would have.
//
// 3. Every mixer channel, the master and the sampler run two envelope followers on their audio all the time
//    (EnvelopeGeneratorArray::processBlock: copies, a filter chain, peak history, a gain ramp; 2% of a core, a quarter
//    of the audio thread with nothing playing). Only the Color FX "gate / comp" reads them (GateCore, CompCore:
//    getEnvelopePtr, getAverage). Here a channel whose input has been exactly zero for 2 million samples (42 s at
//    48 kHz, long past every history in it) skips the followers; the first sample that isn't zero runs them again.
//    idlewake_check=1 counts, for the blocks that would be skipped, whether running the followers changed what the
//    gate reads: it never did (see the log line it writes), so nothing that listens to them can tell.
//    What does differ: the followers' own block counters stand still while skipped, so after such a silence their
//    first peak window ends up to one window earlier or later than it would have.
//    No lock, allocation or logging on the audio thread.
//   config: idlewake=0 to disable; idlewake_midi=0, idlewake_meters=0, idlewake_envelope=0 for one of the three;
//   idlewake_check=1 (for testing) runs the envelope followers anyway and logs whether their output changed
#include "../core/rbmod.h"
#include <atomic>

using namespace rbm;

// Its settings (core/settings.h): what the settings window and the config file's list show of them.
namespace opt {
const Setting midi{"idlewake_midi", true, {.label = "Rest the MIDI output thread"}};
const Setting meters{"idlewake_meters", true, {.label = "Rest the level meters' timers"}};
const Setting envelope{"idlewake_envelope", true, {.label = "Rest the envelope followers on silence"}};
const Setting check{"idlewake_check", false, {.label = "Check the skipped envelope followers", .debug = true}};
const Setting log{"idlewake_log", false, {.label = "Log what was left out every 10 seconds", .debug = true}};
}  // namespace opt

static volatile bool g_on = true;
static volatile bool g_midi = true, g_meters = true, g_envelope = true, g_check = false;  // read in the tick

// ---- 1: the MIDI output thread ---------------------------------------------------------------------------------------

static const char kMidiRun[] = "__ZN6djplay16MidiOutputHelper3runEv";
static const Offset O_midiEvent{0x208, kMidiRun, "add x20", 0};     // MidiOutputHelper: its juce::WaitableEvent
static const Offset O_midiWait{0x1e4, kMidiRun, "ldr x20", 0};      // int: ms the thread waits; the next int is its first value
static const Offset O_midiKeepAlive{0x1f0, kMidiRun, "ldr x20", 1}; // MidiMessage*: the keep-alive (nullptr: none)
static const Offset O_midiSentAt{0x200, kMidiRun, "ldr x20", 2};    // uint32: juce's millisecond counter at the last keep-alive
static const Offset O_midiQueued{0x298, kMidiRun, "ldr x20", 17};   // size_t: priority messages queued (a std::list's size)
static const Addr A_msCounter{"__ZN4juce4Time21getMillisecondCounterEv"};
static const int kIdleMs = 20, kKeepAliveMs = 201;
static std::atomic<int> g_midiIdle, g_midiBusy;

// MidiOutputHelper::run -> bl juce::WaitableEvent::wait(double ms): on the helper's own thread.
extern "C" bool rbm_hook_midiout_wait(void* event, double ms) {
    auto wait = RBM_ORIG(midiout_wait, bool (*)(void*, double));
    char* helper = (char*)event - O_midiEvent;
    int& interval = *(int*)(helper + O_midiWait);
    int first = *(int*)(helper + O_midiWait + 4);
    if (!g_on || !g_midi) {
        if (interval != first) interval = first;
        return wait(event, first);
    }
    if (interval == first) interval = first + kIdleMs;  // (any other value: sendPriorityMessage then signals the event)
    if (*(size_t*)(helper + O_midiQueued)) {            // one a turn, at rekordbox's own pace
        g_midiBusy.fetch_add(1, std::memory_order_relaxed);
        return wait(event, first);
    }
    double idle = kIdleMs;
    if (*(void**)(helper + O_midiKeepAlive)) {
        uint32_t since = at<uint32_t (*)()>(A_msCounter)() - *(uint32_t*)(helper + O_midiSentAt);
        double due = (double)kKeepAliveMs - (double)since;
        idle = due < 1 ? 1 : due < kIdleMs ? due : kIdleMs;
    }
    g_midiIdle.fetch_add(1, std::memory_order_relaxed);
    return wait(event, idle);
}

// ---- 2: level meters -------------------------------------------------------------------------------------------------

static const char kMeterDraw[] = "__ZN6djplay10LevelMeter17drawMetalTexturesEP12MetalContextRKN4juce9RectangleIfEE";
static const Offset O_meterTimer{0xf0, kMeterDraw, "add x21", 0};   // LevelMeter: its juce::MultiTimer
static const Offset O_meterLevel{0x418, kMeterDraw, "str x21", 0};  // int: the level it shows; timer 1 sets it to 0
static std::atomic<int> g_meterSkipped;

// LevelMeter::drawMetalTextures -> bl juce::MultiTimer::startTimer(1, ms), every frame (the second of its two
// startTimer calls; the first, timer 0, is the peak hold and only runs on a new peak). Display thread.
extern "C" void rbm_hook_meter_timer(void* timer, int id, int ms) {
    if (g_on && g_meters && *(int*)((char*)timer - O_meterTimer + O_meterLevel) == 0) {
        g_meterSkipped.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    RBM_ORIG(meter_timer, void (*)(void*, int, int))(timer, id, ms);
}

// ---- 3: envelope followers on silence --------------------------------------------------------------------------------

static const char kEnvProcess[] = "__ZN22EnvelopeGeneratorArray12processBlockERN4juce11AudioBufferIfEEii";
static const Addr A_envVtable{"__ZTV22EnvelopeGeneratorArray"};
static const Offset V_envProcess{0x10, "__ZTV22EnvelopeGeneratorArray", "vt:__ZN22EnvelopeGeneratorArray12processBlockERN4juce11AudioBufferIfEEii", 0};
static const Offset O_bufChannels{0x10, kEnvProcess, "ldr x1", 1};  // juce::AudioBuffer<float>: float** channels (numChannels: its first int)
static const Offset O_envGenerators{0x20, kEnvProcess, "ldr x0", 3}; // EnvelopeGeneratorArray: EnvelopeGenerator** (an OwnedArray's data)
static const Offset O_envCount{0x2c, kEnvProcess, "ldr x0", 2};      // int: how many
static const Addr A_envelopePtr{"__ZNK17EnvelopeGenerator14getEnvelopePtrEv"};  // const float*: one value per sample of the block
static const Addr A_envelopeAverage{"__ZNK17EnvelopeGenerator10getAverageEv"};  // float
static const int64_t kSilentSamples = 2000000;

namespace {
struct Channel {  // one EnvelopeGeneratorArray; audio thread only
    void* array;
    int64_t silent;  // samples of exact zero in a row
};
}  // namespace
static Channel g_channels[32];
static int (*g_envOrig)(void*, void*, int, int);
static std::atomic<int64_t> g_envSkipped, g_envRun, g_checkSame, g_checkChanged;

static Channel* channelOf(void* array) {
    for (Channel& c : g_channels) {
        if (c.array == array) return &c;
        if (!c.array) { c.array = array; return &c; }
    }
    return nullptr;  // (more arrays than rekordbox has: left alone)
}
static bool allZero(const float* p, int n) {  // exactly: +0 or -0, nothing smaller than that counts
    uint32_t any = 0;
    for (int i = 0; i < n; i++) {
        uint32_t bits;
        memcpy(&bits, p + i, 4);
        any |= bits & 0x7fffffff;
    }
    return any == 0;
}
// What the gate / comp reads of an array: every generator's envelope of this block and its average.
static uint64_t outputsOf(void* array, int n) {
    uint64_t h = 1469598103934665603ull;
    void** gens = *(void***)((char*)array + O_envGenerators);
    for (int g = 0, count = *(int*)((char*)array + O_envCount); g < count; g++) {
        const uint32_t* env = (const uint32_t*)at<const float* (*)(void*)>(A_envelopePtr)(gens[g]);
        for (int i = 0; env && i < n; i++) h = (h ^ env[i]) * 1099511628211ull;
        float avg = at<float (*)(void*)>(A_envelopeAverage)(gens[g]);
        uint32_t bits;
        memcpy(&bits, &avg, 4);
        h = (h ^ bits) * 1099511628211ull;
    }
    return h;
}

// EnvelopeGeneratorArray::processBlock(AudioBuffer<float>&, int start, int n): 0 done, 1 not (wrong channel count or
// block size, as the original says itself). Audio thread.
static int envProcess(void* array, void* buffer, int start, int n) {
    if (!g_on || !g_envelope || n <= 0 || *(int*)buffer != 2) return g_envOrig(array, buffer, start, n);
    Channel* c = channelOf(array);
    float** ch = *(float***)((char*)buffer + O_bufChannels);
    if (!c || !allZero(ch[0] + start, n) || !allZero(ch[1] + start, n)) {
        if (c) c->silent = 0;
        g_envRun.fetch_add(1, std::memory_order_relaxed);
        return g_envOrig(array, buffer, start, n);
    }
    if (c->silent < kSilentSamples) {
        c->silent += n;
        g_envRun.fetch_add(1, std::memory_order_relaxed);
        return g_envOrig(array, buffer, start, n);
    }
    if (g_check) {  // run it and see whether anything the gate reads changed
        uint64_t before = outputsOf(array, n);
        int r = g_envOrig(array, buffer, start, n);
        (before == outputsOf(array, n) ? g_checkSame : g_checkChanged).fetch_add(1, std::memory_order_relaxed);
        return r;
    }
    g_envSkipped.fetch_add(1, std::memory_order_relaxed);
    return 0;
}

// ---------------------------------------------------------------------------------------------------------------------

static void tick() {  // main thread, every 0.1 s
    g_midi = opt::midi.integer();
    g_meters = opt::meters.integer();
    g_envelope = opt::envelope.integer();
    g_check = opt::check.integer();
    static int ticks;
    if (++ticks % 100) return;  // every 10 s, with idlewake_log=1 or the check on
    static int64_t same, changed;
    int64_t s = g_checkSame.load(), c = g_checkChanged.load();
    if (g_check && (s != same || c != changed))
        logf("idlewake: check: of the audio blocks that would be skipped, %lld left the envelopes as they were, %lld changed them (since launch)", (long long)s, (long long)c);
    same = s, changed = c;
    if (opt::log.integer())
        logf("idlewake: in 10 s: MIDI thread woke %d times idle, %d with messages queued; %d meter timer restarts left out; envelope blocks %lld run, %lld skipped",
             g_midiIdle.exchange(0), g_midiBusy.exchange(0), g_meterSkipped.exchange(0), (long long)g_envRun.exchange(0), (long long)g_envSkipped.exchange(0));
}

RBM_MOD({.name = "idlewake", .init = [] {
             void** slot = (void**)(at<char*>(A_envVtable) + 0x10 + V_envProcess);
             g_envOrig = (int (*)(void*, void*, int, int))*slot;
             if (!patchSlot(slot, (void*)&envProcess)) logf("idlewake: the envelope followers' vtable could not be changed, they run as before");
         }, .tick = tick, .enabled = &g_on,
         .title = "Idle work",
         .about = "Less work while nothing plays.",
         .area = "speed", .order = 60});
