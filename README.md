<p align="center">
  <img src="docs/platendoos.png" alt="PLATENDOOS" width="360">
</p>

<p align="center"><em>A box of records, and a box of tweaks for rekordbox.</em></p>

# platendoos

platendoos is a set of mods for **rekordbox 7 on macOS** (Apple Silicon; Intel in beta) **and Windows** (x64). It makes rekordbox faster and
smoother, especially with Spotify, and adds a few features it doesn't have. You can tell a modded rekordbox
by the small **PLATENDOOS** chip next to the rekordbox logo at the bottom left. Hover over it to see the
build, or click it to see every mod and whether it's on.

Your own rekordbox is never touched. On macOS platendoos makes a separate copy, `~/Applications/platendoos.app`,
that loads the mods; on Windows `platendoos.exe` starts your installed rekordbox with the mods loaded, without
changing any of its files. Open rekordbox the usual way whenever you want it plain.

<p align="center"><b><a href="https://media.idiidk.nl/platendoos-trailer.mp4">▶ Watch the trailer</a></b> (2:57, with sound), recorded in rekordbox.</p>

> Made for and tested on rekordbox **7.2.19**; **7.2.20** was checked the day it came out and runs with every mod
> on, on both platforms. On macOS, after a rekordbox update the mods try to find their way around by themselves;
> any mod that can't is switched off and logged, so the rest keep working. On Windows every rekordbox build needs
> its own table: for a version without one the patcher makes it on the spot from the signatures it carries, the
> mods it can't place are off, and rekordbox runs as normal. How a new version is checked and fixed is in
> [docs/rekordbox-updates.md](docs/rekordbox-updates.md) and [docs/windows.md](docs/windows.md).
>
> Nine mods are macOS-only: **outputsync**, **betterfx**, **audiomem**, **quickframe**, **idlewake**, and
> **fontcache**, **screeninfo**, **midipoll** and **relayout** (the last four fix slow macOS calls that Windows
> doesn't have). The chip's window lists what's on.

---

## The mods

### Spotify

| Mod | What it does |
|---|---|
| **spotifyapi** | Lets rekordbox load several Spotify track lists at once instead of one after the other. A big playlist shows up roughly twice as fast. |
| **spotifyfix** | Stops Spotify playlists from endlessly reloading themselves, and fixes a deadlock in rekordbox's Spotify client that could freeze every Spotify download until you force quit (a token refresh colliding with a playlist refresh). A Spotify logout (e.g. after a permission error) first lets running downloads end instead of locking up on them. |
| **spotifystart** | Windows, rekordbox 7.2.19 only. Fixes Spotify hanging on "loading the library" after a restart: a client restart that lands while Spotify is still starting no longer blocks that start. Does nothing on any other build. |
| **autocue** | Sets cues (intro, build, drop, breakdown, build 2, drop 2, outro, fake drop) on a playlist as hot cues A–H, as memory cues, or both, Spotify tracks too (read from the stream into memory by **spotifyaudio**). *Open auto cues window* in a playlist's menu, then *Analyse*: each ticked track's arrangement is worked out (which bars repeat which, per stem when betterstems is on, else in three bands of the mix) and the built-in rules or a language model place the cues (the model follows instructions you type, e.g. "drop cues one bar early"). Then you fine-tune in the same window: pick a track, see its waveform, sections and clips, click or double-click a cue to listen, drag or nudge cues by the bar, beat or half beat, place and remove them, ⌘Z / Ctrl+Z to undo. Stems already separated (stemcache) are read from disk, and stems autocue separates are kept for the decks (a Spotify track's in memory only, for the rest of the session). *Layout* says which cue goes on which pad and which are memory cues too (older CDJs show only three or four hot cues; every one has ten memory cues), with presets, and holds five rulesets for where cues go, each rule also settable by itself (build cues 4 to 32 bars before their drop, the intro cue on the first drums or the first bar, the outro cue 16 to 64 bars before the end); they need no model and hold for a model's cues too. Nothing reaches rekordbox until *Write cues*; Undo takes it back. Analyses, answers and edits are remembered. The model is either one from OpenRouter with your own key (any model), or the Claude Code CLI installed on your computer (its own login, no key; the window lists the models it offers). Without either, or with *No model* chosen under ⚙, the built-in rules place the cues: nothing is sent anywhere, no key, no cost (*Layout*, the rulesets and *No model* since 0.6.0). |
| **genres** | Fills the Genre field from what the music sounds like: a style model (Discogs-EffNet from Essentia, 400 Discogs styles, best with electronic music) listens to each track. *Open genres window* in a local playlist's menu lists its tracks; *Detect genres* suggests a genre per track, as the specific style ("Liquid Funk") or a broad genre ("Drum & Bass"), with the next two guesses in a dropdown and the model's confidence; Apply fills empty Genre fields (all of them with Replace), Remove genres empties them, Undo puts them back. Optionally it also fills an empty Genre field whenever rekordbox analyses a track (*Fill empty genres on analysis*, the same switch as *Genre first* in the analysis window): the model listens before the beat grid is made, so the genre it writes is the one the tempo was picked with. The model is CC BY-NC-SA 4.0: non-commercial use. rekordbox doesn't show a Spotify track's genre in Spotify playlists; the mod puts the ones it wrote into the Genre column there. |
| **artcache** | Remembers far more cover art (256 MB in memory instead of rekordbox's 26), so scrolling and loading decks don't download the same images over and over. |
| **spotifyaudio** | Reads a Spotify track's audio from the stream into memory when **autocue**'s or **genres**' window needs to hear the whole track, the way rekordbox's own analysis reads it. Nothing is written to disk: the last few tracks stay in memory (`spotifyaudio_mb=256`) and are gone when rekordbox quits. |
| **spotifylog** | Stops rekordbox from rewriting its Spotify log file several times a second while it syncs. |
| **spotifydl** | Logs where a Spotify track's load time goes: slow or failing requests and how long each download took. Changes nothing. |

### Analysis

| Mod | What it does |
|---|---|
| **analyzer** | *Open analysis window* in any playlist's menu analyses that playlist, Spotify ones too. It runs several tracks at once, in a window that shows progress per track with pause, stop and retry. While a deck is playing, a run takes one track at a time, so analysing during a set doesn't take the cores the audio needs (`analyze_live=0`: it waits until the decks have stopped). With the genres mod the window has a Genre column and *Genre first*: each track's genre is detected first, picks the tempo of its beat grid (bettergrid) and fills its empty Genre field. |
| **parallel** | Spreads the heaviest part of analysing a track over three cores instead of one. |
| **tfpool** | Keeps rekordbox's beat-detection neural network loaded between tracks instead of reloading it for each one. |
| **streamio** | Smarter reading of a track's audio during analysis. That's about 10% faster, and the phrase analysis is 4× faster. |
| **betterkey** | Detects musical keys with a neural network trained for it, which gets keys right more often than rekordbox's built-in detector: 67.5 % of the GiantSteps key dataset's tracks against rekordbox's 56.8 %. |
| **bettergrid** | Better beat grids. Every analysed track also goes through the *Beat This!* beat tracker: its tempo and bar starts (it gets those right more often than rekordbox) with each beat placed exactly on the hits in the audio (where rekordbox was better). When rekordbox and Beat This! disagree on where the bar starts, the music decides: the 1 goes on the beat where the track changes (drops, breakdowns, a new bass line), or else where the kick comes in (`bettergrid_bars=0`: the kick alone). When rekordbox's beats sit on the off-beats (half a beat away) and both Beat This! and the kick drum say so, they are moved onto the beat. When Beat This! is unsure, rekordbox's own grid stays. A track that rekordbox's *Cloud Analysis* (Preferences > Analysis) finds online gets that analysis instead and skips beat detection, so it keeps rekordbox's grid: turn Cloud Analysis off for Beat This! grids on every track. Runs next to rekordbox's own beat detection, so analysing isn't slower; where Beat This! is the slower of the two (Windows), a track on a deck shows rekordbox's grid meanwhile and the combined grid replaces it. Locked grids aren't touched; `bettergrid_apply=0` only compares. The track's BPM in the collection (the BPM column, the deck's title) follows the saved grid: rekordbox keeps its own number there, so a grid at half its tempo used to play at 70 on a track listed as 140 (macOS). |
| **timing** | Logs how long each step of an analysis takes. |
| **quickdeck** | A Spotify track (or any track that isn't analysed yet) shows its waveforms and a beat grid on the deck as soon as the analysis has them, instead of only when the whole analysis is done. With **bettergrid** on, that early grid is already the Beat This! one, or rekordbox's own when that is ready first (on Windows it is, and comes in the same redraw as the waveforms). On macOS the overview waveform grows from the left while the track is still being read, and a new grid only redraws the grid lines instead of the whole waveform. |

### Browsing and playing

| Mod | What it does |
|---|---|
| **stemcache** | Keeps rekordbox's separated stems on disk, so a track you've had on a deck before has its stems ready at once instead of separating again. *Open stems window* in a playlist's menu shows which of its tracks are separated; *Separate stems* there does the whole playlist ahead of time (about 4 s per track) with progress per track (about 15 s per track with **betterstems**, a few seconds when the stems are already on disk). A track that's on a deck counts once the deck has separated it. Spotify tracks get stems on a deck too (rekordbox itself never separates them), from the first load: separated from the stream as it plays. A Spotify track's stems are never stored on disk; the last ones are kept in memory, compressed (`stemcache_memory_mb=384`), so loading the track again in the same session has them at once. `stemcache_spotify=0` turns Spotify stems off. **Which stems are kept** is yours to choose (*Keep on disk* in the stems window, or `stemcache_keep`): *Every track* (the default), *Tracks separated here* (only the playlists and selections you separate in the stems window are stored; any other track is separated when it loads onto a deck and those stems are gone when rekordbox quits) or *None*. **Stems on demand** (the checkbox in the stems window, or `stemcache_ondemand=1`; on Windows new and not yet run on a Windows machine): a deck's track is no longer separated when it loads, only from the moment you mute a stem, turn a PART ISOLATOR knob or take the FX off a stem, and then starting where the track plays (about a second until the stems are there with **betterstems** on an M2 Pro); stems that are stored already are still there at once. Stems are stored as AAC: `stemcache_quality=256` (default; ~35 MB per 5-minute track), `192`, `128` kbps per stem, or `lossless` (~125 MB). Stems are only heard while a stem is turned down (rekordbox plays the original track otherwise), so 256 kbps is inaudible in practice. Older lossless chunks are still used. Uses up to 10 GB (`stemcache_max_gb`) and stops storing when the disk is nearly full. |
| **fastwave** | The deck waveforms come back sooner after they change, e.g. when you mute or unmute a stem or load a track. rekordbox waits a fixed half second before it even starts rebuilding a waveform (the rebuild itself takes about 0.1 s) and then checks every 100 ms whether it's done, so the waveform was blank for most of a second on every stem toggle. This waits 30 ms instead (`fastwave_ms`) and checks every 15 ms; resizing the window keeps rekordbox's own delay. |
| **betterstems** | Separates stems with HTDemucs v4 instead of rekordbox's own engine: cleaner stems (about 9 dB SDR against Spleeter's 5.8), on the GPU, about 1 s per 12 s of audio. Its model (compiled, 103 MB) is in the app, so a copied app has it too; `tools/models/htdemucs_coreml.sh` remakes it (downloads ~1 GB of Python packages). On Windows it runs the ONNX version through rekordbox's own ONNX Runtime, on the GPU (DirectML) when there is one that separates fast enough (a Radeon RX 580 does a track in about 50 s). Without one rekordbox's own engine separates, as without this mod: HTDemucs on the processor would keep most cores busy for minutes per track (`betterstems_slow=1` uses it anyway). On every platform stems wait for a track's beat grid: while the track is in beat detection the stems hold back for a few seconds, so the grid isn't later for them. |
| **betterfx** | Better-sounding mixer effects (macOS). The Color FX **Filter** sweeps smoothly and reaches its intended 25 kHz top (rekordbox's low-pass started at ~12 kHz with a resonant bump there and moved in 256 steps), and full resonance stays under +9 dB instead of peaking at +24 dB. The **DJM-900NXS2 EQ** (the default type) and the **isolator** smooth knob moves over a fixed time, so fast kills no longer thump or click with small audio buffers. Same curves and knob feel as rekordbox otherwise; `betterfx_filter=0` / `betterfx_eq=0` switch a part off. Notes in docs/dsp/. |
| **audiomem** | Keeps the memory rekordbox's audio works with in RAM, so playback doesn't click when the Mac runs short of memory (macOS). When other apps fill the memory, macOS compresses and swaps out what wasn't used for a few seconds, and rekordbox's audio then has to wait for it: each wait is a 3 ms gap on every deck, most of all right after a deck load. Plain rekordbox has the same gaps. The mod pins rekordbox's FX history buffers (about 650 MB that rekordbox writes all the time anyway), keeps the next 20 seconds of every loaded deck ready (`audiomem_ahead`), and stops a load from freezing the window while it clears the new track's buffers (about 0.04 s instead of 0.12 s). Measured with 3 GB of other memory in use: 30 gaps a minute during playback and 14 per load without it, 0 to 6 and 0 to 1 with it. When the log says `audiomem: N GB of swap in use`, close other apps before a set; a larger buffer size in rekordbox's audio settings (256 or 512 instead of 128) gives more margin too. `audiomem_fx=0`, `audiomem_decks=0`, `audiomem_code=0` and `audiomem_lazy=0` switch a part off. |
| **keepmaster** | Changing the sync master never changes the tempo of a deck you hear. rekordbox may still move master by itself (e.g. when you pull the master's channel fader down), but only while every other playing deck already runs at the master's tempo, as synced decks do, so nothing jumps. When the master stops (pause, end, eject), master goes straight to a deck that's playing at the same tempo, so cueing the next track on the stopped deck no longer drags the playing decks along. If moving would change a playing deck's tempo, master stays where it is. MASTER on a deck still works as always; `keepmaster=0` gives rekordbox's own behaviour back. |
| **tidymenu** | A calmer right-click menu on tracks. rekordbox's greyed-out items are left out (ten of its twenty-two on a Spotify track), so only what can be clicked is listed; nothing that works goes away, and an item is back as soon as rekordbox enables it. The mods' window items go in one *platendoos* submenu under short names (*Genres window*), with *Re-analyze track* next to it. `tidymenu_hide=0` keeps the greyed-out items, `tidymenu_group=0` the mods' items in the menu itself. |
| **xfadelock** | A small lock button next to the on-screen crossfader holds the crossfader exactly in the middle. While it's lit blue, neither the controller's crossfader nor dragging the on-screen one moves it. Unlock it and the controller's crossfader takes over again only once it reaches the middle, so nothing jumps (the lock shows orange until then). It starts unlocked every time rekordbox starts. |
| **outputsync** | Lines up speakers and headphones when rekordbox plays to two devices (in rekordbox's *Preferences > Audio*: a switch, a fine-tune slider, and a metronome that clicks on both outputs so you can tune by ear), e.g. PC MASTER OUT to Bluetooth speakers with the headphones on a controller. Bluetooth sounds a few hundred milliseconds late, so the headphone cue ran ahead of the speakers; this delays the faster device by the difference the devices report (the log says how much). Bluetooth is often later than it reports: set `outputsync_ms=` a number of milliseconds and tune it by ear while both play; it's picked up within seconds. The headphones then lag your hands by the same amount. The two devices' clocks drift apart only slowly (measured: 0.3 ms a minute); when rekordbox plays through its own aggregate device, this also turns on macOS's drift compensation there, which rekordbox leaves off. |
| **keycolumn** | A new **Key @ Master** column: each track's key as it would sound at the master deck's tempo, e.g. `8A +1.2`. It updates live as you move the tempo. |
| **multisort** | Sort the track list by several columns: each click on a column header cycles it through ascending, descending and off, and the columns that are on sort together in the order you turned them on (e.g. BPM, then key within each BPM). Their titles show the order. |
| **fontcache** | Makes drawing text, most of the work when the track list repaints, a lot cheaper. Scrolling feels lighter. |
| **quickframe** | Clicks show sooner. rekordbox queues every frame three deep before it reaches the screen, so a button you press lights up about 45 ms later (measured on a 120 Hz MacBook Pro). This keeps the queue at two frames (about 32 ms, just as smooth), and while no deck is playing it shows each frame as soon as it is drawn (about 10 ms). While a deck plays the screen's own rhythm is kept, so the waveforms scroll as smoothly as before. |
| **relayout** | Menus, drop-downs, tooltips and Preferences open sooner, resizing the window is twice as smooth, and options that made the whole window refresh answer in 80 ms instead of 200. rekordbox laid out its whole main window again every time any window opened (15 to 45 ms before the menu showed); on every layout pass it rendered the images of some 300 buttons again, also the ones that hadn't changed; and it lays out and restyles all 16,700 parts of its window every time, though only about 400 are on screen. Panels you can't see now wait until you open them. |

### Controllers

**flx4** makes a Pioneer **DDJ-FLX4** a four-deck controller with stems, using SHIFT combinations the FLX4
doesn't use itself:

| Press | What it does |
|---|---|
| SHIFT + PLAY | That side switches deck: left 1 ↔ 3, right 2 ↔ 4. Its mixer strip (TRIM, EQ, CFX filter, fader), jog wheel, pads, loop buttons and beat FX go along, and the controller's lights and level meter show the new deck. |
| SHIFT + LOOP IN | The pads become STEMS pads: 1–4 turn vocal, inst, bass and drums on and off (lit while playing), 5–8 pick the stems the beat FX applies to. Press it again, or any pad mode button, to go back. |
| SHIFT + LOOP OUT | The EQ knobs control the stems instead: HI vocal, MID inst, LOW drums. Press it again for EQ. |
| SHIFT + SMART FADER | Switches between rekordbox's 2 and 4 deck layouts. |

Each switch shows a short message over rekordbox. With four decks, rekordbox's own on-screen mixer opens so
you can see every channel's settings, and the two channels the FLX4's strips are on are outlined. A knob or
fader only takes over a deck once it reaches that deck's setting, so nothing jumps; until then an orange
mark on the on-screen mixer shows where the FLX4's knob is. Turn it to the on-screen value to take over.
Switching a side never changes a deck's tempo: the tempo fader also takes over only once it reaches the deck's
tempo, and rekordbox's own habit of setting a newly controlled deck to the fader position it remembers for it
(which made a sync master, and every deck synced to it, jump to an old tempo) is switched off
(`flx4_keep_tempo=0` gives it back).
The on-screen mixer strips are laid out like the FLX4's (a smaller TRIM) with each channel's CFX filter knob
under LOW, so all four filters are visible.

The beat FX CH SELECT lever picks sides rather than channels: **1** is the deck the left side controls (1 or
3), **2** the right side's (2 or 4), **1&2** both. The FX select, beat, LEVEL/DEPTH, on/off and RELEASE FX
buttons work as usual on whichever decks that is, and follow a side when it switches deck.

Stems need rekordbox's STEMS feature: *Preferences > Extensions > STEMS*. While it's off, the stems combinations just say so and leave the pads and knobs alone. `flx4_mixer=fixed` in the config
keeps the mixer strips and beat FX on channels 1 and 2 instead. Spotify tracks get stems when **stemcache** is on
(see **stemcache**).

Under the hood, decks 3 and 4 use the same MIDI channels and mapping rows as the FLX4's four-deck sibling,
the DDJ-FLX6, which `build.sh` adds to rekordbox's FLX4 mapping. Where rekordbox's FLX4 support only knows two
decks (jog wheels, pad mode lights, level meters, loop lights), the mod fills in decks 3 and 4. Tested on a real
FLX4.

### Quieter in the background

rekordbox does a surprising amount of work while you're doing nothing. These trim it so it stays cooler and
easier on your battery.

| Mod | What it does |
|---|---|
| **framerate** | Runs the screen at full frame rate while a deck plays or you're using the computer, and at 30 fps the rest of the time. rekordbox itself is the other way round with empty decks: 30 fps even while you click around, so every button and menu in the main window answers up to 33 ms late; with a track on a deck it runs at the full rate all day, also when nothing moves. (`framerate_empty=0` keeps rekordbox's 30 fps with empty decks. Scratching a paused deck with a controller's jog wheel? Set `framerate=0`.) |
| **screeninfo** | Stops rekordbox from asking macOS about your screen on every single redraw. |
| **midipoll** | Checks for newly plugged-in MIDI controllers every 2 seconds instead of every 0.3 seconds, and quickly again whenever something is plugged in. |
| **idlewake** | Less work while nothing plays. rekordbox's MIDI output thread woke up a thousand times a second to find nothing to send, every level meter restarted a timer on each frame, and each mixer channel kept measuring the loudness of pure silence for the gate effect. All three now rest until there is something to do. |
| **badge** | The PLATENDOOS chip. Click it for the settings window: rekordbox's and platendoos's versions, every mod with its switch, and each mod's options with what they do (switches, lists and fields; a search box finds an option by name or description). What you change is written to the config file at once, and *reset* puts an option back to its default. Buttons open the log and the config file itself. It steps aside the moment rekordbox shows a message in its bottom bar, and comes back when the message is gone. |

---

## Installing

### With the patcher (no building)

The **platendoos patcher** installs platendoos for the rekordbox already on the computer. It carries only the
mods (no rekordbox files), so it can be sent to anyone with rekordbox 7. Download it from the
[latest release](https://github.com/idiidk/platendoos/releases/latest) (what changed: [CHANGELOG.md](CHANGELOG.md)):

- **macOS** (Apple Silicon; Intel Macs in beta): unzip `platendoos-patcher-mac-<version>.zip` and open
  **platendoos patcher**. The patcher isn't notarised, so the first time macOS refuses it: open *System Settings >
  Privacy & Security* and choose *Open Anyway* (or right-click the app, *Open*).
- **Intel Macs are beta** (since 0.5.0): the same patcher installs platendoos there too, but it has only been tested
  under Rosetta on an Apple Silicon Mac, not yet on a real Intel Mac. Speed (the GPU models), audio and the
  FLX4 are unknown there. If you try it, the log (`~/Library/Logs/rekordbox-mods.log`) and a note of what you did
  help a lot; rekordbox itself stays as it was. Details: [docs/intel-mac.md](docs/intel-mac.md).
- **Windows** (x64): unzip `platendoos-windows-<version>.zip` anywhere and run **platendoos patcher.exe** in the
  `platendoos` folder. It isn't signed, so SmartScreen may warn: *More info > Run anyway*.

Its window finds rekordbox and checks it before anything is changed: whether it can be patched, and anything
to do first (close rekordbox, an untested or unsupported version, disk space). Every mod has a checkbox, all on
by default; the choices go into the config (`<mod>=0`) on install, and the window starts from what the config
says. *Install* then shows each step as it goes (`--check` lists the technical details: build, hook sites,
address table):

- macOS: makes `~/Applications/platendoos.app`, a copy of rekordbox whose program is patched to load the mods,
  with the mods' models and windows, the FLX4 mapping and an inverted icon, signed for that Mac (takes seconds;
  on APFS the copy takes no extra space). rekordbox itself isn't changed.
- Windows: copies platendoos to `%LOCALAPPDATA%\Programs\platendoos`, with a Start menu shortcut (and a
  desktop one if ticked) and an entry in *Apps & features*, with rekordbox's icon inverted as on macOS (made
  from the installed rekordbox.exe). rekordbox's files aren't changed, except the
  DDJ-FLX4 mapping when that option is on: Windows asks for permission, and rekordbox's own mapping is kept as
  `DDJ-FLX4.midi.csv.original` and put back on uninstall.

Run it again after a rekordbox update to rebuild platendoos for it, or to *Uninstall* (your library, settings
and the mods' caches stay). With a rekordbox it can't patch it says why and changes nothing. On Windows, a
rekordbox version the patcher has no address table for gets one made on the spot from the signatures it
carries (`rbmods\sigs`, see [docs/windows.md](docs/windows.md)): the window says how many of the parts were
found, and the mods missing one are shown as off.
`--check`, `--install` and `--uninstall` do the same without the window (from Terminal:
`"platendoos patcher.app/Contents/MacOS/platendoos patcher" --check`).

Making the patchers: `tools/build/patcher_mac.sh` (→ `build/platendoos-patcher-mac.zip`) and
`tools/win/build.sh` (→ `build/win/platendoos-windows.zip`, the patcher included).

### Releases

Only the sources and the scripts that build platendoos are in this repository: no rekordbox files and nothing
built. The models and the Windows address tables are build inputs that live in the
[`build-inputs`](https://github.com/idiidk/platendoos/releases/tag/build-inputs) release instead:
`tools/build/fetch_inputs.sh` puts them in `models/` and `tools/win/{tables,sigs}/` (or make them yourself, see
[models/README.md](models/README.md) and [docs/windows.md](docs/windows.md)).

To publish a release, commit and push, then:

```sh
tools/build/release.sh 1.0.0            # --draft to check it on GitHub first; --inputs when a model or table changed
```

It builds both patchers at HEAD with that version in them and uploads them to the GitHub release `v1.0.0` with
their checksums. The release notes are that version's section of [CHANGELOG.md](CHANGELOG.md) (write it before
releasing; without one, the notes list the commits since the previous release).

### Building it yourself

You need a Mac (Apple Silicon; an Intel Mac builds too, untested), rekordbox 7 in `/Applications/rekordbox 7/`, and Xcode's command line
tools (`xcode-select --install`).

```sh
git clone https://github.com/idiidk/platendoos.git
cd platendoos
tools/build/fetch_inputs.sh     # the models (not in the repository)
./build.sh
```

That builds the mods and makes the patched copy at `~/Applications/platendoos.app`. This repository is the public
edition of platendoos (see [docs/editions.md](docs/editions.md)). Open it like any other app.
After changing a mod, `./install.sh` rebuilds and restarts it. It signs only the mods' library and leaves
rekordbox's own signature as it is: a newly signed rekordbox is a new app to macOS, which then asks again whether
it may use the microphone, and until that is answered rekordbox's start stands still (after two minutes it goes
on without sound). After `build.sh` or a patcher run macOS asks once: click *Allow*.
For development, `RBM_DEBUGGABLE=1 ./install.sh` (or `build.sh`) also lets a debugger such as lldb attach;
the next plain `./install.sh` takes that permission away again.

**Windows** (x64, rekordbox 7.2.19 or 7.2.20 installed in `C:\Program Files\rekordbox`): the Windows version is built
on a Mac as well:

```sh
brew install llvm lld xwin
xwin --accept-license splat --output ~/rbwin/xwin
tools/win/build.sh
```

Copy `build/win/platendoos/` to the PC (any folder) and start `platendoos.exe`, or run
`platendoos patcher.exe` from it to install it (see above). It finds rekordbox by itself
(`--rekordbox <path to rekordbox.exe>` otherwise). The windows need WebView2, which Windows 11 has built in.
Details, and how a new rekordbox version is mapped, are in [docs/windows.md](docs/windows.md).

## Settings

Every mod is on by default. Settings live in a plain text file:

```
~/Library/Application Support/rekordbox-mods/config.txt
```

Put `name=0` on a line to switch a mod off (e.g. `framerate=0`). Several mods have their own options:
the file lists every mod and option with its default, each on a line starting with `#` (remove the `#` to use
it), and they are described at the top of the mod's file in `mods/`. The file is re-read every couple of seconds, so options apply
as soon as you save it. The exception is a mod's own on/off switch (`name=0` / `name=1`): which mods are on is
decided once, when rekordbox starts, and a change takes effect at the next start. When the file says something
else than what is running, the PLATENDOOS chip's blue icon pulses orange, its tooltip and its window say
"Restart rekordbox to apply: ..." and list the mods that change. Everything the mods do is logged to `~/Library/Logs/rekordbox-mods.log`. The
PLATENDOOS chip's window opens both. On Windows they are `%APPDATA%\rekordbox-mods\config.txt` and
`%LOCALAPPDATA%\rekordbox-mods\rekordbox-mods.log`.

On Windows, **betterstems** and **bettergrid** use the GPU (DirectX 12) when there is one that is fast enough;
the log says what they use. Without a usable GPU, with a slow one, or with `gpu=0` in the config (restart
rekordbox; for when stems sound or grids look wrong on your PC), nothing heavy is added to a deck load: stems come
from rekordbox's own engine (HTDemucs on the processor keeps most cores busy for two minutes per track), and a
track loaded on a deck gets rekordbox's beat grid at once instead of waiting 15 to 25 s for Beat This!. Tracks
analysed from the analysis window still get the Beat This! grid. `betterstems_slow=1` uses HTDemucs anyway,
`bettergrid_slow=1` waits for Beat This! on a deck too. The numbers are in
[docs/windows.md](docs/windows.md), under *Speed*.
Intel Macs (beta, [docs/intel-mac.md](docs/intel-mac.md)) follow the same rules, with the Mac's GPU
through Core ML.

Also on Windows, the mods' own threads run one step below normal priority, so rekordbox's window and drawing
go first (`worker_priority=-1`; `0`: normal, as before; `-2`: two steps down; the default is `0` with fewer than
four cores). A newly loaded, never-analysed track shows its waveforms and rekordbox's beat grid in one redraw
(`quickdeck_merge=0.25`: the seconds the waveforms wait for that grid; `0`: no wait): the window pauses about
0.13 s there, as in plain rekordbox, where it was 0.35 to 0.4 s. A deck load never interrupted the playing
deck in the tests there: 42 recorded loads on a six-core PC, plain rekordbox and platendoos, with and without a
GPU, no dropout.

What happens when a track is loaded on a deck, in what order the mods do their work, and how to read the log
when it feels slow, is in [docs/loading-a-track.md](docs/loading-a-track.md).

A playlist's right-click menu gets only *Open analysis window*, *Open stems window*, *Open auto cues window* and
*Open genres window* (local playlists); the actions are in those windows. The same windows open on just a few
tracks: select them in the track list and right-click. With **tidymenu** the track menu has a *platendoos* submenu
with *Analysis window*, *Auto cues window*, *Stems window* and *Genres window*, and *Re-analyze track* (*Re-analyze 5
tracks*) next to it, which analyses them again right away. With `tidymenu_group=0` (or without tidymenu), the same
items are in the menu itself as *Open … window for 5 tracks*. The window then lists only those tracks, as "5 tracks
from <playlist>", and everything it does is limited to them. Mods work on their own: when a feature needs another
mod (autocue's stems need **betterstems**), its window says so.

Disk use (all under `~/Library/Caches/rekordbox-mods`; on Windows `%LOCALAPPDATA%\rekordbox-mods\cache`):

| Cache | Per 5-minute track | Limit |
|---|---|---|
| Stems of local tracks (`stems/`, AAC 256; 192 on Windows, the most its encoder makes) | ~35 MB | `stemcache_max_gb=10` |

No Spotify audio is ever kept on disk, decoded or as stems: what the mods need of a Spotify track is read from
the stream into memory and gone when rekordbox quits. Analysis results (beat grids, keys, genres, cue points,
waveforms) are kept, as rekordbox itself keeps them.

The stems cache can be on another drive: `stemcache_dir=` a folder there (a USB stick, an SD card, an external
disk). New stems are then staged on the internal disk (`stems-staging/`, up to `stemcache_staging_gb=4`) and
moved to the drive in the background, a piece at a time, only while nothing reads from it. A drive that turns
out to be slow (half a second or more for 1 MB) gets nothing written while a deck plays: what waits stays on
the internal disk until the decks have stopped, and the log says so (`slowdrive: ...`). A slow stick stays a
slow stick, though: once the staging is full, new stems aren't kept until it has emptied. An SSD in a USB case
is the better cache drive. More in [docs/loading-a-track.md](docs/loading-a-track.md).

## How it works, briefly

The mods are compiled into one library, `librbmods.dylib` (`rbmods.dll` on Windows). `build.sh` copies rekordbox
and patches the copy so it loads that library and sends a few dozen specific internal calls through the mods
(listed in `hooks.def`). The mods find everything they need in rekordbox by name when it starts, rather than
relying on fixed addresses, so they have a decent chance of surviving an update.

The Windows rekordbox has no names in it, so there the same names are looked up in a table made for each
rekordbox build by matching it against the mac build of the same version. `platendoos.exe` starts rekordbox,
loads `rbmods.dll` before rekordbox's own code runs, and patches the calls in memory. The mods are the same
source on both: platform code sits behind small interfaces in `core/` (`plat`, `webui`, the image and hook
layers), and the mods' windows are HTML pages in `ui/` shown in the system's web view.

## Project layout

- `core/`: the shared base that finds things in rekordbox, hooks into it and gives the mods their tools
  (`*_mac.mm` / `*_win.cpp` hold the platform parts).
- `ui/`: the mods' windows, one HTML page each, shared by macOS and Windows.
- `patcher/`: the platendoos patcher (shared logic and the `ui/patcher.html` window, with a macOS and a Windows
  part; `macho.h` and `flx4map.h` are C++ versions of `patch_binary.py` and `tools/build/flx4_mapping.py`).
- `mods/`: the mods themselves, one file (or a few) per feature in the tables above.
- `engines/`: the analysis engines the mods use (Beat This! for beat grids, a CNN for keys); they aren't mods.
- `models/`: the trained models the engines and betterstems need, copied into the app on install (not committed: [models/README.md](models/README.md)).
- `tools/`: scripts for building (`build/`), making the models (`models/`), development (`dev/`), checking key detection (`keytest/`) and the Windows build and rekordbox.exe mapping (`win/`).
- `docs/`: longer write-ups of how some of the mods work.
- `trailer/`: the scripts that render the feature trailer from recordings of rekordbox (the media aren't committed).

## Good to know

- This is an unofficial hobby project, not made by or affiliated with AlphaTheta / Pioneer DJ. Use it at your
  own risk, and keep backups of your rekordbox library.
- The Spotify features work with your own Spotify connection in rekordbox and never keep Spotify's audio on
  disk: what they need of a track (stems, a whole track for autocue or genres) is read from the stream into
  memory, as rekordbox itself plays it, and is gone when rekordbox quits.
- Apple Music doesn't play in platendoos on macOS: its tracks are copy-protected, and the library rekordbox uses
  for them only works in rekordbox as AlphaTheta ships it. platendoos leaves it switched off (it would crash the
  patched copy at launch); use plain rekordbox for Apple Music.

## Licence

platendoos is licensed under [CC BY-NC-SA 4.0](LICENSE): you may share and change it, with credit, for
non-commercial purposes only, and what you make from it must use the same licence. The models and third-party
code keep their own licences, listed in [NOTICE.md](NOTICE.md); madmom's key CNN (betterkey) and Discogs-EffNet
(genres) are non-commercial too.
