# midipoll <span class="pd-tag">mac</span>

Checks for newly plugged-in MIDI controllers every 2 seconds instead of every 0.3 seconds, and quickly again whenever
something is plugged in.

<ModInfo name="midipoll" platform="mac" group="Quieter in the background" />

## What it does

rekordbox looks for new or removed MIDI controllers every 300 ms by listing all of macOS's MIDI devices on its main
thread. midipoll runs that check every 2 s instead, and on every turn as before for a few seconds after the number of
MIDI devices changes, so plugging in a controller is still picked up within about 2 s.

Windows doesn't have this slow call.

## Options

| Option | Default | What it does |
|---|---|---|
| `midipoll` | `1` | `0` switches the mod off (restart). |

## Related

[framerate](./framerate) · [screeninfo](./screeninfo) · [flx4](./flx4)
