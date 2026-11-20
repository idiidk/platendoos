# screeninfo <span class="pd-tag">mac</span>

Stops rekordbox from asking macOS about your screen on every single redraw.

<ModInfo name="screeninfo" platform="mac" group="Quieter in the background" />

## What it does

rekordbox's interface toolkit repaints on every display refresh and each time asks macOS for the window's screen
description, to read the screen's number. macOS builds a new description for that every time: about 1 % of the main
thread while idle. screeninfo hands out one per screen, rebuilt when the displays change.

Windows doesn't have this slow call.

## Options

| Option | Default | What it does |
|---|---|---|
| `screeninfo` | `1` | `0` switches the mod off (restart). |

## Related

[framerate](./framerate) · [fontcache](./fontcache) · [midipoll](./midipoll)
