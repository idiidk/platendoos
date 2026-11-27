# outputsync <span class="pd-tag">mac</span>

Lines up speakers and headphones when rekordbox plays to two devices, e.g. PC MASTER OUT to Bluetooth speakers with
the headphones on a controller.

<ModInfo name="outputsync" platform="mac" group="Browsing and playing" />

## Why

Bluetooth sounds a few hundred milliseconds late, so with the master on Bluetooth speakers and the headphone cue on a
controller, the headphones ran ahead of the speakers. macOS doesn't line the devices up by itself.

## What it does

It delays the faster device by the difference the devices report (the log says how much), so speakers and headphones
line up. The headphones then lag your hands by the same amount.

The two devices' clocks drift apart only slowly (measured: Bluetooth speakers against an FLX4, 0.3 ms a minute). When
rekordbox plays through its own aggregate device, the mod also turns on macOS's drift compensation there, which
rekordbox leaves off.

## Using it

In rekordbox's **Preferences > Audio** there is a section **Speakers and headphones** (marked *platendoos ·
outputsync*), right under the audio device:

- **Line up speakers and headphones**: the switch (`outputsync_lineup`).
- A **fine-tune slider** under it, in ms on top of the reported difference (to the right the headphones play later;
  `outputsync_offset`).
- A **metronome** switch: clicks on every output, so you can hear whether they line up and tune by ear. Every fourth
  click is higher, so you can tell which click of the speakers belongs to which of the headphones.
- A line that says what is lined up now. While rekordbox plays to one device there is nothing to line up: the
  slider and the metronome are greyed out, and the line says so.

The switch and the fine-tune are also in the [settings window](./badge#the-settings-window), under **Decks &
sound**.
- **Refresh audio devices** (also in the settings window): rekordbox looks for audio devices again and opens the
  device you have selected anew. For a device you plugged in or changed that doesn't show up or stays silent,
  without restarting rekordbox. The sound stops for a moment. Your choice of device stays as it is.

Bluetooth is often later than it reports. Then set `outputsync_ms=` a number of milliseconds and tune it by ear while
both play; it's picked up within seconds.

## Options

| Option | Default | What it does |
|---|---|---|
| `outputsync` | `1` | `0` switches the mod off (restart). |
| `outputsync_lineup` | `1` | The switch in Preferences > Audio ("Line up speakers and headphones"). |
| `outputsync_offset` | `0` | Fine-tune in ms on top (+ = headphones later): the slider under that switch. |
| `outputsync_ms` | `auto` | `auto`: as the devices report; a number: delay the faster devices by that many ms (re-read live). |
| `outputsync_clocks` | `0` | `1` logs every 30 s how fast each device's clock really runs and how far they drift apart. |

## Windows

macOS only for now: a Windows version needs its own audio device layer.

## Related

[audiomem](./audiomem) · [Performance and dropouts](/guides/performance)
