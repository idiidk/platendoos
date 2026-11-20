# spotifyaudio

Reads a Spotify track's audio from the stream into memory when a window needs to listen to it: the auto cues and
genres windows. Nothing of it is written to disk.

<ModInfo name="spotifyaudio" group="Spotify" needs="Used by [autocue] and [genres] for Spotify tracks" />

## Why

rekordbox only reads a Spotify track's audio while it analyses it or plays it on a deck. The [auto cues](./autocue)
and [genres](./genres) windows listen to tracks on their own, whenever you ask them to, so they need a way to get a
Spotify track's audio too.

## What it does

- When a window asks for a Spotify track, the mod opens it with rekordbox's own Spotify reader, decodes it from the
  stream and hands the audio over, in memory.
- The last few tracks it read stay in memory (`spotifyaudio_mb`, 256 MB by default; about 50 MB for a 5-minute
  track), so asking again, for example
  *Analyse again* in the auto cues window, doesn't stream the track again. Everything is gone when rekordbox quits.
- Spotify's limit of about 30 track downloads a minute applies as for any other load.
<!--@private spotifyaudio-b-->

The analysis window, the beat grids, keys and *Genre first* don't need it: they read the track during rekordbox's own
analysis.

## Options

| Option | Default | What it does |
|---|---|---|
| `spotifyaudio` | `1` | `0` switches the mod off (restart): the windows then skip Spotify tracks. |
| `spotifyaudio_mb` | `256` | Memory kept for the last tracks read, in MB (`0`: none kept). |
| `spotifyaudio_wait` | `90` | Seconds to keep trying to open a track's stream (Spotify's streaming limit). |

## Related

[autocue](./autocue) · [genres](./genres) · [Spotify guide](/guides/spotify)
