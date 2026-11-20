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
| `stemcache_quality` | `256` | How new chunks are stored: `lossless`, `256`, `192` or `128` (AAC kbps per stem). |
| `stemcache_max_gb` | `10` | Delete the least recently used chunks above this size (`0` = no limit). |
| `stemcache_min_free_gb` | `5` | Store nothing new while the disk has less free space than this. |
| `stemcache_dir` | *(empty: the default folder)* | Folder of the cached stems. Default: `~/Library/Caches/rekordbox-mods/stems` (macOS), `%LOCALAPPDATA%\rekordbox-mods\cache\stems` (Windows). |
| `stemcache_staging` | `auto` | Stage new chunks on the internal disk first. `auto`: only when `stemcache_dir` is on an external drive; `1`: always; `0`: never. |
| `stemcache_staging_gb` | `4` | How much may wait on the internal disk; past it a deck's chunks are left out and the stems window's run waits. |

## Needs rekordbox's STEMS

Stems need rekordbox's STEMS feature: *Preferences > Extensions > STEMS*.

## Related

<!--@public stemcache-related-->
[betterstems](./betterstems) · [fastwave](./fastwave) · [flx4](./flx4) (stem pads and knobs) ·
[Spotify guide](/guides/spotify)
<!--@/public-->
