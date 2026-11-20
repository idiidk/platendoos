// betterfx: better-sounding versions of rekordbox's mixer effects. Each part has its own switch; the DSP itself
// is in mods/betterfx_dsp.h and the reverse engineering behind it in docs/dsp/*.md.
//  - Color FX Filter (ColorFxFilterDjm900NXS2, the FLX4's filter knob): the LPF reaches its intended 25 kHz top
//    (rekordbox doesn't pre-warp its cutoff, so the LPF started at ~12 kHz with a +6 dB bump there), the knob
//    sweeps smoothly instead of in 256 steps, and full PARAMETER no longer peaks at +24 dB mid-sweep. Same knob
//    law, dead zone and default sound otherwise.
//  - EQ, DJM-900NXS2 type (rekordbox's default): same curves; knob moves are smoothed over a fixed 5 ms instead
//    of over one audio buffer (with small buffers a fast cut thumped), and a knob change is never dropped.
//  - Isolator: same crossovers and true kill; ~8 ms gain smoothing instead of a 3.3 ms ramp (bass kill clicks).
//  - Beat FX Reverb: rekordbox's 1960s-style network (8 combs, 2 allpasses: metallic ring, flutter) is replaced by
//    an 8-line feedback delay network with diffusion, slow modulation and frequency-dependent decay. Its decay
//    time and room size follow rekordbox's own (from the comb filters), its level follows rekordbox's wet level
//    (rekordbox's network still runs on a copy for that), so the knobs and the dry/wet mix behave as before.
//  - Echo / Pitch Echo: small delay-time changes (tempo fader moves) no longer dip the echo each time; they're
//    applied once the tempo has been steady for 250 ms. Beat changes are immediate, as before.
//  - Roll / Reverse Roll / Slip Roll: each repeat starts exactly on the beat with a short crossfade instead of a
//    6.7 ms fade-out/fade-in dip.
// How: filter, EQ and isolator: the effects' virtual processing methods are swapped in their vtables; each
// replacement reads the knob values from the rekordbox object and keeps its own state per object. Reverb, echo
// and roll: call hooks in hooks.def.
//   config: betterfx=0 to disable (restart)
//           betterfx_filter=1  the Color FX Filter (the FLX4 filter knob)
//           betterfx_eq=1  the DJM-900NXS2 EQ and the isolator
//           betterfx_reverb=1  the Beat FX Reverb (the new reverb sound)
//           betterfx_echo=1  Echo and Pitch Echo: no dips while the tempo changes
//           betterfx_roll=1  Roll, Reverse Roll and Slip Roll: no dip on every repeat
// macOS only for now (.mm): the Windows tables don't have these classes yet.
#include "../core/rbmod.h"
#include "betterfx_dsp.h"
#include <atomic>

using namespace rbm;

static volatile bool g_on = true;
static bool g_filterOn = true, g_eqOn = true, g_reverbOn = true, g_echoOn = true, g_rollOn = true;

namespace {  // file-local

// Per-object state, claimed by the object's address. Audio threads only; a slot is taken with a CAS.
// busy[i]: held while a slot's state is reset (and by the reverb while it runs), so two threads never reset or
// run the same state at once. The audio thread only ever tries it (no waiting): on a miss the object keeps
// rekordbox's DSP for that block.
template <class T, int N> struct Slots {
    std::atomic<void*> owner[N] = {};
    std::atomic<double> used[N] = {};  // when the owner was last processed
    std::atomic<bool> busy[N] = {};
    T state[N];
    bool lock(int i) {
        bool e = false;
        return busy[i].compare_exchange_strong(e, true, std::memory_order_acquire);
    }
    void unlock(int i) { busy[i].store(false, std::memory_order_release); }
    int index(const T* s) const { return (int)(s - state); }
    T* find(void* obj) {  // the object's state if it has one (claims nothing)
        for (int i = 0; i < N; i++)
            if (owner[i].load(std::memory_order_acquire) == obj) return &state[i];
        return nullptr;
    }
    T* get(void* obj, bool* fresh) {
        double t = now();
        for (int i = 0; i < N; i++)
            if (owner[i].load(std::memory_order_acquire) == obj) return used[i].store(t), *fresh = false, &state[i];
        for (int i = 0; i < N; i++) {
            void* expected = nullptr;
            if (owner[i].compare_exchange_strong(expected, obj)) {
                used[i].store(t);  // first: not idle, so nobody takes it from under us
                if (!lock(i)) return owner[i].store(nullptr), nullptr;  // being reset this instant: next block
                state[i].reclaim();
                unlock(i);
                *fresh = true;
                return &state[i];
            }
        }
        // Full: effects whose destructor isn't hooked never release theirs. Take the one idle longest (30 s at
        // least: its object is gone, or will start afresh like after any pause).
        int oldest = -1;
        for (int i = 0; i < N; i++)
            if (t - used[i].load() > 30 && (oldest < 0 || used[i].load() < used[oldest].load())) oldest = i;
        if (oldest >= 0 && lock(oldest)) {
            void* expected = owner[oldest].load();
            if (expected && t - used[oldest].load() > 30 && owner[oldest].compare_exchange_strong(expected, obj)) {
                used[oldest].store(t);
                state[oldest].reclaim();
                unlock(oldest);
                *fresh = true;
                return &state[oldest];
            }
            unlock(oldest);
        }
        return nullptr;  // all in use: the object keeps rekordbox's DSP
    }
    // From the object's destructor (not an audio thread: rekordbox no longer runs the object). Its state is reset
    // now, so what it holds (the reverb's buffer) goes back at once. Waits for a reset in progress (microseconds).
    void release(void* obj) {
        for (int i = 0; i < N; i++) {
            if (owner[i].load() != obj) continue;
            while (!lock(i)) {}
            if (owner[i].load() == obj) state[i].reclaim(), owner[i].store(nullptr, std::memory_order_release);
            unlock(i);
            return;
        }
    }
    // Main thread: f(state) for slots whose owner hasn't been processed for `idle` seconds (skipped while busy).
    template <class F> void idle(double secs, F f) {
        double t = now();
        for (int i = 0; i < N; i++) {
            if (!owner[i].load() || t - used[i].load() <= secs || !lock(i)) continue;
            if (owner[i].load() && t - used[i].load() > secs) f(state[i]);
            unlock(i);
        }
    }
};

struct AudioBufferF {  // juce::AudioBuffer<float>
    int numChannels, size;
    size_t allocatedBytes;
    float** channels;
};

}  // namespace

// ---- Color FX Filter -----------------------------------------------------------------------------------------

#define CFX_PROC "__ZN23ColorFxFilterDjm900NXS221processEffectConcreteEPfS0_i"
static const Addr A_cfxVt{"__ZTV23ColorFxFilterDjm900NXS2"};
static const Offset V_cfxProcess{0xd0, "__ZTV23ColorFxFilterDjm900NXS2", "vt:" CFX_PROC, 0};
static const Offset V_cfxD1{0x0, "__ZTV23ColorFxFilterDjm900NXS2", "vt:__ZN23ColorFxFilterDjm900NXS2D1Ev", 0};
static const Offset V_cfxD0{0x8, "__ZTV23ColorFxFilterDjm900NXS2", "vt:__ZN23ColorFxFilterDjm900NXS2D0Ev", 0};
static const Offset O_cfxKnob{0x260, CFX_PROC, "add x22", 0};    // LinearParameter: the filter knob, 0..1
static const Offset O_cfxParam{0x2e0, CFX_PROC, "add x22", 1};   // the PARAMETER knob (resonance), 0..1
static const Offset O_paramGet{0x40, CFX_PROC, "ldr x8", 0};     // LinearParameter vtable: get(float* out)
static const Offset O_cfxOn{0x3b4, CFX_PROC, "ldr x22", 14};     // bool: the effect is on
static const Offset O_fxRate{0xa0, "__ZN10EffectBase15setSamplingRateEi", "ldr x0", 0};  // EffectBase: int sample rate

typedef int (*CfxProcessFn)(void*, float*, float*, int);
typedef void (*DtorFn)(void*);
static CfxProcessFn o_cfxProcess;
static DtorFn o_cfxD1, o_cfxD0;
static Slots<bfx::CfxFilter, 32> g_cfx;

static float paramValue(void* obj, size_t off) {
    void* p = (char*)obj + off;
    float v = 0;
    (*reinterpret_cast<void (***)(void*, float*)>(p))[O_paramGet / 8](p, &v);
    return v;
}

static int cfxProcess(void* self, float* L, float* R, int n) {
    if (!g_on || !g_filterOn) return o_cfxProcess(self, L, R, n);
    bool fresh;
    bfx::CfxFilter* f = g_cfx.get(self, &fresh);
    if (!f) return o_cfxProcess(self, L, R, n);
    int rate = *(int*)((char*)self + O_fxRate);
    if (rate <= 0) return o_cfxProcess(self, L, R, n);
    f->process(L, R, n, paramValue(self, O_cfxKnob), paramValue(self, O_cfxParam), *(bool*)((char*)self + O_cfxOn), rate);
    return 0;
}
static void cfxD1(void* self) { g_cfx.release(self), o_cfxD1(self); }
static void cfxD0(void* self) { g_cfx.release(self), o_cfxD0(self); }

// ---- EQ (DJM-900NXS2) and isolator ---------------------------------------------------------------------------

#define NXS2_BLOCK "__ZN13EqualizerNXS212processBlockERN4juce11AudioBufferIfEEii"
#define NXS2_FLOATS "__ZN13EqualizerNXS212processBlockEPfS0_i"
static const Addr A_nxs2Vt{"__ZTV13EqualizerNXS2"};
static const Offset V_nxs2Block{0x10, "__ZTV13EqualizerNXS2", "vt:" NXS2_BLOCK, 0};
static const Offset V_nxs2Floats{0x18, "__ZTV13EqualizerNXS2", "vt:" NXS2_FLOATS, 0};
static const Offset O_nxs2Hi{0x20, "__ZN13EqualizerNXS212setParameterEjf", "str", 0};   // float knob 0..255
static const Offset O_nxs2Mid{0x2c, "__ZN13EqualizerNXS212setParameterEjf", "str", 4};
static const Offset O_nxs2Low{0x38, "__ZN13EqualizerNXS212setParameterEjf", "str", 2};
static const Offset O_nxs2Rate{0x8, "__ZN13EqualizerNXS210initializeEv", "ldr", 0};      // double sample rate
static const Offset O_nxs2FHi{0x48, "__ZN13EqualizerNXS210initializeEv", "ldr", 2};      // rekordbox's Filter* per band
static const Offset O_nxs2FMid{0x50, "__ZN13EqualizerNXS210initializeEv", "ldr", 7};
static const Offset O_nxs2FLow{0x58, "__ZN13EqualizerNXS210initializeEv", "ldr", 16};
static const Offset O_filterA2{0x60, "__ZN6Filter15make_eq_peakingEdd", "str", 1};        // Filter: b0 b1 a1 b2 a2 end here
static const Addr A_nxs2KnobTbl{"__ZN13EqualizerNXS213mp_s_vr_eqtblE"};    // int16_t*: knob -> table index
static const Addr A_nxs2QTbl{"__ZN13EqualizerNXS217mp_mid_LowQ_eqtblE"};   // float*: MID Q
static const Addr A_nxs2GainTbl{"__ZN13EqualizerNXS217mp_mid_Gain_eqtblE"}; // float*: MID dB

#define ISO_BLOCK "__ZN8Isolator12processBlockERN4juce11AudioBufferIfEEii"
#define ISO_FLOATS "__ZN8Isolator12processBlockEPfS0_i"
static const Addr A_isoVt{"__ZTV8Isolator"};
static const Offset V_isoBlock{0x10, "__ZTV8Isolator", "vt:" ISO_BLOCK, 0};
static const Offset V_isoFloats{0x18, "__ZTV8Isolator", "vt:" ISO_FLOATS, 0};
static const Offset O_isoHi{0x20, "__ZN8Isolator12setParameterEjf", "str", 0};   // int knob 0..255
static const Offset O_isoMid{0x24, "__ZN8Isolator12setParameterEjf", "str", 2};
static const Offset O_isoLow{0x28, "__ZN8Isolator12setParameterEjf", "str", 1};
static const Offset O_isoRate{0x8, "__ZN8Isolator10initializeEv", "ldr", 1};
static const Addr A_isoCurve{"__ZN8Isolator9mpCurveAdE"};  // float*: knob -> linear gain (256)

typedef int (*BlockFn)(void*, AudioBufferF*, int, int);
typedef int (*FloatsFn)(void*, float*, float*, int);
static BlockFn o_nxs2Block, o_isoBlock;
static FloatsFn o_nxs2Floats, o_isoFloats;
static Slots<bfx::Nxs2Eq, 32> g_nxs2;
static Slots<bfx::Isolator, 32> g_iso;
static std::atomic<int> g_checks{0};
static char g_checkMsg[320];
static std::atomic<bool> g_checkReady{false};

// Once: our coefficients for rekordbox's current (resting) knobs against the ones rekordbox's own filters hold.
static void selfCheck(void* self, bfx::Nxs2Eq* eq, const float t[3]) {
    if (g_checks.fetch_add(1) != 0) return;
    const size_t fOff[3] = {O_nxs2FHi, O_nxs2FMid, O_nxs2FLow};
    double worst = 0;
    for (int b = 0; b < 3; b++) {
        const double* rc = (const double*)(*(char**)((char*)self + fOff[b]) + O_filterA2 - 0x20);  // b0 b1 a1 b2 a2
        bfx::Coefs c = eq->design(b, b == 1 ? round(t[b]) : t[b]);
        double mine[5] = {c.b0, c.b1, c.a1, c.b2, c.a2};
        for (int i = 0; i < 5; i++) worst = std::max(worst, fabs(mine[i] - rc[i]));
    }
    // No logging on the audio thread: format into a static buffer, the main thread's tick writes it out.
    snprintf(g_checkMsg, sizeof g_checkMsg,
             "betterfx: EQ self-check at knobs %.1f/%.1f/%.1f, %.0f Hz: largest coefficient difference from rekordbox's %.2g%s",
             t[0], t[1], t[2], eq->fs, worst, worst < 1e-6 ? " (same filters)" : " (MISMATCH: check the curves)");
    g_checkReady.store(true, std::memory_order_release);
}

static int nxs2Run(void* self, float* const* ch, int nch, int start, int n) {
    bool fresh;
    bfx::Nxs2Eq* eq = g_nxs2.get(self, &fresh);
    if (!eq) return -1;
    double rate = *(double*)((char*)self + O_nxs2Rate);
    if (rate <= 0) return -1;
    if (!eq->knobTable) {
        eq->knobTable = *(const int16_t* const*)at<char*>(A_nxs2KnobTbl);
        eq->qTable = *(const float* const*)at<char*>(A_nxs2QTbl);
        eq->gainTable = *(const float* const*)at<char*>(A_nxs2GainTbl);
        if (!eq->knobTable || !eq->qTable || !eq->gainTable) return -1;
    }
    float t[3] = {*(float*)((char*)self + O_nxs2Hi), *(float*)((char*)self + O_nxs2Mid), *(float*)((char*)self + O_nxs2Low)};
    if (fresh) {
        eq->reset(rate);
        selfCheck(self, eq, t);
    }
    eq->process(ch, nch, start, n, t, rate);
    return 0;
}
static int nxs2Block(void* self, AudioBufferF* buf, int start, int n) {
    if (g_on && g_eqOn && n > 0 && buf && buf->channels && nxs2Run(self, buf->channels, buf->numChannels, start, n) == 0) return 0;
    return o_nxs2Block(self, buf, start, n);
}
static int nxs2Floats(void* self, float* L, float* R, int n) {
    float* ch[2] = {L, R};
    if (g_on && g_eqOn && n > 0 && L && R && nxs2Run(self, ch, 2, 0, n) == 0) return 0;
    return o_nxs2Floats(self, L, R, n);
}

static int isoRun(void* self, float* const* ch, int nch, int start, int n) {
    bool fresh;
    bfx::Isolator* iso = g_iso.get(self, &fresh);
    const float* curve = *(const float* const*)at<char*>(A_isoCurve);
    double rate = *(double*)((char*)self + O_isoRate);
    if (!iso || !curve || rate <= 0) return -1;
    auto gain = [&](size_t off) { return (double)curve[std::clamp(*(int*)((char*)self + off), 0, 255)]; };
    double t[3] = {gain(O_isoHi), gain(O_isoMid), gain(O_isoLow)};
    if (fresh) iso->reset(rate);
    iso->process(ch, nch, start, n, t, rate);
    return 0;
}
static int isoBlock(void* self, AudioBufferF* buf, int start, int n) {
    if (g_on && g_eqOn && n > 0 && buf && buf->channels && isoRun(self, buf->channels, buf->numChannels, start, n) == 0) return 0;
    return o_isoBlock(self, buf, start, n);
}
static int isoFloats(void* self, float* L, float* R, int n) {
    float* ch[2] = {L, R};
    if (g_on && g_eqOn && n > 0 && L && R && isoRun(self, ch, 2, 0, n) == 0) return 0;
    return o_isoFloats(self, L, R, n);
}

// ---- Reverb --------------------------------------------------------------------------------------------------

static const Offset O_revComb0{0xd8, "__ZN10ReverbCore28updateAndSetDynamicParameterEb", "ldr", 2};  // ReverbCombFilter* (first)
static const Offset O_revAp1{0x128, "__ZN10ReverbCore22handleReverbProcessingEPfS0_i", "ldr", 7};     // ReverbAllPassFilter*
static const Offset O_revAp2{0x130, "__ZN10ReverbCore22handleReverbProcessingEPfS0_i", "ldr", 8};
static const Offset O_revRate{0x8, "__ZN10ReverbCore21onSamplingRateChangedEi", "ldr", 0};  // float sample rate
static const Addr A_combDelay{"__ZNK16ReverbCombFilter12getDelayTimeENS_9ChannelIdE"};  // ms
static const Addr A_combGain{"__ZNK16ReverbCombFilter7getGainENS_9ChannelIdE"};         // feedback gain
// The first comb's left delay at room size 50 (SingleBeatFxReverb::getDynamicParameter's table), for the room scale.
static const double kComb0Ms = 22.09375;

typedef void (*ReverbFn)(void*, float*, float*, int);

// The FDN's level against rekordbox's network (5 summed combs) at the same input: measured 7..8 on music and
// single hits at 2 s and 6 s decay (tools/dev/reverb_render.cpp). The level matching below only trims around it.
static const double kReverbGain = 7.5;

static const int kScratch = 8192;

namespace {  // file-local
// No thread_local here: a dylib's thread-locals are malloc'ed and set up on a thread's first use of any of them,
// which for these would be inside the audio callback. Scratch and the allpass skip are per object instead (an
// object is only ever processed by one thread at a time).
struct ReverbState {
    bfx::Fdn fdn;
    double eRef = 0, eMine = 0, gain = kReverbGain, lastCall = 0;
    bool cleared = false;
    std::atomic<void*> skip[2] = {};    // the allpasses handleReverbProcessing calls next: already done on the copy
    float refL[kScratch], refR[kScratch];  // the copy rekordbox's network runs on (touched in init)
    void reclaim() {
        fdn.reclaim();
        eRef = eMine = 0, gain = kReverbGain, lastCall = 0, cleared = false;
        skip[0].store(nullptr), skip[1].store(nullptr);
    }
};
// The slot's busy flag, held while the reverb runs (tick may drop an idle slot's buffer meanwhile otherwise).
struct SlotHold {
    Slots<ReverbState, 16>& s;
    int i;
    ~SlotHold() { s.unlock(i); }
};
}  // namespace
static Slots<ReverbState, 16> g_reverb;

extern "C" void rbm_hook_bfx_reverbCombs(void* core, float* L, float* R, int n) {
    auto orig = RBM_ORIG(bfx_reverbCombs, ReverbFn);
    bool fresh;
    ReverbState* st = (g_on && g_reverbOn && n > 0 && n <= kScratch) ? g_reverb.get(core, &fresh) : nullptr;
    if (!st || !g_reverb.lock(g_reverb.index(st))) return orig(core, L, R, n);
    SlotHold hold{g_reverb, g_reverb.index(st)};
    st->skip[0].store(nullptr), st->skip[1].store(nullptr);
    if (!st->fdn.alloc()) return orig(core, L, R, n);
    void* comb0 = *(void**)((char*)core + O_revComb0);
    double d = at<float (*)(void*, int)>(A_combDelay)(comb0, 0), g = at<float (*)(void*, int)>(A_combGain)(comb0, 0);
    if (!(d > 0 && g > 0 && g < 1)) return orig(core, L, R, n);
    double t60 = -6.9078 * (d * 0.001) / log(g), room = d / kComb0Ms;
    double fs = *(float*)((char*)core + O_revRate);
    if (!(fs >= 22050 && fs <= 192000)) return orig(core, L, R, n);
    // rekordbox's network on a copy: the level the new one is matched to (and its state stays as it would be)
    float *refL = st->refL, *refR = st->refR;
    memcpy(refL, L, n * sizeof(float)), memcpy(refR, R, n * sizeof(float));
    orig(core, refL, refR, n);
    auto ap = RBM_ORIG(bfx_reverbAp1, ReverbFn);
    void *ap1 = *(void**)((char*)core + O_revAp1), *ap2 = *(void**)((char*)core + O_revAp2);
    ap(ap1, refL, refR, n);
    ap(ap2, refL, refR, n);
    st->skip[0].store(ap1), st->skip[1].store(ap2);  // the two allpass calls that follow in handleReverbProcessing: done above
    // A gap (the effect was off) or a cleared rekordbox reverb: start the tail from silence too.
    double t = now();
    bool silent = true;
    for (int i = 0; i < n && silent; i++) silent = refL[i] == 0 && refR[i] == 0 && L[i] == 0 && R[i] == 0;
    double blk = n / fs;
    if (((st->lastCall > 0 && t - st->lastCall > std::max(0.25, 4 * blk)) || silent) && !st->cleared) {
        st->fdn.clear();
        st->eRef = st->eMine = 0, st->gain = kReverbGain;
    }
    st->cleared = silent;
    st->lastCall = t;
    st->fdn.process(L, R, n, t60, room, fs);
    // Level: starts at the measured ratio and follows rekordbox's wet energy slowly (1.5 s), within +-6 dB of it:
    // a fast or unbounded follower pumps with the music and starts far off.
    double er = 0, em = 0;
    for (int i = 0; i < n; i++) er += refL[i] * refL[i] + refR[i] * refR[i], em += L[i] * L[i] + R[i] * R[i];
    double a = 1 - exp(-blk / 1.5);
    st->eRef += (er - st->eRef) * a, st->eMine += (em - st->eMine) * a;
    double target = st->gain;
    if (st->eMine > 1e-9 && st->eRef > 1e-9) target = std::clamp(sqrt(st->eRef / st->eMine), kReverbGain * 0.5, kReverbGain * 2);
    double g0 = st->gain, step = (target - g0) * a / n;
    for (int i = 0; i < n; i++) {
        double gi = g0 + step * (i + 1);
        L[i] = (float)(L[i] * gi), R[i] = (float)(R[i] * gi);
    }
    st->gain = g0 + step * n;
}
// ReverbCore's destructor: its slot and FDN buffer are free again. Every owner (BeatFxReverb, MultiBeatFxReverb,
// FxPlusReverbUp/Down) news its cores and deletes them through the vtable, so swapping these catches each one.
static const Addr A_revVt{"__ZTV10ReverbCore"};
static const Offset V_revD1{0x0, "__ZTV10ReverbCore", "vt:__ZN10ReverbCoreD1Ev", 0};
static const Offset V_revD0{0x8, "__ZTV10ReverbCore", "vt:__ZN10ReverbCoreD0Ev", 0};
static DtorFn o_revD1, o_revD0;
static void revD1(void* self) { g_reverb.release(self), o_revD1(self); }
static void revD0(void* self) { g_reverb.release(self), o_revD0(self); }

// An allpass the combs hook already ran on the copy (its pointer waits in its reverb's slot): skipped once.
static bool skipAllpass(void* ap) {
    for (auto& s : g_reverb.state)
        for (auto& k : s.skip) {
            void* e = ap;
            if (ap && k.load(std::memory_order_relaxed) == ap && k.compare_exchange_strong(e, nullptr)) return true;
        }
    return false;
}
extern "C" void rbm_hook_bfx_reverbAp1(void* ap, float* L, float* R, int n) {
    if (skipAllpass(ap)) return;
    RBM_ORIG(bfx_reverbAp1, ReverbFn)(ap, L, R, n);
}
extern "C" void rbm_hook_bfx_reverbAp2(void* ap, float* L, float* R, int n) {
    if (skipAllpass(ap)) return;
    RBM_ORIG(bfx_reverbAp2, ReverbFn)(ap, L, R, n);
}

// ---- Echo: delay-time changes --------------------------------------------------------------------------------
// rekordbox closes a 3.3 ms gate, jumps the delay and opens it again on any change of the delay time, and the dip
// goes round the feedback loop. While the tempo fader moves that is a stream of dips. Small changes (under 3%)
// now wait until the requested time has been steady for 250 ms, then apply once; beat-length changes are as before.

static const Offset O_dmCur{0x30, "__ZN11DelayModule12setDelayTimeEfb", "movz", 1};      // float ms in use
static const Offset O_dmPending{0x38, "__ZN11DelayModule12setDelayTimeEfb", "ldr", 1};   // bool: a change waits for the gate
static const Offset O_dmPendMs{0x3c, "__ZN11DelayModule12setDelayTimeEfb", "movz", 0};   // float ms waiting

typedef void (*SetDelayFn)(void*, float, bool);
namespace {  // file-local
struct Deferred { std::atomic<void*> dm{nullptr}; float want = 0; double since = 0, seen = 0; };
}  // namespace
static Deferred g_deferred[64];

static void setDelay(SetDelayFn orig, void* dm, float ms, bool smooth) {
    if (!g_on || !g_echoOn || !smooth) return orig(dm, ms, smooth);
    const char* p = (const char*)dm;
    float cur = *(const bool*)(p + O_dmPending) ? *(const float*)(p + O_dmPendMs) : *(const float*)(p + O_dmCur);
    Deferred* d = nullptr;
    for (auto& e : g_deferred)
        if (e.dm.load(std::memory_order_relaxed) == dm) { d = &e; break; }
    if (cur <= 0 || fabsf(ms - cur) > 0.03f * cur || ms == cur) {
        if (d) d->dm.store(nullptr);
        return orig(dm, ms, smooth);
    }
    double t = now();
    if (!d) {
        for (auto& e : g_deferred) {
            void* expected = e.dm.load();
            if (expected && t - e.seen < 10) continue;  // in use (an entry not called for 10 s: its effect is off or gone)
            if (e.dm.compare_exchange_strong(expected, dm)) { d = &e, d->want = -1; break; }
        }
        if (!d) return orig(dm, ms, smooth);
    }
    d->seen = t;
    if (ms != d->want) d->want = ms, d->since = t;
    if (t - d->since >= 0.25) {
        d->dm.store(nullptr);
        orig(dm, ms, smooth);
    }
}
extern "C" void rbm_hook_bfx_echoDelay1(void* dm, float ms, bool smooth) { setDelay(RBM_ORIG(bfx_echoDelay1, SetDelayFn), dm, ms, smooth); }
extern "C" void rbm_hook_bfx_echoDelay2(void* dm, float ms, bool smooth) { setDelay(RBM_ORIG(bfx_echoDelay2, SetDelayFn), dm, ms, smooth); }
extern "C" void rbm_hook_bfx_pitchEchoDelay1(void* dm, float ms, bool smooth) { setDelay(RBM_ORIG(bfx_pitchEchoDelay1, SetDelayFn), dm, ms, smooth); }
extern "C" void rbm_hook_bfx_pitchEchoDelay2(void* dm, float ms, bool smooth) { setDelay(RBM_ORIG(bfx_pitchEchoDelay2, SetDelayFn), dm, ms, smooth); }

// ---- Roll: loop wrap -----------------------------------------------------------------------------------------
// rekordbox fades the loop out for 3.3 ms while reading past its end, jumps, and fades the start in: a ~6.7 ms dip
// on every repeat, and each repeat starts ~3.3 ms late. Now the reader jumps exactly at the loop end (on the beat),
// the loop start comes in over 0.7 ms (attack kept) and the audio that continues past the loop end fades out over
// the same 3.3 ms underneath it (the audio rekordbox's own fade-out reads): no dip, no click.

#define ROLL_READ "__ZN16RollBufferReader4readEPfS0_i"
static const Offset O_rollGate{0x38, ROLL_READ, "add", 1};      // SignalGate
static const Offset O_rollRing{0x30, ROLL_READ, "ldr", 4};      // RingBuffer*
static const Offset O_rollPos{0x98, ROLL_READ, "add", 3};       // int: read position
static const Offset O_rollStart{0xa0, ROLL_READ, "add", 4};     // int: loop start (then 8 more bytes of loop parameters)
static const Offset O_rollUp{0xa8, ROLL_READ, "ldr", 3};        // bool: positions go up (0: the ring is read backwards)
static const Offset O_rollPending{0xac, ROLL_READ, "ldr", 17};  // bool: new loop parameters wait at +0xb0
static const Offset O_rollNew{0xb0, ROLL_READ, "add", 5};       // the waiting loop parameters (12 bytes)
static const Offset O_rollFlag{0xc4, ROLL_READ, "str", 7};      // cleared when the gate opens
static const Offset O_ringL{0x18, "__ZN10RingBuffer5clearEv", "ldr", 1};  // float* left; the right one follows it
// RingBuffer's first field is its length (where reads wrap).

typedef void (*RollReadFn)(void*, float*, float*, int);
typedef void (*GateFn)(void*);

namespace {  // file-local
struct RollWrap { int at; int tailPos; bool down; int loopLen; };
struct RollCall {
    void* reader = nullptr;
    int pos0 = 0, base = 0, n = 0, count = 0;
    bool down = false;
    RollWrap wraps[16];
};
struct RollState {  // a crossfade that runs on into the next call
    int tailPos = 0, done = 0, len = 0, headLen = 0;
    bool down = false;
    int lastEndPos = -1;
    RollCall call;  // the read in progress, for the wrap hook (per reader, not thread_local: see ReverbState)
    void reclaim() { *this = RollState(); }
};
}  // namespace
static Slots<RollState, 32> g_rollState;
static const double kRollFs = 44100;  // crossfade lengths in samples (48 kHz: 8% shorter, inaudible)

static inline int ringLen(void* ring) { return *(int*)ring; }
static inline int wrapPos(int pos, int len) {
    pos %= len;
    return pos < 0 ? pos + len : pos;
}
static inline float ringAt(void* ring, int ch, int pos) {
    float* b = *(float**)((char*)ring + O_ringL + 8 * ch);
    return b[wrapPos(pos, ringLen(ring))];
}
// samples read going from `from` to `to`
static inline int distance(int from, int to, int len, bool down) { return wrapPos(down ? from - to : to - from, len); }

// From crossfade sample x.done on: out = head * fadeIn + tail * fadeOut, over out[k..end).
static void applyXfade(void* ring, float* L, float* R, int k, int end, RollState& x) {
    for (; k < end && x.done < x.len; k++, x.done++) {
        double th = std::min(1.0, (x.done + 0.5) / x.headLen), tt = (x.done + 0.5) / x.len;
        double wh = sin(M_PI / 2 * th), wt = cos(M_PI / 2 * tt);
        wh *= wh, wt *= wt;
        int p = x.down ? x.tailPos - x.done : x.tailPos + x.done;
        L[k] = (float)(L[k] * wh + ringAt(ring, 0, p) * wt);
        R[k] = (float)(R[k] * wh + ringAt(ring, 1, p) * wt);
    }
}

static void rollRead(RollReadFn orig, void* reader, float* L, float* R, int n) {
    if (!g_on || !g_rollOn || !L || !R || n <= 0) return orig(reader, L, R, n);
    bool fresh;
    RollState* st = g_rollState.get(reader, &fresh);
    char* r = (char*)reader;
    void* ring = *(void**)(r + O_rollRing);
    if (!st || !ring || ringLen(ring) <= 0) return orig(reader, L, R, n);
    int len = ringLen(ring);
    int pos0 = *(int*)(r + O_rollPos);
    if (st->lastEndPos != pos0) st->done = st->len;  // the reader was moved meanwhile: drop a leftover crossfade
    st->call = RollCall();
    st->call.reader = reader, st->call.pos0 = pos0, st->call.n = n, st->call.down = !*(bool*)(r + O_rollUp);
    orig(reader, L, R, n);
    RollCall c = st->call;
    st->call.reader = nullptr;
    int endPos = *(int*)(r + O_rollPos);
    st->lastEndPos = endPos;
    // Only if the reader went where the wraps say (no other jump, e.g. rekordbox's own gate cycle): else no crossfades.
    if (c.n - c.base != distance(c.pos0, endPos, len, c.down)) {
        st->done = st->len;
        return;
    }
    if (st->done < st->len) applyXfade(ring, L, R, 0, c.count ? c.wraps[0].at : n, *st);
    for (int w = 0; w < c.count; w++) {
        const RollWrap& wr = c.wraps[w];
        st->tailPos = wr.tailPos, st->down = wr.down, st->done = 0;
        st->len = std::max(8, std::min((int)(0.0033 * kRollFs), wr.loopLen / 2));
        st->headLen = std::max(4, std::min((int)(0.0007 * kRollFs), st->len / 2));
        applyXfade(ring, L, R, wr.at, w + 1 < c.count ? c.wraps[w + 1].at : n, *st);
    }
}

// The loop end: rekordbox would close the gate (fade out while reading on, then jump). Jump now, gate stays open.
extern "C" void rbm_hook_bfx_rollWrap(void* gate) {
    char* r = (char*)gate - O_rollGate;
    void* ring = *(void**)(r + O_rollRing);
    RollState* st = g_on && g_rollOn ? g_rollState.find(r) : nullptr;
    if (!st || st->call.reader != r || st->call.count >= 16 || !ring || ringLen(ring) <= 0)
        return RBM_ORIG(bfx_rollWrap, GateFn)(gate);
    RollCall& call = st->call;
    int len = ringLen(ring);
    int tail = *(int*)(r + O_rollPos), start = *(int*)(r + O_rollStart);
    int at = call.base + distance(call.pos0, tail, len, call.down);
    if (at > call.n) return RBM_ORIG(bfx_rollWrap, GateFn)(gate);  // lost track: rekordbox's way
    RollWrap& w = call.wraps[call.count++];
    w.at = at, w.tailPos = tail, w.down = call.down;
    w.loopLen = std::max(1, distance(start, tail, len, call.down));
    // what rekordbox does once the gate has closed: jump to the loop start, take waiting loop parameters, open
    *(int*)(r + O_rollPos) = start;
    if (*(bool*)(r + O_rollPending)) {
        *(bool*)(r + O_rollPending) = false;
        memcpy(r + O_rollStart, r + O_rollNew, 12);
    }
    *(bool*)(r + O_rollFlag) = false;
    call.pos0 = start, call.base = at, call.down = !*(bool*)(r + O_rollUp);
}
#define ROLL_HOOK(name) \
    extern "C" void rbm_hook_##name(void* reader, float* L, float* R, int n) { rollRead(RBM_ORIG(name, RollReadFn), reader, L, R, n); }
ROLL_HOOK(bfx_rollRead1)
ROLL_HOOK(bfx_rollRead2)
ROLL_HOOK(bfx_rollRead3)
ROLL_HOOK(bfx_rollRead4)
ROLL_HOOK(bfx_rollRead5)
ROLL_HOOK(bfx_rollRead6)

// ---- install -------------------------------------------------------------------------------------------------

template <class F> static bool swap(const Addr& vt, const Offset& slot, void* mine, F* orig) {
    void** p = (void**)(at<char*>(vt) + 0x10 + slot);
    void* old = patchSlot(p, mine);
    if (!old) return false;
    *orig = (F)old;
    return true;
}

static void tick() {  // main thread, every 0.1 s
    if (g_checkReady.exchange(false, std::memory_order_acquire)) logf("%s", g_checkMsg);
    // A reverb not run for 30 s (its effect off; rekordbox keeps the objects) gives its buffer back: the pool has
    // fewer buffers than there are reverb objects. If it runs again it takes a clean one and starts from silence,
    // as it would after any pause.
    if (g_reverbOn) g_reverb.idle(30, [](ReverbState& s) { if (s.fdn.buf) s.reclaim(); });
    bfx::Fdn::scrubPool();
}

static void init() {
    g_filterOn = cfgInt("betterfx_filter", 1) != 0;
    g_eqOn = cfgInt("betterfx_eq", 1) != 0;
    g_reverbOn = cfgInt("betterfx_reverb", 1) != 0;
    g_echoOn = cfgInt("betterfx_echo", 1) != 0;
    g_rollOn = cfgInt("betterfx_roll", 1) != 0;
    std::string done;
    // Originals first, so a replacement can always fall back.
    if (g_filterOn) {
        if (swap(A_cfxVt, V_cfxD1, (void*)&cfxD1, &o_cfxD1) && swap(A_cfxVt, V_cfxD0, (void*)&cfxD0, &o_cfxD0) &&
            swap(A_cfxVt, V_cfxProcess, (void*)&cfxProcess, &o_cfxProcess))
            done += " filter";
        else
            logf("betterfx: could not patch the Color FX Filter"), g_filterOn = false;
    }
    if (g_eqOn) {
        bool ok = swap(A_nxs2Vt, V_nxs2Floats, (void*)&nxs2Floats, &o_nxs2Floats) &&
                  swap(A_nxs2Vt, V_nxs2Block, (void*)&nxs2Block, &o_nxs2Block) &&
                  swap(A_isoVt, V_isoFloats, (void*)&isoFloats, &o_isoFloats) &&
                  swap(A_isoVt, V_isoBlock, (void*)&isoBlock, &o_isoBlock);
        if (ok) done += " eq isolator";
        else logf("betterfx: could not patch the EQ"), g_eqOn = false;
    }
    // the call hooks (hooks.def): only where the patcher found every call site in this build
    g_reverbOn = g_reverbOn && hookPatched(HK_bfx_reverbCombs) && hookPatched(HK_bfx_reverbAp1) && hookPatched(HK_bfx_reverbAp2);
    g_echoOn = g_echoOn && hookPatched(HK_bfx_echoDelay1) && hookPatched(HK_bfx_echoDelay2);
    g_rollOn = g_rollOn && hookPatched(HK_bfx_rollWrap) && hookPatched(HK_bfx_rollRead1) && hookPatched(HK_bfx_rollRead2);
    if (g_reverbOn) {
        bfx::Fdn::prepare();  // buffers made here, never on the audio thread
        for (auto& s : g_reverb.state) memset(s.refL, 0, sizeof s.refL), memset(s.refR, 0, sizeof s.refR);  // pages touched now too
        if (!swap(A_revVt, V_revD1, (void*)&revD1, &o_revD1) || !swap(A_revVt, V_revD0, (void*)&revD0, &o_revD0))
            logf("betterfx: ReverbCore's destructor not found: reverb buffers come back only after 30 s idle");
        done += " reverb";
    }
    if (g_echoOn) done += " echo";
    if (g_rollOn) done += " roll";
    logf("betterfx: on:%s", done.empty() ? " nothing" : done.c_str());
}

RBM_MOD({.name = "betterfx", .init = init, .tick = tick, .enabled = &g_on});
