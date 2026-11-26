# idlewake <span class="pd-tag">mac</span>

Less work while nothing plays: three things rekordbox keeps doing around the clock now rest until there is
something to do.

<ModInfo name="idlewake" platform="mac" group="Quieter in the background" />

## Why

With no track loaded and nobody touching it, rekordbox uses about a fifth of a processor core and wakes up some
4000 times a second. Part of that is work with nothing to work on:

- **The MIDI output thread** woke up every millisecond, a thousand times a second, to see whether there was a
  message for a controller.
- **Every level meter** restarted a timer on each frame it was drawn, also while it showed nothing. Each restart
  wakes another thread.
- **Every mixer channel** measures how loud its audio is, all the time, for the *gate / comp* Color FX: also when the
  audio is pure silence, and whether or not that effect is in use. A quarter of the audio thread's work with nothing
  playing.

## What it does

- The MIDI thread sleeps up to 20 ms when no message is waiting, and is woken the moment one arrives. A queue of
  messages goes out at the same pace as before, and the controller's keep-alive message is still sent on time.
- A level meter that shows nothing leaves its timer alone.
- A channel whose audio has been exact digital silence for about 40 seconds stops measuring it, and starts again
  with the first sample that isn't zero.

Measured idle on a MacBook Pro (M2 Pro): 23 % of a core down to 21 %, 3600 context switches a second down to
2850, the MIDI thread from 1000 wake-ups a second to 37.

## Is the sound the same?

Yes. Only the loudness measurement is skipped, never audio, and only on silence. `idlewake_check=1` is a test
switch that measures anyway and counts, for every block that would be skipped, whether the measurement's result
changed: in a run with a track played and stopped, 337,000 blocks left it exactly as it was and none changed it.
The one thing that can differ: right after such a silence, the gate's first loudness window may start a fraction of
a second earlier or later than it would have.

## Options

| Option | Default | What it does |
|---|---|---|
| `idlewake` | `1` | `0` switches the mod off (restart). |
| `idlewake_midi` | `1` | `0`: the MIDI output thread wakes every millisecond again. |
| `idlewake_meters` | `1` | `0`: level meters restart their timer on every frame again. |
| `idlewake_envelope` | `1` | `0`: silent channels are measured again. |
| `idlewake_check` | `0` | `1` measures silent channels anyway and logs whether the result changed (for testing). |
| `idlewake_log` | `0` | `1` logs every 10 s what was left out. |

The three parts can be switched while rekordbox runs.

## Related

[midipoll](./midipoll) · [framerate](./framerate) · [screeninfo](./screeninfo)
