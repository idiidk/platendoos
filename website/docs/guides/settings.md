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
  chip's blue icon pulses orange, and its tooltip and window say *Restart rekordbox to apply: …* and list what
  changes.
- **A few options are read once too**, when rekordbox starts: the parts of betterfx and audiomem, `gpu`,
  `spotifyapi_slots` (they say *restart* or *from the next launch* in their description). The Preferences window marks
  them *applies after a restart* and counts them in *Restart rekordbox to apply*.
- The [Preferences window](/mods/badge#the-preferences-window) has a **Restart rekordbox** button for all of these: it
  shows up when something is waiting for a restart.

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

Generated from the settings the mods declare in their own source files (`core/settings.h` in the repository). Mods marked
<span class="pd-tag">mac</span> exist on macOS only, <span class="pd-tag">win</span> on Windows only; a few
defaults differ per platform.

<!--@public settings-options-->
### analyzer

Playlist analysis: Open analysis window in a playlist's menu analyses the whole playlist, Spotify ones too. [More →](/mods/analyzer)

| Option | Default | Meaning |
|---|---|---|
| `analyzer` | `1` | `0` switches the mod off, `1` on. Applies after a restart of rekordbox. |
| `analyze_decks` | `3,4` | Decks to use when analysing on decks |
| `analyze_direct` | `1` | Analyse without decks. Off: tracks are loaded onto spare decks instead. (1 on, 0 off) |
| `analyze_fast` | `0` | Skip rekordbox's optional analyses. Phrase, vocal detection and similarity are left out; rekordbox adds them when the track is loaded for playing. (1 on, 0 off) |
| `analyze_gap` | `4` | Seconds between two loads [s] |
| `analyze_import` | `1` | Import Spotify tracks that aren't in the collection yet. rekordbox only analyses collection tracks. (1 on, 0 off) |
| `analyze_import_timeout` | `20` | Wait for an imported track [s] |
| `analyze_live` | `1` | Tracks at once while a deck is playing. 0: the run waits until the decks have stopped. |
| `analyze_min_free_gb` | `5` | Stop a run below this much free disk space [GB] |
| `analyze_parallel` | `6` | Tracks analysed at once. More is quicker until the computer runs out of cores; 6 was fastest on a 10-core Mac. |
| `analyze_reopen` | `1` | Reopen the playlist analysed last after a restart (1 on, 0 off) |
| `analyze_start_timeout` | `15` | Retry a track whose analysis doesn't start after [s] |
| `analyze_streams` | `3` | Spotify tracks loaded at once, all decks counted |
| `analyze_timeout` | `90` (macOS), `180` (Windows) | Give a track up after [s] |
| `analyze_window` | `1` | Open the analysis window when a run starts (1 on, 0 off) |

### artcache

Cover art: Remembers far more cover art, so scrolling and loading decks don't download it again. [More →](/mods/artcache)

| Option | Default | Meaning |
|---|---|---|
| `artcache` | `1` | `0` switches the mod off, `1` on. Applies after a restart of rekordbox. |
| `artcache_mb` | `256` | Memory for cover art [MB] |

### audiomem <span class="pd-tag">mac</span>

Audio memory: Keeps the memory rekordbox's audio works with in RAM, so playback doesn't click when the Mac runs short of memory. [More →](/mods/audiomem)

| Option | Default | Meaning |
|---|---|---|
| `audiomem` | `1` | `0` switches the mod off, `1` on. Applies after a restart of rekordbox. |
| `audiomem_ahead` | `20` | Seconds kept ready ahead of the playhead [s] (applies after a restart) |
| `audiomem_code` | `1` | Keep rekordbox's own code in memory (1 on, 0 off) (applies after a restart) |
| `audiomem_decks` | `1` | Keep a playing deck's audio ready (1 on, 0 off) (applies after a restart) |
| `audiomem_fx` | `1` | Keep the FX buffers in memory (1 on, 0 off) (applies after a restart) |
| `audiomem_lazy` | `1` | Prepare a deck's buffers off the main thread (1 on, 0 off) (applies after a restart) |

### autocue

Auto cues: Open auto cues window in a playlist's menu places intro, build, drop and outro cues. Which cue goes on which pad is set there, under Layout. [More →](/mods/autocue)

| Option | Default | Meaning |
|---|---|---|
| `autocue` | `1` | `0` switches the mod off, `1` on. Applies after a restart of rekordbox. |
| `autocue_build_bars` | `0` | Build cues before their drop (0: Where the build starts; 4: 4 bars; 8: 8 bars; 16: 16 bars; 32: 32 bars) |
| `autocue_claude_model` | `haiku` | Claude Code model. An alias (haiku, sonnet) or a full name. |
| `autocue_claude_path` | *(empty)* | Where the claude program is. Only if it isn't found by itself. |
| `autocue_intro` | `sound` | Intro cue at (sound: The first sound; drums: The first drums; start: The track's first bar) |
| `autocue_memory` | `00000000` | Memory cues. Per cue in the same order: 1 also writes it as a memory cue. |
| `autocue_model` | `anthropic/claude-haiku-5.5` | OpenRouter model. Any model id from openrouter.ai/models. |
| `autocue_outro_bars` | `0` | Outro cue before the end (0: Where the last loud part ends; 16: 16 bars; 32: 32 bars; 64: 64 bars) |
| `autocue_pads` | `ABCDEFGH` | Hot cue pads. The pad of Intro, Build, Drop, Breakdown, Build 2, Drop 2, Outro and Fake drop, in that order (- for none). Easier in the Auto cues window, under Layout. |
| `autocue_parallel` | `3` | Tracks analysed at once |
| `autocue_provider` | `openrouter` | Who places the cues (openrouter: A model from OpenRouter (your key); claude: Claude Code on this computer; none: No model: the built-in rules) |
| `autocue_stems` | `1` | Listen to the stems. Off: three bands of the mix instead, faster, no separation. (1 on, 0 off) |

### badge

PLATENDOOS chip: The chip in rekordbox's bottom bar: hover for the versions, click for a small window with a way to these Preferences. [More →](/mods/badge)

| Option | Default | Meaning |
|---|---|---|
| `badge` | `1` | `0` switches the mod off, `1` on. Applies after a restart of rekordbox. |

### betterfx <span class="pd-tag">mac</span>

Mixer effects: Better-sounding versions of rekordbox's filter, EQ, reverb, echo and roll. [More →](/mods/betterfx)

| Option | Default | Meaning |
|---|---|---|
| `betterfx` | `1` | `0` switches the mod off, `1` on. Applies after a restart of rekordbox. |
| `betterfx_echo` | `1` | Echo and Pitch Echo. No dips while the tempo changes. (1 on, 0 off) (applies after a restart) |
| `betterfx_eq` | `1` | EQ and isolator. The same curves, without thumps on fast moves. (1 on, 0 off) (applies after a restart) |
| `betterfx_filter` | `1` | Color FX Filter. The FLX4's filter knob: a smooth sweep and the full range. (1 on, 0 off) (applies after a restart) |
| `betterfx_reverb` | `1` | Reverb. A new, smoother reverb. (1 on, 0 off) (applies after a restart) |
| `betterfx_roll` | `1` | Roll, Reverse Roll and Slip Roll. No dip on every repeat. (1 on, 0 off) (applies after a restart) |

### bettergrid

Beat grids: Better beat grids: the Beat This! beat tracker's tempo and bar starts, with every beat on the hits in the audio. [More →](/mods/bettergrid)

| Option | Default | Meaning |
|---|---|---|
| `bettergrid` | `1` | `0` switches the mod off, `1` on. Applies after a restart of rekordbox. |
| `bettergrid_apply` | `1` | Use the better grids. Off: both grids are only compared in the log, and rekordbox's is kept. (1 on, 0 off) |
| `bettergrid_bars` | `1` | Let the music decide where the bar starts. When rekordbox and Beat This! disagree: the 1 goes on the beat where the track changes. (1 on, 0 off) |
| `bettergrid_debug` | `0` | Log the beats of both grids |
| `bettergrid_genre` | `1` | Let the genre pick between half and double tempo. A drum & bass track at 87 becomes 174. (1 on, 0 off) |
| `bettergrid_halfbeat` | `1` | Move grids that sit on the off-beats |
| `bettergrid_kick` | `1` | Move the grid onto the kicks. When the kick sits a steady 50 ms or more off the grid over the whole track. (1 on, 0 off) |
| `bettergrid_min_fit` | `0.6` | Trust Beat This! from this fit on |
| `bettergrid_offbeat` | `1` | Fix grids that sit on a hi-hat or a clap (1 on, 0 off) |
| `bettergrid_rb_first` | `1` | Show rekordbox's grid on the deck meanwhile (1 on, 0 off) |
| `bettergrid_slow` | `0` | Wait for Beat This! on a deck even where it is slow. Windows and Intel Macs. (1 on, 0 off) |
| `bettergrid_title` | `BT BPM` | Title of the Beat This! BPM column |
| `bettergrid_wait` | `20` (macOS), `60` (Windows) | Wait for the combined grid at most [s] |
| `bettergrid_warmup` | `1` | Run the model once at launch (1 on, 0 off) |

### betterkey

Keys: Detects keys with a neural network trained for it, which is right more often than rekordbox. [More →](/mods/betterkey)

| Option | Default | Meaning |
|---|---|---|
| `betterkey` | `1` | `0` switches the mod off, `1` on. Applies after a restart of rekordbox. |
| `betterkey_min_conf` | `0` | Only when the network is at least this sure. 0 to 1. 0: always. |
| `betterkey_mode` | `compare` | Key to use (compare: rekordbox's (compare only); replace: The network's) |
| `betterkey_passcheck` | `0` | Check the audio taken from the analysis pass. Compares it with a fresh read of the file and logs the difference. (1 on, 0 off) |
| `betterkey_title` | `CNN Key` | Title of the key column |
| `betterkey_wait` | `120` | Wait for the network at most [s] |

### betterstems

HTDemucs stems: Separates stems with HTDemucs v4 instead of rekordbox's own engine: cleaner stems. [More →](/mods/betterstems)

| Option | Default | Meaning |
|---|---|---|
| `betterstems` | `1` | `0` switches the mod off, `1` on. Applies after a restart of rekordbox. |
| `betterstems_grid_first` | `1` | Wait for the beat grid before separating. Off: a track's stems start at once, next to its beat detection. (1 on, 0 off) |
| `betterstems_live_rest` | `0` | Rest between segments while a deck plays. In times the segment's own duration. |
| `betterstems_safe` | `0` | Safer GPU mode. Windows: for a graphics card that gives noisy stems. (0: Off; 1: No graph fusion; 2: Safe mode) (applies after a restart) (Windows only) |
| `betterstems_slow` | `0` | Use HTDemucs even where it is slow. Windows and Intel Macs: on the CPU, or on a GPU that needs over a second for a segment. (1 on, 0 off) (applies after a restart) |
| `betterstems_warmup` | `1` | Run the model once at launch (1 on, 0 off) |

### fastwave

Quick waveforms: Deck waveforms come back sooner after a stem is muted or a track is loaded. [More →](/mods/fastwave)

| Option | Default | Meaning |
|---|---|---|
| `fastwave` | `1` | `0` switches the mod off, `1` on. Applies after a restart of rekordbox. |
| `fastwave_ms` | `30` | Wait before redrawing. rekordbox's own wait is 500 ms. [ms] |

### flx4

DDJ-FLX4: Four decks and stems on a DDJ-FLX4, on SHIFT combinations the controller doesn't use itself. [More →](/mods/flx4)

| Option | Default | Meaning |
|---|---|---|
| `flx4` | `1` | `0` switches the mod off, `1` on. Applies after a restart of rekordbox. |
| `flx4_device` | `DDJ-FLX4` | MIDI device. For another device that uses the same mapping. |
| `flx4_keep_tempo` | `1` | A deck you switch to keeps its tempo. Off: it takes the tempo rekordbox remembers its fader at. (1 on, 0 off) |
| `flx4_log` | `0` | Log every MIDI message (1 on, 0 off) |
| `flx4_meterlog` | `0` | Log the level meters (1 on, 0 off) |
| `flx4_mixer` | *(empty)* | Mixer channels (empty: Follow the decks; fixed: Stay on decks 1 and 2) |

### fontcache <span class="pd-tag">mac</span>

Text drawing: Makes drawing text a lot cheaper, so scrolling the track list feels lighter. [More →](/mods/fontcache)

| Option | Default | Meaning |
|---|---|---|
| `fontcache` | `1` | `0` switches the mod off, `1` on. Applies after a restart of rekordbox. |

### framerate

Frame rate: Full frame rate while a deck plays or you use the computer, 30 a second the rest of the time. [More →](/mods/framerate)

| Option | Default | Meaning |
|---|---|---|
| `framerate` | `1` | `0` switches the mod off, `1` on. Applies after a restart of rekordbox. |
| `framerate_empty` | `1` | Full frame rate with empty decks too (1 on, 0 off) |
| `framerate_idle_after` | `3` | Go to 30 a second after. Seconds without touching the mouse or keyboard. [s] |

### general



| Option | Default | Meaning |
|---|---|---|
| `gpu` | `1` | Run the models on the graphics card. Off: Beat This! and the genre model run on the processor, and stems come from rekordbox's own engine. (1 on, 0 off) (applies after a restart) |
| `worker_priority` | `1` | Priority of background work. What the mods' analysis and separation threads run at, so the window and the audio stay quick. Automatic: below normal on a PC with four or more cores. (1: Automatic; 0: Normal; -1: Below normal; -2: Lowest) (Windows only) |

### genres

Genres: Fills the Genre field from what the music sounds like. [More →](/mods/genres)

| Option | Default | Meaning |
|---|---|---|
| `genres` | `1` | `0` switches the mod off, `1` on. Applies after a restart of rekordbox. |
| `genres_detail` | `style` | Genre to write (style: The specific style (Deep House); broad: The broad genre (House)) |
| `genres_on_analyse` | `0` | Fill empty Genre fields when a track is analysed (1 on, 0 off) |
| `genres_warmup` | `1` | Load the model at launch (1 on, 0 off) |

### idlewake <span class="pd-tag">mac</span>

Idle work: Less work while nothing plays. [More →](/mods/idlewake)

| Option | Default | Meaning |
|---|---|---|
| `idlewake` | `1` | `0` switches the mod off, `1` on. Applies after a restart of rekordbox. |
| `idlewake_check` | `0` | Check the skipped envelope followers (1 on, 0 off) |
| `idlewake_envelope` | `1` | Rest the envelope followers on silence (1 on, 0 off) |
| `idlewake_log` | `0` | Log what was left out every 10 seconds (1 on, 0 off) |
| `idlewake_meters` | `1` | Rest the level meters' timers (1 on, 0 off) |
| `idlewake_midi` | `1` | Rest the MIDI output thread (1 on, 0 off) |

### keepmaster

Steady sync master: Changing the sync master never changes the tempo of a deck you hear. [More →](/mods/keepmaster)

| Option | Default | Meaning |
|---|---|---|
| `keepmaster` | `1` | `0` switches the mod off, `1` on. Applies after a restart of rekordbox. |

### keycolumn

Key at master tempo: A column with each track's key as it would sound at the master deck's tempo. [More →](/mods/keycolumn)

| Option | Default | Meaning |
|---|---|---|
| `keycolumn` | `1` | `0` switches the mod off, `1` on. Applies after a restart of rekordbox. |
| `keycol_title` | `Key @ Master` | Title of the column |

### linein

Turntables and other inputs: Play a turntable, a CD player or any audio input through a deck, with the mixer's EQ, filter, faders and headphone cue on it. [More →](/mods/linein)

| Option | Default | Meaning |
|---|---|---|
| `linein` | `1` | `0` switches the mod off, `1` on. Applies after a restart of rekordbox. |
| `linein_1` | *(empty)* | Deck 1: input. The audio input this deck plays: the device's name, "name #2" for a second one of the same name, or its UID. |
| `linein_1_ch` | `1-2` | Deck 1: channels. The device's channels to take: 1-2, 3-4, or 1 for a mono input. |
| `linein_1_gain` | `0` | Deck 1: gain. Gain on the input, on top of the mixer's trim. [dB] |
| `linein_1_on` | `1` | Deck 1: on. Off: this deck plays tracks only. (1 on, 0 off) |
| `linein_2` | *(empty)* | Deck 2: input. The audio input this deck plays: the device's name, "name #2" for a second one of the same name, or its UID. |
| `linein_2_ch` | `1-2` | Deck 2: channels. The device's channels to take: 1-2, 3-4, or 1 for a mono input. |
| `linein_2_gain` | `0` | Deck 2: gain. Gain on the input, on top of the mixer's trim. [dB] |
| `linein_2_on` | `1` | Deck 2: on. Off: this deck plays tracks only. (1 on, 0 off) |
| `linein_3` | *(empty)* | Deck 3: input. The audio input this deck plays: the device's name, "name #2" for a second one of the same name, or its UID. |
| `linein_3_ch` | `1-2` | Deck 3: channels. The device's channels to take: 1-2, 3-4, or 1 for a mono input. |
| `linein_3_gain` | `0` | Deck 3: gain. Gain on the input, on top of the mixer's trim. [dB] |
| `linein_3_on` | `1` | Deck 3: on. Off: this deck plays tracks only. (1 on, 0 off) |
| `linein_4` | *(empty)* | Deck 4: input. The audio input this deck plays: the device's name, "name #2" for a second one of the same name, or its UID. |
| `linein_4_ch` | `1-2` | Deck 4: channels. The device's channels to take: 1-2, 3-4, or 1 for a mono input. |
| `linein_4_gain` | `0` | Deck 4: gain. Gain on the input, on top of the mixer's trim. [dB] |
| `linein_4_on` | `1` | Deck 4: on. Off: this deck plays tracks only. (1 on, 0 off) |
| `linein_empty` | `1` | Only while the deck is empty. Loading a track gives the deck back to the track; unloading it brings the input back. Off: the deck always plays its input, and a loaded track stays silent. (1 on, 0 off) |
| `linein_extra_ms` | `0` | Extra latency. For an input whose audio arrives unevenly. Normally the deck plays about 12 ms behind the input. [ms] |
| `linein_log` | `0` | Log the inputs every 10 seconds. Per deck: the input's clock against the engine's, the distance kept and the level. (1 on, 0 off) |

### midipoll <span class="pd-tag">mac</span>

Controller detection: Looks for newly plugged-in MIDI controllers every 2 seconds instead of every 0.3. [More →](/mods/midipoll)

| Option | Default | Meaning |
|---|---|---|
| `midipoll` | `1` | `0` switches the mod off, `1` on. Applies after a restart of rekordbox. |

### multisort

Sort by several columns: Each click on a column header adds it to the sort. [More →](/mods/multisort)

| Option | Default | Meaning |
|---|---|---|
| `multisort` | `1` | `0` switches the mod off, `1` on. Applies after a restart of rekordbox. |

### outputsync <span class="pd-tag">mac</span>

Speakers and headphones: Lines up speakers and headphones when rekordbox plays to two devices, such as Bluetooth speakers and a controller. [More →](/mods/outputsync)

| Option | Default | Meaning |
|---|---|---|
| `outputsync` | `1` | `0` switches the mod off, `1` on. Applies after a restart of rekordbox. |
| `outputsync_clocks` | `0` | Log the devices' clocks every 30 seconds (1 on, 0 off) |
| `outputsync_lineup` | `1` | Line up speakers and headphones. Delays the quicker device so both sound at the same moment. (1 on, 0 off) |
| `outputsync_ms` | `auto` | Delay to use. auto: what the devices report. A number: delay the quicker devices by that many milliseconds. |
| `outputsync_offset` | `0` | Fine-tune. To the right the headphones play later, to the left earlier. [ms] |

### parallel

Analysis on three cores: Spreads the heaviest part of analysing a track over three cores instead of one. [More →](/mods/parallel)

| Option | Default | Meaning |
|---|---|---|
| `parallel` | `1` | `0` switches the mod off, `1` on. Applies after a restart of rekordbox. |

### prefs

rekordbox's preferences in this window: Shows rekordbox's own preferences in this window, next to the mods' settings: the same settings, set through rekordbox's own controls. rekordbox's Preferences command (⌘, its menu, the gear) opens this window too; pages it can't show open rekordbox's own dialog. [More →](/mods/prefs)

| Option | Default | Meaning |
|---|---|---|
| `prefs` | `1` | `0` switches the mod off, `1` on. Applies after a restart of rekordbox. |

### quickdeck

Decks show sooner: A track that isn't analysed yet shows its waveforms and a beat grid on the deck as soon as there are any. [More →](/mods/quickdeck)

| Option | Default | Meaning |
|---|---|---|
| `quickdeck` | `1` | `0` switches the mod off, `1` on. Applies after a restart of rekordbox. |
| `quickdeck_gridonly` | `1` | A new grid redraws only the grid. Off: the deck's waveforms are built again, as rekordbox does. (1 on, 0 off) |
| `quickdeck_log` | `0` | Log each partial overview (1 on, 0 off) |
| `quickdeck_merge` | `0.25` | Wait for rekordbox's own grid. Windows: so one redraw of the deck shows both. [s] |
| `quickdeck_progressive` | `0` | Start analysing while a Spotify track downloads. An unfinished experiment. (1 on, 0 off) |
| `quickdeck_stream` | `1` | The overview waveform grows while the track is read (1 on, 0 off) |

### quickframe <span class="pd-tag">mac</span>

Quicker clicks: What you click shows on screen sooner: a shorter queue of frames. [More →](/mods/quickframe)

| Option | Default | Meaning |
|---|---|---|
| `quickframe` | `1` | `0` switches the mod off, `1` on. Applies after a restart of rekordbox. |
| `quickframe_drawables` | `2` | Frames queued for the display (2: 2 (quicker); 3: 3 (rekordbox's own)) |
| `quickframe_playing` | `0` | Also while a deck plays. Quicker clicks, but a frame that is late then shows as a small stutter. (1 on, 0 off) |
| `quickframe_stopped` | `1` | Show frames at once while nothing plays (1 on, 0 off) |

### relayout <span class="pd-tag">mac</span>

Lighter layout: Menus and Preferences open sooner and resizing is smoother: rekordbox no longer lays out what isn't showing. [More →](/mods/relayout)

| Option | Default | Meaning |
|---|---|---|
| `relayout` | `1` | `0` switches the mod off, `1` on. Applies after a restart of rekordbox. |
| `relayout_check` | `0` | Check every button image that was kept (1 on, 0 off) |
| `relayout_hidden` | `1` | Skip panels that aren't showing (1 on, 0 off) |
| `relayout_log` | `0` | Log each layout pass (1 on, 0 off) |

### screeninfo <span class="pd-tag">mac</span>

Screen lookups: Stops rekordbox from asking macOS about your screen on every redraw. [More →](/mods/screeninfo)

| Option | Default | Meaning |
|---|---|---|
| `screeninfo` | `1` | `0` switches the mod off, `1` on. Applies after a restart of rekordbox. |

### spotifyapi

Spotify lists: Loads several Spotify track lists at once instead of one after the other. [More →](/mods/spotifyapi)

| Option | Default | Meaning |
|---|---|---|
| `spotifyapi` | `1` | `0` switches the mod off, `1` on. Applies after a restart of rekordbox. |
| `spotifyapi_slots` | `3` | Lists loaded at once (applies after a restart) |

### spotifyaudio

Spotify audio for the windows: Lets the auto cues and genres windows hear a Spotify track, read from the stream into memory. [More →](/mods/spotifyaudio)

| Option | Default | Meaning |
|---|---|---|
| `spotifyaudio` | `1` | `0` switches the mod off, `1` on. Applies after a restart of rekordbox. |
| `spotifyaudio_mb` | `256` | Memory for the tracks read last [MB] |
| `spotifyaudio_wait` | `90` | Keep trying to open a stream for [s] |

### spotifydl

Spotify load times: Logs where a Spotify track's load time goes. [More →](/mods/spotifydl)

| Option | Default | Meaning |
|---|---|---|
| `spotifydl` | `1` | `0` switches the mod off, `1` on. Applies after a restart of rekordbox. |
| `spotifydl_debug` | `0` | Also log every audio piece (1 on, 0 off) |

### spotifyfix

Spotify fixes: Stops Spotify playlists from reloading themselves forever, and fixes a deadlock in rekordbox's Spotify client. [More →](/mods/spotifyfix)

| Option | Default | Meaning |
|---|---|---|
| `spotifyfix` | `1` | `0` switches the mod off, `1` on. Applies after a restart of rekordbox. |

### spotifylog

Spotify log: Stops rekordbox from rewriting its Spotify log file several times a second. [More →](/mods/spotifylog)

| Option | Default | Meaning |
|---|---|---|
| `spotifylog` | `1` | `0` switches the mod off, `1` on. Applies after a restart of rekordbox. |

### spotifystart

Spotify start fix: Fixes Spotify hanging on "loading" at launch in rekordbox 7.2.19. [More →](/mods/spotifystart)

| Option | Default | Meaning |
|---|---|---|
| `spotifystart` | `1` | `0` switches the mod off, `1` on. Applies after a restart of rekordbox. |

### stemcache

Stems that are kept: Keeps separated stems, so a track has them at once the next time. A playlist's menu has Open stems window to separate a whole playlist ahead. [More →](/mods/stemcache)

| Option | Default | Meaning |
|---|---|---|
| `stemcache` | `1` | `0` switches the mod off, `1` on. Applies after a restart of rekordbox. |
| `stemcache_dir` | *(empty)* | Folder. Empty: the default folder. |
| `stemcache_keep` | `all` | Save stems to disk. Everything: every track that gets stems. Only what I separate: a track you just load onto a deck gets stems that are thrown away when rekordbox quits. (all: Everything; chosen: Only what I separate in the stems window; none: Nothing) |
| `stemcache_lazy_model` | `1` | Load rekordbox's own stem model only when needed. With HTDemucs separating, rekordbox's own model (680 MB for the quality one) stays out of memory until it has to separate after all. (1 on, 0 off) (applies after a restart) |
| `stemcache_log` | `0` | Log every chunk and stem control (1 on, 0 off) |
| `stemcache_max_gb` | `10` | Disk space for stems. Past this the stems used longest ago are deleted. 0: no limit. [GB] |
| `stemcache_memory_mb` | `384` | Memory for stems that aren't saved [MB] |
| `stemcache_min_free_gb` | `5` | Save nothing new below this much free space [GB] |
| `stemcache_ondemand` | `0` | Separate only when a stem control is used. A deck's track is separated from the moment you mute a stem, turn a PART ISOLATOR knob or take the FX off a stem, starting where it plays. (1 on, 0 off) |
| `stemcache_quality` | `256` | Quality of saved stems. 256 kbps takes about 35 MB for a 5-minute track, lossless about 125 MB. (128: 128 kbps; 192: 192 kbps; 256: 256 kbps; lossless: Lossless) |
| `stemcache_quick` | `1` | Stems at the playhead at once. With HTDemucs: when you use a stem control where the track has no stems yet, a few seconds at the playhead are separated first, before the rest of that part of the track (macOS). (1 on, 0 off) |
| `stemcache_spotify` | `1` | Stems for Spotify tracks (1 on, 0 off) |
| `stemcache_staging` | `auto` | Write to the internal disk first (0: Never; 1: Always; auto: When the folder is on an external drive) |
| `stemcache_staging_gb` | `4` | How much may wait on the internal disk [GB] |

### streamio

Faster reading: Reads a track's audio faster during analysis. [More →](/mods/streamio)

| Option | Default | Meaning |
|---|---|---|
| `streamio` | `1` | `0` switches the mod off, `1` on. Applies after a restart of rekordbox. |
| `streamio_phrase` | `1` | Reuse the audio for phrase analysis (1 on, 0 off) |

### tfpool

Beat model stays loaded: Keeps rekordbox's beat-detection model loaded between tracks instead of loading it for each one. [More →](/mods/tfpool)

| Option | Default | Meaning |
|---|---|---|
| `tfpool` | `1` | `0` switches the mod off, `1` on. Applies after a restart of rekordbox. |
| `tfpool_warmup` | `1` | Warm the beat models up at launch (1 on, 0 off) |

### tidymenu

Tidy track menu: A calmer right-click menu on tracks. [More →](/mods/tidymenu)

| Option | Default | Meaning |
|---|---|---|
| `tidymenu` | `1` | `0` switches the mod off, `1` on. Applies after a restart of rekordbox. |
| `tidymenu_group` | `1` | Put the mods' items in a platendoos submenu (1 on, 0 off) |
| `tidymenu_hide` | `1` | Hide greyed-out items (1 on, 0 off) |
| `tidymenu_log` | `0` | Log the items of each menu (1 on, 0 off) |

### timing

Analysis timing: Logs how long each step of an analysis takes. [More →](/mods/timing)

| Option | Default | Meaning |
|---|---|---|
| `timing` | `1` | `0` switches the mod off, `1` on. Applies after a restart of rekordbox. |

### usbphrases

Phrases from USB sticks: A track loaded straight from a USB stick shows its phrases under the waveform, as a collection track does. They come from the stick's export; nothing is copied into your collection, and phrase edits on such a deck are not saved. [More →](/mods/usbphrases)

| Option | Default | Meaning |
|---|---|---|
| `usbphrases` | `1` | `0` switches the mod off, `1` on. Applies after a restart of rekordbox. |

### xfadelock

Crossfader lock: A small lock next to the on-screen crossfader holds it exactly in the middle. [More →](/mods/xfadelock)

| Option | Default | Meaning |
|---|---|---|
| `xfadelock` | `1` | `0` switches the mod off, `1` on. Applies after a restart of rekordbox. |
<!--@/public-->
