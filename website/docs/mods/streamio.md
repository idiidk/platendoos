# streamio

Smarter reading of a track's audio during analysis. That's about 10 % faster overall, and the phrase analysis is
4× faster.

<ModInfo name="streamio" group="Analysis" />

## What it does

- rekordbox's analysis reads 512 frames at a time and gives its core away after each (about 90,000 times per track),
  and between the stages of every block too. With several tracks analysed at once, that adds up. streamio skips those
  hand-overs: measured on 50 tracks with six at once, 7-12 s faster per run, about 10 %.
- Phrase analysis then reads the whole track twice more, decoding it again each time. streamio keeps the audio pass's
  samples instead (exactly the same values; tracks up to 15 minutes, about 160 MB) and the phrase passes read those.
  On Windows the phrase stage went from 0.7-0.9 s to 0.2-0.3 s, with byte-identical analysis files.

## Options

| Option | Default | What it does |
|---|---|---|
| `streamio` | `1` | `0` switches the mod off (restart). |
| `streamio_phrase` | `1` | `0` reads the audio again for phrase analysis. |

## Related

[parallel](./parallel) · [tfpool](./tfpool) · [analyzer](./analyzer)
