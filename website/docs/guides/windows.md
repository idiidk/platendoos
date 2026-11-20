# Windows notes

platendoos runs on rekordbox for Windows (x64) with the same mods as on macOS, minus the six macOS-only ones. This
page is what's different for you; how it works inside is in [Windows internals](/internals/windows).

## How it starts

`platendoos.exe` starts your installed rekordbox with the mods loaded, without changing any of rekordbox's files. It
finds rekordbox by itself (`platendoos.exe --rekordbox <path to rekordbox.exe>` otherwise). The patcher installs it in
`%LOCALAPPDATA%\Programs\platendoos` with a Start menu shortcut; see [Installation](/getting-started/installation#windows).

The mods' windows need **WebView2**, which Windows 11 has built in.

## Which mods

| | Mods |
|---|---|
<!--@public windows-mods-->
| On Windows (28) | analyzer, artcache, autocue, badge, bettergrid, betterkey, betterstems, fastwave, flx4, framerate, genres, keepmaster, keycolumn, multisort, parallel, quickdeck, spotifyapi, spotifyaudio, spotifydl, spotifyfix, spotifylog, spotifystart, stemcache, streamio, tfpool, tidymenu, timing, xfadelock |
<!--@/public-->
| macOS only (6) | audiomem, betterfx, outputsync (not ported yet); fontcache, screeninfo, midipoll (fix macOS calls Windows doesn't have) |
| Windows only | spotifystart (rekordbox 7.2.19 for Windows) |

Smaller differences: the collection BPM following bettergrid's grid is macOS only; tfpool's warm-up is macOS only;
stems are stored at most at 192 kbps AAC (Windows' encoder).

## The GPU (DirectML)

[betterstems](/mods/betterstems), [bettergrid](/mods/bettergrid) and [genres](/mods/genres) run their models through
rekordbox's own ONNX Runtime, on the graphics card through **DirectML** (DirectX 12) when there is one that is fast
enough. The log says what they use.

- **HTDemucs stems** only on a GPU that separates a segment within 1 s (timed at launch; the log says e.g. "a segment
  takes 0.47 s on the GPU (DirectML)"). Otherwise rekordbox's own engine makes the stems: HTDemucs on the processor
  would keep most cores busy for two minutes per track. `betterstems_slow=1` uses it anyway.
- **Beat This! grids**: where it is slow (the processor, or a GPU that needs over a second for 30 s of audio), a
  track loaded on a **deck** gets rekordbox's beat grid at once instead of waiting 15 to 25 s for Beat This!. Tracks
  analysed from the analysis window still get the Beat This! grid. `bettergrid_slow=1` waits on a deck too.
- **`gpu=0`** (restart rekordbox) keeps the models off the GPU: for when stems sound or grids look wrong on your PC.
  Beat This! and the genre model then run on the processor, and stems come from rekordbox's own engine.

Without a usable GPU, a deck load adds nothing heavy: the beat grid shows about 8 s after the load on a six-core PC,
as with the mods off (instead of 18 to 31 s before 0.3.0).

A GPU run that fails later (a driver reset, out of memory) moves that model to the processor and is retried.

## Measured on a PC

On an i5-9600K (6 cores) with a Radeon RX 580 and 16 GB, rekordbox 7.2.20, a Spotify track loaded on a deck for the first
time, never analysed:

| | Track | Playable | Waveform | Beat grid | Stems done |
|---|---|---|---|---|---|
| GPU (the defaults), nothing playing | 4:56 | 0.8 s | 6.9 s | 11.2 s | ~50 s (HTDemucs) |
| `gpu=0`, the defaults since 0.3.0, another deck playing | 6:45 | 0.6 s | 7.7 s | 7.9 s | ~22 s (rekordbox's engine) |

Seconds from the load. All the cases are in [Windows internals](/internals/windows#speed).

No dropout was found: 42 recorded deck loads next to a playing deck, plain rekordbox and platendoos, with and without
a GPU. (Windows' shared audio there has a 10 ms period and absorbs a short hold-up.)

## Thread priority

The mods' own threads run one step below normal priority, so rekordbox's window and drawing go first:

| `worker_priority` | Priority of the mods' threads |
|---|---|
| `-1` | One step below normal (the default with four or more logical cores) |
| `0` | Normal, as before 0.3.0 (the default with fewer cores) |
| `-2` | Two steps below normal |

A newly loaded, never-analysed track shows its waveforms and rekordbox's beat grid in one redraw
(`quickdeck_merge=0.25`: the seconds the waveforms wait for that grid; `0`: no wait).

## Paths

| What | Where |
|---|---|
| Config | `%APPDATA%\rekordbox-mods\config.txt` |
| Log | `%LOCALAPPDATA%\rekordbox-mods\rekordbox-mods.log` |
| Caches | `%LOCALAPPDATA%\rekordbox-mods\cache\` |
| Installed | `%LOCALAPPDATA%\Programs\platendoos` |

## A new rekordbox version

Every rekordbox build for Windows needs its own address table. The patcher carries tables for 7.2.19 and 7.2.20;
for another version it makes one on the spot from the signatures it carries, says how many parts it found, and the
mods missing one are off. Run the patcher again after every rekordbox update.

## Defaults that differ

| Option | macOS | Windows |
|---|---|---|
| `analyze_timeout` | 90 s | 180 s |
| `bettergrid_wait` | 20 s | 60 s |

A slower PC needs longer waits.
