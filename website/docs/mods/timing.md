# timing

Logs how long each step of an analysis takes.

<ModInfo name="timing" group="Analysis" />

## What it logs

Each analysed track's line in the log (from the [analysis window](./analyzer)) gets its stages, for example:

```text
load→analysis 4.1s; #1 audio+beats+waveform 5.6s, phrase 1.2s, key 0.8s, similarity 1.9s+
```

Every analysis of the track counts (rekordbox sometimes analyses a loaded track twice): `#1`, `#2`. Use it with
[spotifydl](./spotifydl) to see where a slow load or analysis spent its time; see
[Loading a track](/guides/loading-a-track).

## Options

| Option | Default | What it does |
|---|---|---|
| `timing` | `1` | `0` switches the mod off (restart). |

## Related

[spotifydl](./spotifydl) · [analyzer](./analyzer)
