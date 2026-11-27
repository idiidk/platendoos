# stemcache

<!--@public stemcache-intro-->
Keeps rekordbox's separated stems on disk, so a track you've had on a deck before has its stems ready at once instead
of separating again. Separates whole playlists ahead of time, and gives **Spotify tracks** stems on the deck.

<ModInfo name="stemcache" group="Browsing and playing" needs="[betterstems] separates with HTDemucs" />
<!--@/public-->

## Why

rekordbox separates a track into stems (its STEMS feature) every time it loads, in chunks of about 11-12 s, and
throws them away shortly after the track leaves the decks. Load it again and it separates again. And it never
separates Spotify tracks at all.

## What it does

- Every chunk rekordbox (or [betterstems](./betterstems)) separates is stored. A chunk whose audio was separated
  before (same engine, model version and stem count) comes from disk instead. Chunks are found by the audio itself,
  so a track can never get another track's stems.
<!--@public stemcache-spotify-->
- **Spotify tracks get stems** on a deck, from the first load: separated from the audio as it streams in, and
  never written to disk. The last ones are kept in memory, compressed (`stemcache_memory_mb`, 384 MB by default),
  so loading the track again in the same session has its stems at once; after a restart they're separated again.
<!--@/public-->

## The stems window

Right-click a playlist and choose **Open stems window** (or select tracks and use the track menu). It shows which of
the playlist's tracks are separated.

<!--@private stemcache-window-shot-->

- **Separate stems** separates the whole playlist ahead of time, one track at a time, with progress per track:
  about 4 s per track with rekordbox's engine, about 15 s with [betterstems](./betterstems), a few seconds when the
  stems are already on disk. **Stop** stops it.
<!--@public stemcache-window-->
- Statuses: *Already separated*, *Separated*, *Separating 2/32* (chunks), *Waiting*, and *On a deck* for Spotify
  tracks: they aren't separated ahead of time, but get their stems on a deck.
<!--@/public-->
- A track that's on a deck counts once the deck has separated it.

## Which stems are kept

A track gets stems in one of two ways: you load it onto a deck (rekordbox separates it then), or you separate it
ahead of time with the **Separate stems** button in the stems window (right-click a playlist, *Open stems window*).
**Save stems to disk** at the bottom of that window (the option `stemcache_keep`) says which of those are saved, so
they are ready at once the next time:

| | |
|---|---|
| **Everything** (`all`, the default) | Every track that gets stems is saved, both ways. |
| **Only what I separate in this window** (`chosen`) | Only tracks you separate with the **Separate stems** button are saved. A track you just load onto a deck still gets stems, but they are thrown away when rekordbox quits (until then they stay in memory, so loading it again in the same session is quick: `stemcache_memory_mb`). |
| **Nothing** (`none`) | Nothing new is saved, also not with **Separate stems**. |

For example: put your 300 favourite tracks in a playlist, open the stems window on it, choose **Only what I separate
in this window** and press **Separate stems** once. Those 300 are saved; everything else you play gets its stems
when needed and never fills your disk.

Stems that are on disk already are used whatever this says, and the size limit (`stemcache_max_gb`) still applies:
when the stored stems grow past it, the ones used longest ago go first. For a few hundred tracks you want to keep,
raise it: a 5-minute track takes about 35 MB.

## Stems on demand

rekordbox separates every track the moment it loads onto a deck, whether you use its stems or not. With **Decks:
separate only when a stem control is used** in the stems window (`stemcache_ondemand=1`) it waits:

- Stems that are stored (or still in memory) are there at once, as always.
- Any other track plays unseparated. The moment you **mute a stem**, turn a **PART ISOLATOR** knob away from its
  centre, or take the **FX off a stem**, on screen or on a controller, the separation starts, beginning with the
  part of the track that is playing. With [betterstems](./betterstems) on an M2 Pro the muted stem drops out about
  a second after the press, anywhere in the track; the rest of the track follows in the background.
- A control that is already on when a track loads (a mute you left on) asks for the new track's stems right away.

Until the stems are there the deck plays the whole track, as rekordbox does during any separation. To be ready
that fast, the first piece of a track (about 12 seconds of it, half a second of work) is still separated when it
loads.

With rekordbox's *STEMS Analysis Process* set to its quality engine, rekordbox itself separates strictly in order
from the start of the track, so a press late in a track takes longer there; with the default engine it starts at
the playhead.

On Windows it is the same code; it was run on a Windows PC with rekordbox 7.2.19 (and checked against 7.2.20 in an
emulator).

## Quality and size

Stems are stored as AAC:

| `stemcache_quality` | Per 5-minute track |
|---|---|
| `256` (default) | ~35 MB |
| `192`, `128` | less |
| `lossless` | ~125 MB |

Stems are only heard while a stem is turned down (rekordbox plays the original track otherwise), so 256 kbps is
inaudible in practice. Windows' encoder goes up to 192 kbps, so there 256 is stored at 192. Older lossless chunks are
still used.

The cache uses up to 10 GB (`stemcache_max_gb`; the least recently used chunks go first) and stops storing when the
disk has less than 5 GB free (`stemcache_min_free_gb`).

## On another drive

Set `stemcache_dir` to a folder on a USB stick, SD card or external disk. New chunks then go to the internal disk
first (`stems-staging`) and one background thread moves them over, a piece at a time, never while a deck or an
analysis reads from that drive. Past `stemcache_staging_gb` a deck's chunks are left out (separated again next time)
and the stems window's run waits. See [Caches and external drives](/guides/caches).

## Options

| Option | Default | What it does |
|---|---|---|
| `stemcache` | `1` | `0` switches the mod off (restart). |
<!--@public stemcache-opt-spotify-->
| `stemcache_spotify` | `1` | `0`: no stems for Spotify tracks (otherwise: on a deck, in memory). |
| `stemcache_memory_mb` | `384` | Memory kept for Spotify tracks' stems, compressed, so a track loaded again this session has them at once (`0` = none). |
<!--@/public-->
| `stemcache_keep` | `all` | Which tracks' stems are saved: `all`, `chosen` (only tracks separated with the stems window's *Separate stems* button) or `none`. See [Which stems are kept](#which-stems-are-kept). |
| `stemcache_ondemand` | `0` | `1`: a deck's track is separated only once a stem control is used on it. See [Stems on demand](#stems-on-demand). |
| `stemcache_quality` | `256` | How new chunks are stored: `lossless`, `256`, `192` or `128` (AAC kbps per stem). |
| `stemcache_max_gb` | `10` | Delete the least recently used chunks above this size (`0` = no limit). |
| `stemcache_min_free_gb` | `5` | Store nothing new while the disk has less free space than this. |
| `stemcache_dir` | *(empty: the default folder)* | Folder of the cached stems. Default: `~/Library/Caches/rekordbox-mods/stems` (macOS), `%LOCALAPPDATA%\rekordbox-mods\cache\stems` (Windows). |
| `stemcache_staging` | `auto` | Stage new chunks on the internal disk first. `auto`: only when `stemcache_dir` is on an external drive; `1`: always; `0`: never. |
| `stemcache_staging_gb` | `4` | How much may wait on the internal disk; past it a deck's chunks are left out and the stems window's run waits. |
| `stemcache_log` | `0` | `1` logs every chunk and every stem control (for debugging). |

## Needs rekordbox's STEMS

Stems need rekordbox's STEMS feature: *Preferences > Extensions > STEMS*.

## Related

<!--@public stemcache-related-->
[betterstems](./betterstems) · [fastwave](./fastwave) · [flx4](./flx4) (stem pads and knobs) ·
[Spotify guide](/guides/spotify)
<!--@/public-->
