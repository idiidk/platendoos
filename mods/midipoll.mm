// mac-only: CoreMIDI device polling; no Windows equivalent, leave it out of a Windows build.
// midipoll: rekordbox looks for new or removed MIDI controllers every 300 ms by listing all of CoreMIDI's
// devices on the main thread. This runs that check every 2 s instead, and on every turn as before for a few
// seconds after the number of MIDI sources or destinations changes (rekordbox debounces a change over a few
// checks), so plugging in a controller is still picked up within about 2 s.
//   config: midipoll=0 to disable
#include "../core/rbmod.h"
#include <CoreMIDI/CoreMIDI.h>

using namespace rbm;

static volatile bool g_on = true;

// Main thread.
extern "C" void rbm_hook_midi_check(void* server) {
    static double checkedAt, changedAt;
    static ItemCount lastCount = ~(ItemCount)0;
    double t = now();
    ItemCount count = MIDIGetNumberOfSources() * 1000 + MIDIGetNumberOfDestinations();
    if (count != lastCount) lastCount = count, changedAt = t;
    if (g_on && t - checkedAt < 2 && t - changedAt > 3) return;
    checkedAt = t;
    RBM_ORIG(midi_check, void (*)(void*))(server);
}

// The window's sidebar area this mod is shown in (core/settings.h: the first declaration of an area counts).
namespace { const rbm::settings::Area kArea{"controller", "Hardware",
    "The DDJ-FLX4 and other MIDI controllers.",
    "<rect x=\"2\" y=\"3\" width=\"12\" height=\"10\" rx=\"1.5\"/><path d=\"M5.5 5.5v5M10.5 5.5v5M4.3 8.6h2.4M9.3 6.8h2.4\"/>", 60}; }

RBM_MOD({.name = "midipoll", .enabled = &g_on,
         .title = "Controller detection",
         .about = "Looks for newly plugged-in MIDI controllers every 2 seconds instead of every 0.3.",
         .area = "controller", .order = 20});
