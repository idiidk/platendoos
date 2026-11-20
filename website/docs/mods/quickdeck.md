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

## Options

| Option | Default | What it does |
|---|---|---|
| `quickdeck` | `1` | `0` switches the mod off (restart). |
| `quickdeck_merge` | `0.25` | Windows: seconds the waveforms wait for rekordbox's own grid, so that one redraw of the deck shows both (`0`: no wait). |

## Windows

On Windows rekordbox's own grid is usually ready before Beat This!'s, so it comes in the same redraw as the
waveforms (`quickdeck_merge`), and the combined grid replaces it when it's done. One redraw instead of two cut the
window's pause at a load from 0.35-0.4 s to about 0.13 s, as in plain rekordbox.

## Related

<!--@public quickdeck-related-->
[bettergrid](./bettergrid) · [fastwave](./fastwave) ·
<!--@/public-->
[Loading a track](/guides/loading-a-track)
