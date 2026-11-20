// spotifystart: fixes Spotify never starting ("Spotify-bibliotheek laden") on rekordbox 7.2.19 for Windows.
// 7.2.19's Spotify client runs its jobs on a pool of one thread. A job that restarts the client (shutdown, create
// again, clear the queue) waits until the SDK has finished starting. The SDK can't finish while that job holds the
// only thread, because the websocket messages it needs are jobs on the same pool, so both wait for ever. Here that
// restart job does nothing while the SDK is still starting: the start already under way is the one we want.
// Only for this exact build: it checks the build and the code it patches, and otherwise leaves rekordbox alone.
//   config: spotifystart=0 to disable
// windows-only
#ifdef _WIN32
#include "../core/rbmod.h"
#include <windows.h>
#include <atomic>
#include <cstring>

using namespace rbm;

namespace {

const uint32_t kStamp = 0x6A97B2E4, kImage = 0x6573000;
const uint32_t kJob = 0x2fa5840;        // the restart job's body (std::function target)
const uint32_t kShutdownCall = kJob + 0x0d, kCreateCall = kJob + 0x16, kClearJump = kJob + 0x33;
const size_t kClientStarting = 0x81;    // Client: set when the SDK is asked to start, cleared by its init callback
const uint8_t kJobBytes[] = {
    0x40, 0x53, 0x48, 0x83, 0xec, 0x20, 0x48, 0x8b, 0xd9, 0x48, 0x8b, 0x49, 0x08, 0xe8, 0x5e, 0x12, 0x00, 0x00,
    0x48, 0x8b, 0x4b, 0x08, 0xe8, 0xc5, 0x15, 0x00, 0x00, 0x48, 0x8b, 0x4b, 0x08, 0x45, 0x33, 0xc9, 0x33, 0xd2,
    0x41, 0xb8, 0xe8, 0x03, 0x00, 0x00, 0x48, 0x8b, 0x49, 0x70, 0x48, 0x83, 0xc4, 0x20, 0x5b, 0xe9, 0x28, 0xac,
    0xab, 0xff};

typedef void (*ClientFn)(void*);
typedef uint64_t (*ClearFn)(void*, uint64_t, uint64_t, uint64_t);
ClientFn g_shutdown, g_create;
ClearFn g_clear;
thread_local bool t_skip;  // this job run is being skipped (the job runs start to end on one thread)
std::atomic<int> g_skipped{0};
bool g_patched;

void hookShutdown(void* client) {
    t_skip = modEnabled("spotifystart") && *((volatile uint8_t*)client + kClientStarting);
    if (!t_skip) return g_shutdown(client);
    if (g_skipped.fetch_add(1) < 5) logf("spotifystart: left out a Spotify client restart while the SDK was starting");
}
void hookCreate(void* client) {
    if (!t_skip) g_create(client);
}
uint64_t hookClear(void* pool, uint64_t a, uint64_t b, uint64_t c) {
    if (!t_skip) return g_clear(pool, a, b, c);
    t_skip = false;
    return 1;
}

struct Install {
    Install() {
        auto base = (uint8_t*)GetModuleHandleW(nullptr);
        auto nt = (IMAGE_NT_HEADERS*)(base + ((IMAGE_DOS_HEADER*)base)->e_lfanew);
        if (nt->FileHeader.TimeDateStamp != kStamp || nt->OptionalHeader.SizeOfImage != kImage) return;
        if (memcmp(base + kJob, kJobBytes, sizeof kJobBytes)) return;
        uint8_t* near_ = nullptr;
        for (uint8_t* p = base + kImage + 0x200000; p < base + 0x70000000 && !near_; p += 0x10000)
            near_ = (uint8_t*)VirtualAlloc(p, 0x1000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        if (!near_) return;
        const uint32_t sites[3] = {kShutdownCall, kCreateCall, kClearJump};
        void* hooks[3] = {(void*)&hookShutdown, (void*)&hookCreate, (void*)&hookClear};
        void* origs[3];
        for (int i = 0; i < 3; i++) {
            uint8_t* t = near_ + i * 16;
            t[0] = 0xFF; t[1] = 0x25; memset(t + 2, 0, 4); memcpy(t + 6, &hooks[i], 8);
            int32_t d; memcpy(&d, base + sites[i] + 1, 4);
            origs[i] = base + sites[i] + 5 + d;
        }
        g_shutdown = (ClientFn)origs[0]; g_create = (ClientFn)origs[1]; g_clear = (ClearFn)origs[2];
        DWORD o; VirtualProtect(near_, 0x1000, PAGE_EXECUTE_READ, &o);
        VirtualProtect(base + kJob, sizeof kJobBytes, PAGE_EXECUTE_READWRITE, &o);
        for (int i = 0; i < 3; i++) {
            int32_t d = (int32_t)((near_ + i * 16) - (base + sites[i] + 5));
            memcpy(base + sites[i] + 1, &d, 4);
        }
        DWORD x; VirtualProtect(base + kJob, sizeof kJobBytes, o, &x);
        FlushInstructionCache(GetCurrentProcess(), base + kJob, sizeof kJobBytes);
        g_patched = true;
    }
} g_install;

void init() { logf(g_patched ? "spotifystart: on" : "spotifystart: not this rekordbox build, nothing patched"); }

}  // namespace

RBM_MOD({.name = "spotifystart", .init = init});
#endif
