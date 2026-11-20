# Editions

This is the **public edition** of platendoos. It never writes Spotify audio to disk, decoded or derived (stems):
what the mods need of a Spotify track is read from the stream into memory and is gone when rekordbox quits.
Analysis results (beat grids, keys, genres, cue points, waveforms: what rekordbox itself stores for Spotify tracks)
are kept as rekordbox keeps them. The build defines `RBM_PUBLIC=1` (`rbm::kPublicEdition` in `core/rbmod.h`), and
the PLATENDOOS chip's window and the patcher say `public`.


## The public edition and Spotify

| Feature | How |
|---|---|
| **Stems for Spotify tracks on a deck** | stemcache lets rekordbox separate a Spotify track (it normally refuses); the separator reads the stream through rekordbox's own Spotify reader (`core/spotifyread.cpp` keeps it up with the download). The stems stay in rekordbox's memory as for any streaming track. Chunks of Spotify tracks are never stored on disk; the last ones are kept in memory, compressed (`stemcache_memory_mb`, default 384 MB, 0 = none), so loading the track again in the same session gets its stems at once. Which track a separation is of comes from the separator's uri, which rekordbox asks about when it starts (hook `stem_sep4`); a separation whose track isn't known is kept in memory too, never on disk. |
| Spotify tracks in the autocue and genres windows | the **spotifyaudio** mod (service `spotifyaudio.read`) decodes the track from the stream with rekordbox's own reader into memory when a window asks, keeping the last few (`spotifyaudio_mb`, default 256 MB). |
| autocue stems for Spotify tracks | separated in memory and handed to stemcache with `stemcache.storeSpotify` (kept in memory, as a deck's) |
| analyzer, bettergrid, betterkey, genres on analysis, quickdeck | read the stream during rekordbox's own analysis, in memory, as before |
| artcache | Spotify covers in memory only |
| The stems window | Spotify tracks show *On a deck*: they get their stems when they load onto a deck |

## Services (between mods)

| Service | Offered by |
|---|---|
| `spotifyaudio.read` | spotifyaudio |
| `stemcache.cached`, `stemcache.store`, `stemcache.storeSpotify` | stemcache |
