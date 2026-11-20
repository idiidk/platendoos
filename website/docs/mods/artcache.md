# artcache

<!--@public artcache-intro-->
Remembers far more cover art, so scrolling and loading decks don't download the same images over and over.

<ModInfo name="artcache" group="Spotify" />
<!--@/public-->

## Why

rekordbox's artwork cache (cover art in the browser, Spotify playlist icons) keeps about 26 MB of images (roughly
160 covers at 200 px), drops the 50 oldest whenever it's full, and downloads every image it no longer holds again,
one at a time. Scrolling a long Spotify playlist or a library sync kept re-downloading the same covers, and a deck
fetches its cover on the main thread (about 180 ms each).

## What it does

- Keeps up to 256 MB of artwork in memory (`artcache_mb`).
<!--@public artcache-b-->
- Spotify covers are kept in memory only, for as long as rekordbox runs.
<!--@/public-->

## Options

| Option | Default | What it does |
|---|---|---|
| `artcache` | `1` | `0` switches the mod off (restart). |
| `artcache_mb` | `256` | Memory cache size in MB. |

## Related

<!--@public artcache-related-->
[spotifyapi](./spotifyapi)
<!--@/public-->
