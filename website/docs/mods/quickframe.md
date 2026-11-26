# quickframe <span class="pd-tag">mac</span>

Clicks show on screen sooner. rekordbox queues every frame three deep before it reaches the display; this keeps the
queue at two frames, and shows each frame as soon as it is drawn while no deck is playing.

<ModInfo name="quickframe" platform="mac" group="Browsing and playing" />

## Why

Everything in rekordbox's main window (buttons, lists, waveforms) is painted into one picture and shown through a
Metal layer that rekordbox leaves at macOS's defaults: three frames in flight, each presented in step with the
display. At the full frame rate rekordbox fills that queue, so every frame waits behind two others.

Measured on a MacBook Pro with a 120 Hz display, from a button asking to be repainted to the frame being on the
glass:

| | Repaint to screen |
|---|---|
| rekordbox (three frames queued) | 42 to 47 ms |
| Two frames queued | 32 ms |
| Shown as drawn (no deck playing) | 8 to 12 ms until handed to macOS, on the glass at the next refresh or the one after |

What the queue buys is smooth scrolling: a frame that is drawn late is hidden by the ones still waiting. With two
frames the frames on screen are exactly as regular as with three (95 % one refresh apart, the rest two, in both).
Without a queue a late frame shows: 7 to 70 gaps of more than two refreshes in ten seconds of a playing deck.

## What it does

- Two frames in the queue instead of three, always.
- While no deck plays, nothing scrolls, and each frame goes to the screen as soon as it is drawn. While a deck
  plays, the display's own rhythm is kept, so the waveforms scroll as smoothly as before.

The window is put on screen by macOS's window server either way, so nothing tears.

[framerate](./framerate) decides how often a frame is drawn; this mod, how long a drawn frame takes to show.

## Options

| Option | Default | What it does |
|---|---|---|
| `quickframe` | `1` | `0` switches the mod off (restart). |
| `quickframe_drawables` | `2` | Frames queued for the display: `2`, or rekordbox's own `3`. |
| `quickframe_playing` | `0` | `1` shows frames as they are drawn while a deck plays too: quicker clicks, but a late frame then shows as a hitch in the waveform. |
| `quickframe_stopped` | `1` | `0` keeps the display's rhythm while no deck plays too. |

## Related

[framerate](./framerate) · [relayout](./relayout) · [fontcache](./fontcache)
