# betterstems

Separates stems with **HTDemucs v4** instead of rekordbox's own engine: cleaner stems (about 9 dB SDR against
Spleeter's 5.8), on the GPU, about 1 s per 12 s of audio.

<ModInfo name="betterstems" group="Browsing and playing" needs="[stemcache] (it is stemcache's engine)" />

## Why

rekordbox separates stems with Spleeter by default (about 5.8 dB SDR on the MUSDB18 test set) or, as its quality
option, Hybrid Demucs v3 (about 7.7). Meta's Hybrid Transformer Demucs v4 scores about 9 dB, and on a Mac's GPU it
is faster than either of rekordbox's engines on the processor (about 0.25 s per 7.8 s of audio on an M2 Pro).

## What it does

[stemcache](./stemcache) hands each chunk to HTDemucs instead of rekordbox's engine, and stores the result apart from
rekordbox's own stems (so switching betterstems on or off never mixes the two). Without the model, the mod is off.

- **macOS:** the model runs on Core ML; it is compiled into the app (103 MB), so a copied app has it too.
- **Windows:** the ONNX version runs through rekordbox's own ONNX Runtime, on the GPU (DirectML) when there is one
  that separates fast enough (a Radeon RX 580 does a track in about 50 s). Without one, rekordbox's own engine
  separates, as without this mod: HTDemucs on the processor would keep most cores busy for minutes per track
  (`betterstems_slow=1` uses it anyway).

### The beat grid goes first

On every platform, stems wait for a track's beat grid: while a track is in beat detection, the stems hold back for a
few seconds, so the grid isn't later because of them. The log says `betterstems: waited 2.1 s for an analysis' beat
stage`. With it, beat detection under a deck load takes as long as alone (3.7 s against 3.8 s; 5.7 s before).

### Loaded once at launch

The model is loaded once per launch, one model at a time with the others (Beat This!, HTDemucs, the genre model,
about 6 s together in the background). On macOS it does one run on silence at launch (the first run is slow), so the
first track of the night doesn't pay for it.

## Options

| Option | Default | What it does |
|---|---|---|
| `betterstems` | `1` | `0` switches the mod off (restart): rekordbox's own engine separates. |
| `betterstems_grid_first` | `1` | `0`: stems of a track that is being analysed start at once, next to its beat detection. |
| `betterstems_live_rest` | `0` | While a deck plays: a rest after each model segment, this many times the segment's own time (`1` = the stems take twice as long and the GPU is free half the time). |
| `betterstems_warmup` | `1` | `0`: no first run of the model at launch (macOS). |
| `betterstems_slow` | `0` | Windows: `1`: HTDemucs also where it is slow (on the processor, or on a GPU that needs over a second for a segment); `0`: rekordbox's own engine separates there. |
| `gpu` | `1` | Windows: `0` keeps the models off the GPU (stems then come from rekordbox's own engine). Restart. |

## Notes

- The model is MIT-licensed (Meta). `tools/models/htdemucs_coreml.sh` remakes it from Meta's weights (downloads about
  1 GB of Python packages); see [Building from source](/internals/building).
- Stems need rekordbox's STEMS feature: *Preferences > Extensions > STEMS*.

## Related

[stemcache](./stemcache) · [autocue](./autocue) (separates per stem with it) · [Windows notes](/guides/windows) ·
[Loading a track](/guides/loading-a-track)
