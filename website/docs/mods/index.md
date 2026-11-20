# All mods

Every mod is on by default and can be switched off on its own with `<mod>=0` in the config (or its checkbox in the
patcher); a restart of rekordbox applies that. Mods marked <span class="pd-tag">mac</span> exist on macOS only,
<span class="pd-tag">win</span> on Windows only.

## Spotify

| Mod | What it does |
|---|---|
<!--@private mods-index-spotify-first-->
| [spotifyapi](./spotifyapi) | Lets rekordbox load several Spotify track lists at once instead of one after the other. A big playlist shows up roughly twice as fast. |
| [spotifyfix](./spotifyfix) | Stops Spotify playlists from endlessly reloading, and fixes a deadlock that could freeze every Spotify download until you force quit. |
| [spotifystart](./spotifystart) <span class="pd-tag">win</span> | rekordbox 7.2.19 for Windows only: fixes Spotify hanging on "loading the library" after a restart. |
| [autocue](./autocue) | Sets hot cues A–H (intro, build, drop, breakdown, build 2, drop 2, outro, fake drop) on a playlist's tracks from their arrangement, placed by a language model or built-in rules. |
| [genres](./genres) | Fills the Genre field from what the music sounds like, with a style model that knows 400 Discogs styles. |
| [spotifyaudio](./spotifyaudio) | Reads a Spotify track from the stream into memory for the auto cues and genres windows. Nothing is written to disk. |
<!--@private mods-index-spotify-b-->
<!--@public mods-index-artcache-->
| [artcache](./artcache) | Remembers far more cover art, so scrolling and loading decks don't download the same covers again. |
<!--@/public-->
| [spotifylog](./spotifylog) | Stops rekordbox from rewriting its Spotify log file several times a second while it syncs. |
| [spotifydl](./spotifydl) | Logs where a Spotify track's load time goes. Changes nothing. |

## Analysis

| Mod | What it does |
|---|---|
| [analyzer](./analyzer) | *Open analysis window* in any playlist's menu analyses that playlist, Spotify ones too, several tracks at once, with progress per track. |
| [bettergrid](./bettergrid) | Better beat grids: rekordbox's beats combined with the Beat This! tracker's tempo and bar starts; the genre picks 140, not 70. |
| [betterkey](./betterkey) | A neural key detector: 67.5 % of the GiantSteps key dataset right, against rekordbox's 56.8 %. |
| [quickdeck](./quickdeck) | A track that isn't analysed yet shows its waveforms and a beat grid on the deck as soon as the analysis has them. |
| [parallel](./parallel) | Spreads the heaviest part of analysing a track over three cores instead of one. |
| [tfpool](./tfpool) | Keeps rekordbox's beat-detection neural network loaded between tracks. |
| [streamio](./streamio) | Smarter reading of a track's audio during analysis: about 10 % faster, phrase analysis 4× faster. |
| [timing](./timing) | Logs how long each step of an analysis takes. |

## Browsing and playing

| Mod | What it does |
|---|---|
<!--@public mods-index-stemcache-->
| [stemcache](./stemcache) | Keeps separated stems on disk, separates whole playlists ahead of time, and gives Spotify tracks stems on the deck. |
<!--@/public-->
| [betterstems](./betterstems) | Separates stems with HTDemucs v4 on the GPU: cleaner stems than rekordbox's own engine. |
| [fastwave](./fastwave) | Deck waveforms come back in 30 ms instead of half a second after a stem toggle or a load. |
| [audiomem](./audiomem) <span class="pd-tag">mac</span> | Keeps the memory rekordbox's audio works with in RAM, so playback doesn't click when the Mac runs short of memory. |
| [betterfx](./betterfx) <span class="pd-tag">mac</span> | Better-sounding mixer effects: the Color FX Filter, the EQ and isolator, Reverb, Echo, Roll. |
| [keepmaster](./keepmaster) | Changing the sync master never changes the tempo of a deck you hear. |
| [tidymenu](./tidymenu) | A calmer track menu: no greyed-out items, and the mods' items in one *platendoos* submenu. |
| [multisort](./multisort) | Sort the track list by several columns at once. |
| [keycolumn](./keycolumn) | A **Key @ Master** column: each track's key as it would sound at the master deck's tempo. |
| [xfadelock](./xfadelock) <span class="pd-tag">mac</span> | A lock button that holds the crossfader exactly in the middle. |
| [outputsync](./outputsync) <span class="pd-tag">mac</span> | Lines up speakers and headphones when rekordbox plays to two devices, e.g. Bluetooth speakers and a controller. |
| [fontcache](./fontcache) <span class="pd-tag">mac</span> | Makes drawing text a lot cheaper, so scrolling feels lighter. |

## Controllers

| Mod | What it does |
|---|---|
| [flx4](./flx4) | Makes a Pioneer DDJ-FLX4 a four-deck controller with stems, on SHIFT combinations the FLX4 doesn't use itself. |

## Quieter in the background

rekordbox does a surprising amount of work while you're doing nothing. These trim it so it stays cooler and easier
on your battery.

| Mod | What it does |
|---|---|
| [framerate](./framerate) | Only runs the screen at full frame rate while a deck plays or you're using the computer. |
| [badge](./badge) | The PLATENDOOS chip and its window. |
| [screeninfo](./screeninfo) <span class="pd-tag">mac</span> | Stops rekordbox from asking macOS about your screen on every single redraw. |
| [midipoll](./midipoll) <span class="pd-tag">mac</span> | Checks for newly plugged-in MIDI controllers every 2 seconds instead of every 0.3 seconds. |

## Mods that work together

The mods find each other by name, never by being built together: each works alone, and a window says when a
feature needs another mod. The main pairs:

| Feature | Needs |
|---|---|
<!--@public mods-index-pair-stems-->
| Stems for Spotify tracks on a deck | [stemcache](./stemcache) |
<!--@/public-->
| HTDemucs stems kept on disk | [betterstems](./betterstems) + [stemcache](./stemcache) |
| *Genre first*: the genre picks the beat grid's tempo | [genres](./genres) + [bettergrid](./bettergrid) (+ [analyzer](./analyzer) for the switch) |
| A Beat This! grid on the deck early | [quickdeck](./quickdeck) + [bettergrid](./bettergrid) |
<!--@private mods-index-pair-b-->
<!--@public mods-index-pair-autocue-->
| Auto cues per stem | [autocue](./autocue) + [betterstems](./betterstems) |
<!--@/public-->
| The CNN Key column and Re-analyze | [betterkey](./betterkey), [analyzer](./analyzer) |
