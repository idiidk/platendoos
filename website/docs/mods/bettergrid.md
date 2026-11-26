# bettergrid

Better beat grids. Every analysed track also goes through the **Beat This!** beat tracker, and the two grids are
combined: Beat This!'s tempo and bar starts, with each beat placed exactly on the hits in the audio.

<ModInfo name="bettergrid" group="Analysis" needs="[genres] picks the tempo's octave; [quickdeck] shows its grid on the deck early" />

## Why

Listening tests showed that each is better at something else:

- **Beat This!** finds the tempo, which octave of it is meant (140 or 70) and where the bar starts more reliably.
- **rekordbox** places the beats themselves more exactly (Beat This! works in 20 ms steps).

So bettergrid takes the best of both.

## What it does

When rekordbox analyses a track, its audio (decoded once, shared with [betterkey](./betterkey)) goes through Beat
This! on another thread, next to rekordbox's own beat detection, so analysing isn't slower. When rekordbox's beat
job is done, the combined grid is saved instead of rekordbox's, and the phrase and key analysis use it too.

**When both find the same tempo**, rekordbox's beats stay exactly where they are, and only the bar start comes from
the combination:

- **Where the bar starts.** When rekordbox and Beat This! disagree on the 1, the music decides: the 1 goes on the
  beat where the track changes (a drop, a breakdown, a new bass line), or else on the beat where the kick comes in
  (`bettergrid_bars=0`: the kick alone). On 330 tracks where both agree, it put the 1 right 96 times in 100, against 78
  for the kick rule (0.3.2, not released yet).
- **Grids on the off-beats.** About 3 % of tracks get rekordbox's beats half a beat away, on the off-beats. When Beat
  This! hears the beats there over the whole track and the kick drum says so too (its hits, or its level clearly
  higher between rekordbox's beats), the beats are moved half a beat.
- **Grids on a hi-hat or a clap.** rekordbox sometimes puts its beats a fixed distance from the kicks that is
  neither nothing nor half a beat: 60 to 170 ms early or late over the whole track, on a hat or a clap (Robert
  Hood's *Snakes and Politics*: on the hats, 96 ms after every kick). When nearly all of Beat This!'s beats lie at
  one such distance from rekordbox's, and the kick itself sets in there and clearly not at rekordbox's beats, the
  combined grid is used. Of 15 tracks where the two analyses lay that far apart, this changed two and left the
  rest alone.
- A BPM a hair off a whole one is redrawn at the whole BPM when the hits fit as well; the beat at the very start of
  the track that rekordbox sometimes leaves out is added.

**When rekordbox's tempo is wrong** (half, double, 3:2), the combined grid takes Beat This!'s tempo and bar start,
and puts each beat on the hits in the audio.

**When Beat This! is unsure** (its fit is under `bettergrid_min_fit`), rekordbox's own grid stays.

### The genre picks 140, not 70

Beat This! counts tempos within 70-200 BPM, so a drum & bass track can come out at 83. With the
[genres](./genres) mod, the track's genre (from the genres mod's stored result, rekordbox's Genre field, or the
style model on the audio already decoded) is mapped to the BPMs DJs count that genre in: drum & bass 160-185,
house 115-130, hip hop 80-115 and so on, hardcore included. When the tempo is outside that range and twice or half
of it is inside, the grid goes to that tempo. A genre never brings in a tempo neither analysis found.
`bettergrid_genre=0` turns this off.

### The BPM in the collection follows the grid <span class="pd-tag">mac</span>

rekordbox kept its own BPM number next to the grid, so a track listed as 140 could have a grid at 70 (and play at
70). On macOS, the track's BPM in the collection (the BPM column, the deck's title) follows the saved grid.

### Locked grids, long tracks

Locked grids aren't touched. A track whose beat job doesn't run (a key-only analysis) is left alone. If anything
fails or takes too long, rekordbox's own grid is kept; so is a track longer than the 20 minutes of audio that are
analysed (only its bars can change).

## On the deck

With [quickdeck](./quickdeck), a track that isn't analysed yet shows its grid on the deck as soon as there is one:
the Beat This! grid when it's ready first (usually), rekordbox's own when that is (`bettergrid_rb_first`), and the
combined grid replaces it when it's done.

## Cloud Analysis

A track that rekordbox's **Cloud Analysis** (*Preferences > Analysis*) finds online gets that analysis instead and
skips beat detection, so it keeps rekordbox's grid. Turn Cloud Analysis off for Beat This! grids on every track.

## The BT BPM column and the log

- A **BT BPM** column (`bettergrid_title`) shows Beat This!'s BPM for every track.
- Every analysed track is logged with both grids and added to `rekordbox-bettergrid.csv` next to the log.
- `bettergrid_apply=0` only compares: it logs both grids and keeps rekordbox's.

## Options

| Option | Default | What it does |
|---|---|---|
| `bettergrid` | `1` | `0` switches the mod off (restart): rekordbox's analysis runs exactly as without it. |
| `bettergrid_apply` | `1` | `0`: compare only, log both grids and keep rekordbox's. |
| `bettergrid_min_fit` | `0.6` | Beat This!'s fit below which rekordbox's grid is kept. |
| `bettergrid_bars` | `1` | When the bar starts differ: the music's changes pick one; `0`: the kick-in rule alone. |
| `bettergrid_halfbeat` | `1` | rekordbox's beats half a beat off: `1` moved when Beat This! and the kick band both say so, `2` when Beat This! alone says so, `0` never. |
| `bettergrid_offbeat` | `1` | rekordbox's beats a fixed 60 ms or more off the kicks (not half a beat): `1` the combined grid when Beat This! and the kick both say so, `0` never. |
| `bettergrid_genre` | `1` | The track's genre (genres mod) picks the tempo's octave; `0` never. |
| `bettergrid_rb_first` | `1` | `0`: while it waits, the deck doesn't show rekordbox's own grid meanwhile. |
| `bettergrid_wait` | `20` (macOS), `60` (Windows) | Seconds the analysis waits for the combined grid at most. |
| `bettergrid_warmup` | `1` | `0`: no first run of the model at launch (macOS). |
| `bettergrid_slow` | `0` | Windows: `1` also waits for Beat This! for a track on a deck where it is slow (on the processor, or a GPU that needs over a second for 30 s of audio). |
| `bettergrid_title` | `BT BPM` | Header name of the column. |
| `bettergrid_debug` | `0` | `1` logs the beats of both grids (for debugging). |
| `gpu` | `1` | macOS: `0` keeps bettergrid on the processor. Windows: `0` keeps the models off the GPU (Beat This! and the genre model on the processor, stems from rekordbox's own engine). Restart. |

## Platform notes

- **macOS:** Beat This! runs on Core ML, 0.5 to 1 s per track next to rekordbox's beat detection, and is nearly
  always done first.
- **Windows:** it runs through rekordbox's own ONNX Runtime, on the GPU with DirectML (about 0.9 s per minute of
  track on a Radeon RX 580). Where it is slow (no usable GPU, or `gpu=0`), a track loaded on a **deck** gets
  rekordbox's grid at once instead of waiting 15 to 25 s; tracks analysed from the analysis window still get the
  Beat This! grid. The collection BPM following the grid is macOS only. See [Windows notes](/guides/windows).

## Related

[genres](./genres) · [quickdeck](./quickdeck) · [betterkey](./betterkey) · [analyzer](./analyzer) ·
[Loading a track](/guides/loading-a-track)
