# spotifylog

Stops rekordbox from rewriting its Spotify log file several times a second while it syncs.

<ModInfo name="spotifylog" group="Spotify" />

## Why

rekordbox's Spotify log (`Spotify.log` in rekordbox's own log folder) is capped at 1 MB, and after every batch of
lines rekordbox trims it by reading the whole file and writing it out again: up to about 2 MB of disk writes a
second while the library syncs. With the mod it is trimmed at most every 10 minutes (the file can grow a little past
the cap in between).

## Options

| Option | Default | What it does |
|---|---|---|
| `spotifylog` | `1` | `0` switches the mod off (restart). |

## Related

[framerate](./framerate) · [All mods](./#quieter-in-the-background)
