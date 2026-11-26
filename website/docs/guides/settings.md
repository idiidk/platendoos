# Settings and config.txt

Every mod is on by default. The quickest way to change a setting is the [PLATENDOOS chip](/mods/badge)'s window:
click the chip, pick a mod, and set its options there; each one says what it does, and a search box finds an option
by name. Whatever you set there is saved in one plain text file, which you can also edit yourself:

| Platform | File |
|---|---|
| macOS | `~/Library/Application Support/rekordbox-mods/config.txt` |
| Windows | `%APPDATA%\rekordbox-mods\config.txt` |

The chip's window opens it too (**Open config file**). The patcher writes your mod choices into it when you
install.

## How the file works

The mods create it on the first start. It lists every mod and option this platform has, with its default, each on a
line starting with `#`, which means *off*: remove the `#` to use a line, then change its value.

```ini
# ---- stemcache ----
# Keeps rekordbox's separated stems on disk, so a track you've had on a deck before has its stems
# ready at once instead of separating again.
#stemcache=1
#   delete the least recently used chunks above this size (0 = no limit)
#stemcache_max_gb=10
```

To keep at most 30 GB of stems:

```ini
stemcache_max_gb=30
```

To switch a mod off:

```ini
framerate=0
```

## When a change applies

- **Options** apply as soon as you save the file: it is re-read every couple of seconds.
- **A mod's own on/off switch** (`name=0` / `name=1`) applies at the **next start** of rekordbox: which mods are on
  is decided once, when rekordbox starts. When the file says something else than what is running, the PLATENDOOS
  chip's blue icon pulses orange, and its tooltip and window say *Restart rekordbox to apply: …* and list the mods
  that change.
- A few options say *restart* or *from the next launch* in their description (for example `gpu`, `spotifyapi_slots`
  and audiomem's parts): those are read once too.

The patcher's checkboxes and the config are the same thing: unticking a mod in the patcher writes `<mod>=0`, and the
patcher's window starts from what the config says.

## Other settings

| Setting | Platform | What it does |
|---|---|---|
| `worker_priority` | Windows | The mods' own threads run one step below normal priority, so rekordbox's window and drawing go first. `-1` (default), `0`: normal, as before 0.3.0; `-2`: two steps down. The default is `0` on a PC with fewer than four logical cores. |
| `column<id>=` | Windows | Written by the mods: where the mods' own track list columns (Key @ Master, CNN Key, BT BPM) are, since rekordbox's saved layout doesn't know them. |
| `analyze_playlist` | both | Written by the analyzer: the playlist analysed last, reopened after a restart. |

Lines the mods write themselves aren't meant to be edited.

## Every option

Generated from the defaults the mods carry (`core/config_template.h` in the repository). Mods marked
<span class="pd-tag">mac</span> exist on macOS only, <span class="pd-tag">win</span> on Windows only; a few
defaults differ per platform.

<!--@public settings-options-->
### analyzer

Open analysis window in any playlist's menu analyses that playlist, Spotify ones too. [More →](/mods/analyzer)

| Option | Default | Meaning |
|---|---|---|
| `analyzer` | `1` | `0` switches the mod off, `1` on. Applies after a restart of rekordbox. |
| `analyze_decks` | `3,4` | decks to use when analyze_direct=0 (1-4) |
| `analyze_direct` | `1` | analyse without decks (0: load onto the decks below) |
| `analyze_fast` | `0` | 1: tracks this mod loads skip rekordbox's optional analyses (phrase, vocal detection, similarity embedding); rekordbox adds those later, when the track is loaded for playing |
| `analyze_gap` | `4` | min seconds between two loads (spares the streaming service) |
| `analyze_import` | `1` | direct mode: import a Spotify playlist track that isn't in the collection yet (like playing it does), since rekordbox only analyses collection tracks; 0: such tracks fail |
| `analyze_import_timeout` | `20` | seconds to wait for an imported track to show up in the collection |
| `analyze_live` | `1` | tracks analysed at once while a deck is playing (0: the run waits for the decks to stop) |
| `analyze_min_free_gb` | `5` | stop a run when the disk has less free space than this, in GB (at most 1: a run writes only rekordbox's analysis files) |
| `analyze_parallel` | `6` | tracks analysed at once in direct mode (6 is fastest on a 10-core Mac) |
| `analyze_reopen` | `1` | open the playlist analysed last after a restart (analyze_playlist, saved when a run starts) |
| `analyze_start_timeout` | `15` | seconds before a track whose analysis never starts is retried |
| `analyze_streams` | `3` | max Spotify tracks loaded at once, counting all decks |
| `analyze_timeout` | `90` (macOS), `180` (Windows) | seconds before a track that never finishes is given up (180 on Windows) |
| `analyze_window` | `1` | open the analysis window when a run starts |
| `bettergrid_min_fit` | `0.6` | Beat This!'s fit below which rekordbox's grid is kept |
| `genres_on_analyse` | `0` | 1: fill empty Genre fields when rekordbox analyses a track |

### artcache

Remembers far more cover art (256 MB in memory instead of rekordbox's 26), so scrolling and loading decks don't download the same images over and over. [More →](/mods/artcache)

| Option | Default | Meaning |
|---|---|---|
| `artcache` | `1` | `0` switches the mod off, `1` on. Applies after a restart of rekordbox. |
| `artcache_mb` | `256` | memory cache size in MB |

### audiomem <span class="pd-tag">mac</span>

Keeps the memory rekordbox's audio works with in RAM, so playback doesn't click when the Mac runs short of memory (macOS). [More →](/mods/audiomem)

| Option | Default | Meaning |
|---|---|---|
| `audiomem` | `1` | `0` switches the mod off, `1` on. Applies after a restart of rekordbox. |
| `audiomem_ahead` | `20` | seconds of audio kept ready ahead of a playing deck |
| `audiomem_code` | `1` | 0: rekordbox's own code and data aren't kept in memory (restart) |
| `audiomem_decks` | `1` | 0: a playing deck's audio isn't kept ready ahead of the playhead (restart) |
| `audiomem_fx` | `1` | 0: the FX history buffers aren't kept in memory (restart) |
| `audiomem_lazy` | `1` | 0: a deck load zeroes its new buffers at once on the main thread, as rekordbox does (restart) |

### autocue

Sets cues (intro, build, drop, breakdown, build 2, drop 2, outro, fake drop) on a playlist as hot cues A–H, as memory cues, or both, Spotify tracks too (read from the stream into memory by spotifyaudio). [More →](/mods/autocue)

| Option | Default | Meaning |
|---|---|---|
| `autocue` | `1` | `0` switches the mod off, `1` on. Applies after a restart of rekordbox. |
| `autocue_build_bars` | `0` | 4, 8, 16 or 32 puts build cues that many bars before their drop (0: where the build starts) |
| `autocue_claude_model` | `haiku` | Claude Code's model (an alias or a full name) |
| `autocue_claude_path` | *(empty)* | the claude program, if it isn't found by itself |
| `autocue_intro` | `sound` | where the intro cue goes: sound (the first bar with sound), drums (the first bar with drums) or start (the track's first bar) |
| `autocue_memory` | `00000000` | per cue, in autocue_pads' order: 1 writes it as a memory cue too (or only, without a pad) |
| `autocue_model` | `anthropic/claude-haiku-5.5` | any model id from openrouter.ai/models |
| `autocue_outro_bars` | `0` | 16, 32 or 64 puts the outro cue that many bars before the track ends (0: where the last loud part ends) |
| `autocue_pads` | `ABCDEFGH` | the hot cue pad of Intro, Build, Drop, Breakdown, Build 2, Drop 2, Outro and Fake drop, in that order (- for none); set in the window under Layout |
| `autocue_parallel` | `3` | tracks analysed at once (one of them separates stems at a time) |
| `autocue_provider` | `openrouter` | or claude: the local Claude Code CLI; none: no model, the built-in rules place the cues |
| `autocue_stems` | `1` | 0 analyses three bands of the mix instead of stems (faster: no separation) |

### badge

The PLATENDOOS chip: its window shows the versions, every mod and whether it's on, and opens the log and this file. [More →](/mods/badge)

| Option | Default | Meaning |
|---|---|---|
| `badge` | `1` | `0` switches the mod off, `1` on. Applies after a restart of rekordbox. |

### betterfx <span class="pd-tag">mac</span>

Better-sounding mixer effects (macOS). [More →](/mods/betterfx)

| Option | Default | Meaning |
|---|---|---|
| `betterfx` | `1` | `0` switches the mod off, `1` on. Applies after a restart of rekordbox. |
| `betterfx_echo` | `1` | Echo and Pitch Echo: no dips while the tempo changes |
| `betterfx_eq` | `1` | the DJM-900NXS2 EQ and the isolator |
| `betterfx_filter` | `1` | the Color FX Filter (the FLX4 filter knob) |
| `betterfx_reverb` | `1` | the Beat FX Reverb (the new reverb sound) |
| `betterfx_roll` | `1` | Roll, Reverse Roll and Slip Roll: no dip on every repeat |

### bettergrid

Better beat grids: combines the Beat This! beat tracker's tempo and bar starts with beats placed on the hits in the audio. [More →](/mods/bettergrid)

| Option | Default | Meaning |
|---|---|---|
| `bettergrid` | `1` | `0` switches the mod off, `1` on. Applies after a restart of rekordbox. |
| `bettergrid_apply` | `1` | 0: compare only, log both grids and keep rekordbox's |
| `bettergrid_bars` | `1` | rekordbox's and Beat This!'s bar starts differ: the music's changes (barphase.h) pick one; 0 the kick-in rule alone |
| `bettergrid_debug` | `0` | 1 logs the beats of both grids (for debugging) |
| `bettergrid_genre` | `1` | the track's genre (genres mod) picks the tempo's octave; 0 never |
| `bettergrid_halfbeat` | `1` | rekordbox's beats half a beat off the beat: 1 moved when Beat This!'s beats and the kick band (its onsets, or its level clearly higher between) both say so, 2 when Beat This!'s beats alone say so, 0 never |
| `bettergrid_kick` | `1` | the kick sits a fixed 50 ms or more off the grid over the whole track, also off Beat This!'s beats (a percussion layer on the beat, the kick after it): 1 the grid moved onto the kicks; 0 never |
| `bettergrid_offbeat` | `1` | rekordbox's beats a fixed 60 ms or more off the kicks: 1 the combined grid when Beat This! and the kick band both say so, 0 never |
| `bettergrid_rb_first` | `1` | 0: while it waits, the deck doesn't show rekordbox's own grid meanwhile (quickdeck) |
| `bettergrid_slow` | `0` | Windows, Intel Macs: 1: also wait for Beat This! for a track on a deck where it is slow (it runs on the CPU, or on a GPU that needs over a second for 30 s of audio) |
| `bettergrid_title` | `BT BPM` | header name of the column |
| `bettergrid_wait` | `20` (macOS), `60` (Windows) | seconds the analysis waits for the combined grid at most (60 on Windows and Intel Macs) |
| `bettergrid_warmup` | `1` | 0: no first run of the model at launch (Apple Silicon) |
| `gpu` | `1` | macOS: 0 keeps bettergrid on the CPU (Intel Macs: stems come from rekordbox's own engine then). Windows: 0 keeps the models off the GPU: Beat This! and the genre model run on the CPU, stems come from rekordbox's own engine |

### betterkey

Detects musical keys with a neural network trained for it, which gets keys right more often than rekordbox's built-in detector: 67.5 % of the GiantSteps key dataset's tracks against rekordbox's 56.8 %. [More →](/mods/betterkey)

| Option | Default | Meaning |
|---|---|---|
| `betterkey` | `1` | `0` switches the mod off, `1` on. Applies after a restart of rekordbox. |
| `betterkey_min_conf` | `0` | replace only when the CNN's probability is at least this (0..1) |
| `betterkey_mode` | `compare` | compare: keep rekordbox's key, log both; replace: use the CNN's key |
| `betterkey_title` | `CNN Key` | header name of the column |
| `betterkey_wait` | `120` | seconds the key stage waits for the CNN's result before it keeps rekordbox's key |
| `streamio_phrase` | `1` | 0 reads the audio again for phrase analysis |

### betterstems

Separates stems with HTDemucs v4 instead of rekordbox's own engine: cleaner stems (about 9 dB SDR against Spleeter's 5.8), on the GPU, about 1 s per 12 s of audio. [More →](/mods/betterstems)

| Option | Default | Meaning |
|---|---|---|
| `betterstems` | `1` | `0` switches the mod off, `1` on. Applies after a restart of rekordbox. |
| `betterstems_grid_first` | `1` | 0: stems of a track that is being analysed start at once, next to its beat detection |
| `betterstems_live_rest` | `0` | while a deck plays: a rest after each model segment, this many times the segment's own time (1 = the stems take twice as long and the GPU is free half the time) |
| `betterstems_safe` | `0` | Windows: 1: the GPU without DirectML's graph fusion from the start, 2: in DirectML's safe mode (also without the GPU maker's own kernels). Both are tried by themselves, in this order, when the GPU gives unusable stems |
| `betterstems_slow` | `0` | macOS: Windows, Intel Macs: 1: HTDemucs also where it is slow (on the CPU, or on a GPU that needs over a second for a segment); 0: rekordbox's own engine separates there. Windows: Windows: 1: HTDemucs also where it is slow (on the CPU, or on a GPU that needs over a second for a segment); 0: rekordbox's own engine separates there |
| `betterstems_warmup` | `1` | 0: no first run of the model at launch (Apple Silicon) |

### fastwave

The deck waveforms come back sooner after they change, e.g. when you mute or unmute a stem or load a track. [More →](/mods/fastwave)

| Option | Default | Meaning |
|---|---|---|
| `fastwave` | `1` | `0` switches the mod off, `1` on. Applies after a restart of rekordbox. |
| `fastwave_ms` | `30` | wait before rebuilding a waveform (rekordbox's own is 500) |

### flx4

DDJ-FLX4 as a four-deck controller with stems. [More →](/mods/flx4)

| Option | Default | Meaning |
|---|---|---|
| `flx4` | `1` | `0` switches the mod off, `1` on. Applies after a restart of rekordbox. |
| `flx4_device` | `DDJ-FLX4` | MIDI device to map (for testing with another device using the same mapping) |
| `flx4_keep_tempo` | `1` | 0: a deck a side switches to takes the tempo rekordbox remembers its fader at |
| `flx4_log` | `0` | 1 logs every MIDI message in and out |
| `flx4_meterlog` | `0` | 1 logs the level meters (for debugging) |
| `flx4_mixer` | *(empty)* | fixed keeps the mixer channels on decks 1/2 |

### fontcache <span class="pd-tag">mac</span>

Makes drawing text, most of the work when the track list repaints, a lot cheaper. [More →](/mods/fontcache)

| Option | Default | Meaning |
|---|---|---|
| `fontcache` | `1` | `0` switches the mod off, `1` on. Applies after a restart of rekordbox. |

### framerate

Runs the screen at full frame rate while a deck plays or you're using the computer, and at 30 fps the rest of the time. [More →](/mods/framerate)

| Option | Default | Meaning |
|---|---|---|
| `framerate` | `1` | `0` switches the mod off, `1` on. Applies after a restart of rekordbox. |
| `framerate_empty` | `1` | 0 keeps rekordbox's 30 fps while the decks are empty, also while you use the mouse |
| `framerate_idle_after` | `3` | seconds without input before the UI drops to 30 fps |

### genres

Fills the Genre field from what the music sounds like: a style model (Discogs-EffNet from Essentia, 400 Discogs styles, best with electronic music) listens to each track. [More →](/mods/genres)

| Option | Default | Meaning |
|---|---|---|
| `genres` | `1` | `0` switches the mod off, `1` on. Applies after a restart of rekordbox. |
| `genres_detail` | `style` | style: the specific style ("Deep House"); broad: the broad genre ("House") |
| `genres_warmup` | `1` | 0: the style model loads when it is first needed, not at launch |

### idlewake <span class="pd-tag">mac</span>

Less work while nothing plays. [More →](/mods/idlewake)

| Option | Default | Meaning |
|---|---|---|
| `idlewake` | `1` | `0` switches the mod off, `1` on. Applies after a restart of rekordbox. |
| `idlewake_check` | `0` | 1 runs the envelope followers anyway and logs whether their output changed (for testing) |
| `idlewake_envelope` | `1` | 0: the envelope followers of silent channels run again (they feed the Color FX gate / comp) |
| `idlewake_log` | `0` | 1 logs every 10 s what was left out (for debugging) |
| `idlewake_meters` | `1` | 0: level meters restart their timer on every frame again |
| `idlewake_midi` | `1` | 0: the MIDI output thread wakes every millisecond again |

### keepmaster

Changing the sync master never changes the tempo of a deck you hear. [More →](/mods/keepmaster)

| Option | Default | Meaning |
|---|---|---|
| `keepmaster` | `1` | `0` switches the mod off, `1` on. Applies after a restart of rekordbox. |

### keycolumn

A new Key @ Master column: each track's key as it would sound at the master deck's tempo, e.g. 8A +1.2. [More →](/mods/keycolumn)

| Option | Default | Meaning |
|---|---|---|
| `keycolumn` | `1` | `0` switches the mod off, `1` on. Applies after a restart of rekordbox. |
| `keycol_title` | `Key @ Master` | header name of the column |

### midipoll <span class="pd-tag">mac</span>

Checks for newly plugged-in MIDI controllers every 2 seconds instead of every 0.3 seconds, and quickly again whenever something is plugged in. [More →](/mods/midipoll)

| Option | Default | Meaning |
|---|---|---|
| `midipoll` | `1` | `0` switches the mod off, `1` on. Applies after a restart of rekordbox. |

### multisort

Sort the track list by several columns: each click on a column header cycles it through ascending, descending and off, and the columns that are on sort together in the order you turned them on (e.g. BPM, then key within each BPM). [More →](/mods/multisort)

| Option | Default | Meaning |
|---|---|---|
| `multisort` | `1` | `0` switches the mod off, `1` on. Applies after a restart of rekordbox. |

### outputsync <span class="pd-tag">mac</span>

Lines up speakers and headphones when rekordbox plays to two devices (in rekordbox's Preferences > Audio: a switch, a fine-tune slider, and a metronome that clicks on both outputs so you can tune by ear), e.g. PC MASTER OUT to Bluetooth speakers with the headphones on a controller. [More →](/mods/outputsync)

| Option | Default | Meaning |
|---|---|---|
| `outputsync` | `1` | `0` switches the mod off, `1` on. Applies after a restart of rekordbox. |
| `outputsync_clocks` | `0` | log every 30 s how fast each device's clock really runs and how far they drift apart (measured: Bluetooth speakers vs an FLX4, 5 ppm = 0.3 ms a minute) |
| `outputsync_lineup` | `1` | the switch in rekordbox's Preferences > Audio ("Line up speakers and headphones") |
| `outputsync_ms` | `auto` | auto: as the devices report; a number: delay the faster devices by that many ms (tune while listening to the speakers and headphones together; re-read live) |
| `outputsync_offset` | `0` | fine-tune in ms on top (+ = headphones later), the slider under that switch; there too a metronome switch: clicks on every output, to hear whether they line up |

### parallel

Spreads the heaviest part of analysing a track over three cores instead of one. [More →](/mods/parallel)

| Option | Default | Meaning |
|---|---|---|
| `parallel` | `1` | `0` switches the mod off, `1` on. Applies after a restart of rekordbox. |

### quickdeck

A Spotify track (or any track that isn't analysed yet) shows its waveforms and a beat grid on the deck as soon as the analysis has them, instead of only when the whole analysis is done. [More →](/mods/quickdeck)

| Option | Default | Meaning |
|---|---|---|
| `quickdeck` | `1` | `0` switches the mod off, `1` on. Applies after a restart of rekordbox. |
| `quickdeck_gridonly` | `1` | 0: a new beat grid builds the deck's waveforms again, as rekordbox does |
| `quickdeck_log` | `0` | 1 logs each partial overview (for debugging) |
| `quickdeck_merge` | `0.25` | Windows: seconds the waveforms wait for rekordbox's own grid, so that one redraw of the deck shows both (0: no wait) |
| `quickdeck_progressive` | `0` | an unfinished experiment (macOS): 1 lets a Spotify track's analysis start reading while the track downloads, so its overview waveform grows during the download; not proven safe (the one time it ran, Spotify signed rekordbox out for over an hour right after): leave it off |
| `quickdeck_stream` | `1` | 0: the overview waveform appears when the whole track has been read, as in rekordbox |

### quickframe <span class="pd-tag">mac</span>

Clicks show sooner. [More →](/mods/quickframe)

| Option | Default | Meaning |
|---|---|---|
| `quickframe` | `1` | `0` switches the mod off, `1` on. Applies after a restart of rekordbox. |
| `quickframe_drawables` | `2` | frames queued for the display: 2, or rekordbox's own 3 |
| `quickframe_playing` | `0` | 1 shows frames as they are made while a deck plays too (quicker clicks; a late frame then shows as a hitch in the waveform) |
| `quickframe_stopped` | `1` | 0 keeps the display's rhythm while no deck plays too |

### relayout <span class="pd-tag">mac</span>

Menus, drop-downs, tooltips and Preferences open sooner, resizing the window is twice as smooth, and options that made the whole window refresh answer in 80 ms instead of 200. [More →](/mods/relayout)

| Option | Default | Meaning |
|---|---|---|
| `relayout` | `1` | `0` switches the mod off, `1` on. Applies after a restart of rekordbox. |
| `relayout_check` | `0` | 1 renders every button anyway and logs those whose images differ from the ones it would have kept (for testing) |
| `relayout_hidden` | `1` | 0 lays out and restyles hidden panels every time, as rekordbox does |
| `relayout_log` | `0` | 1 logs how many buttons kept their images in a layout pass (for debugging) |

### screeninfo <span class="pd-tag">mac</span>

Stops rekordbox from asking macOS about your screen on every single redraw. [More →](/mods/screeninfo)

| Option | Default | Meaning |
|---|---|---|
| `screeninfo` | `1` | `0` switches the mod off, `1` on. Applies after a restart of rekordbox. |

### spotifyapi

Lets rekordbox load several Spotify track lists at once instead of one after the other. [More →](/mods/spotifyapi)

| Option | Default | Meaning |
|---|---|---|
| `spotifyapi` | `1` | `0` switches the mod off, `1` on. Applies after a restart of rekordbox. |
| `spotifyapi_slots` | `3` | Spotify lists loaded at once (applies from the next launch) |

### spotifyaudio

Reads a Spotify track's audio from the stream into memory when autocue's or genres' window needs to hear the whole track, the way rekordbox's own analysis reads it. [More →](/mods/spotifyaudio)

| Option | Default | Meaning |
|---|---|---|
| `spotifyaudio` | `1` | `0` switches the mod off, `1` on. Applies after a restart of rekordbox. |
| `spotifyaudio_mb` | `256` | memory for the tracks decoded last (0: none kept) |
| `spotifyaudio_wait` | `90` | seconds to keep trying to open a track's stream (Spotify's streaming limit) |

### spotifydl

Logs where a Spotify track's load time goes: slow or failing requests and how long each download took. [More →](/mods/spotifydl)

| Option | Default | Meaning |
|---|---|---|
| `spotifydl` | `1` | `0` switches the mod off, `1` on. Applies after a restart of rekordbox. |
| `spotifydl_debug` | `0` | also log every audio piece, and the first audio answer in full |

### spotifyfix

Stops Spotify playlists from endlessly reloading themselves, and fixes a deadlock in rekordbox's Spotify client that could freeze every Spotify download until you force quit (a token refresh colliding with a playlist refresh). [More →](/mods/spotifyfix)

| Option | Default | Meaning |
|---|---|---|
| `spotifyfix` | `1` | `0` switches the mod off, `1` on. Applies after a restart of rekordbox. |

### spotifylog

Stops rekordbox from rewriting its Spotify log file several times a second while it syncs. [More →](/mods/spotifylog)

| Option | Default | Meaning |
|---|---|---|
| `spotifylog` | `1` | `0` switches the mod off, `1` on. Applies after a restart of rekordbox. |

### spotifystart <span class="pd-tag">win</span>

Fixes Spotify hanging on "loading the library" after a restart. Only does anything on rekordbox 7.2.19 for Windows. [More →](/mods/spotifystart)

| Option | Default | Meaning |
|---|---|---|
| `spotifystart` | `1` | `0` switches the mod off, `1` on. Applies after a restart of rekordbox. |

### stemcache

Keeps rekordbox's separated stems on disk, so a track you've had on a deck before has its stems ready at once instead of separating again. [More →](/mods/stemcache)

| Option | Default | Meaning |
|---|---|---|
| `stemcache` | `1` | `0` switches the mod off, `1` on. Applies after a restart of rekordbox. |
| `stemcache_dir` | *(empty)* | folder of the cached stems (empty: the default one) |
| `stemcache_keep` | `all` | which tracks' stems are saved to disk: all (every track that gets stems, on a deck or with Separate stems), chosen (only tracks separated with the stems window's Separate stems button; a track that is only loaded onto a deck gets stems that are thrown away when rekordbox quits) or none (nothing new is saved) |
| `stemcache_log` | `0` | 1 logs every chunk and every stem control (for debugging) |
| `stemcache_max_gb` | `10` | delete the least recently used chunks above this size (0 = no limit) |
| `stemcache_memory_mb` | `384` | stems that aren't stored on disk (Spotify tracks', and what stemcache_keep leaves out) kept in memory for loading the track again in this session; the least recently used go first (0: none) |
| `stemcache_min_free_gb` | `5` | store nothing new while the disk has less free space than this |
| `stemcache_ondemand` | `0` | 1: a deck's track is separated only once a stem control is used on it (a stem muted, a PART ISOLATOR knob turned, the FX taken off a stem), from where it plays; stems that are stored already still come at once (on Windows new and not yet run on a Windows machine) |
| `stemcache_quality` | `256` | how new chunks are stored: lossless \| 256 \| 192 \| 128 (AAC kbps per stem) |
| `stemcache_spotify` | `1` | no stems for Spotify tracks (otherwise: on a deck at once, separated from the stream) |
| `stemcache_staging` | `auto` | new chunks go to the internal disk first (&lt;the mods' cache folder>/stems-staging) and one background thread moves them to stemcache_dir, a piece at a time and never while a deck or an analysis reads from that drive. auto: only when stemcache_dir is on an external drive \| 1: always \| 0: never |
| `stemcache_staging_gb` | `4` | how much may wait on the internal disk; past it a deck's chunks are left out (separated again the next time) and the stems window's run waits |

### streamio

Reads the audio faster during analysis: about 10% faster overall and 4x faster phrase analysis. [More →](/mods/streamio)

| Option | Default | Meaning |
|---|---|---|
| `streamio` | `1` | `0` switches the mod off, `1` on. Applies after a restart of rekordbox. |

### tfpool

Keeps rekordbox's beat-detection neural network loaded between tracks instead of reloading it for each one. [More →](/mods/tfpool)

| Option | Default | Meaning |
|---|---|---|
| `tfpool` | `1` | `0` switches the mod off, `1` on. Applies after a restart of rekordbox. |
| `tfpool_warmup` | `1` | 0 skips the warm-up of the beat models at launch |

### tidymenu

A calmer right-click menu on tracks. [More →](/mods/tidymenu)

| Option | Default | Meaning |
|---|---|---|
| `tidymenu` | `1` | `0` switches the mod off, `1` on. Applies after a restart of rekordbox. |
| `tidymenu_group` | `1` | 0: the mods' items stay in the menu itself, under their long names |
| `tidymenu_hide` | `1` | 0: rekordbox's greyed-out items stay |
| `tidymenu_log` | `0` | 1 logs the items of each menu shown (for debugging) |

### timing

Logs how long each step of an analysis takes. [More →](/mods/timing)

| Option | Default | Meaning |
|---|---|---|
| `timing` | `1` | `0` switches the mod off, `1` on. Applies after a restart of rekordbox. |

### xfadelock

A small lock button next to the on-screen crossfader holds the crossfader exactly in the middle. [More →](/mods/xfadelock)

| Option | Default | Meaning |
|---|---|---|
| `xfadelock` | `1` | `0` switches the mod off, `1` on. Applies after a restart of rekordbox. |
<!--@/public-->
