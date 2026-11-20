# Loading a track: what happens, and in what order

What rekordbox does when a track goes on a deck, what platendoos adds, and how the extra work is ordered so
that the deck you just loaded gets what you wait for first. The example is the heaviest case: a Spotify track
that was never loaded or analysed before.

## What rekordbox does by itself

1. It asks Spotify for the track and downloads it in pieces. The deck can play after a first small buffer.
2. A track that was never analysed is analysed: one pass over the audio (waveforms), then beat detection, then
   phrases and key. The deck gets its waveforms and beat grid when the whole analysis is finished.
3. Nothing is kept: the next load streams again. Spotify tracks get no stems, and at most three of them can be
   on the decks.

## What platendoos adds

| When | Mod | What it does |
|---|---|---|
| The download starts | **stemcache**, **betterstems** | Separates stems from the first load (rekordbox never does for Spotify), with HTDemucs, and stores them: next time they are ready at once. |
| The analysis | **tfpool**, **parallel**, **streamio** | Make rekordbox's own analysis faster: its beat model stays loaded, the heaviest part runs on three cores, the audio is read once and shared. |
| Next to rekordbox's beat detection | **bettergrid** | Runs the Beat This! tracker on the same audio and combines the two grids. The track's genre picks which tempo is meant (140, not 70); with no genre known, the genre model listens right then. |
| The analysis | **betterkey** | Detects the key with its own network, on the audio already read. |
| During the analysis | **quickdeck** | Puts things on the deck as they are ready, not at the end: the waveforms, then a first Beat This! grid, then the final grid. |
| After the analysis | **bettergrid**, **genres** | The BPM shown for the track becomes the saved grid's tempo. With *Genre first* on, an empty Genre field is filled. |
| Throughout | **spotifyfix**, **artcache** | Guard against rekordbox's Spotify lock-up; keep the cover on disk. |

**spotifydl** and **timing** only write to the log: where the time of this load went.

Not on a load: auto cues (only from their window). A track that is already analysed isn't analysed again; for it
a load is the download and the stems.

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
(`bettergrid_rb_first=0`: the deck waits for the combined one). Without **quickdeck** the deck shows nothing
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
  of the night doesn't pay for it. rekordbox's own beat model is warmed up the same way (**tfpool**).
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

## The stems cache on an external drive

The stems cache can live on another drive (`stemcache_dir`). A cheap USB stick reads fast (120 MB/s measured)
but can't take writes and reads together: after about 70 MB of writes the one measured answered one request a
second for minutes, and a read waited behind everything queued. So what the mods write to an external drive in
the background gives way to reads:

- **Everything is staged on the internal disk first.** New stems go to `stems-staging` (in the mods' cache
  folder on the internal disk), and they are used from there until they have moved.
- **One writer for all mods, a piece at a time** (at most 1 MB): moving stems, removing the oldest when the cache
  is full. No piece starts while a deck or an analysis reads from the drive, or while a deck loads.
- **Nothing is written to a slow drive while a deck plays.** A drive that once took half a second or more for
  a piece counts as slow for the next 30 minutes; while a deck plays (or played in the last 30 s) nothing is
  written to it. What waits stays on the internal disk and moves once the decks have stopped. The log says so:
  `slowdrive: a deck plays and the cache drive has been slow: nothing is written to it until the decks have stopped`.

What it can't do: make a slow drive fast. Staging has a size (`stemcache_staging_gb=4`); when a long set with
many new tracks fills it, stem chunks are left out (and separated again next time). An SSD in a USB case doesn't
have this problem.

| Config | What it does |
|---|---|
| `stemcache_staging` | `auto` (default): staged when the cache folder is on an external drive; `1` always; `0` never. |
| `stemcache_staging_gb` | How much may wait on the internal disk (4 GB). |

## Clicks when the Mac is short of memory

rekordbox's audio runs in blocks of 128 samples: 2.9 ms each (its audio settings; 256 and 512 are the other
choices). A block that isn't ready in time is silence: a gap of 2.9 ms on every deck, more when the wait is
longer. macOS reports each as an overload, and a recording of rekordbox's output showed one gap per overload.

Almost all of them have one cause: **the audio thread waits for memory that macOS had put away.** When other
apps fill the memory, macOS compresses and swaps out what wasn't used for a few seconds. Measured on a 16 GB Mac
with 6 to 8 GB in swap: 9 to 23 gaps a minute with one deck just playing, and more right after a deck load,
which allocates and clears several hundred MB at once. What the audio thread was waiting for:

- **rekordbox's FX history buffers** (nine in ten): 214 buffers of 3 MB (654 MB) that rekordbox writes all the
  time, FX on or off, a new 16 KB page every tenth of a second per buffer.
- **The playing deck's own audio**: decoded into memory at the load, read minutes later.
- Rarely used parts of the program, now and then.

Plain rekordbox has the same gaps (with every mod off: 32 a minute and 10 per load under the test below), and
none of the mods' own code on the audio thread can wait: no locks, no allocation, no logging.

**audiomem** (macOS) takes the cause away where it can: it pins the FX buffers that are being written, keeps the
next 20 s of every loaded deck's audio in memory (4 s on a deck that isn't playing), and keeps rekordbox's code
and data in memory. Measured with 3 GB of other memory in use on top:

| | Gaps per minute, one deck playing | Gaps per deck load | Longest audio block |
|---|---|---|---|
| Every mod off | 32 | 10 | 37 ms |
| All mods, audiomem off | 30 | 14 | 21 ms |
| All mods, audiomem on (two runs) | 6 and 0 | 1 and 0.1 | 9 and 4 ms |

What it can't cover: a jump further than the 20 s (a hot cue, needle search) and small rarely used objects. The
log says when memory is short (`audiomem: 6.5 GB of swap in use ...`): close other apps before a set, and
consider a buffer size of 256 or 512, since four in five of the waits are shorter than 5.8 ms.

**The window standing still at a load** is rekordbox clearing the new track's buffers on the main thread: about
five buffers of 57 MB for a local file, built twice. With `audiomem_lazy` the buffers come from the system
already empty and nothing is cleared up front: about 0.04 s instead of 0.12 s per load.

On Windows no dropout was found in any run, plain or with the mods (below).

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

The measurements, per case, are in [windows.md](windows.md) under *Speed* and *A deck load while another deck
plays*.
