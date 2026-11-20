// framerate: rekordbox's UI runs at the display's full refresh rate (setting RenderDelay, 120) as soon as any deck
// holds a track, even a paused one, and redraws every frame whether anything changed or not. This keeps it at the
// 30 fps rekordbox itself uses with empty decks unless a deck is playing or the mouse or keyboard was used in the
// last few seconds.
// (A controller's jog wheel on a paused deck isn't input the Mac sees: set framerate=0 when scratching that way.)
//   config: framerate=0 to disable; framerate_idle_after=<seconds> (default 3)
#include "../core/rbmod.h"

using namespace rbm;

static const Addr A_mainComponent{"__ZN13MainComponent11getInstanceEv"};
static const Addr A_changeFrameRate{"__ZN13MainComponent15changeFrameRateEb"};  // (bool full): full = RenderDelay, else 30

static bool g_rekordboxSet;  // rekordbox changed the rate itself since we last set it
static int g_applied = -1;   // what we last asked for

// UiManager::changeLoadStatus fetches MainComponent just before it sets the rate from the decks' load state.
extern "C" void* rbm_hook_fr_loadStatus() {
    g_rekordboxSet = true;
    return RBM_ORIG(fr_loadStatus, void* (*)())();
}

static void setFull(bool full) {
    if (void* mc = at<void* (*)()>(A_mainComponent)()) at<void (*)(void*, bool)>(A_changeFrameRate)(mc, full);
    g_applied = full;
    g_rekordboxSet = false;
}

static void tick() {
    bool loaded = false, playing = false;
    for (int ch = 0; ch < deckCount() && ch < 8; ch++) loaded |= deckLoaded(ch), playing |= deckPlaying(ch);
    static double activeAt;
    double idleAfter = cfgNum("framerate_idle_after", 3);
    double input = plat::idleSeconds();
    if (playing || input < idleAfter) activeAt = now();
    // rekordbox's own rule (full rate while a deck is loaded), only while something is happening.
    bool full = loaded && now() - activeAt < idleAfter;
    if (full != g_applied || g_rekordboxSet) setFull(full);
}

// Switched off: hand the rate back to rekordbox's rule once.
static void restore() {
    if (g_applied < 0) return;
    bool loaded = false;
    for (int ch = 0; ch < deckCount() && ch < 8; ch++) loaded |= deckLoaded(ch);
    setFull(loaded);
    g_applied = -1;
}

RBM_MOD({.name = "framerate", .init = [] {
             plat::everyOnMain(0.1, [] { modEnabled("framerate") ? tick() : restore(); });
         }});
