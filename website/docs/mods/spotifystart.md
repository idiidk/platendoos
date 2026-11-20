# spotifystart <span class="pd-tag">win</span>

Fixes Spotify hanging on "loading the library" after a restart. Only does anything on **rekordbox 7.2.19 for
Windows**.

<ModInfo name="spotifystart" platform="win" group="Spotify" />

## What it fixes

rekordbox 7.2.19 for Windows runs its Spotify client's jobs on a pool of **one** thread. A job that restarts the
client waits until Spotify's library has finished starting, but Spotify can't finish starting while that job holds
the only thread: the messages it needs are jobs on the same pool. Both wait for ever, and rekordbox shows Spotify
"loading the library" until you restart it (plain 7.2.19 stalled in 2 of 4 starts in the tests).

With the mod, that restart job does nothing while Spotify is still starting: the start already under way is the one
that's wanted. In the tests: 8 of 8 restarts were ready in about 2 s.

## Only for that build

The mod checks the rekordbox build and the exact code it changes, and otherwise leaves rekordbox alone. On any other
build, 7.2.20 included, it logs "not this rekordbox build, nothing patched". 7.2.20 doesn't need it:
every test start of 7.2.20 had Spotify ready about 0.3 s after the client was created.

## Options

| Option | Default | What it does |
|---|---|---|
| `spotifystart` | `1` | `0` switches the mod off (restart). |

## Related

[spotifyfix](./spotifyfix) · [Windows notes](/guides/windows)
