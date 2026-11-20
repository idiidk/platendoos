# spotifyapi

Lets rekordbox load several Spotify track lists at once instead of one after the other. A big playlist shows up
roughly twice as fast.

<ModInfo name="spotifyapi" group="Spotify" />

## Why

rekordbox's Spotify service holds one lock around each request to Spotify, including the wait for the answer. So
while the library syncs, every playlist, every lookup and the playlist you just clicked queue behind each other,
and all streaming services share a pool of 3 background threads. On Windows, a 50-track playlist filled in seconds
with the mod, where plain rekordbox showed a spinner for over 20 s.

## What it does

The track-list and lookup requests each take one of several slots (3 by default) instead of the single lock; the
request code only reads shared state, and token refreshes happen outside the lock. The background pool gets 6
threads. Nothing to click: playlists just fill sooner.

## Options

| Option | Default | What it does |
|---|---|---|
| `spotifyapi` | `1` | `0` switches the mod off (restart). |
| `spotifyapi_slots` | `3` | Spotify lists loaded at once (applies from the next launch). |

## Notes

- rekordbox 7.2.20 rewrote the two functions this mod hooks; platendoos 0.3.0 and later have the sites for both
  7.2.19 and 7.2.20.

## Related

[spotifyfix](./spotifyfix) · [artcache](./artcache) · [Spotify guide](/guides/spotify)
