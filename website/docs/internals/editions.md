# Editions

platendoos is built in two editions from one source tree:

| Edition | What it is |
|---|---|
| **public** | What the releases and the public source tree are. Everything on this site. |
| **full** | The developer's own build: the public edition plus a few private parts that aren't published. |

Both have the same mods, windows and options, apart from those private parts.

## Spotify in the public edition

Rule: **no Spotify audio, decoded or derived (stems), is written to disk.** What platendoos does with a Spotify
track's audio happens in memory and is gone when rekordbox quits. What rekordbox itself keeps of a Spotify track, its
analysis (beat grid, key, waveforms) and your cues, is kept as usual, and so are the genres the mods find.

| Feature | How |
|---|---|
| Stems for Spotify tracks on a deck | [stemcache](/mods/stemcache) lets rekordbox separate a Spotify track, from the stream through rekordbox's own Spotify reader. The stems stay in memory as for any streaming track; the last ones are kept there, compressed (`stemcache_memory_mb`, 384 MB), so loading the track again in the same session has them at once. |
| Spotify tracks in the auto cues and genres windows | [spotifyaudio](/mods/spotifyaudio) reads the track from the stream into memory when a window asks, and keeps the last few there (`spotifyaudio_mb`, 256 MB). |
| Auto cues per stem on Spotify tracks | separated in memory, kept in stemcache's memory store |
| The analysis window, beat grids, keys, *Genre first*, quickdeck | read the stream during rekordbox's own analysis, as in plain rekordbox |
| Cover art | [artcache](/mods/artcache) keeps Spotify covers in memory only |

The stems window separates local tracks ahead of time; Spotify tracks get their stems on the deck.

<!--@private editions-full-->

## Which edition is running

The PLATENDOOS chip's window shows the edition next to the version: `public` or `full (internal)`. The log's first
line says it too:

```text
rbmods: platendoos <build> (public edition)
```

## Building each

The build scripts take the edition from `RBM_EDITION`: `full` (the default) or `public`. A source tree without the
`private/` folder, such as the public one, always builds `public`.

```sh
./compile.sh                          # the default edition: build/librbmods.dylib
RBM_EDITION=public ./compile.sh       # the public edition
RBM_EDITION=public ./install.sh ~/Applications/platendoos.app   # swap the public dylib into an app
RBM_EDITION=public tools/build/patcher_mac.sh                   # the public macOS patcher
RBM_EDITION=public tools/win/build.sh                           # the public Windows package
tools/build/release.sh 0.3.2          # builds and uploads the public edition
```

For C++ the build scripts set `RBM_PUBLIC=1` for the public edition (`rbm::kPublicEdition` in `core/rbmod.h`).
The output of both editions goes to the same places (`build/librbmods.dylib`, `build/win/...`): the last build wins,
and `build/edition` records which one it was.

The config template (`core/config_template.h`, made by `tools/build/config_gen.py`) holds each edition's options,
and the [settings page](/guides/settings) lists this edition's.

## This site

From `website/`: `npm run dev` (live preview), `npm run build` (to `docs/.vitepress/dist/`), `npm run preview`
(serves that build). These build the public edition. `npm run build` also checks the build for anything of the
full edition and fails if it finds some (`npm run check:public` runs the check alone).

<!--@private editions-site-full-->
