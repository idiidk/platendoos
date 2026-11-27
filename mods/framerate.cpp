// framerate: rekordbox's UI runs at the display's full refresh rate (setting RenderDelay, 120) as soon as any deck
// holds a track, even a paused one, and at 30 fps while the decks are empty, and presents a frame every tick whether
// anything changed or not. The rate is not only the waveforms': every button, list and hover in the main window is
// painted into one image and shown at the next frame, so at 30 fps each click shows up to 33 ms late (measured:
// repaint to frame 25 ms in the middle, 3.5 ms at the full rate). This runs the full rate while something is
// happening, a deck playing or the mouse or keyboard used in the last few seconds, with or without a track on a
// deck, and 30 fps the rest of the time.
// (A controller's jog wheel on a paused deck isn't input the Mac sees: set framerate=0 when scratching that way.)
//   config: framerate=0 to disable; framerate_idle_after=<seconds> (default 3); framerate_empty=0: 30 fps while the
//   decks are empty, also while you use the mouse (rekordbox's own rule there)
#include "../core/rbmod.h"

using namespace rbm;

// Its settings (core/settings.h): what the settings window and the config file's list show of them.
namespace opt {
const Setting empty{"framerate_empty", true, {.label = "Full frame rate with empty decks too"}};
const Setting idleAfter{"framerate_idle_after", 3, {.label = "Go to 30 a second after", .help = "Seconds without touching the mouse or keyboard.", .unit = "s", .min = 1, .max = 600, .step = 1}};
}  // namespace opt

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
    double idleAfter = opt::idleAfter.number();
    double input = plat::idleSeconds();
    if (playing || input < idleAfter) activeAt = now();
    // Full rate only while something is happening; with empty decks too (rekordbox itself: 30 fps there, always).
    bool full = (loaded || opt::empty.integer()) && now() - activeAt < idleAfter;
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

// The window's sidebar area this mod is shown in (core/settings.h: the first declaration of an area counts).
namespace { const rbm::settings::Area kArea{"speed", "Speed",
    "A quicker window and less work in the background.",
    "<path d=\"M9 1.5 3.5 9H8l-1 5.5L12.5 7H8z\"/>", 70}; }

RBM_MOD({.name = "framerate", .init = [] {
             plat::everyOnMain(0.1, [] { modEnabled("framerate") ? tick() : restore(); });
         },
         .title = "Frame rate",
         .about = "Full frame rate while a deck plays or you use the computer, 30 a second the rest of the time.",
         .area = "speed", .order = 10});
