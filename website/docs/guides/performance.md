# Performance and dropouts

platendoos adds heavy work to rekordbox (three neural networks, caching, stems) and takes work away (the
[quieter-in-the-background](/mods/#quieter-in-the-background) mods, faster analysis). This page is about keeping
playback clean while it does.

## The short version

- **macOS:** keep [audiomem](/mods/audiomem) on. Before a set, close other apps if the log says
  `audiomem: N GB of swap in use`. On a Mac that swaps heavily, set rekordbox's audio buffer size to 256 or 512
  (*Preferences > Audio*).
- **Analyse before the set**, not during: while a deck plays, a run takes one track at a time (`analyze_live=1`) or
  waits (`analyze_live=0`).
<!--@public perf-ahead-->
- **Separate stems ahead** for local tracks (the stems window): a track with stored stems needs no separation at a
  load. Analysed Spotify tracks ([Spotify guide](/guides/spotify#preparing-a-set)) skip the analysis at a load.
<!--@/public-->
<!--@public perf-drive-->
- **External stems drive:** an SSD in a USB case, not a cheap stick ([Caches](/guides/caches)).
<!--@/public-->
- **Windows:** see [Windows notes](/guides/windows) for the GPU and `worker_priority`.

## What the mods do during a set

- **Stems wait for the beat grid.** While a track is in beat detection, stems hold back, so a deck's grid isn't late
  because of them (`betterstems_grid_first`).
- **One analysis at a time while a deck plays** (`analyze_live`).
- **The models load one at a time at launch**, about 6 s together in the background; before 0.3.0, loading them all
  at once could take up to a minute.
- **Nothing is written to a slow cache drive while a deck plays.**
- `betterstems_live_rest` can leave the GPU free part of the time while a deck plays (off by default).

## Clicks when the Mac is short of memory (macOS)

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

[**audiomem**](/mods/audiomem) (macOS) takes the cause away where it can: it pins the FX buffers that are being written, keeps the
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

## The window standing still at a deck load

- macOS: with `audiomem_lazy`, about 0.04 s instead of 0.12 s per load.
- Windows: about 0.13 s, as in plain rekordbox, down from 0.35-0.4 s, thanks to `worker_priority` and
  `quickdeck_merge` (waveforms and grid in one redraw).

## Less work while idle

[framerate](/mods/framerate) keeps the window at 30 fps unless a deck plays or you're using the computer;
[fontcache](/mods/fontcache), [screeninfo](/mods/screeninfo) and [midipoll](/mods/midipoll) (macOS) remove slow calls
rekordbox makes all the time; [spotifylog](/mods/spotifylog) stops it rewriting its Spotify log several times a
second.
