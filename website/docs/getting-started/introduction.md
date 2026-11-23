# Introduction

platendoos is a set of mods for **rekordbox 7 on macOS** (Apple Silicon; Intel in beta) **and Windows** (x64). It makes
rekordbox faster and smoother, especially with Spotify, and adds a few features it doesn't have.

You can tell a modded rekordbox by the small **PLATENDOOS** chip next to the rekordbox logo at the bottom left.
Hover over it to see the build, or click it to see every mod and whether it's on.

<figure class="shot mid"><img src="/img/chip.webp" alt="The rekordbox logo at the bottom left with the PLATENDOOS chip next to it"><figcaption>The chip in rekordbox's bottom bar.</figcaption></figure>

## Your rekordbox stays untouched

- **macOS:** platendoos makes a separate copy, `~/Applications/platendoos.app`, that loads the mods. The rekordbox
  in `/Applications` isn't changed. On APFS the copy takes no extra disk space.
- **Windows:** `platendoos.exe` starts your installed rekordbox with the mods loaded, without changing any of its
  files. The one exception is the DDJ-FLX4 mapping, and only when you tick that option in the patcher (see
  [Installation](./installation#windows)).

Open rekordbox the usual way whenever you want it plain. Your library, settings and collection are the same in
both: platendoos is rekordbox, with the mods loaded.

## What it does

<!--@public intro-count-->
There are 34 mods, in five groups. Every one is on by default and can be switched off on its own.
<!--@/public-->

| Group | In short | Mods |
|---|---|---|
<!--@public intro-spotify-row-->
| [Spotify](/mods/#spotify) | Load track lists faster, fix rekordbox's Spotify lock-ups, stems, auto cues and genres for Spotify tracks | spotifyapi, spotifyfix, spotifystart, autocue, genres, spotifyaudio, artcache, spotifylog, spotifydl |
<!--@/public-->
| [Analysis](/mods/#analysis) | Analyse whole playlists, better beat grids and keys, waveforms on the deck sooner, faster analysis | analyzer, bettergrid, betterkey, quickdeck, parallel, tfpool, streamio, timing |
| [Browsing and playing](/mods/#browsing-and-playing) | Stems kept on disk and separated with HTDemucs, sync that doesn't jump, a calmer track menu, multi-column sorting, better FX, no clicks when memory is short | stemcache, betterstems, fastwave, audiomem, betterfx, keepmaster, tidymenu, multisort, keycolumn, xfadelock, outputsync, fontcache |
| [Controllers](/mods/#controllers) | A DDJ-FLX4 as a four-deck controller with stems | flx4 |
| [Quieter in the background](/mods/#quieter-in-the-background) | Less work while you do nothing | framerate, badge, screeninfo, midipoll |

The [mods overview](/mods/) has a line on each.

## macOS and Windows

The mods are the same source on both platforms. A few only exist on one:

- **macOS only (6):** outputsync, betterfx, audiomem, and fontcache, screeninfo and midipoll (the last three fix
  slow macOS calls that Windows doesn't have).
- **Windows only (1):** spotifystart, a fix for rekordbox 7.2.19 for Windows that does nothing on any other build.

<!--@public intro-platform-count-->
So the Mac has 33 mods and Windows 28. The chip's window lists what's on.
<!--@/public-->

## Supported rekordbox versions

| rekordbox | macOS | Windows |
|---|---|---|
| **7.2.19** | Made for it and tested on it | Made for it and tested on it |
| **7.2.20** | Checked the day it came out: every mod on | Checked the day it came out: every mod on (a hand-checked address table ships with the patcher) |
| Later versions | The mods find their way around by themselves; a mod that can't is switched off and logged | The patcher makes an address table on the spot; mods it can't place are off |

On macOS, after a rekordbox update the mods try to find their way around by themselves; any mod that can't is
switched off and logged, so the rest keep working. On Windows every rekordbox build needs its own table: for a
version without one, the patcher makes it on the spot from the signatures it carries, the mods it can't place are
off, and rekordbox runs as normal. More in [When rekordbox updates](/internals/rekordbox-updates).

## Requirements

- **macOS:** a Mac with Apple Silicon and rekordbox 7 in `/Applications/rekordbox 7/`, macOS 13 or later.
  **Intel Macs are beta:** the patcher installs there too, but platendoos has only been tested under Rosetta so
  far, not on a real Intel Mac.
- **Windows:** an x64 PC with rekordbox 7.2.19 or 7.2.20 installed (in `C:\Program Files\rekordbox`). The mods'
  windows need WebView2, which Windows 11 has built in. A DirectX 12 graphics card is used when it's fast enough
  (see [Windows notes](/guides/windows)).
- Enough disk space for the caches: by default up to 20 GB of Spotify audio and 10 GB of stems, both adjustable
  (see [Caches and external drives](/guides/caches)).

## Good to know

- This is an unofficial hobby project, not made by or affiliated with AlphaTheta / Pioneer DJ or Spotify. Use it
  at your own risk, and keep backups of your rekordbox library.
- The Spotify features work with your own Spotify connection in rekordbox and keep audio only as a local cache on
  your computer.
- platendoos is licensed under CC BY-NC-SA 4.0: non-commercial use only. Some of the models it uses are
  non-commercial too. See [Licence and credits](/internals/licence).

Next: [install it](./installation).
