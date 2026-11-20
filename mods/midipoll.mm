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

RBM_MOD({.name = "midipoll", .enabled = &g_on});
