# framerate

Runs the screen at full frame rate while a deck plays or you're using the computer, and at 30 fps the rest of the
time.

<ModInfo name="framerate" group="Quieter in the background" />

## Why

rekordbox's window runs at the display's full refresh rate (up to 120 fps) as soon as any deck holds a track, even a
paused one, and shows a new frame every time whether anything changed or not. That keeps the computer warm and the
battery draining while you're doing nothing.

With empty decks it does the opposite: 30 fps, always. And the frame rate isn't only the waveforms': every button,
list and hover in the main window is painted into one picture that is shown at the next frame. At 30 fps a click
shows up to 33 ms late (measured: 25 ms in the middle from a button's repaint to the start of the frame that shows
it, against 3.5 ms at the full rate), so browsing and preparing with nothing loaded felt sluggish.

## What it does

Runs the window at the full rate while something is happening: a deck is playing, or the mouse or keyboard was used
in the last few seconds (`framerate_idle_after`, 3 s), with or without a track on a deck. The rest of the time it
runs at 30 fps. `framerate_empty=0` keeps rekordbox's own 30 fps while the decks are empty.

[quickframe](./quickframe) shortens the way from a finished frame to the screen.

::: warning Scratching a paused deck with a controller?
A controller's jog wheel on a paused deck isn't input the computer sees as you using it. If you scratch that way, set
`framerate=0`.
:::

## Options

| Option | Default | What it does |
|---|---|---|
| `framerate` | `1` | `0` switches the mod off (restart). |
| `framerate_idle_after` | `3` | Seconds without input before the window drops to 30 fps. |
| `framerate_empty` | `1` | `0` keeps rekordbox's 30 fps while the decks are empty, also while you use the mouse. |

## Related

[quickframe](./quickframe) · [relayout](./relayout) · [screeninfo](./screeninfo) · [midipoll](./midipoll) · [fontcache](./fontcache)
