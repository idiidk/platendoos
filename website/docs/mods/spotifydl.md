# spotifydl

Logs where a Spotify track's load time goes: slow or failing requests and how long each download took. Changes
nothing.

<ModInfo name="spotifydl" group="Spotify" />

## Why

Measuring deck loads showed where a Spotify track's time goes. Spotify's library first resolves the track (some of
its requests then hang for a 10-second timeout, once twice in a row), then downloads the file in 512 KB pieces, one
after another over one connection: about 10 MB/s when Spotify's servers are quick, but 0.3 to 1.5 s a piece when
they aren't.

## What it logs

- Any request that takes over a second or fails (host and path; never the query, because the addresses carry access
  tokens).
- Per track download: time to the first byte, total time, MB, and how long the pieces kept rekordbox waiting.

Read it in the log when a Spotify load felt slow: whether it was Spotify or the computer. See
[Loading a track](/guides/loading-a-track).

## Options

| Option | Default | What it does |
|---|---|---|
| `spotifydl` | `1` | `0` switches the mod off (restart). |
| `spotifydl_debug` | `0` | `1` also logs every audio piece, and the first audio answer in full. |

## Related

<!--@public spotifydl-related-->
[timing](./timing) · [Troubleshooting](/guides/troubleshooting)
<!--@/public-->
