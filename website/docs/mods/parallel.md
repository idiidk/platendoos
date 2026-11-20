# parallel

Spreads the heaviest part of analysing a track over three cores instead of one.

<ModInfo name="parallel" group="Analysis" />

## What it does

rekordbox's analysis reads a track in blocks and feeds each block, one after the other on one thread, to the beat
detector (about 55 % of the pass) and the waveform builder (about 35 %). The two only read the block and don't
depend on each other, so each gets its own worker: the analysis thread hands the block over and goes on decoding
the next one. Every block still reaches each in order, so the results are exactly the same.

## Options

| Option | Default | What it does |
|---|---|---|
| `parallel` | `1` | `0` switches the mod off (restart). |

## Related

[tfpool](./tfpool) · [streamio](./streamio) · [analyzer](./analyzer) · [timing](./timing)
