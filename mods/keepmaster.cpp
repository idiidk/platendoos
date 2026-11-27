// keepmaster: a change of sync master never changes the tempo of a deck you hear. rekordbox picks the sync master
// again on every audio block, by priority (a deck whose channel fader is down ranks lower), and every deck with BEAT
// SYNC on follows the new master's BPM at once: fade out the master while a deck at another tempo plays, and the
// synced decks jump to that tempo. And a stopped master stays master: cue the next track on it and the synced decks
// that are still playing follow its tempo.
// With this:
//  - rekordbox may move the master by itself (fader down) only while every other playing deck already runs at the
//    master's tempo (or half/double: what a synced deck does), so the move changes nothing you hear;
//  - when the master stops (pause, end, unload), the master goes at once to a playing deck at the same tempo;
//  - otherwise the master stays where it is. MASTER on a deck still moves it, as always.
// How: SyncManager::decideSyncMaster re-elects by priority only if shouldChangeMasterByPriority() (always true in
// rekordbox), else it keeps the current master while that is still eligible (a stopped deck is). The mod answers
// that from what tick() saw last (deck BPMs are read on the main thread, not the audio thread), and hands over a
// stopped master with DjEngineIF::setMaster, as the MASTER button does.
//   config: keepmaster=0 for rekordbox's own behaviour (switched on the next audio block)
#include "../core/rbmod.h"
#include <atomic>
#include <cmath>
#include <stdlib.h>
#include <string>

using namespace rbm;

static volatile bool g_on = true;

// The two sync managers (decks; and LocalSyncManager) and their shouldChangeMasterByPriority's vtable slot, counted
// from the symbol + 0x10 (SyncManager's vtable has a virtual-base offset first, so that is not quite its address
// point on mac; tools/win/maketable.py allows for it).
static const Addr A_syncVtable{"__ZTVN6rbxfrm11SyncManagerE"};
static const Offset V_syncShould{0x60, "__ZTVN6rbxfrm11SyncManagerE", "vt:__ZNK6rbxfrm11SyncManager28shouldChangeMasterByPriorityEv", 0};
static const Addr A_localVtable{"__ZTVN6rbxfrm16LocalSyncManagerE"};
static const Offset V_localShould{0x60, "__ZTVN6rbxfrm16LocalSyncManagerE", "vt:__ZNK6rbxfrm16LocalSyncManager28shouldChangeMasterByPriorityEv", 0};
static const Addr A_existMaster{"__ZNK8djengine10DjEngineIF11existMasterEv"};
static const Addr A_getMaster{"__ZNK8djengine10DjEngineIF9getMasterEv"};  // channel or -1
// DjEngineIF::setMaster(channel, EnableOrNot): 1 = make it master (2 toggles), as the MASTER button
static const Addr A_setMaster{"__ZN8djengine10DjEngineIF9setMasterENS_15EnPlayerChannelEN6rbxfrm11EnableOrNotE"};

static std::atomic<bool> g_safe{false};  // a re-election by priority can't change a playing deck's tempo
static bool shouldChange(const void*) { return !g_on || g_safe.load(std::memory_order_relaxed); }  // audio thread

// Same tempo for sync: equal, or half/double (a synced deck may run at either).
static bool sameTempo(double a, double b) {
    if (a <= 0 || b <= 0) return false;
    for (double r : {1.0, 2.0, 0.5})
        if (std::fabs(a / (b * r) - 1) < 0.0005) return true;
    return false;
}

#ifndef _WIN32
// Test commands (command-keepmaster in the config folder): "state" | "play N" | "pause N" | "sync N 0|1" | "master N"
// Looked up when a command arrives, not as Addrs: a missing one only disables that command, never the mod.
// (macOS only. On Windows the table is dropped after start-up, so symbol() would give 0 there: no test command.)
static uintptr_t testFn(const char* mangled) {
    uintptr_t a = symbol(mangled);
    if (!a) logf("keepmaster: test command unavailable on this build (%s not found)", mangled);
    return a;
}
static void testCommand(void* e) {
    std::string c = devCommand("keepmaster");
    if (c.empty()) return;
    int ch = c.find(' ') != std::string::npos ? atoi(c.c_str() + c.find(' ') + 1) - 1 : -1;
    if (c == "state") {
        logf("keepmaster: master %d, re-election %s", at<bool (*)(void*)>(A_existMaster)(e) ? at<int (*)(void*)>(A_getMaster)(e) + 1 : 0,
             g_safe ? "allowed" : "held");
        uintptr_t isSync = symbol("__ZNK8djengine10DjEngineIF6isSyncENS_15EnPlayerChannelE");  // -1 below: unknown
        for (int d = 0; d < deckCount(); d++)
            logf("keepmaster: deck %d playing %d sync %d %.2f BPM (track %.2f)", d + 1, deckPlaying(d), isSync ? at<bool (*)(void*, int)>(isSync)(e, d) : -1,
                 deckBpm(d, true), deckBpm(d, false));
    } else if (ch >= 0 && ch < deckCount()) {
        uintptr_t f;
        if (!c.compare(0, 4, "play")) {
            if ((f = testFn("__ZN8djengine10DjEngineIF4playENS_15EnPlayerChannelEb"))) at<void (*)(void*, int, bool)>(f)(e, ch, false);
        } else if (!c.compare(0, 5, "pause")) {
            if ((f = testFn("__ZN8djengine10DjEngineIF5pauseENS_15EnPlayerChannelE"))) at<void (*)(void*, int)>(f)(e, ch);
        } else if (!c.compare(0, 4, "sync")) {
            if ((f = testFn("__ZN8djengine10DjEngineIF7setSyncENS_15EnPlayerChannelEb"))) at<void (*)(void*, int, bool)>(f)(e, ch, c.back() == '1');
        } else if (!c.compare(0, 6, "master")) {
            at<int (*)(void*, int, int)>(A_setMaster)(e, ch, 1);
        }
    }
}
#endif

static void tick() {
    static double last, handedAt;
    double t = now();
    if (!g_on || t - last < 0.03) return;
    last = t;
    void* e = engine();
    if (!e) return;
#ifndef _WIN32
    testCommand(e);
#endif
    int m = at<bool (*)(void*)>(A_existMaster)(e) ? at<int (*)(void*)>(A_getMaster)(e) : -1;
    if (m < 0 || m >= deckCount()) {
        g_safe = true;  // no master: rekordbox elects one anyway
        return;
    }
    double mb = deckBpm(m, true);
    bool mismatch = false;
    int same = -1;
    for (int d = 0; d < deckCount(); d++) {
        if (d == m || !deckPlaying(d)) continue;
        if (sameTempo(deckBpm(d, true), mb)) {
            if (same < 0) same = d;
        } else {
            mismatch = true;
        }
    }
    g_safe = !mismatch;
    if (!deckPlaying(m) && same >= 0 && t - handedAt > 0.5) {
        handedAt = t;
        at<int (*)(void*, int, int)>(A_setMaster)(e, same, 1);
        logf("keepmaster: deck %d stopped: master to deck %d (playing at the same tempo, %.2f BPM)", m + 1, same + 1, mb);
    }
}

// By the method's vtable slot (on Windows the method is a stub shared with other slots, so no scanning for it).
static bool patch(const Addr& vtable, const Offset& slot, const char* name) {
    void** vt = (void**)(at<char*>(vtable) + 0x10 + slot);
    if (patchSlot(vt, (void*)&shouldChange)) return true;
    logf("keepmaster: could not patch %s's shouldChangeMasterByPriority: the master switches as usual", name);
    return false;
}

static void init() {
    bool deck = patch(A_syncVtable, V_syncShould, "SyncManager");
    bool local = patch(A_localVtable, V_localShould, "LocalSyncManager");
    if (deck && local) logf("keepmaster: a master change never changes the tempo of a playing deck");
}

// The window's sidebar area this mod is shown in (core/settings.h: the first declaration of an area counts).
namespace { const rbm::settings::Area kArea{"decks", "Decks & sound",
    "What you hear and mix: inputs, outputs, the mixer's effects.",
    "<circle cx=\"8\" cy=\"8\" r=\"6\"/><circle cx=\"8\" cy=\"8\" r=\"1.6\"/>", 10}; }

RBM_MOD({.name = "keepmaster", .init = init, .tick = tick, .enabled = &g_on,
         .title = "Steady sync master",
         .about = "Changing the sync master never changes the tempo of a deck you hear.",
         .area = "decks", .order = 50});
