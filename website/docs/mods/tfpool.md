# tfpool

Keeps rekordbox's beat-detection neural network loaded between tracks instead of reloading it for each one.

<ModInfo name="tfpool" group="Analysis" />

## What it does

For every track, rekordbox's beat detection loads its neural network from disk, runs it once and deletes it again.
Loading takes about 0.3-0.5 s, about 15 % of beat detection. tfpool keeps a pool of loaded networks: a track takes a
loaded one, and returns it when done. Each is used by one analysis at a time, as before, and gives the same beat
grids.

At launch it loads and runs the network once (the warm-up), so the first track of the night doesn't pay for it.

## Options

| Option | Default | What it does |
|---|---|---|
| `tfpool` | `1` | `0` switches the mod off (restart). |
| `tfpool_warmup` | `1` | `0` skips the warm-up at launch. |

The warm-up is macOS only.

## Related

[parallel](./parallel) · [streamio](./streamio) · [Loading a track](/guides/loading-a-track)
