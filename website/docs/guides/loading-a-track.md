# Loading a track

What rekordbox does when a track goes on a deck, what platendoos adds, and how the extra work is ordered so
that the deck you just loaded gets what you wait for first. The example is the heaviest case: a Spotify track
<!--@public load-intro-->
that was never loaded before (not analysed).
<!--@/public-->

## What rekordbox does by itself

1. It asks Spotify for the track and downloads it in pieces. The deck can play after a first small buffer.
2. A track that was never analysed is analysed: one pass over the audio (waveforms), then beat detection, then
   phrases and key. The deck gets its waveforms and beat grid when the whole analysis is finished.
<!--@public load-plain3-->
3. Only the analysis is kept: the next load streams again. Spotify tracks get no stems.
<!--@/public-->

## What platendoos adds

| When | Mod | What it does |
|---|---|---|
<!--@private load-row-a-->
<!--@public load-row-stems-->
| The download starts | [**stemcache**](/mods/stemcache), [**betterstems**](/mods/betterstems) | Separates stems from the first load (rekordbox never does for Spotify), with HTDemucs. A Spotify track's stems stay in memory (never on disk), so a second load in the same session has them at once; a local track's are stored for next time. |
<!--@/public-->
| The analysis | [**tfpool**](/mods/tfpool), [**parallel**](/mods/parallel), [**streamio**](/mods/streamio) | Make rekordbox's own analysis faster: its beat model stays loaded, the heaviest part runs on three cores, the audio is read once and shared. |
| Next to rekordbox's beat detection | [**bettergrid**](/mods/bettergrid) | Runs the Beat This! tracker on the same audio and combines the two grids. The track's genre picks which tempo is meant (140, not 70); with no genre known, the genre model listens right then. |
| The analysis | [**betterkey**](/mods/betterkey) | Detects the key with its own network, on the audio already read. |
| During the analysis | [**quickdeck**](/mods/quickdeck) | Puts things on the deck as they are ready, not at the end: the waveforms, then a first Beat This! grid, then the final grid. |
| After the analysis | [**bettergrid**](/mods/bettergrid), [**genres**](/mods/genres) | The BPM shown for the track becomes the saved grid's tempo. With *Genre first* on, an empty Genre field is filled. |
<!--@public load-row-throughout-->
| Throughout | [**spotifyfix**](/mods/spotifyfix), [**artcache**](/mods/artcache) | Guard against rekordbox's Spotify lock-up; keep the cover in memory. |
<!--@/public-->

[**spotifydl**](/mods/spotifydl) and [**timing**](/mods/timing) only write to the log: where the time of this load went.

Not on a load: auto cues (only from their window). A track that is already analysed isn't analysed again; for it
<!--@public load-already-->
a load is the download and the stems.
<!--@/public-->

## What you see, and when

Measured on an M2 Pro (10 cores) with rekordbox 7.2.20, nothing else playing, a 3.5-minute track:

| | Time |
|---|---|
| Deck playable | 0.1 s after the load |
| Waveforms on the deck | about 2.5 s after the analysis starts |
| First beat grid (Beat This!) | about 4.7 s |
| Final grid | about 6.3 s |
| Analysis finished | about 7.7 s |
| Stems | start when beat detection is done; a 4-minute track is separated about 20 s after the load |

Most of that is rekordbox's own beat detection (3.5 to 5 s for this track with nothing else running; it depends
on the track). Beat This! takes 0.5 to 1 s next to it and is nearly always done first. When it isn't (a
Windows graphics card needs about a second per minute of track), the deck shows rekordbox's own grid the moment
rekordbox has it, as plain rekordbox would, and the combined grid replaces it when it is ready
(`bettergrid_rb_first=0`: the deck waits for the combined one). Without [**quickdeck**](/mods/quickdeck) the deck shows nothing
until "analysis finished".

## How the work is ordered

Three neural networks run on the graphics card (Beat This!, HTDemucs and the genre model), next to rekordbox's
own beat detection on every core. Left to themselves they get in each other's way. The rules:

- **One model at a time when rekordbox starts.** The models are loaded once per launch and stay loaded until
  rekordbox quits. Loading two at the same moment is far slower than one after the other (measured: 24 to 64 s
  for HTDemucs instead of 1 to 3 s), so every mod hands its launch work to one queue (core's `startupTask`):
  Beat This!, then HTDemucs, then the genre model, about 6 s together, in the background. rekordbox is usable
  straight away; a track loaded within those seconds waits for the model it needs.
- **A first run at launch (macOS).** A model's first run is slow (HTDemucs: 0.8 s for a segment that takes 0.3 s
  afterwards; longer when something else runs). Each model does one run on silence at launch, so the first track
  of the night doesn't pay for it. rekordbox's own beat model is warmed up the same way ([**tfpool**](/mods/tfpool)).
- **The beat grid before the stems.** While a track is in beat detection, stems wait (at most 8 s per segment).
  With it, beat detection under a deck load takes as long as alone (3.7 s against 3.8 s; 5.7 s before). The log
  says `betterstems: waited 2.1 s for an analysis' beat stage`.

- **One analysis at a time while a deck plays.** A run from the analysis window analyses several tracks at once
  (`analyze_parallel`); while a deck is playing it takes one at a time, so a run started during a set doesn't
  take the cores the audio needs (`analyze_live=1`: how many at once then; `0`: the run waits until the decks
  have stopped).

The switches, for testing (all on by default; `0` turns one off):

| Config | What it does |
|---|---|
| `betterstems_grid_first` | Stems wait while an analysis is at its beat stage. |
| `betterstems_warmup`, `bettergrid_warmup` | The model's first run at launch (macOS). |
| `genres_warmup` | The genre model loads at launch; with `0`, when it is first needed. |
| `tfpool_warmup` | rekordbox's beat model is loaded and run once at launch. |
| `bettergrid_rb_first` | While the analysis waits for Beat This!, the deck shows rekordbox's grid meanwhile. |

## On Windows

The same order applies; the models run through DirectML on the graphics card. What differs:

- A first run costs nothing extra there, so there are no first runs at launch; one stems segment and one Beat
  This! chunk are run on a GPU only to time it.
- Beat This! is slower than on a Mac (about 0.9 s per minute of track on a Radeon RX 580), and the analysis
  waits for it. The deck doesn't: it shows rekordbox's grid 0.1 s after the waveforms, and the combined grid
  replaces it when Beat This! is done (measured: 4.1 s later for a 4-minute track).
- Without a usable GPU (or with `gpu=0`), nothing heavy is added to a deck load: rekordbox's own engine makes the
  stems, and a track on a deck keeps rekordbox's grid at once. The beat grid then shows about 8 s after the load
  on a six-core PC, as with the mods off, instead of 18 to 31 s. `betterstems_slow=1` and `bettergrid_slow=1`
  give the old behaviour back.

- The mods' threads run one step below normal priority (`worker_priority=-1`), and the waveforms wait up to
  a quarter of a second for rekordbox's grid so that one redraw of the deck shows both (`quickdeck_merge`).
  Before, two redraws came back to back while Beat This! had every core, and the window stood still for 0.35 to
  0.4 s; now about 0.13 s, as in plain rekordbox.
- No dropout was found there: 42 recorded deck loads next to a playing deck, plain and with the mods, with a GPU
  and with `gpu=0` at 89 to 100% CPU. (Windows' shared audio there has a 10 ms period and absorbs a 5 ms hold-up;
  the Mac measured above runs 2.9 ms blocks.)

The measurements, per case, are in [Windows internals](/internals/windows) under *Speed* and *A deck load while another deck
plays*.


## Reading the log when a load feels slow

Every step above is logged. [spotifydl](/mods/spotifydl) says how long Spotify took to answer and to download the
track, [timing](/mods/timing) how long each analysis stage took, and the stems and grid mods when they waited for
each other (`betterstems: waited 2.1 s for an analysis' beat stage`). A cache drive that was slow says so too
(`slowdrive: ...`). See [Caches and external drives](/guides/caches) and
[Performance and dropouts](/guides/performance).
