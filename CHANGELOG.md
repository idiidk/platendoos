# Changes

What changed per release, for people using platendoos. The details of each mod are in the
[README](README.md); how things work inside, in [docs/](docs/).

## 0.7.4 (2026-11-27)

- **Phrases for tracks from a USB stick** (new mod **usbphrases**; macOS and Windows). A track loaded on a deck straight from a
  rekordbox USB export shows its phrase blocks under the waveform in Performance mode, as your own tracks do (rekordbox
  showed none for them). They are read from the stick's export; nothing is imported into or written to your collection,
  and editing a phrase on such a deck isn't saved. Works with rekordbox 7.2.19 and 7.2.20, tested with a real USB stick on
  Windows. Turn on rekordbox's own phrase display (Preferences > View > Layout > Phrase): *full waveform* puts the
  blocks under a deck's overview.
- **Stems where you are, in about a third of a second** (**stemcache**, **betterstems**; macOS). rekordbox
  separates a track in parts of 11 seconds and a deck only gets a part's stems when the whole part is done. Now,
  when you use a stem control at a place that has no stems yet, or jump to one, the seven seconds around the playhead
  are separated on their own first and handed to the deck at once; the rest of that part follows as before.
  Measured on an M2 Pro with HTDemucs: a muted stem is gone 0.3 s after the press where it took a second, and 0.4
  to 0.6 s after a jump into a part not yet separated, where it took 1.4 s; with the speed setting 0.8 to 1.4 s
  where it took 2.2 s. `stemcache_quick=0` turns it off.
- **The part you're at is always separated first** (**stemcache**). The separation still runs through the whole
  track in the background from the moment it is on a deck, but the part under the playhead (and the next one, just
  before you get there) now goes first, after every jump too, whether a stem control is on or not. So the stems
  are usually there already when you reach for one: a mute then takes under a tenth of a second.
- **Stems from the playhead with the quality setting** (**stemcache**). With *STEMS Analysis Process* set to
  quality (rekordbox's Demucs model), rekordbox separated a track strictly from the start, so a stem muted two
  minutes in waited until everything before it was done (5-8 s). That is what the two changes above replace. On
  Windows the part under the playhead goes first as well (not yet tried on a Windows PC).
- **About 900 MB less memory** (**stemcache**, **betterstems**; macOS and Windows). rekordbox loads its own
  stem model when it starts, 680 MB for the quality one and more once it has run, and keeps it for the whole
  session, although with betterstems every stem comes from HTDemucs or from the stored stems. That model now
  stays out of memory until it is needed after all (HTDemucs switched off or failing; it then loads in about two
  seconds). Idle after starting, platendoos took 2.8 GB before and 1.9 GB now on a Mac (quality setting), and
  5.4 GB before and 4.5 GB now on a Windows PC (speed setting). Switching *STEMS Analysis Process* in
  *Preferences* keeps the new model out of memory as well. `stemcache_lazy_model=0` loads it at startup as before.
- **No more waiting for the beat analysis** (**betterstems**). While a stem control is on, HTDemucs no longer
  holds back for a newly loaded track's beat analysis (up to 8 s).
- **Cleaner stems where one 11-second part meets the next** (**stemcache**, **betterstems**; macOS). rekordbox
  gives its separator only a sliver of audio around each part: 93 ms with the speed setting, 23 ms with the quality
  setting. HTDemucs is least sure at the ends of what it gets, so the stems were rougher there, every 11 seconds,
  most with the quality setting. Each part now gets 1 to 1.35 s more audio on each side, for no extra model time;
  measured over six tracks against the same music separated in one piece, with the quality setting the error at
  those edges was twice the error elsewhere and is now no higher than elsewhere. Stems stored before are separated
  again once (they are kept apart); on Windows stems are separated as before.
- **FLX4 pads and beat FX after switching** (**flx4**). Going into or out of the STEMS pads could leave the controller
  on PAD FX with its pads dark, or with stem lights stuck on; the pad mode button went out on decks 3 and 4; and right
  after rekordbox started, the beat FX lever's position wasn't known yet, so the first SHIFT + PLAY put the beat FX on
  no channel at all. All three are fixed.
- **Livelier audio settings in the Preferences window** (**prefs**, **outputsync**). A new audio device (a Bluetooth
  speaker that just connected) shows up after *Refresh* and when devices change; the output delay applies while you
  drag its slider, so the metronome follows at once; a *Now:* line shows the delay in use.
- **No more grids that run backwards** (**bettergrid**). On two hardcore tracks rekordbox's analysis saved two beat
  grids one after the other (the track at 215 BPM, then again from the start at a quarter of that), and bettergrid
  kept that because Beat This! was unsure of those tracks. A grid whose beats go back in time is now never saved:
  only its longest stretch of beats in order is kept, and the track's BPM follows that grid.

## 0.7.3 (2026-11-27)

- **A turntable through a deck** (new mod **linein**; macOS and Windows). Any audio input your computer sees (a USB turntable, an
  audio interface with a turntable or CDJ on it) can be played through a deck, so the deck's channel on the mixer
  works on it: trim, EQ, filter, faders, crossfader, headphone cue and the FX, with any controller, a DDJ-FLX4
  included. It uses rekordbox's own INPUT THROUGH mode of a deck: the deck's display says so and rekordbox reads the
  tempo from what comes in. Pick a device per deck in the Preferences window (two of the same name show as `#2`). A deck plays its input while no track is loaded on it; load a track and it plays the track. The
  input is resampled continuously to follow the engine's clock, about 12 ms behind. Tested with a loopback device,
  not yet with a turntable: reports are welcome.
- **rekordbox's own preferences in the Preferences window** (new mod **prefs**; macOS and Windows). rekordbox's Preferences
  dialog is ten categories with tabs and pages that scroll a long way. The Preferences window now has a second group in its sidebar, *rekordbox*, with those categories and their pages one click away. A page
  shows what rekordbox's page has, tidied: a list of radio buttons is one choice, a checkbox a switch, a section
  with a single setting one line, rekordbox's "?" help a small mark next to the setting. The search box finds
  rekordbox's settings and the mods' at once and you can set them from the results. Nothing is copied: the pages
  are read from rekordbox's own Preferences page, kept in the background, in rekordbox's language, and a change
  operates rekordbox's own control, so rekordbox applies and saves it itself. What can't be shown this way (lists,
  text fields, the keyboard shortcuts, your account, DVS) is a line with a button that opens rekordbox's own
  dialog on that page.
- **A small window for the chip, and a Preferences window** (**badge**). Clicking the PLATENDOOS chip opens a small
  window: the versions, how many mods are on, what waits for a restart, the log and config file, and a
  *Preferences…* button. That opens the Preferences window, with every setting: platendoos's and rekordbox's own.
  It is rebuilt around what you want to set instead of around the mods' names: the search at the top of the
  sidebar, areas below it (Decks & sound, Stems, Grids, keys & cues, Spotify, Track list, Hardware, Speed, General), each feature a
  card with a plain title, and each option a line in plain words with the control that fits it: a switch, a choice,
  a number with its unit, a slider, a list of devices. Rarely needed options are folded under *More options*,
  logging under *Fault-finding*. Search finds any setting; a narrow window gets tabs and stacked lines. Works the
  same on Windows, where it shows the features that exist there. A mod's name in `config.txt` shows when you hover
  its title, and a deck's line in *Turntables and other inputs* is greyed out until it has an input.
- **rekordbox's Preferences open the Preferences window** (**prefs**): ⌘, the menu item and the gear open it at the
  category rekordbox's dialog showed last; Plan, Keyboard and DVS still open rekordbox's own dialog. A setting that
  only matters with another one (*Fine-tune* with *Line up speakers and headphones*, the Claude Code model with
  *Claude Code* chosen) is greyed out, with a line that says what it waits for. rekordbox's words in capitals read as
  words (*Crossfader Curve*, *Reset*), and its sliders with *Small* and *Large* at the ends lost their bare number.
  The areas *Analysis* and *Controller* are now *Grids, keys & cues* and *Hardware*, apart from rekordbox's own.
  *Speakers and headphones* (**outputsync**) is now on rekordbox's Audio page in the window, under rekordbox's own
  audio settings; *Decks & sound* has a line that leads there. The same for *Turntables and other inputs*
  (Audio > Input/Output), *Mixer effects* (Controller > Effect, its EQ switch on Mixer) and *Crossfader lock*
  (Controller > Mixer). The Preferences window is an ordinary window now: rekordbox's main window can cover it.
- **The metronome for lining up speakers and headphones is in the Preferences window too** (**outputsync**): a switch
  under *Fine-tune*, so you can tune by ear without rekordbox's own dialog. It stops when the window closes.
- **The Preferences window opens on rekordbox's settings**, which come first in its sidebar, and where you left it
  last time. A button at the bottom of the sidebar (and *Open in rekordbox* on each page) opens rekordbox's own
  Preferences dialog. Clicking some platendoos areas (Speed, Hardware and others after the first) did nothing: fixed.
- **Settings SDK** (internal): the sidebar's areas are declared by the mods (`settings::Area`), a mod says itself
  which platform it is untested on (`.untested`, replacing `core/defaultoff.h`), settings can say what they need
  (`.needs`), the window's JSON is written by `webui::Json`, and the build checks every declaration can be read for
  the docs and the patcher (`settings_parse.py --check`).
- **Settings are declared by the mods themselves** (internal). Each mod now declares its options in its own source
  file, with their labels, units, ranges and choices (`core/settings.h`); the window, the commented list in
  `config.txt` and the option tables on the website are all made from those declarations. A new `config.txt` also
  lists the two general options (`gpu`, Windows' `worker_priority`), and a config file saved with a byte-order
  mark (Notepad) no longer gets the list twice.
- **Speakers and headphones in Preferences > Audio looks like one of rekordbox's own sections** (macOS,
  **outputsync**): a card with a heading, marked *platendoos · outputsync*. It no longer shows up at the bottom of
  every other Preferences page. The metronome's every fourth click is higher, so you know which click you are
  lining up; while rekordbox plays to one device, where there is nothing to line up, the slider and the metronome
  are greyed out and a line says why.
- **Refresh audio devices** (macOS, **outputsync**). A button in that section and in the Preferences window: rekordbox
  looks for audio devices again and opens the device you have selected anew, for a device that was plugged in or
  changed and doesn't show up or stays silent, without a restart.
- **A Restart rekordbox button in the Preferences window** (**badge**). When something you changed needs a restart, the
  orange bar that says what is waiting now has a button: rekordbox quits the way its own Quit does and opens again
  by itself a few seconds later. While a deck is playing it asks once more first. macOS and Windows.
- **The window knows which options need a restart.** A few options are read once, when rekordbox starts (the parts
  of betterfx and audiomem, `gpu`, `spotifyapi_slots`). They now say *applies after a restart* next to their name,
  and a change to one of them counts in *Restart rekordbox to apply*, like a mod's own switch.
- **Column titles change as you set them.** `keycol_title`, `bettergrid_title` and `betterkey_title` used to show
  only in lists built after the change (in practice: after a restart), without a word about it. The headers that
  are there now get the new title within a second.
- **autocue's layout options are followed live** (`autocue_pads`, `autocue_memory`, `autocue_build_bars`,
  `autocue_intro`, `autocue_outro_bars`, `autocue_stems`): set in the Preferences window or the config file, they now
  do what the same change in the Auto cues window does. They were read once at startup.
- **Stems on demand and "keep only what I separate" now run on a Windows PC too**: they were released in 0.7.0 after
  an emulator test only. Both behave there as on a Mac.
- **linein and prefs ran on a Windows PC** and are on there by default, like on a Mac: rekordbox's pages and
  changes in the Preferences window, and an input heard through a deck (a loopback test input; a turntable on Windows
  is still untested). The Recording page shows on Windows too.

## 0.7.2 (2026-11-26)

- **Grids on the kick when something else is on the beat** (**bettergrid**). Some tracks have a percussion layer
  on the beat that both rekordbox and Beat This! follow, while the kick comes a little after it (Robert Hood's
  *Red Passion II - A*: the kick 71 ms after every grid line). The kick is now measured against the finished grid
  too: when it sits 50 ms or more off the grid over the whole track, on most beats and clearly, the grid is moved
  onto the kicks (`bettergrid_kick=0` turns this off). Of 103 tracks checked, only that one changed.
- **Grids that slide off the music** (**bettergrid**). On a few tracks rekordbox labels its beats with one tempo
  but spaces them at another (135.88 against 135.95 on the same track), so the grid ends up 200 ms off the music
  by the end. bettergrid compared only the label with Beat This!'s tempo and kept such a grid; now Beat This!'s grid
  is used when rekordbox's beats drift 40 ms or more from both. Tracks analysed before keep their grid until you
  analyse them again (right-click, *Re-analyze*).

## 0.7.1 (2026-11-26)

- **Clearer stem saving choices** (**stemcache**). The stems window's *Keep on disk* is now *Save stems to disk*,
  with the choices *Everything*, *Only what I separate in this window* and *Nothing* (were *Every track*, *Tracks
  separated here* and *None*), and the line below them says in plain words what each one does: *Only what I
  separate in this window* saves only the tracks you separate with the *Separate stems* button; a track you just
  load onto a deck still gets stems, but they are thrown away when rekordbox quits. Nothing else changed: the
  setting (`stemcache_keep`) keeps its values, so your choice stays.

## 0.7.0 (2026-11-26)

- **A settings window** (**badge**). Clicking the PLATENDOOS chip now opens every mod's settings: the list of mods
  with their switches on the left, the selected mod's options on the right, each with what it does and a switch, a
  list or a field to set it. A search box finds a mod or an option by name or description, *reset* puts an option
  back to its default, and options meant for fault-finding sit under their own folded heading. Everything is
  written to `config.txt` as you change it, so the file and the window always say the same; options apply within a
  few seconds, a mod's own switch after a restart of rekordbox (the window says which mods are waiting for one).
- **Choose which stems are kept** (**stemcache**). *Keep on disk* in the stems window (or `stemcache_keep`):
  *Every track* stores the stems of everything that is separated, as before; *Tracks separated here* stores only
  the playlists and selections you separate in the stems window, while any other track is separated when it
  loads onto a deck and its stems are forgotten when rekordbox quits (they stay in memory for loading it again in
  the same session); *None* stores nothing new. So you can separate your favourite playlists once, keep those, and
  let everything else come and go.
- **Stems on demand** (**stemcache**). With *Decks: separate only when a stem control is used* (or
  `stemcache_ondemand=1`) a track that loads onto a deck is no longer separated right away. Stems that are stored
  already still come at once. For any other track the separation starts the moment you mute a stem, turn a PART
  ISOLATOR knob or take the FX off a stem, and it starts where the track is playing: with HTDemucs on an M2 Pro
  the muted stem drops out about a second after the press, anywhere in the track, and the rest of the track
  follows in the background. Until then the deck plays the whole track. Off by default. With rekordbox's STEMS
  analysis set to its quality engine rekordbox separates in order from the start of the track, so a press late in
  a track takes longer there. On Windows this is the same code, checked against rekordbox 7.2.19 and 7.2.20 in
  an emulator but not yet run on a Windows machine, so reports are welcome.
- **Clicks show sooner** (macOS, new mod **quickframe**). rekordbox queued every frame three deep on its way to the
  screen, so a button you pressed lit up 42 to 47 ms later (measured on a 120 Hz MacBook Pro). The queue is now two
  frames (32 ms, exactly as smooth), and while no deck is playing each frame is shown as soon as it is drawn (about
  10 ms). While a deck plays the screen's own rhythm is kept, so the waveforms scroll as before.
- **Menus and Preferences open sooner, resizing is twice as smooth** (macOS, new mod **relayout**). Every time any
  window opened (a menu, a drop-down, a tooltip, Preferences) rekordbox first laid out its whole main window again,
  and on every layout pass it rendered the images of some 300 buttons again, also the ones that hadn't changed.
  Preferences now opens in about 60 ms instead of 100, a drop-down in 15 to 30 instead of 60, and dragging the
  window to another size takes half the work.
- **Panels you can't see wait until you open them** (macOS, **relayout**). rekordbox's window is made of 16,700
  parts and shows about 400; it laid out and restyled all of them every time. Some options (auto gain, needle lock,
  the beat count mode, the mixer mode) made the whole window refresh, which froze rekordbox for about 200 ms, half
  of it spent on the 64 hidden sampler slots. That is now about 80 ms.
- **Full frame rate with empty decks** (**framerate**). With nothing loaded rekordbox runs at 30 fps, also while you
  click around, which adds up to 33 ms to everything. The window now runs at the full rate whenever you use the
  mouse or keyboard, loaded or not, and still drops to 30 fps when nothing happens. `framerate_empty=0` gives
  rekordbox's own behaviour back.
- **Beat grids on a hi-hat** (**bettergrid**). rekordbox sometimes puts its beat lines a fixed distance from the
  kicks, on a hat or a clap: about 100 ms late over the whole track on Robert Hood's *Snakes and Politics* and
  *Side Effect*. bettergrid only corrected lines that were exactly half a beat off. It now also catches these,
  when Beat This! and the kick drum itself both say where the beat is, and uses the combined grid for them. A
  track gets the corrected grid when it is analysed again.
- **The overview waveform streams in** (macOS, **quickdeck**). After loading a track that isn't analysed yet, the
  overview under the title used to stay empty until rekordbox had read the whole track: two to five seconds. It
  now grows from the left while the track is being read, a few times a second.
- **The waveform stays put when the grid comes in** (macOS, **quickdeck**). Loading a new track showed the waveform,
  then a first grid, then the final one, and with each grid the whole waveform went blank and was drawn again.
  That is how rekordbox handles any new grid. Now only the grid lines are redrawn.
- **Less work in the background** (macOS, new mod **idlewake**). With nothing playing, rekordbox's MIDI output
  thread woke up a thousand times a second, every level meter restarted a timer on each frame, and every mixer
  channel kept measuring the loudness of pure silence for the gate effect. All three now rest until there is
  something to do: about a tenth less processor use while idle. The sound is the same (the measurement is only
  skipped on exact silence, and a test switch showed its result never changed there). The FLX4 and crossfader-lock
  mods also looked through every part of rekordbox's window ten times a second; they now remember where what they
  need is.
- **Windows.** New there in this release and built from the same code, but not yet run on a Windows machine: the
  settings window, the choice of which stems are kept, stems on demand (off until you switch it on) and the full
  frame rate with empty decks. The other changes to the window, the overview that streams in, the grid-only
  redraw and the lighter background work are macOS only for now.

## 0.6.2 (2026-11-25)

- **HTDemucs stems on every Mac** (macOS). On Macs set up with the patcher, betterstems never ran: it looked for
  its model only where a development setup keeps it, not inside the app where the patcher puts it, so the log said
  "betterstems: no model" and rekordbox's own engine made the stems. It now finds the model in the app, so stems
  come from HTDemucs as intended: cleaner, and separated on the GPU. A track gets its HTDemucs stems the next time
  it is loaded on a deck; stems stored earlier by rekordbox's engine are kept apart and no longer used while
  betterstems is on. Windows was not affected. Found in the same crash report as 0.6.1's fix.

## 0.6.1 (2026-11-25)

- **No more crash at launch with Apple Music signed in** (macOS). With an Apple Music account signed in to
  rekordbox, platendoos crashed about ten seconds after it opened, every time. Apple Music's tracks are
  copy-protected, and the library rekordbox uses for that checks which app it runs in: it only works in rekordbox
  exactly as AlphaTheta ships it, and in platendoos's patched copy that check crashed. platendoos now leaves that
  library switched off, so rekordbox starts as usual. The protection itself is untouched, which means Apple
  Music's tracks don't play in platendoos; they do in plain rekordbox, which stays installed next to it. Nothing
  else changes. Thanks to the DJ who sent the crash report.

## 0.6.0 (2026-11-23)

- **Auto cues: memory cues, and your own pad layout.** Many older CDJs show only hot cues A–C or A–D, and every
  CDJ has ten memory cues. The auto cues window has a new **Layout** button (next to ⚙): per cue (intro, build,
  drop, breakdown, build 2, drop 2, outro, fake drop) you pick its hot cue pad, A–H or none, and whether it is
  also written as a memory cue. Six presets: eight pads (A–H, the default, as before), eight pads + memory cues,
  the main four on A–D with the rest on E–H + memory cues, four pads (A–D) + memory cues, three pads (A–C) +
  memory cues, and memory cues only. A layout change applies to every track in the window at once, without asking
  the model again.
- **Memory cues** get the cue's name as their comment and no colour. A track holds ten: the memory cues already
  on it count, and what doesn't fit is left out (the status line says how many). Two cues on the same spot give
  one memory cue. **Replace existing cues** now also removes a track's other memory cues when new ones are written
  to it. **Undo write** takes back the last **Write cues** or **Remove cues**, as before.
- **Auto cues: rulesets.** The lower half of Layout says where the intro, build and outro cues go, as a choice of
  five rulesets: Arrangement (every cue where its part starts, the default, as before), builds 8 bars before the
  drop, builds 16 bars before the drop, Mix points (intro on the first drums, builds 16 bars before the drop,
  outro 32 bars before the end) and Long blends (intro on the first drums, builds 32 bars before the drop, outro
  64 bars before the end). Each rule can also be set by itself: the intro cue on the first bar with sound, the
  first bar with drums or the track's first bar; build cues where the build starts or 4, 8, 16 or 32 bars before
  their drop; the outro cue where the last loud part ends or 16, 32 or 64 bars before the track ends. The rules
  work without a model and hold for a model's cues too. Where a rule doesn't fit a track (no room for the build
  before its drop, say), that cue stays where it was.
- **Auto cues without a language model.** Under ⚙ there is now **No model** next to OpenRouter and Claude Code
  (`autocue_provider=none`): the built-in rules place the cues, nothing is sent anywhere, and there is no key, no
  account and no cost. The instructions box at the top of the window makes way for the ruleset dropdown. Cues a
  model gave a track before stay available under *Cues from: The model*.
- **Where an analysis' time goes.** The log (`~/Library/Logs/rekordbox-mods.log` on macOS) has a line per
  auto cues analysis with the time spent reading the audio, separating stems and working out the arrangement. One
  example, on an Apple Silicon Mac with HTDemucs: a local 3:43 track took 0.7 s, 18.5 s and 1.5 s.
- Hovering a cue card in the auto cues window no longer shifts its text.
- All of this was tested on macOS in rekordbox. On Windows it builds but has not been run yet.

## 0.5.1 (2026-11-23)

- **HTDemucs stems that were noise on some graphics cards** (Windows). On some cards the stems came out as a loud
  high-pitched noise with a white waveform (0.5.0 noticed that and fell back to rekordbox's own engine). The cause
  is a fault in DirectML, which rekordbox ships: it mixes up the two halves of an operation the HTDemucs model
  used 48 times. The model no longer uses that operation, so these cards now get HTDemucs stems, at the same
  speed. Stems separated with HTDemucs on Windows before are made again (the model's file changed), so noise
  that was stored is not used again.
- **A graphics card that still gives unusable stems** gets two more tries before rekordbox's own engine takes
  over: HTDemucs without DirectML's graph fusion (about 10% slower), then in DirectML's safe mode. A test segment
  at launch decides this before any track. `betterstems_safe=1` or `2` starts in one of these modes. The log now
  names the graphics card and its driver version.

## 0.5.0 (2026-11-23)

- **Intel Macs, in beta.** The macOS patcher now also installs platendoos on Intel Macs (rekordbox 7.2.19 and
  7.2.20), with every mod. It is **not tested on a real Intel Mac yet**: it was tested under Rosetta on an Apple
  Silicon Mac, which runs the same Intel code but says little about speed. On an Intel Mac the patcher says it is
  beta. Where the Mac's graphics are too slow for the neural networks (or with `gpu=0`), it works as on Windows:
  stems come from rekordbox's own engine and a track on a deck gets rekordbox's beat grid at once (the analysis
  window still waits for the Beat This! grid). If you try it, the log (`~/Library/Logs/rekordbox-mods.log`) and a
  note of what you did help a lot. Details: [docs/intel-mac.md](docs/intel-mac.md).
- **No more crash report when quitting rekordbox** (macOS). A background thread of the mods could still
  be running while rekordbox shut down and abort the quit; the mods' own threads now stop as soon as rekordbox
  starts quitting.
- **Stems that come out as noise switch HTDemucs off** (Windows, Intel Macs). Each piece of separated audio is
  checked; if the graphics card returns noise instead of stems, rekordbox's own engine separates for the rest of
  the session and the log says why.
- The beat-detection warm-up at launch no longer uses the models of another copy of platendoos (a test install).

## 0.4.2 (2026-11-23)

- **macOS 13, 14 and 15 work again.** The macOS packages of earlier releases were built for the macOS version they
  were made on (26) and needed system libraries that only macOS 15.4 and later have, so on older versions the
  patcher and the mods couldn't start. They are built for macOS 13 and later now, as the patcher's requirements
  always said. On macOS 13, betterstems' HTDemucs model can't load (it needs macOS 14): rekordbox's own engine
  separates the stems there, as without the mod.

## 0.4.1 (2026-11-23)

- **No more crash when a track loads with rekordbox set to three stems.** With rekordbox's STEMS set to three
  parts (vocal, inst, drums) instead of four, rekordbox crashed a few seconds after a track was loaded on a deck.
  The stems mods (stemcache, betterstems) took a three-stem separation for a four-stem one and wrote the bass stem
  to memory that wasn't theirs. Fixed on macOS and Windows; with four stems nothing changes. Thanks to the people
  on r/Rekordbox who reported it and sent their logs.
- **AIFF files on Windows.** The auto cues and genres windows read a track's audio file through Windows' own
  decoders, and Windows has none for AIFF. platendoos now reads `.aif` / `.aiff` files itself there (AIFF and
  uncompressed AIFF-C, 8 to 32 bits and float). On macOS they were read already.

## 0.4.0 (2026-11-20)

- **Spotify audio stays off the disk.** platendoos no longer keeps Spotify tracks' audio on disk, decoded or as
  stems: what the mods need of a Spotify track is read from the stream into memory, as rekordbox itself plays it,
  and is gone when rekordbox quits. Analysis results (beat grids, keys, genres, cue points, waveforms) are kept as
  before. Spotify covers are kept in memory only.
- **Spotify stems on a deck, in memory.** A Spotify track gets its stems from the first load, separated from the
  stream. They are never stored on disk; the last ones stay in memory, compressed (`stemcache_memory_mb=384`), so
  loading the track again in the same session has its stems at once.
- **spotifyaudio** (new): the auto cues and genres windows read a Spotify track from the stream into memory
  (`spotifyaudio_mb=256` keeps the last few for a while), so they work on Spotify playlists without anything on
  disk.
- The PLATENDOOS chip's window and the patcher say which edition runs (see [docs/editions.md](docs/editions.md)).
- **xfadelock on Windows**: the crossfader lock button next to the on-screen crossfader now works in rekordbox
  for Windows too (7.2.19 and 7.2.20).

- **bettergrid: bars start on the first kick; a grid on the off-beats is moved onto the kicks.** Some tracks had
  their first kick on beat 3 or 4: where rekordbox and Beat This! disagree on the bar start, the beat where the kick
  comes in now counts. And a track whose kicks sat clearly between rekordbox's beats, but where Beat This! wasn't
  sure enough everywhere, stayed on the off-beats; the kick drum's own level now settles it.
- **bettergrid: the music picks the bar start** when rekordbox and Beat This! disagree: the 1 goes on the beat where
  the track changes (a drop, a breakdown, a new bass line), the beat where the kick comes in only when that doesn't
  settle it. On 330 tracks where both agree, it put the 1 right 96 times in 100, against 78 for the kick rule.
  `bettergrid_bars=0` turns it off. The idea comes from rbxport's documentation (our own code).

## 0.3.1 (2026-11-19)

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
- How a new rekordbox version is checked before anyone runs it: [docs/rekordbox-updates.md](docs/rekordbox-updates.md).

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
- What happens at a deck load, in what order: [docs/loading-a-track.md](docs/loading-a-track.md).

### Caches on an external drive

- The stems cache can be on a USB stick, SD card or external disk without getting in the way of playback. New
  stems are staged on the internal disk and moved over in the background, a piece at a time, only while nothing
  reads from the drive. A drive that turns out to be slow gets nothing written while a deck plays. An SSD in a USB
  case is still the better cache drive.

### Track list and windows

- **tidymenu** (new, macOS): the right-click menu on tracks leaves out rekordbox's greyed-out items, and the mods'
  items go in one *platendoos* submenu, with *Re-analyze track* next to it.
- The mods' windows open on just the selected tracks too: select some, right-click, and the window lists only
  those ("5 tracks from <playlist>").
- **Re-analyze** selected tracks from the track list's menu: they are analysed again right away, without
  rekordbox's Analysis Setting dialog.
- Analysis window: Spotify tracks wait out Spotify's "streaming limit reached" instead of failing, and with the
  genres mod a Genre column and *Genre first*.
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
