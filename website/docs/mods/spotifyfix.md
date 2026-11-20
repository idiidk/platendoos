# spotifyfix

Stops Spotify playlists from endlessly reloading themselves, and fixes a deadlock in rekordbox's Spotify client that
could freeze every Spotify download until you force quit.

<ModInfo name="spotifyfix" group="Spotify" />

## What it fixes

**Playlists that reload in a loop.** Spotify's API returns a revision id that flip-flops between two values for a
playlist that hasn't changed. rekordbox compares it in four places and, on any difference, throws the cached track
list away and loads it again, endlessly. With the mod those comparisons report "unchanged"; real changes to a track
list are still picked up by the normal refreshes.

**The download deadlock.** When a new Spotify client token arrives while a playlist is being refreshed, rekordbox's
Spotify client could lock up: every Spotify download stalled until rekordbox was force quit. The mod hands the new
token over so that the two no longer wait for each other.

**Logging out during downloads.** A Spotify logout (for example after a permission error) first lets running
downloads end instead of locking up on them.

## Options

| Option | Default | What it does |
|---|---|---|
| `spotifyfix` | `1` | `0` restores rekordbox's original behaviour (restart). |

## Related

[spotifyapi](./spotifyapi) · [spotifystart](./spotifystart) · [Spotify guide](/guides/spotify) ·
[Troubleshooting](/guides/troubleshooting)
