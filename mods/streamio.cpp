// streamio: how rekordbox's analysis reads a track's audio.
// CBeatAnalyzeThread::getStreamAt reads 512 frames at a time and calls Thread::yield (sched_yield) after each,
// ~90,000 times per track, and analyze() yields between the stages of every block too; with several tracks
// analysed at once every one can hand the core away. Skipped.
// (Measured on 50 tracks at analyze_parallel=6: 7-12 s faster per run, ~10%. Waking up sooner while the
// reader catches up, by checking it every 0.5 ms instead of sleeping 10 ms, cut the stream waits but made the
// audio pass slower overall, so getStreamAt's sleeps are left alone.)
// Phrase analysis then reads the whole track twice more (Analyze_AB, Analyze_CD), rewinding the reader and decoding
// it again each time. The audio pass's samples are kept instead (as the 16-bit values getStreamAt converts them
// from, so exactly the same floats; tracks up to 15 minutes, ~160 MB) and the phrase passes read those.
//   config: streamio=0 to disable; streamio_phrase=0 to read the audio again for phrase analysis
#include "../core/rbmod.h"
#include <vector>

using namespace rbm;

// Its settings (core/settings.h): what the settings window and the config file's list show of them.
namespace opt {
const Setting phrase{"streamio_phrase", true, {.label = "Reuse the audio for phrase analysis", .advanced = true}};
}  // namespace opt

static volatile bool g_on = true;

#define YIELD_HOOK(name) \
    extern "C" void rbm_hook_##name() { \
        if (!g_on) RBM_ORIG(name, void (*)())(); \
    }
YIELD_HOOK(gs_yield)
YIELD_HOOK(an_yield0)
YIELD_HOOK(an_yield1)
YIELD_HOOK(an_yield2)
YIELD_HOOK(an_yield3)
YIELD_HOOK(an_yield4)
YIELD_HOOK(an_yield5)

// CBeatAnalyzeThread::getStreamAt(reader, pos, short* L, short* R, block, float* outL, float* outR, count): reads
// count frames from pos into outL/outR (as floats of the 16-bit samples, zero-filled past the end), returns count.
// (unsigned long: rekordbox declares them so; 64-bit on mac, 32-bit on Windows, where size_t would read garbage.)
typedef unsigned long (*GetStreamFn)(void*, void*, unsigned long, int16_t*, int16_t*, unsigned long, float*, float*, unsigned long);

// The reader's setPosition, as analyze() rewinds it before each phrase pass.
#ifdef _WIN32
// MSVC multiple inheritance: getStreamAt gets the whole reader, and analyze() calls setPosition(0) through its pcm
// interface, a sub-object at +0x250, in that vtable's slot 0x28 (`add rcx, 0x250; mov rax, [rcx];
// call [rax+0x28]` with edx 0, before the AB and CD passes; tools/win/verify/streamio.out.json).
static const Offset kPcmInterface{0x250, "__ZN8analyzer18CBeatAnalyzeThread7analyzeEv", "win:reader pcm interface", 0};
static const Offset kSetPositionSlot{0x28, "__ZN8analyzer18CBeatAnalyzeThread7analyzeEv", "win:reader setPosition slot", 0};
static void setPosition(void* reader, int pos) {
    void* pcm = (char*)reader + kPcmInterface;
    (*reinterpret_cast<void (***)(void*, int)>(pcm))[kSetPositionSlot / 8](pcm, pos);
}
#else
static const Offset V_setPosition{0x28, "__ZTVN6rbxfrm17PcmReaderTemplateINS_6Float2EEE",
                                   "vt:__ZN6rbxfrm17PcmReaderTemplateINS_6Float2EE11setPositionEi", 0};
static void setPosition(void* reader, int pos) {
    (*reinterpret_cast<void (***)(void*, int)>(reader))[V_setPosition / 8](reader, pos);
}
#endif

// One per analysis thread: what this track's audio pass has read so far, from frame 0 on.
namespace {
struct Samples {
    const void* reader = nullptr;
    bool ok = false;      // read from 0 without gaps, and not too long
    bool served = false;  // the current phrase pass was read from here (the reader is still at its start)
    std::vector<int16_t> l, r;
};
}  // namespace
static thread_local Samples t_samples;
static const size_t kMaxFrames = 15 * 60 * 48000;

static void keep(void* reader, size_t pos, const float* l, const float* r, size_t n) {
    Samples& s = t_samples;
    if (pos == 0) {
        s.reader = reader;
        s.ok = opt::phrase.integer() != 0;
        s.l.clear(), s.r.clear();
    }
    if (!s.ok || reader != s.reader || pos != s.l.size() || pos + n > kMaxFrames) {
        s.ok = false;
        std::vector<int16_t>().swap(s.l), std::vector<int16_t>().swap(s.r);
        return;
    }
    s.l.resize(pos + n), s.r.resize(pos + n);
    for (size_t i = 0; i < n; i++) s.l[pos + i] = (int16_t)l[i], s.r[pos + i] = (int16_t)r[i];
}

static unsigned long serve(void* reader, size_t pos, float* l, float* r, size_t n) {
    Samples& s = t_samples;
    if (pos == 0) s.served = false;
    if (g_on && s.ok && reader == s.reader && pos + n <= s.l.size()) {
        for (size_t i = 0; i < n; i++) l[i] = s.l[pos + i], r[i] = s.r[pos + i];
        s.served = true;
        return (unsigned long)n;
    }
    // Reading from the reader after all: it decodes ahead from where it was last positioned (the pass rewound it
    // to 0), and would wait for reads of the part served from here that never come.
    if (s.served) {
        setPosition(reader, (int)pos);
        s.served = false;
    }
    return 0;
}

extern "C" unsigned long rbm_hook_an_streamMain(void* thread, void* reader, unsigned long pos, int16_t* sl, int16_t* sr, unsigned long block,
                                         float* l, float* r, unsigned long n) {
    unsigned long got = RBM_ORIG(an_streamMain, GetStreamFn)(thread, reader, pos, sl, sr, block, l, r, n);
    if (g_on) keep(reader, pos, l, r, got);
    return got;
}
extern "C" unsigned long rbm_hook_an_streamAB(void* thread, void* reader, unsigned long pos, int16_t* sl, int16_t* sr, unsigned long block,
                                       float* l, float* r, unsigned long n) {
    if (unsigned long got = serve(reader, pos, l, r, n)) return got;
    return RBM_ORIG(an_streamAB, GetStreamFn)(thread, reader, pos, sl, sr, block, l, r, n);
}
// The last pass: the samples aren't needed after it.
extern "C" unsigned long rbm_hook_an_streamCD(void* thread, void* reader, unsigned long pos, int16_t* sl, int16_t* sr, unsigned long block,
                                       float* l, float* r, unsigned long n) {
    unsigned long got = serve(reader, pos, l, r, n);
    if (!got) got = RBM_ORIG(an_streamCD, GetStreamFn)(thread, reader, pos, sl, sr, block, l, r, n);
    if (pos + n >= t_samples.l.size()) t_samples = Samples();
    return got;
}

// "streamio.passAudio": the samples this analysis thread's audio pass has read (16-bit values, from frame 0 on; valid
// on this thread until the phrase passes are done). betterkey hands them to Beat This! and the key CNN at the beat job
// instead of decoding the track once more.
static bool passAudio(const void** reader, const int16_t** l, const int16_t** r, size_t* n) {
    const Samples& s = t_samples;
    if (!g_on || !s.ok || s.l.empty()) return false;
    *reader = s.reader, *l = s.l.data(), *r = s.r.data(), *n = s.l.size();
    return true;
}
RBM_PROVIDE("streamio", "streamio.passAudio", passAudio, &g_on);

RBM_MOD({.name = "streamio", .enabled = &g_on,
         .title = "Faster reading",
         .about = "Reads a track's audio faster during analysis.",
         .area = "analysis", .order = 90});
