# fastwave

The deck waveforms come back sooner after they change, e.g. when you mute or unmute a stem or load a track.

<ModInfo name="fastwave" group="Browsing and playing" />

## Why

When a deck's waveform has to be rebuilt, rekordbox throws the old one away at once, then waits a fixed **half
second** before it even starts building the new one (the build itself takes about 0.1 s), and checks every 100 ms
whether it's done. Measured on a playing deck, the new build started exactly 0.50 s after each stem toggle: the
waveform was blank for most of a second every time.

## What it does

It waits 30 ms instead (`fastwave_ms`; still long enough to merge a burst of changes into one rebuild) and checks
every 15 ms. Resizing the window keeps rekordbox's own delay.

## Options

| Option | Default | What it does |
|---|---|---|
| `fastwave` | `1` | `0` switches the mod off (restart). |
| `fastwave_ms` | `30` | Wait before rebuilding a waveform, in ms (rekordbox's own is 500). |

## Related

[stemcache](./stemcache) · [quickdeck](./quickdeck) · [flx4](./flx4) (stem pads)
