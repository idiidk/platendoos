# linein

A turntable, a CD player or any other audio input, played through a deck. The deck's channel on the mixer then
works on it like on a track: trim, EQ, filter, channel fader, crossfader, headphone cue and the FX.

<ModInfo name="linein" group="Controllers" needs="any controller: a [flx4] too" />

rekordbox itself only lets you play an input through a deck on hardware that has its own inputs (a DVS interface, a
DDJ with LINE / PHONO sockets). A controller like the DDJ-FLX4 has none. With linein the input can be any audio
device your computer sees: a USB turntable, an audio interface with a turntable or CDJ on it, even a virtual device.

It uses rekordbox's own **INPUT THROUGH** mode of a deck (what THROUGH in the DVS deck menu does), so:

- the deck's display says **INPUT THROUGH**,
- rekordbox reads the tempo from what comes in, for the Beat FX,
- the deck's own track, if one is loaded, is silent meanwhile.

## Setting it up

1. Connect the device. For a USB turntable that is all; it shows up in *Audio MIDI Setup* under a name such as
   `USB AUDIO CODEC`.
2. Click the PLATENDOOS chip, go to **Decks & sound**, and under *Turntables and other inputs* pick the device for
   deck 1 from the list (and for deck 2, and so on). The list has every audio input that is connected; a second
   device of the same name shows as `#2`. In the config file these are `linein_1` .. `linein_4`.
3. The first time, macOS asks whether rekordbox may use the **microphone**. Say yes: that permission covers every
   audio input.

Deck 1 now shows INPUT THROUGH and plays the turntable through its channel.

**Two devices of the same make** have the same name. Write `USB AUDIO CODEC #2` for the second one, or use the
devices' UIDs: the log lists every input with its UID (the command file `command-linein` with the word `list` in it,
in the config folder, logs them at any time).

**An audio interface with several inputs**: give both decks the same device and different channels, `1-2` for one
and `3-4` for the other (the *Channels* field next to the device). A mono input: `1` plays it on both sides.

## Tracks and inputs on the same deck

By default a deck plays its input only **while no track is loaded on it**. Load a track onto the deck and it plays
the track like any deck; unload the track and the input is back. So nothing needs switching in the middle of a set.

- `linein_empty=0`: the deck plays its input whatever is loaded (a loaded track stays silent).
- `linein_1_on=0`: deck 1's input is off; the deck only plays tracks.

## Sound and latency

The input device runs on its own clock, never exactly the engine's, and often at another rate (48 kHz into a
44.1 kHz engine). Its audio is resampled continuously to follow the engine's clock, so nothing is dropped or
repeated, however long it plays. Measured with a test tone through a loopback device: everything that isn't the tone
is about 100 dB below it at 48 into 44.1 kHz, and 135 dB at equal rates.

The deck plays about **12 ms behind the input** with the usual 128-sample blocks (the input device is asked for
128), on top of what rekordbox's own output adds. If an input ever arrives too unevenly for that, 2 ms are added per
underrun for the rest of the session; `linein_extra_ms` adds a fixed amount from the start.

`linein_1_gain` sets a gain in dB on top of the mixer's trim, for a quiet input.

## Options

| Option | Default | What it does |
|---|---|---|
| `linein` | `1` | `0` switches the mod off (restart). |
| `linein_1` … `linein_4` | | Deck 1-4's audio input: the device's name, `name #2` for a second one of that name, or its UID. Empty: none. |
| `linein_1_on` … `linein_4_on` | `1` | `0`: that deck plays tracks only. |
| `linein_1_ch` … `linein_4_ch` | `1-2` | The device's channels to take (`3-4`; `1` for a mono input). |
| `linein_1_gain` … `linein_4_gain` | `0` | Gain in dB on the input, -24 to 24. |
| `linein_empty` | `1` | A deck plays its input only while no track is loaded on it. `0`: always. |
| `linein_extra_ms` | `0` | Latency to add, for an input whose audio arrives unevenly. |
| `linein_log` | `0` | `1`: every 10 s per deck, the input's clock against the engine's, the distance kept, the level. |

Everything applies within a second or two of changing it; no restart.

## Good to know

- Tested so far with a loopback device and the built-in microphone, not yet with a turntable. Reports are welcome,
  with the log (`linein_log=1`).
- **Windows**: on since 2026-11-27. It ran on a PC with a loopback test input (correct level, no dropouts, the
  input reopened after the device went away), not yet with a real recording device or a turntable. The devices are
  the recording devices Windows lists (*Sound settings > Input*), and Windows must allow desktop apps to use the
  microphone (*Settings > Privacy & security > Microphone*). The deck plays about 5 ms later than on a Mac. Reports
  are welcome, with the log.
- rekordbox switches every deck's input through off again at many mixer and controller events. linein holds it on
  for its decks, so the sound doesn't drop; the deck's tempo-from-the-input setting is put back within a second.
- When the device is unplugged the deck goes back to playing tracks; plugged in again, the input returns.
