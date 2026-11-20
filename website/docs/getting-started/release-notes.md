# Release notes

What changed per release, for people using platendoos. Downloads are on the
[releases page](https://github.com/idiidk/platendoos/releases). After updating, run the new patcher: it updates the
installed platendoos in place (see [Installation](./installation#after-a-rekordbox-update)).


## 0.3.2 (unreleased)

- **bettergrid: bars start on the first kick; a grid on the off-beats is moved onto the kicks.** Some tracks had
  their first kick on beat 3 or 4: where rekordbox and Beat This! disagree on the bar start, the beat where the kick
  comes in now counts. And a track whose kicks sat clearly between rekordbox's beats, but where Beat This! wasn't
  sure enough everywhere, stayed on the off-beats; the kick drum's own level now settles it.
- **bettergrid: the music picks the bar start** when rekordbox and Beat This! disagree: the 1 goes on the beat where
  the track changes (a drop, a breakdown, a new bass line), the beat where the kick comes in only when that doesn't
  settle it. On 330 tracks where both agree, it put the 1 right 96 times in 100, against 78 for the kick rule.
  `bettergrid_bars=0` turns it off. The idea comes from rbxport's documentation (our own code).

## 0.3.1 (2026-11-19)

<!--@private rn-031-a-->
- **tidymenu on Windows**: the calmer track menu (no greyed-out items, the mods' items in one *platendoos*
  submenu) now works in rekordbox for Windows too, 7.2.19 and 7.2.20.
- betterkey's measured accuracy in the README: 67.5 % of the GiantSteps key dataset against rekordbox's 56.8 %.

## 0.3.0 (2026-11-19)

### rekordbox 7.2.20

- **rekordbox 7.2.20 is supported on both platforms**, with every mod on. 7.2.19 keeps working: the patcher
  handles either version.
- macOS: seven hooks had moved in 7.2.20, in Spotify's track lists and in stem separation. Each now has a second
  site that the patcher tries for the newer build.
- Windows: the patcher carries a hand-checked address table for 7.2.20, so all 27 Windows mods load on it. For a
  rekordbox version it has no table for, it makes one on the spot from the signatures it carries; mods it can't
  place stay off, and rekordbox runs as normal.
- The PLATENDOOS chip and the patcher no longer warn about an untested build on 7.2.20.
- How a new rekordbox version is checked before anyone runs it: [When rekordbox updates](/internals/rekordbox-updates).

### DDJ-FLX4

- **Switching a side (SHIFT + PLAY) never changes a deck's tempo any more.** rekordbox set a deck it newly
  controls to the tempo fader position it last remembered for that deck. On a sync master that changed the tempo
  of every playing deck synced to it, in the middle of a mix. That is now switched off (`flx4_keep_tempo=0` gives
  rekordbox's behaviour back).
- The tempo fader takes over a deck only once it reaches that deck's real tempo, also when sync moved the tempo
  meanwhile.
- The decks the two sides control are shown by their deck numbers in blue (macOS). Messages about a deck appear
  over that deck.

### Playback

- **audiomem** (new, macOS): no more clicks when the Mac runs short of memory. When other apps fill the memory,
  macOS compresses or swaps out what wasn't used for a few seconds, and rekordbox's audio then has to wait for it.
  Each wait is a gap of 3 ms or more on every deck, most of all right after a deck load; plain rekordbox has the
  same gaps. audiomem keeps rekordbox's FX buffers and the next 20 s of every loaded deck in memory. Measured with
  3 GB of other memory in use: 30 gaps a minute and 14 per deck load without it, 0 to 6 and 0 to 1 with it.
- A deck load freezes the window for less time: about 0.04 s instead of 0.12 s on macOS (audiomem). On Windows it
  is down to about 0.13 s, as in plain rekordbox, from 0.35 to 0.4 s.
- **xfadelock** (new, macOS): a lock button next to the on-screen crossfader holds the crossfader exactly in the
  middle.
- Analysing during a set no longer takes the cores the audio needs: while a deck plays, an analysis run takes
  one track at a time (`analyze_live`).

### Loading tracks

- The neural networks load one after another at launch, each with a first run, so the first track of the night
  isn't slower. Before, loading them all at once could take up to a minute.
- Stems wait while a track is in beat detection, so its beat grid is no longer late because of them.
- A deck shows rekordbox's own grid while Beat This! is still busy, and the combined grid replaces it when ready.
- Windows: HTDemucs runs only on a graphics card fast enough for it, otherwise rekordbox's own engine makes the
  stems. A deck where Beat This! would be slow gets rekordbox's grid at once. Without a usable GPU, a deck load
  adds nothing heavy (the grid in about 8 s instead of 18 to 31 s). `betterstems_slow=1` and `bettergrid_slow=1`
  give the old behaviour back.
- Windows: the mods' threads run just below normal priority (`worker_priority`), and waveforms and grid come in
  one redraw (`quickdeck_merge`). The playlist rekordbox reopens at launch no longer stays empty.
- What happens at a deck load, in what order: [Loading a track](/guides/loading-a-track).

<!--@public rn-030-drive-->
### The stems cache on an external drive

- The stems cache can be on a USB stick, SD card or external disk without getting in the way of playback. New
  stems are staged on the internal disk and moved over in the background, a piece at a time, only while nothing
  reads from the drive. A drive that turns out to be slow gets nothing written while a deck plays. An SSD in a USB
  case is still the better cache drive.
<!--@/public-->

### Track list and windows

- **tidymenu** (new, macOS): the right-click menu on tracks leaves out rekordbox's greyed-out items, and the mods'
  items go in one *platendoos* submenu, with *Re-analyze track* next to it.
- The mods' windows open on just the selected tracks too: select some, right-click, and the window lists only
  those ("5 tracks from &lt;playlist&gt;").
- **Re-analyze** selected tracks from the track list's menu: they are analysed again right away, without
  rekordbox's Analysis Setting dialog.
<!--@public rn-030-analysis-->
- Analysis window: Spotify tracks wait out Spotify's "streaming limit reached" instead of failing, and with the
  genres mod a Genre column and *Genre first*.
<!--@/public-->
- **multisort**: the other sorted columns are drawn blue with their arrow; *Key @ Master* re-sorts within them.
- Picking one of the mods' menu items no longer also starts rekordbox's *Analyse tracks*.

### Genres and beat grids

- **genres**: *Genre first* (also *Fill empty genres on analysis*): when rekordbox analyses a track, its genre is
  detected first, picks the tempo of the beat grid (140, not 70) and fills an empty Genre field. Hardcore tempo
  ranges added. The genres window was redesigned. The Genre column shows on Spotify rows too.
- **bettergrid**: the track's BPM in the collection follows the saved grid (macOS). rekordbox kept its own number
  there, so a track listed as 140 could have a grid at 70.
- **autocue** and **genres** are on by default on Windows, after being run there.

### Installing

- macOS: `install.sh` and the patcher keep rekordbox's own signature. A re-signed rekordbox is a new app to
  macOS, which asks again whether it may use the microphone; until that is answered, rekordbox's start stands
  still, and after two minutes it goes on without sound. After the patcher, macOS asks once: click *Allow*.

## 0.2.0 (2026-11-16)

The first public release: the patchers for macOS and Windows, made for rekordbox 7.2.19. Licence CC BY-NC-SA 4.0.
