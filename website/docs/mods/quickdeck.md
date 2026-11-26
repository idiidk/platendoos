# quickdeck

A Spotify track (or any track that isn't analysed yet) shows its waveforms and a beat grid on the deck as soon as the
analysis has them, instead of only when the whole analysis is done.

<ModInfo name="quickdeck" group="Analysis" needs="[bettergrid] for the early Beat This! grid" />

## Why

rekordbox analyses a newly loaded track in stages: one pass over the audio (it builds every waveform while the stream
downloads), the beat job, then phrase, vocal and key analysis. Only at the very end does it write the waveforms and
tell the deck. Until then the deck shows nothing.

## What it does

The deck is reloaded at three earlier points, with the same call rekordbox makes at the end:

1. **When the beat job starts**: the waveforms are handed over, so the deck shows every waveform right after the
   audio pass.
2. **When bettergrid's Beat This! grid is ready** (it runs next to the beat job and is usually done 1-3 s sooner):
   saved as a first grid, only when Beat This! is sure of it.
3. **When rekordbox's beat job has saved its grid** (bettergrid's final one), before phrase and key analysis.

The reloads only happen for a track that is on a deck.

<figure class="shot"><img src="/img/quickdeck.webp" alt="Deck 1 with a never-analysed Spotify track: waveform, beat grid and 174.00 BPM shown"><figcaption>A never-analysed Spotify track on deck 1, a few seconds after the load: waveforms and grid already there.</figcaption></figure>

Measured on an M2 Pro with rekordbox 7.2.20 and a 3.5-minute Spotify track: waveforms about 2.5 s after the
analysis starts, the first Beat This! grid at about 4.7 s, the final grid at 6.3 s, the analysis finished at 7.7 s.
Without quickdeck the deck shows nothing until that last moment. See [Loading a track](/guides/loading-a-track).

## The overview streams in (macOS)

rekordbox draws the overview waveform (the whole track, under the title) only when it has read the entire track:
two to five seconds in which the deck shows nothing. But it builds that waveform piece by piece while it reads. So
four times a second quickdeck takes what there is, has rekordbox's own code turn it into an overview, and hands it
to the deck directly: the overview grows from the left while the track is still coming in. Each update takes about
3 ms on a background thread and no reload of the deck. When the whole track has been read, the final overview
replaces it as before (it differs by a brightness step or two at most: the levels are scaled to the loudest part,
which isn't known until the end). `quickdeck_stream=0` switches it off.

## Only the grid is redrawn (macOS)

rekordbox itself builds a deck's waveforms again whenever the deck is handed a different beat grid: the waveform
goes blank and comes back. With a first grid and then the final one, that happened twice per load, on top of the
waveforms appearing. Now a new grid (a track's very first one too) only redraws the grid lines, the way rekordbox's own grid editor does it, and
the waveform stays on screen. (The blue waveform styles still get their waveform built once more with the final
grid: their zoomed waveform is written together with it.) `quickdeck_gridonly=0` gives rekordbox's behaviour back.

## Options

| Option | Default | What it does |
|---|---|---|
| `quickdeck` | `1` | `0` switches the mod off (restart). |
| `quickdeck_merge` | `0.25` | Windows: seconds the waveforms wait for rekordbox's own grid, so that one redraw of the deck shows both (`0`: no wait). |
| `quickdeck_gridonly` | `1` | macOS: `0`: a new beat grid builds the deck's waveforms again, as rekordbox does. |
| `quickdeck_stream` | `1` | macOS: `0`: the overview waveform appears when the whole track has been read, as in rekordbox. |
| `quickdeck_log` | `0` | `1` logs each partial overview and how the last one compares with the final one; `2` also checks the waveform style that isn't shown. |

## Windows

On Windows rekordbox's own grid is usually ready before Beat This!'s, so it comes in the same redraw as the
waveforms (`quickdeck_merge`), and the combined grid replaces it when it's done. One redraw instead of two cut the
window's pause at a load from 0.35-0.4 s to about 0.13 s, as in plain rekordbox.

## Related

<!--@public quickdeck-related-->
[bettergrid](./bettergrid) · [fastwave](./fastwave) ·
<!--@/public-->
[Loading a track](/guides/loading-a-track)
