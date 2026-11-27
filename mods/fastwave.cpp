// fastwave: the deck waveforms redraw sooner after they change (a stem muted or unmuted, a track loaded).
// When a waveform has to be rebuilt, rekordbox throws the old one away at once, then waits a fixed 500 ms (timer 1
// of the WidgetWaveView) before building the new one (~0.1 s of work on its own thread), and looks for the finished
// result every 100 ms. Measured on a playing deck: the new build started exactly 0.50 s after each stem toggle, so most
// of the blank waveform was that wait. A stem toggle reaches it through operateWavePartUpdateMuted ->
// setWavePartSelected -> resizedFrame (found with lldb); a track load through initializeWave. For those the wait is
// fastwave_ms (default 30 ms, still long enough to merge a burst of changes into one rebuild); resizing the window
// keeps rekordbox's 500 ms. The check for the finished result runs every 15 ms.
//   config: fastwave=0 to disable; fastwave_ms=<wait before rebuilding> (default 30; rekordbox's own is 500)
#include "../core/rbmod.h"
#include <algorithm>

using namespace rbm;

// Its settings (core/settings.h): what the settings window and the config file's list show of them.
namespace opt {
const Setting ms{"fastwave_ms", 30, {.label = "Wait before redrawing", .help = "rekordbox's own wait is 500 ms.", .unit = "ms", .min = 1, .max = 500, .step = 1}};
}  // namespace opt

static volatile bool g_on = true;

typedef void (*StartTimer)(void* multiTimer, int timerId, int ms);

static int waitMs() { return std::max(1, std::min(opt::ms.integer(), 500)); }

// The first few changes of each kind, to see in the log that it works.
#define logOnce(site, id, ms, now)                                                                     \
    do {                                                                                               \
        static int logged = 0;                                                                         \
        if (logged < 3) logged++, logf("fastwave: %s timer %d: %d ms -> %d ms", site, id, ms, now);   \
    } while (0)

// initializeWave: "rebuild the waveform in 500 ms" (two call sites).
#define RBM_FW_INIT(n)                                                                        \
    extern "C" void rbm_hook_fastwave_init##n(void* t, int id, int ms) {                      \
        int use = g_on && id == 1 && ms == 500 ? waitMs() : ms;                               \
        if (use != ms) logOnce("rebuild", id, ms, use);                                       \
        RBM_ORIG(fastwave_init##n, StartTimer)(t, id, use);                                   \
    }
RBM_FW_INIT(0)
RBM_FW_INIT(1)

// A stem was muted or unmuted: the resizedFrame this leads to is for that, not for a resize.
static thread_local bool t_muting;
extern "C" void rbm_hook_fastwave_muted(void* view, bool a, bool b, bool c, bool d) {
    t_muting = true;
    RBM_ORIG(fastwave_muted, void (*)(void*, bool, bool, bool, bool))(view, a, b, c, d);
    t_muting = false;
}
extern "C" void rbm_hook_fastwave_resize(void* t, int id, int ms) {
    int use = g_on && t_muting && id == 1 && ms == 500 ? waitMs() : ms;
    if (use != ms) logOnce("stem toggle", id, ms, use);
    RBM_ORIG(fastwave_resize, StartTimer)(t, id, use);
}

// timerCallback: "check every 100 ms whether the new waveform is built".
extern "C" void rbm_hook_fastwave_check(void* t, int id, int ms) {
    int use = g_on && id == 4 && ms == 100 ? 15 : ms;
    if (use != ms) logOnce("check", id, ms, use);
    RBM_ORIG(fastwave_check, StartTimer)(t, id, use);
}

// timerCallback: "the data isn't there yet, try again in 500 ms".
extern "C" void rbm_hook_fastwave_retry(void* t, int id, int ms) {
    int use = g_on && id == 1 && ms == 500 ? 100 : ms;
    RBM_ORIG(fastwave_retry, StartTimer)(t, id, use);
}

RBM_MOD({.name = "fastwave", .enabled = &g_on,
         .title = "Quick waveforms",
         .about = "Deck waveforms come back sooner after a stem is muted or a track is loaded.",
         .area = "decks", .order = 70});
