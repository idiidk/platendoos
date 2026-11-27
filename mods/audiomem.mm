// audiomem: keeps the memory rekordbox's audio thread works with in RAM, so playback doesn't click when the Mac
// runs short of memory. rekordbox's audio callback has 2.9 ms per block (buffer size 128) and reads and writes
// ordinary heap memory. When other apps (or a deck load, which allocates hundreds of MB) push the Mac into
// compressing and swapping memory, macOS takes pages that weren't touched for a few seconds, and the audio thread
// has to wait for each one to come back: 1 to 10 ms for a page, a missed block, 2.9 ms (or more) of silence on
// every deck. Measured on a 16 GB Mac with 8 GB in swap: up to 23 such gaps a minute during plain playback, 2 to
// 10 at every deck load; nine in ten came from the first kind of memory below.
//  - FX history buffers (audiomem_fx): rekordbox keeps 3 MB of the last seconds of audio per channel and FX slot
//    (RingBuffer, 218 of them), and writes about half of them all the time whether an FX is on or not, each 16 KB
//    page once per lap of 9 s. Every buffer that is being written is pinned (mlock): macOS can't take it away any
//    more. They are memory rekordbox touches constantly anyway, so pinning costs nothing while there is memory to
//    spare.
//  - The decks' audio (audiomem_decks): a loaded track sits decoded in memory (PageBuffer), filled at the load and
//    read when the deck gets there, minutes later. A background thread looks at every loaded deck's position four
//    times a second and brings back what the next audiomem_ahead seconds need (4 s on a deck that isn't playing,
//    so pressing play is clean too). A jump further than that (hot cue, needle search) can still meet a missing
//    page.
//  - rekordbox's own code and data (audiomem_code, 90 MB): the data is pinned and the code read through every few
//    seconds (macOS doesn't let code be pinned), so a part of the program the audio thread only runs now and then
//    (at a load) isn't read back from disk at that moment.
//  - A deck load (audiomem_lazy): rekordbox builds the new track's buffers on the main thread and zeroes all of
//    them at once (8 bytes per frame: 127 MB for six minutes, per stem too, and twice per load), which freezes the
//    UI for 0.1 to 0.3 s and makes the Mac find all that memory in one go. With this the buffers come from the
//    system already zero (calloc) and each page is first touched when its audio is written, off the main thread.
// What it can't do: memory the audio thread touches rarely and that isn't in the list (small objects all over the
// heap) can still be away when it is needed. On a Mac that swaps heavily a larger buffer size in rekordbox's audio
// settings (256 or 512) is the wider margin: most of the waits are shorter than 5 ms.
// The log says what was pinned and, once a minute while it happens, how much deck audio had to be brought back.
//   config: audiomem=0 to disable (restart)
//           audiomem_fx=0 | audiomem_decks=0 | audiomem_code=0 | audiomem_lazy=0   one part off (restart)
//           audiomem_ahead=20      seconds of audio kept ready ahead of a playing deck
// Test commands (command-audiomem in the config folder): "state" logs the counts; "fx 0", "decks 0", "lazy 0",
// "code 0" (and 1) switch one part while rekordbox runs.
// macOS only for now (.mm): mlock/mincore and rekordbox's mac allocation sites; Windows pages memory out too
// (working-set trimming), the same idea there would need VirtualLock and the Windows call sites.
#import <Foundation/Foundation.h>
#include "../core/rbmod.h"
#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>
#include <dlfcn.h>
#include <mach-o/dyld.h>
#include <mach-o/loader.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/mman.h>
#include <sys/sysctl.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <unistd.h>

using namespace rbm;

// Its settings (core/settings.h): what the settings window and the config file's list show of them.
namespace opt {
const Setting decks{"audiomem_decks", true, {.label = "Keep a playing deck's audio ready", .restart = true}};
const Setting ahead{"audiomem_ahead", 20, {.label = "Seconds kept ready ahead of the playhead", .unit = "s", .min = 2, .max = 120, .step = 1, .restart = true, .needs = "audiomem_decks"}};
const Setting fx{"audiomem_fx", true, {.label = "Keep the FX buffers in memory", .restart = true}};
const Setting code{"audiomem_code", true, {.label = "Keep rekordbox's own code in memory", .restart = true}};
const Setting lazy{"audiomem_lazy", true, {.label = "Prepare a deck's buffers off the main thread", .restart = true, .advanced = true}};
}  // namespace opt

static volatile bool g_on = true;

static const Addr A_getPlayingTime{"__ZNK8djengine10DjEngineIF14getPlayingTimeENS_15EnPlayerChannelE"};  // ms
// RingBuffer: its buffer (both channels in one block), and where it writes next (one int per direction).
static const Offset O_ringBuf{0x18, "__ZN10RingBuffer5clearEv", "ldr", 1};
static const Offset O_ringWritePos{0x28, "__ZN10RingBuffer17setWriteSubscriptEiNS_9DirectionE", "str", 0};

namespace {

// The VM page size: 16 KB on Apple Silicon, 4 KB on Intel (and under Rosetta). mincore() reports one byte per page.
const size_t kPage = (size_t)getpagesize();
// The most pages keepDecks looks at in one chunk at a time: 8 MB of audio (24 s at 44.1 kHz) in either half.
#if defined(__x86_64__)
constexpr size_t kMaxPagesAtOnce = 2048;
#else
constexpr size_t kMaxPagesAtOnce = 512;
#endif

enum RingState { kNew, kPinning, kPinned };
// obj: the RingBuffer (null if it couldn't be told), maker: an address in the function that made it, pos: where it
// wrote when last looked at.
struct Ring { size_t bytes; RingState state; void* obj; void* maker; bool seen; int pos[2]; };
// One deck buffer: its chunks in track order, each chunkBytes long (8 bytes per frame: a Float4 is two stereo frames
// of two floats, in both halves; on x86_64 only rekordbox's Float2, which read() copies them out to, is 16 bytes).
struct Buf { uint64_t id; size_t chunkBytes; int alive; std::vector<char*> chunks; };
struct State {
    // FX buffers. Held for a map operation only, never across a system call: one FX (Helix) reallocates its
    // buffer from the audio thread, so the hooks below can run there.
    std::mutex ringLock;
    std::unordered_map<void*, Ring> rings;
    std::vector<void*> ringsNew;
    // Deck buffers (built and freed on the main thread and rekordbox's deleter thread, never the audio thread).
    // The keeper holds it while it reads one page, which can take milliseconds.
    std::mutex lock;
    std::vector<Buf*> bufs;
    std::unordered_map<void*, std::pair<Buf*, int>> chunkOf;  // chunk -> its buffer and index
    uint64_t nextId = 1;
};
State& st() { static State* s = new State; return *s; }  // never destroyed: hooks run until the process is gone

thread_local Buf* t_building;      // the buffer whose constructor runs on this thread
thread_local void* t_fresh;        // its chunk that came from calloc: needs no bzero

std::atomic<bool> g_fx{false}, g_decks{false}, g_lazy{false}, g_readCode{true};
std::atomic<int> g_ahead{20};
struct Deck { std::atomic<bool> loaded{false}, playing{false}; std::atomic<int> ms{0}; };
Deck g_deck[8];
std::atomic<long> g_ringsPinned{0}, g_ringBytes{0}, g_ringsFailed{0}, g_broughtBack{0}, g_lazyChunks{0}, g_lazyBytes{0};
size_t g_codeBytes;

// ---- FX history buffers ------------------------------------------------------------------------------------------

void ringMade(void* p, size_t n, void* obj, void* maker) {
    if (!p || !n) return;  // also before init has read the switches: rekordbox makes most of them at launch
    // obj is what rekordbox's function had in x19 at the call (x86_64: r12 or r14): its RingBuffer in the builds
    // looked at. Believed only if that object's buffer is this one (it is stored before the bzero).
    if (obj && ((uintptr_t)obj & 7 || (uintptr_t)obj < 0x100000000 || *(void**)((char*)obj + O_ringBuf) != p)) obj = nullptr;
    std::lock_guard<std::mutex> l(st().ringLock);
    st().rings[p] = {n, kNew, obj, maker, false, {0, 0}};
}

void ringFreed(void* p) {
    if (!p) return;
    size_t bytes = 0;
    {
        std::lock_guard<std::mutex> l(st().ringLock);
        auto it = st().rings.find(p);
        if (it == st().rings.end()) return;
        if (it->second.state == kPinned) bytes = it->second.bytes;
        st().rings.erase(it);  // one being pinned right now: pinRings sees it gone and unpins
    }
    if (bytes) {
        munlock(p, bytes);
        g_ringsPinned--, g_ringBytes -= (long)bytes;
    }
}

// rekordbox makes 218 of these buffers (662 MB), one set for every FX it has, and writes about half of them all the
// time (the history every channel's FX unit keeps, FX on or off) and the rest only while their FX is on. A buffer
// is pinned from the moment it is seen being written: its write position is looked at once a second. That the
// channel is silent doesn't make it cheap: when macOS has taken such a page it has to find a free one to give back,
// and under memory pressure that is what takes milliseconds.
// The RingBuffer is read with mach_vm_read_overwrite, which fails instead of crashing on an object freed a moment
// ago (nothing here may hold the lock during a read that can wait for the disk). Where the RingBuffer isn't known,
// the buffers made by EffectLayerBase (the always-written ones) are pinned by the name of the function that made
// them.
void findUsedRings() {
    struct Fresh { void* p; void* obj; void* maker; bool seen; int pos[2]; bool used; };
    std::vector<Fresh> fresh;
    {
        std::lock_guard<std::mutex> l(st().ringLock);
        for (auto& kv : st().rings)
            if (kv.second.state == kNew) fresh.push_back({kv.first, kv.second.obj, kv.second.maker, kv.second.seen, {kv.second.pos[0], kv.second.pos[1]}, false});
    }
    for (auto& f : fresh) {
        if (!f.obj) {
            Dl_info di;
            f.used = !f.seen && f.maker && dladdr(f.maker, &di) && di.dli_sname && strstr(di.dli_sname, "15EffectLayerBase");
            continue;
        }
        int pos[2] = {0, 0}; mach_vm_size_t got = 0;
        if (mach_vm_read_overwrite(mach_task_self(), (mach_vm_address_t)f.obj + O_ringWritePos, sizeof pos, (mach_vm_address_t)pos, &got) != KERN_SUCCESS) continue;
        f.used = f.seen && (pos[0] != f.pos[0] || pos[1] != f.pos[1]);
        f.pos[0] = pos[0], f.pos[1] = pos[1];
    }
    std::lock_guard<std::mutex> l(st().ringLock);
    for (auto& f : fresh) {
        auto it = st().rings.find(f.p);
        if (it == st().rings.end() || it->second.state != kNew || it->second.obj != f.obj) continue;
        it->second.seen = true, it->second.pos[0] = f.pos[0], it->second.pos[1] = f.pos[1];
        if (f.used) st().ringsNew.push_back(f.p);
    }
}

// Pins the buffers found in use.
void pinRings() {
    for (;;) {
        void* p = nullptr; size_t bytes = 0;
        {
            std::lock_guard<std::mutex> l(st().ringLock);
            while (!st().ringsNew.empty() && !p) {
                void* q = st().ringsNew.back();
                st().ringsNew.pop_back();
                auto it = st().rings.find(q);
                if (it != st().rings.end() && it->second.state == kNew) p = q, bytes = it->second.bytes, it->second.state = kPinning;
            }
        }
        if (!p) return;
        int err = mlock(p, bytes) == 0 ? 0 : errno;
        bool gone;
        {
            std::lock_guard<std::mutex> l(st().ringLock);
            auto it = st().rings.find(p);
            gone = it == st().rings.end() || it->second.state != kPinning || it->second.bytes != bytes;
            if (!gone) {
                if (err) st().rings.erase(it);
                else it->second.state = kPinned;
            }
        }
        if (gone) { if (!err) munlock(p, bytes); }  // freed (and perhaps made again) while it was being pinned
        else if (!err) g_ringsPinned++, g_ringBytes += (long)bytes;
        else if (g_ringsFailed++ == 0) logf("audiomem: an FX buffer couldn't be pinned (%s): macOS's limit on pinned memory?", strerror(err));
    }
}

// Test command "fx 0": lets go of every pinned buffer (they are pinned again with "fx 1").
void unpinRings() {
    std::vector<std::pair<void*, size_t>> was;
    {
        std::lock_guard<std::mutex> l(st().ringLock);
        for (auto& kv : st().rings)
            if (kv.second.state == kPinned) kv.second.state = kNew, kv.second.seen = false, was.push_back({kv.first, kv.second.bytes});
    }
    // A buffer freed between the two steps is unpinned twice at worst (the second fails, harmlessly).
    for (auto& w : was) munlock(w.first, w.second), g_ringsPinned--, g_ringBytes -= (long)w.second;
}

// ---- the decks' audio --------------------------------------------------------------------------------------------

void* bufCtor(void* (*orig)(void*, int, int), void* self, int pageSize, int numPages) {
    if (!g_on) return orig(self, pageSize, numPages);
    Buf* b = new Buf{0, 0, 0, {}};
    b->chunks.reserve(numPages > 0 && numPages <= 0x1000 ? numPages : 16);
    t_building = b;
    void* r = orig(self, pageSize, numPages);
    t_building = nullptr;
    t_fresh = nullptr;
    if (b->chunks.empty()) { delete b; return r; }
    std::lock_guard<std::mutex> l(st().lock);
    b->id = st().nextId++;
    b->alive = (int)b->chunks.size();
    st().bufs.push_back(b);
    for (size_t i = 0; i < b->chunks.size(); i++) st().chunkOf[b->chunks[i]] = {b, (int)i};
    return r;
}

// Brings back what the decks will read next. A page is only read while the lock is held and its chunk is still
// the buffer's (a buffer is freed from other threads); pages that are in memory cost one mincore per chunk.
void keepDecks() {
    int ahead = g_ahead.load();
    for (Deck& d : g_deck) {
        if (!d.loaded.load()) continue;
        long ms = d.ms.load(), from = ms - 1000, to = ms + (d.playing.load() ? ahead * 1000L : 4000);
        if (from < 0) from = 0;
        // Which buffers are this deck's isn't known: every buffer gets every deck's window (a few MB each), at both
        // usual sample rates.
        for (int rate : {44100, 48000}) {
            long f0 = from * rate / 1000, f1 = to * rate / 1000;
            for (size_t bi = 0;; bi++) {
                uint64_t id; size_t chunkBytes, count;
                {
                    std::lock_guard<std::mutex> l(st().lock);
                    if (bi >= st().bufs.size()) break;
                    Buf* b = st().bufs[bi];
                    id = b->id, chunkBytes = b->chunkBytes, count = b->chunks.size();
                }
                if (chunkBytes < kPage) continue;
                size_t b0 = (size_t)f0 * 8, b1 = (size_t)f1 * 8;  // byte positions in the track
                for (size_t c = b0 / chunkBytes; c <= b1 / chunkBytes && c < count; c++) {
                    char* p;
                    {
                        std::lock_guard<std::mutex> l(st().lock);
                        if (bi >= st().bufs.size() || st().bufs[bi]->id != id || c >= st().bufs[bi]->chunks.size()) break;
                        p = st().bufs[bi]->chunks[c];
                    }
                    if (!p) continue;
                    size_t lo = c * chunkBytes < b0 ? b0 - c * chunkBytes : 0;
                    size_t hi = (c + 1) * chunkBytes > b1 ? b1 - c * chunkBytes : chunkBytes;
                    // The pages that hold bytes lo..hi of the chunk (its first and last page may be shared with a
                    // neighbour: they are mapped all the same, and what is read is always a byte of the chunk).
                    uintptr_t a0 = ((uintptr_t)p + lo) & ~(kPage - 1), a1 = ((uintptr_t)p + hi + kPage - 1) & ~(kPage - 1);
                    if (a1 <= a0) continue;
                    // The next 3 s are read every time, in memory or not: pages in use are the last ones macOS takes.
                    uintptr_t near1 = (uintptr_t)p + ((size_t)(ms + 3000) * rate / 1000 * 8 > c * chunkBytes ? (size_t)(ms + 3000) * rate / 1000 * 8 - c * chunkBytes : 0);
                    char in[kMaxPagesAtOnce];
                    size_t n = (a1 - a0) / kPage;
                    if (n > sizeof in) n = sizeof in;
                    if (mincore((void*)a0, n * kPage, in) != 0) continue;  // freed meanwhile
                    for (size_t i = 0; i < n; i++) {
                        // Only what macOS took away: a page that was never written (a part of the track that isn't
                        // buffered, a stem that doesn't exist) has no flag, and reading it would make it real.
                        bool near = a0 + i * kPage < near1 && (in[i] & (MINCORE_INCORE | MINCORE_PAGED_OUT));
                        if (!near && ((in[i] & MINCORE_INCORE) || !(in[i] & MINCORE_PAGED_OUT))) continue;
                        bool away = !(in[i] & MINCORE_INCORE);
                        std::lock_guard<std::mutex> l(st().lock);
                        if (bi >= st().bufs.size() || st().bufs[bi]->id != id || c >= st().bufs[bi]->chunks.size() || st().bufs[bi]->chunks[c] != p) break;
                        uintptr_t a = a0 + i * kPage;
                        volatile char v = *(volatile char*)(a < (uintptr_t)p ? (uintptr_t)p : a);
                        (void)v;
                        if (away) g_broughtBack++;
                    }
                }
            }
        }
    }
}

// ---- rekordbox's code and data -----------------------------------------------------------------------------------

// Data segments are pinned. Code can't be (macOS refuses mlock on executable memory): it is read through every few
// seconds instead, which keeps its pages counted as in use.
struct Range { uintptr_t at; size_t bytes; };
std::vector<Range> g_code;
size_t g_dataBytes;

void pinImage(const mach_header_64* h, intptr_t slide) {
    const load_command* lc = (const load_command*)(h + 1);
    for (uint32_t i = 0; i < h->ncmds; i++, lc = (const load_command*)((const char*)lc + lc->cmdsize)) {
        if (lc->cmd != LC_SEGMENT_64) continue;
        const segment_command_64* s = (const segment_command_64*)lc;
        if (!strcmp(s->segname, "__TEXT")) g_code.push_back({(uintptr_t)(s->vmaddr + slide), (size_t)s->vmsize}), g_codeBytes += s->vmsize;
        else if (!strcmp(s->segname, "__DATA_CONST") || !strcmp(s->segname, "__DATA")) {
            if (mlock((void*)(s->vmaddr + slide), s->vmsize) == 0) g_dataBytes += s->vmsize;
        }
    }
}

void pinCode() {
    pinImage((const mach_header_64*)_dyld_get_image_header(0), _dyld_get_image_vmaddr_slide(0));
    Dl_info di;
    if (dladdr((void*)&pinCode, &di) && di.dli_fbase)
        for (uint32_t i = 1; i < _dyld_image_count(); i++)
            if ((const void*)_dyld_get_image_header(i) == di.dli_fbase)
                pinImage((const mach_header_64*)di.dli_fbase, _dyld_get_image_vmaddr_slide(i));
}

void readCode() {
    unsigned sum = 0;
    for (const Range& r : g_code)
        for (size_t o = 0; o < r.bytes; o += kPage) sum += *(volatile const unsigned char*)(r.at + o);
    (void)sum;
}

double swapUsedGb() {
    xsw_usage x; size_t n = sizeof x;
    return sysctlbyname("vm.swapusage", &x, &n, nullptr, 0) == 0 ? x.xsu_used / 1073741824.0 : 0;
}

void summary(const char* when) {
    size_t bufs, bytes = 0;
    {
        std::lock_guard<std::mutex> l(st().lock);
        bufs = st().bufs.size();
        for (Buf* b : st().bufs) bytes += b->chunks.size() * b->chunkBytes;
    }
    logf("audiomem: %s%ld FX buffers pinned (%.0f MB), rekordbox's data pinned (%.0f MB) and its code kept in memory (%.0f MB); %zu deck buffers "
         "(%.0f MB), %ld pages of deck audio brought back so far; %ld buffer parts (%.0f MB) left for the system to zero; swap in use %.1f GB",
         when, g_ringsPinned.load(), g_ringBytes.load() / 1048576.0, g_dataBytes / 1048576.0, g_codeBytes / 1048576.0, bufs, bytes / 1048576.0, g_broughtBack.load(),
         g_lazyChunks.load(), g_lazyBytes.load() / 1048576.0, swapUsedGb());
}

void keeper() {
    pthread_setname_np("rbmods audiomem");
    if (opt::code.integer()) pinCode();
    long lastPinned = -1, lastBack = 0;
    double pinnedAt = 0, backAt = now(), codeAt = 0;
    unsigned round = 0;
    for (;;) {
        usleep(250000);
        parkIfQuitting();
        if (!g_on) continue;
        if (!g_code.empty() && g_readCode && now() - codeAt >= 4) codeAt = now(), readCode();
        if (g_fx && ++round % 4 == 0) findUsedRings(), pinRings();
        if (g_decks) keepDecks();
        // One line once the FX buffers have been the same for 5 s (rekordbox makes them at launch), and again
        // whenever their number changes; one a minute while deck audio has to be brought back.
        long p = g_ringsPinned.load();
        if (p != lastPinned) lastPinned = p, pinnedAt = now();
        else if (pinnedAt && now() - pinnedAt > 5) pinnedAt = 0, summary("");
        long b = g_broughtBack.load();
        if (b - lastBack >= 64 && now() - backAt > 60) {
            logf("audiomem: brought %ld pages (%.1f MB) of deck audio back into memory in the last %.0f s: macOS is taking memory away "
                 "(swap in use %.1f GB)", b - lastBack, (b - lastBack) * kPage / 1048576.0, now() - backAt, swapUsedGb());
            lastBack = b, backAt = now();
        } else if (now() - backAt > 60) lastBack = b, backAt = now();
    }
}

void init() {
    g_fx = opt::fx.integer() != 0;
    g_decks = opt::decks.integer() != 0;
    g_lazy = opt::lazy.integer() != 0;
    int a = opt::ahead.integer();
    g_ahead = a < 2 ? 2 : a > 120 ? 120 : a;
    double swap = swapUsedGb();
    if (swap >= 2) logf("audiomem: %.1f GB of swap in use: this Mac is short of memory, and audio is the first thing to notice. Closing other apps before a set helps", swap);
    std::thread(keeper).detach();
}

void tick() {
    void* e = engine();
    int n = deckCount();
    for (int ch = 0; ch < 8; ch++) {
        bool loaded = ch < n && deckLoaded(ch);
        g_deck[ch].ms = loaded && e ? at<int (*)(void*, int)>(A_getPlayingTime)(e, ch) : 0;
        g_deck[ch].playing = loaded && deckPlaying(ch);
        g_deck[ch].loaded = loaded;
    }
    // Test commands: "state", and "fx|decks|lazy|code 0|1" switch one part while rekordbox runs (measuring with and without).
    std::string c = devCommand("audiomem");
    if (c.empty()) return;
    bool v = c.size() > 2 && c.back() == '1';
    if (!c.compare(0, 3, "fx ")) { g_fx = v; if (!v) unpinRings(); }
    else if (!c.compare(0, 6, "decks ")) g_decks = v;
    else if (!c.compare(0, 5, "lazy ")) g_lazy = v;
    else if (!c.compare(0, 5, "code ")) g_readCode = v;
    else if (c != "state") return;
    summary(("command '" + c + "': ").c_str());
}

}  // namespace

RBM_MOD({.name = "audiomem", .init = init, .tick = tick, .enabled = &g_on,
         .title = "Audio memory",
         .about = "Keeps the memory rekordbox's audio works with in RAM, so playback doesn't click when the Mac runs short of memory.",
         .area = "decks", .order = 60});

// ---- hooks -------------------------------------------------------------------------------------------------------

// RingBuffer's constructors and getMemory: bzero(buffer, bytes) right after new float[n]. The RingBuffer itself is
// in x19 there (the trampoline hands it on; x86_64: r12 in the constructors, r14 in getMemory); the hook is entered
// by a jump, so one frame up is that function's own return address: who made the buffer (all three keep a frame
// pointer in both halves).
extern "C" void rbm_ring_zero(void* p, size_t n, void* obj, int which) {
    void (*orig)(void*, size_t) = which == 0 ? RBM_ORIG(ring_zero0, void (*)(void*, size_t))
                                : which == 1 ? RBM_ORIG(ring_zero1, void (*)(void*, size_t)) : RBM_ORIG(ring_zero2, void (*)(void*, size_t));
    orig(p, n);
    ringMade(p, n, obj, __builtin_return_address(1));
}
#if defined(__aarch64__)
#define RING_ZERO(i)                                                                                   \
    asm(".text\n.p2align 2\n.globl _rbm_hook_ring_zero" #i "\n_rbm_hook_ring_zero" #i ":\n"            \
        "mov x2, x19\nmov w3, #" #i "\nb _rbm_ring_zero\n");
RING_ZERO(0)
RING_ZERO(1)
RING_ZERO(2)
#elif defined(__x86_64__)
#define RING_ZERO(i, reg)                                                                              \
    asm(".text\n.p2align 4\n.globl _rbm_hook_ring_zero" #i "\n_rbm_hook_ring_zero" #i ":\n"            \
        "movq %" #reg ", %rdx\nmovl $" #i ", %ecx\njmp _rbm_ring_zero\n");
RING_ZERO(0, r12)
RING_ZERO(1, r12)
RING_ZERO(2, r14)
#endif
// ... and delete[] buffer (x86_64: the destructors' delete[] is a tail call, which a plain hook takes as it is).
#define RING_FREE(i)                                                         \
    extern "C" void rbm_hook_ring_free##i(void* p) {                        \
        ringFreed(p);                                                        \
        RBM_ORIG(ring_free##i, void (*)(void*))(p);                          \
    }
RING_FREE(0)
RING_FREE(1)
RING_FREE(2)

// PageBuffer<Float4>::PageBuffer(pageSize, numPages), from the three places that build one.
#define PAGEBUF_CTOR(i)                                                                              \
    extern "C" void* rbm_hook_pagebuf_ctor##i(void* self, int pageSize, int numPages) {             \
        return bufCtor(RBM_ORIG(pagebuf_ctor##i, void* (*)(void*, int, int)), self, pageSize, numPages); \
    }
PAGEBUF_CTOR(0)
PAGEBUF_CTOR(1)
PAGEBUF_CTOR(2)

// Inside it: one new[] per chunk ...
#if defined(__x86_64__)
// (x86_64: posix_memalign(&p, 16, n), freed with free. calloc's blocks are 16-byte aligned, as every malloc block.)
extern "C" int rbm_hook_pagebuf_new(void** out, size_t align, size_t n) {
    void* p = nullptr;
    int r = 0;
    Buf* b = t_building;
    if (b && g_lazy && n >= kPage && align <= 16) {
        if ((p = calloc(1, n))) *out = p, t_fresh = p, g_lazyChunks++, g_lazyBytes += (long)n;
    }
    if (!p) r = RBM_ORIG(pagebuf_new, int (*)(void**, size_t, size_t))(out, align, n), p = r == 0 ? *out : nullptr;
    if (b && p) {
        if (b->chunks.empty()) b->chunkBytes = n;
        if (n == b->chunkBytes) b->chunks.push_back((char*)p);
    }
    return r;
}
#else
extern "C" void* rbm_hook_pagebuf_new(size_t n) {
    void* p = nullptr;
    Buf* b = t_building;
    if (b && g_lazy && n >= kPage) {
        // Zero from the system, not touched here. operator delete[] frees it like any other block (rekordbox uses
        // the standard operator new, which is malloc).
        if ((p = calloc(1, n))) t_fresh = p, g_lazyChunks++, g_lazyBytes += (long)n;
    }
    if (!p) p = RBM_ORIG(pagebuf_new, void* (*)(size_t))(n);
    if (b && p) {
        if (b->chunks.empty()) b->chunkBytes = n;
        if (n == b->chunkBytes) b->chunks.push_back((char*)p);
    }
    return p;
}
#endif

// ... and its bzero.
extern "C" void rbm_hook_pagebuf_zero(void* p, size_t n) {
    if (p && p == t_fresh) return;
    RBM_ORIG(pagebuf_zero, void (*)(void*, size_t))(p, n);
}

// ~PageBuffer: delete[] of each chunk (x86_64: free).
extern "C" void rbm_hook_pagebuf_free(void* p) {
    if (p) {
        std::lock_guard<std::mutex> l(st().lock);
        auto it = st().chunkOf.find(p);
        if (it != st().chunkOf.end()) {
            Buf* b = it->second.first;
            b->chunks[it->second.second] = nullptr;
            st().chunkOf.erase(it);
            if (--b->alive <= 0) {
                for (size_t i = 0; i < st().bufs.size(); i++)
                    if (st().bufs[i] == b) { st().bufs.erase(st().bufs.begin() + i); break; }
                delete b;
            }
        }
    }
    RBM_ORIG(pagebuf_free, void (*)(void*))(p);
}
