# relayout <span class="pd-tag">mac</span>

Menus, drop-downs, tooltips and Preferences open sooner, resizing the window is about twice as smooth, and options
that made the whole window refresh answer in 80 ms instead of 200: rekordbox no longer lays out and renders what
didn't change, or what you can't see.

<ModInfo name="relayout" platform="mac" group="Browsing and playing" />

## Why

Three things rekordbox does over and over on its main thread, where every click waits its turn:

**It lays out its whole main window whenever any window opens.** rekordbox asks macOS to be told when a window
lands on a screen, for all windows, and answers each time by laying out the main window: all four decks, the mixer,
the browser. A pop-up menu, a drop-down, a tooltip and Preferences are windows too, so each of them cost 15 to 45 ms
before it showed.

**It renders button images that haven't changed.** A layout pass (a panel shown or hidden, another deck layout, a
track loaded, the window resized) tells every hot cue pad, memory cue and similar button where its picture goes,
also the ones that stay where they are. Each time, the button throws away its eight images and renders them again.
While the window is dragged to another size that was more than half of the main thread's work: about 300 buttons
per step, and at launch alone some 6000 renders whose result was identical to what was there.

**It lays out and restyles panels nobody sees.** rekordbox's main window is made of 16,700 parts, and about 400 of
them are on screen. Every panel exists all the time: 64 sampler slots, 512 user pads, the lighting panel, the grid
and cue editors of all four decks. A layout pass lays all of them out, shown or not: a quarter of the pass. And some
options (auto gain and needle lock under *Preferences > Controller*, the beat count mode, the mixer mode, ...) tell
the whole window to refresh how it looks: 190 to 270 ms in which rekordbox doesn't react, half of it the 64 hidden
sampler slots reading their icons from disk and drawing their buttons again.

## What it does

- Only the main window landing on another screen lays it out again.
- A button keeps its images when everything they are made from is the same as when they were made: the picture, its
  place, the button's size and the display's scale.
- A hidden panel (the sampler and its slots, the lighting panel, the user pads, hot cue pads and the grid and cue
  editors of decks that aren't shown) skips its layout and its refresh, and does them the moment it is about to be
  seen. Pictures of the window taken with and without this, after resizing and then opening the sampler or
  switching the deck layout, are identical byte for byte.

Measured on a MacBook Pro (M2 Pro), main thread busy:

| | rekordbox | With relayout |
|---|---|---|
| Preferences opens | 100 ms | 58 ms |
| The layout drop-down opens | 60 ms | 14 to 32 ms |
| Window dragged through 40 sizes | 3.3 s | 1.6 s |
| An option that refreshes the whole window | 197 ms | 79 ms |

`relayout_check=1` is a test switch: it renders every button anyway and logs each one whose new images differ from
the ones it would have kept. In a run with loads, playback, resizes and panels shown and hidden, 42,489 renders came
out the same and none different.

## Options

| Option | Default | What it does |
|---|---|---|
| `relayout` | `1` | `0` switches the mod off (restart). |
| `relayout_hidden` | `1` | `0` lays out and refreshes hidden panels every time, as rekordbox does. Can be switched while rekordbox runs. |
| `relayout_log` | `0` | `1` logs each second how many buttons kept their images and how many were rendered, and which hidden panels waited or caught up. |
| `relayout_check` | `0` | `1` renders every button anyway and logs those whose images differ (for testing). |

## Related

[quickframe](./quickframe) · [framerate](./framerate) · [fontcache](./fontcache) · [screeninfo](./screeninfo)
