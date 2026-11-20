# framerate

Only runs the screen at full frame rate while a deck plays or you're using the computer. Otherwise it drops to the
30 fps rekordbox uses with empty decks.

<ModInfo name="framerate" group="Quieter in the background" />

## Why

rekordbox's window runs at the display's full refresh rate (up to 120 fps) as soon as any deck holds a track, even a
paused one, and redraws every frame whether anything changed or not. That keeps the computer warm and the battery
draining while you're doing nothing.

## What it does

Keeps the window at 30 fps unless a deck is playing or the mouse or keyboard was used in the last few seconds
(`framerate_idle_after`, 3 s).

::: warning Scratching a paused deck with a controller?
A controller's jog wheel on a paused deck isn't input the computer sees as you using it. If you scratch that way, set
`framerate=0`.
:::

## Options

| Option | Default | What it does |
|---|---|---|
| `framerate` | `1` | `0` switches the mod off (restart). |
| `framerate_idle_after` | `3` | Seconds without input before the window drops to 30 fps. |

## Related

[screeninfo](./screeninfo) · [midipoll](./midipoll) · [fontcache](./fontcache)
