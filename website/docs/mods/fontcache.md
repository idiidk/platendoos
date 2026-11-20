# fontcache <span class="pd-tag">mac</span>

Makes drawing text, most of the work when the track list repaints, a lot cheaper. Scrolling feels lighter.

<ModInfo name="fontcache" platform="mac" group="Browsing and playing" />

## What it does

rekordbox's interface toolkit builds a fresh font object for every run of text it draws, and macOS then creates a
Core Text font for it. Profiling showed that at about 17 % of the main thread while lists repaint. Those fonts never
change, so fontcache hands out one shared font per typeface and size instead.

Windows doesn't have this slow call, so there's no Windows version.

## Options

| Option | Default | What it does |
|---|---|---|
| `fontcache` | `1` | `0` switches the mod off (restart). |

## Related

[screeninfo](./screeninfo) · [framerate](./framerate)
