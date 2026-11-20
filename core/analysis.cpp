// Shared infrastructure of rekordbox's track analysis, so that the mods around it (timing, betterkey, bettergrid,
// quickdeck, analyzer, streamio) can each be left out of a build, or switched off, without the others noticing.
//  - the analysis thread -> track URI lookup (analysisSong);
//  - the hooks that mark the stages of CBeatAnalyzeThread::analyze (analysisMark): the marks go to whichever of the
//    services "timing.mark" (timing), "analysis.stage" (quickdeck) and "bettergrid.stage" (bettergrid) is provided,
//    and each stage's start also to "genres.analysing" (genres);
//  - the hooks around the beat job and the key job (SetSepcialInfo): the stage marks, the pass audio, and the calls
//    to "bettergrid.prefetch" / "betterkey.keyJob" (see core/rbmod.h "analysis");
//  - the audio of a track being analysed, decoded once and shared (analysisAudio).
// Its recipes belong to the pseudo-mod "analysis": when one doesn't resolve in a rekordbox build, only this
// shared audio and song lookup are off (analysisAudio returns null, nothing is marked), not every mod.
#include "rbmod.h"
#include <math.h>
#include <string.h>
#include <algorithm>
#include <atomic>
#include <future>
#include <map>
#include <mutex>
#include <vector>

using namespace rbm;

static const Addr A_pcmReaderCtor{"__ZN6rbxfrm17PcmReaderTemplateINS_6Float2EEC1ERKN4juce6StringEiNS3_6Thread8PriorityEiiPKNS_18AudioReaderFactoryE", intptr_t(0), "analysis"};  // rbxfrm::PcmReaderTemplate<Float2>(String const&, int, Thread::Priority, int, int, AudioReaderFactory const*)
static const Addr A_pcmGetStreamAt{"__ZN6rbxfrm17PcmReaderTemplateINS_6Float2EE11getStreamAtEiPfS3_i", intptr_t(0), "analysis"};  // PcmReaderTemplate<Float2>::getStreamAt(int start, float* L, float* R, int count)
static const Addr A_readerChannels{"__ZNK6rbxfrm15ReaderOwnerTmplINS_6Float2EE13getChannelNumEv", intptr_t(0), "analysis"};  // ReaderOwnerTmpl<Float2>::getChannelNum() const
static const Addr A_readerRate{"__ZNK6rbxfrm15ReaderOwnerTmplINS_6Float2EE17getFileSampleRateEv", intptr_t(0), "analysis"};  // ReaderOwnerTmpl<Float2>::getFileSampleRate() const -> int
static const Offset S_pcmReader{0x2a8, "__ZN8analyzer18CBeatAnalyzeThread7analyzeEv", "movz x0", intptr_t(0), "analysis"};  // sizeof(PcmReaderTemplate<Float2>): analyze() news one before constructing it
static const Offset O_readerOwner{0x1d8, "__ZN6rbxfrm17PcmReaderTemplateINS_6Float2EE6unloadEv", "add x19", 1, "analysis"};  // PcmReaderTemplate: its ReaderOwnerTmpl
static const Offset O_readerLength{0x1f8, "__ZN6rbxfrm17PcmReaderTemplateINS_6Float2EE6unloadEv", "str x19", intptr_t(0), "analysis"};  // PcmReaderTemplate: length in frames (int)
static const Offset O_threadSource{0x260, "__ZN8analyzer18CBeatAnalyzeThread15getSongFilePathEv", "ldr x21", intptr_t(0), "analysis"};  // CBeatAnalyzeThread: analyzer::DataSource* of the song
static const Offset V_sourcePath{0x28, "__ZTVN8analyzer10DataSourceE", "vt:__ZNK8analyzer10DataSource7GetPathEv", intptr_t(0), "analysis"};  // DataSource vtable: path (juce::String const&)

namespace rbm {

static bool usable() { return !versionOff("analysis"); }

bool analysisSong(const void* thread, std::string* uri) {
    if (!thread || !usable()) return false;
    void* source = *(void* const*)((const char*)thread + O_threadSource);
    const JStr* path = source ? (*reinterpret_cast<const JStr* (***)(void*)>(source))[V_sourcePath / 8](source) : nullptr;
    if (!path || !path->text) return false;
    *uri = path->text;
    return true;
}

// ---- stage marks ------------------------------------------------------------------------------

namespace {  // file-local
struct Current { const void* thread = nullptr; std::string uri; std::vector<std::string> seen; };
}  // namespace
static thread_local Current t_cur;  // the analysis this thread runs

typedef void (*TimingMarkFn)(const char* uri, const char* stage, bool fresh);
typedef void (*TimingWaitFn)(const char* uri, int ms);
typedef void (*StageFn)(const void* thread, const char* uri, const char* stage);

// Analyses at their beat stage (rekordbox's beat detection, and Beat This! and the style model next to it): from the
// beat job's start to the analysis' next stage, counted per analysis thread.
static std::atomic<int> g_beatStages{0};
static std::atomic<double> g_beatStageAt{0};
static thread_local bool t_beatStage;
static void beatStage(bool on) {
    if (t_beatStage) t_beatStage = false, g_beatStages--;
    if (on) t_beatStage = true, g_beatStages++, g_beatStageAt = now();
}
// (an analysis cancelled at its beat stage never says it ended: not after 15 s)
bool analysisBeatStage() { return g_beatStages > 0 && now() - g_beatStageAt < 15; }

void analysisMark(const void* thread, const char* stage) {
    if (strcmp(stage, "poll")) beatStage(!strcmp(stage, "beat job"));
    auto timing = service<TimingMarkFn>("timing.mark");
    auto deck = service<StageFn>("analysis.stage");
    auto grid = service<void (*)(const void*, const char*)>("bettergrid.stage");
    auto genres = service<void (*)(const char*)>("genres.analysing");
    if (!timing && !deck && !grid && !genres) return;
    if (!thread) {  // Windows: hooks that don't see the thread; they run on the analysis thread
        thread = t_cur.thread;
        if (!thread) return;
    }
    bool start = !strcmp(stage, "start");
    bool fresh = start || t_cur.thread != thread;
    std::string uri;
    bool known = analysisSong(thread, &uri);
    // A reused CBeatAnalyzeThread on the next song: only analyzer sends "start", and it may be left out or off.
    if (!fresh && known && uri != t_cur.uri) fresh = true;
    if (fresh) {
        if (!known) {
            if (!usable()) return;
            uri = "?";
        }
        t_cur.thread = thread;
        t_cur.uri = uri;
        t_cur.seen.clear();
    }
    bool poll = !strcmp(stage, "poll");
    if (!poll && !start) {  // first time only (phrase analysis runs in several calls)
        for (auto& s : t_cur.seen)
            if (s == stage) return;
        t_cur.seen.push_back(stage);
    }
    if (timing) timing(t_cur.uri.c_str(), stage, fresh);
    // quickdeck: hands the deck what this stage has ready (on this thread, before the stage starts)
    if (!poll && deck) deck(thread, t_cur.uri.c_str(), stage);
    // bettergrid: ends what an earlier analysis left on this thread
    if (!poll && grid) grid(thread, stage);
    // The analysis' last step: nothing of it needs the shared audio any more (the key stage frees it; this is for an
    // analysis that had none, or whose key mod couldn't run).
    if (!strcmp(stage, "seek table") && t_cur.uri != "?") analysisAudioDone(t_cur.uri);
    // genres: fills the track's Genre field once its analysis has gone quiet (no stage is always the last one: an
    // analysis in memory has no seek table)
    if (!poll && genres && t_cur.uri != "?") genres(t_cur.uri.c_str());
}

// juce::Thread::sleep(ms) in the analysis' "stream not ready yet" polls.
static void streamWait(int ms) {
    if (!t_cur.thread) return;
    if (auto f = service<TimingWaitFn>("timing.wait")) f(t_cur.uri.c_str(), ms);
}

}  // namespace rbm

extern "C" void rbm_hook_an_wait1(int ms) { streamWait(ms); RBM_ORIG(an_wait1, void (*)(int))(ms); }
extern "C" void rbm_hook_an_wait2(int ms) { streamWait(ms); RBM_ORIG(an_wait2, void (*)(int))(ms); }

// Marker hooks: note the stage, then continue into the original call with every argument register
// (and x8, for calls returning a struct) untouched.
#if defined(__aarch64__)
#define MARK_HOOK(name, reg, stage) \
    extern "C" void rbm_mark_##name(const void* thread) { analysisMark(thread, stage); } \
    extern "C" void* rbm_orig_##name() { return RBM_ORIG(name, void*); } \
    asm(".text\n.p2align 2\n.globl _rbm_hook_" #name "\n_rbm_hook_" #name ":\n" \
        "stp x29, x30, [sp, #-0xe0]!\nmov x29, sp\n" \
        "stp x0, x1, [sp, #0x10]\nstp x2, x3, [sp, #0x20]\nstp x4, x5, [sp, #0x30]\nstp x6, x7, [sp, #0x40]\n" \
        "str x8, [sp, #0x50]\nstp q0, q1, [sp, #0x60]\nstp q2, q3, [sp, #0x80]\nstp q4, q5, [sp, #0xa0]\nstp q6, q7, [sp, #0xc0]\n" \
        "mov x0, " #reg "\nbl _rbm_mark_" #name "\nbl _rbm_orig_" #name "\nmov x16, x0\n" \
        "ldp q6, q7, [sp, #0xc0]\nldp q4, q5, [sp, #0xa0]\nldp q2, q3, [sp, #0x80]\nldp q0, q1, [sp, #0x60]\n" \
        "ldr x8, [sp, #0x50]\nldp x6, x7, [sp, #0x40]\nldp x4, x5, [sp, #0x30]\nldp x2, x3, [sp, #0x20]\nldp x0, x1, [sp, #0x10]\n" \
        "ldp x29, x30, [sp], #0xe0\nbr x16\n");
#else
// Windows (MSVC x64): the hook saves the argument registers (rcx, rdx, r8, r9, xmm0-3), calls analysisMark with
// the thread (`thr`: the call's first argument is the thread, else null = the analysis thread's current one), then
// jumps to the original with every argument register untouched. (Stack arguments and the return address are
// untouched too: the hook is entered by a call, like the original.)
#define MARK_HOOK(name, reg, stage, thr) \
    extern "C" void rbm_mark_##name(const void* thread) { analysisMark(thread, stage); } \
    extern "C" void* rbm_orig_##name() { return RBM_ORIG(name, void*); } \
    asm(".text\n.globl rbm_hook_" #name "\nrbm_hook_" #name ":\n" \
        "push rcx\npush rdx\npush r8\npush r9\nsub rsp, 104\n" \
        "movups xmmword ptr [rsp+32], xmm0\nmovups xmmword ptr [rsp+48], xmm1\n" \
        "movups xmmword ptr [rsp+64], xmm2\nmovups xmmword ptr [rsp+80], xmm3\n" \
        thr \
        "call rbm_mark_" #name "\ncall rbm_orig_" #name "\nmov r11, rax\n" \
        "movups xmm3, xmmword ptr [rsp+80]\nmovups xmm2, xmmword ptr [rsp+64]\n" \
        "movups xmm1, xmmword ptr [rsp+48]\nmovups xmm0, xmmword ptr [rsp+32]\n" \
        "add rsp, 104\npop r9\npop r8\npop rdx\npop rcx\njmp r11\n");
#define THREAD_IS_ARG0 "mov rcx, qword ptr [rsp+128]\n"
#define NO_THREAD "xor ecx, ecx\n"
#endif

#if defined(__aarch64__)
MARK_HOOK(an_poll, x0, "poll")
MARK_HOOK(an_phrase, x20, "phrase")
MARK_HOOK(an_embed, x20, "similarity")
MARK_HOOK(an_seek, x0, "seek table")
#else
MARK_HOOK(an_poll, x0, "poll", THREAD_IS_ARG0)
MARK_HOOK(an_phrase, x20, "phrase", NO_THREAD)
MARK_HOOK(an_embed, x20, "similarity", NO_THREAD)
MARK_HOOK(an_seek, x0, "seek table", NO_THREAD)  // inlined: the hook is the seek-table builder's call
#endif

// ---- audio of the track being analysed ------------------------------------------------------------

namespace rbm {

// Decodes the whole song (mono, native rate) with rekordbox's reader.
static std::vector<float> decode(const JStr* path, int* rate) {
    void* r = ::operator new(S_pcmReader);
    at<void (*)(void*, const JStr*, int, int, int, int, const void*)>(A_pcmReaderCtor)(r, path, 0, 0, 0xd55, 0, nullptr);
    int length = *(const int*)((const char*)r + O_readerLength);
    void* owner = (char*)r + O_readerOwner;
    int channels = at<int (*)(void*)>(A_readerChannels)(owner);
    *rate = at<int (*)(void*)>(A_readerRate)(owner);
    std::vector<float> mono;
    // Up to 20 minutes; longer mixes are cut (the key is a global estimate anyway).
    if (length > 0 && *rate > 0) length = std::min(length, *rate * 60 * 20);
    if (length > 0 && *rate > 0) {
        mono.resize(length);
        float L[512], R[512];
        for (int pos = 0; pos < length; pos += 512) {
            int n = std::min(512, length - pos);
            at<int (*)(void*, int, float*, float*, int)>(A_pcmGetStreamAt)(r, pos, L, R, 512);
            for (int i = 0; i < n; i++) mono[pos + i] = channels >= 2 ? 0.5f * (L[i] + R[i]) : L[i];
        }
    }
    deleteObject(r);
    return mono;
}

// Shared decodes (see analysisAudio): uri -> (decode, when it was asked for).
namespace {
struct Decoded { std::shared_ptr<const std::vector<float>> mono; int rate = 0; };
}  // namespace
static std::mutex g_audioLock;
static std::map<std::string, std::pair<std::shared_future<Decoded>, double>> g_audio;
static const double kAudioKept = 300;  // s

// Drops the audio nobody came to free (a cancelled analysis): g_audioLock held.
static void dropOldAudio(double t) {
    for (auto it = g_audio.begin(); it != g_audio.end();) it = t - it->second.second > kAudioKept ? g_audio.erase(it) : std::next(it);
}
// ...also when no analysis follows that would: a while after each one was put there (any thread).
static void dropOldAudioLater() {
    plat::onMainAfter(kAudioKept + 5, [] {
        std::lock_guard<std::mutex> l(g_audioLock);
        dropOldAudio(now());
    });
}

std::shared_ptr<const std::vector<float>> analysisAudio(const std::string& uri, int* rate) {
    if (!usable()) return nullptr;
    std::unique_lock<std::mutex> l(g_audioLock);
    double t = now();
    dropOldAudio(t);
    auto it = g_audio.find(uri);
    std::shared_future<Decoded> f;
    if (it != g_audio.end()) {
        f = it->second.first;
        l.unlock();
    } else {  // decode here, outside the lock; others asking for it wait on the future
        std::promise<Decoded> p;
        f = p.get_future().share();
        g_audio[uri] = {f, t};
        l.unlock();
        dropOldAudioLater();
        JStr path(uri.c_str());
        Decoded d;
        d.mono = std::make_shared<std::vector<float>>(decode(&path, &d.rate));
        p.set_value(d);
    }
    const Decoded& d = f.get();
    *rate = d.rate;
    return d.mono;
}
void analysisAudioDone(const std::string& uri) {
    std::lock_guard<std::mutex> l(g_audioLock);
    g_audio.erase(uri);
}

}  // namespace rbm

typedef bool (*PassAudioFn)(const void** reader, const int16_t** l, const int16_t** r, size_t* n);

// Analysis thread, at the beat job: the audio pass has read the whole track already (streamio keeps it for the phrase
// passes), so analysisAudio gets that instead of decoding the track again (~1 s of CPU, and Beat This!'s early grid
// comes that much sooner). The same 16-bit samples rekordbox analyses; the whole track, or nothing (decoded as before).
static void audioFromPass(const std::string& uri) {
    auto pass = service<PassAudioFn>("streamio.passAudio");
    if (!pass || !usable()) return;
    if (!provided("betterkey.prefetch") && !provided("bettergrid.prefetch")) return;  // nothing would use (and free) it
    {
        std::lock_guard<std::mutex> l(g_audioLock);
        if (g_audio.count(uri)) return;  // decoding (or decoded) already
    }
    const void* reader;
    const int16_t *L, *R;
    size_t n;
    bool check = cfgInt("betterkey_passcheck", 0) != 0;
    if (!pass(&reader, &L, &R, &n)) {
        if (check) logf("analysis: no pass audio for %s", uri.c_str());
        return;
    }
    int length = *(const int*)((const char*)reader + O_readerLength);
    void* owner = (char*)reader + O_readerOwner;
    int rate = at<int (*)(void*)>(A_readerRate)(owner);
    // (the audio pass may stop up to a block short of the end: that much is left silent)
    if (length <= 0 || rate <= 0 || n + rate / 2 < (size_t)length) {
        if (check) logf("analysis: pass audio of %s too short: %zu of %d frames at %d Hz", uri.c_str(), n, length, rate);
        return;
    }
    length = std::min(length, rate * 60 * 20);  // as decode()
    auto mono = std::make_shared<std::vector<float>>(length);
    for (int i = 0, m = (int)std::min(n, (size_t)length); i < m; i++) (*mono)[i] = (L[i] + R[i]) * (0.5f / 32768.0f);
    if (check) {  // dev check: the same as a decode?
        int r2 = 0;
        double t = now();
        JStr path(uri.c_str());
        std::vector<float> d = decode(&path, &r2);
        size_t m = std::min(d.size(), mono->size()), off = 0, clipped = 0;
        float worst = 0, peak = 0;
        for (size_t i = 0; i < m; i++) {
            float e = std::fabs(d[i] - (*mono)[i]);
            worst = std::max(worst, e), peak = std::max(peak, std::fabs(d[i]));
            bool atLimit = i < n && (L[i] >= 32767 || L[i] <= -32768 || R[i] >= 32767 || R[i] <= -32768);  // clipped to 16 bits
            if (e > 1e-3f) (atLimit ? clipped : off)++;
        }
        logf("analysis: pass audio %zu of %d frames at %d Hz, decode %zu at %d Hz (%.2f s): largest difference %.4f, decoded peak %.3f, "
             "%zu samples differ where 16 bits clipped, %zu elsewhere", n, length, rate, d.size(), r2, now() - t, worst, peak, clipped, off);
    }
    std::promise<Decoded> p;
    p.set_value(Decoded{mono, rate});
    {
        std::lock_guard<std::mutex> l(g_audioLock);
        dropOldAudio(now());
        g_audio.emplace(uri, std::make_pair(p.get_future().share(), now()));
    }
    dropOldAudioLater();
}

// ---- the beat job and the key job ---------------------------------------------------------------

// Runs on the analysis thread, before the key job (thread = the CBeatAnalyzeThread, from x20).
extern "C" uint64_t rbm_key_specialInfo(void* controller, int job, void* const* info, const char* thread) {
    auto orig = RBM_ORIG(key_specialInfo, uint64_t (*)(void*, int, void* const*));
    if (job == 2) {
        analysisMark(thread, "key");
        if (info && *info)
            if (auto f = service<void (*)(const void*, const std::string&, void*)>("betterkey.keyJob")) {
                std::string uri;
                if (analysisSong(thread, &uri)) f(thread, uri, *info);
                else logf("analysis: key job without a song source");
            }
    }
    return orig(controller, job, info);
}
#if defined(__aarch64__)
asm(R"(
    .text
    .p2align 2
    .globl _rbm_hook_key_specialInfo
_rbm_hook_key_specialInfo:
    mov  x3, x20
    b    _rbm_key_specialInfo
)");
#else
// Windows: no trampoline; the table's "hookreg key_specialInfo r15 3" line passes the thread (r15) as argument 3 of rbm_key_specialInfo.
#endif

// Beat job of the same analysis: logged so the log shows which tracks rekordbox really analyses.
extern "C" uint64_t rbm_beat_specialInfo(void* controller, int job, void* const* info, const char* thread) {
    std::string uri;
    bool known = analysisSong(thread, &uri);
    logf("analysis: rekordbox analyses %s", known ? uri.c_str() : "?");
    analysisMark(thread, "beat job");
    if (known) audioFromPass(uri);
    if (auto f = service<void (*)(const void*)>("bettergrid.prefetch")) f(thread);
    return RBM_ORIG(beat_specialInfo, uint64_t (*)(void*, int, void* const*))(controller, job, info);
}
#if defined(__aarch64__)
asm(R"(
    .text
    .p2align 2
    .globl _rbm_hook_beat_specialInfo
_rbm_hook_beat_specialInfo:
    mov  x3, x20
    b    _rbm_beat_specialInfo
)");
#else
// Windows: no trampoline; the table's "hookreg beat_specialInfo r15 3" line passes the thread (r15) as argument 3 of rbm_beat_specialInfo.
#endif
