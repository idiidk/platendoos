# Phrases for tracks from a USB stick (usbphrases)

rekordbox shows a deck's phrase blocks (intro, up, chorus, down, outro, or the names of the low and mid moods) only for
tracks in your own collection. A track loaded straight from a USB stick that rekordbox exported shows none, though the
stick has them: the export writes each track's phrases into its `ANLZ0000.EXT` file, in the `PSSI` tag. With
**usbphrases**, such a track shows its phrases on the deck in Performance mode, the same way a collection track does.

## How it works

- A deck's phrases come from `SongStructInformation::loadSongStructInfoFromDb(song id)`. That asks
  `DatabaseMediator::getSongStructInfo`, which reads the main (collection) controller's row for the id, and only for a
  local track (row type 1 or 6, flag `0x402` bit 0) reads the analysis file the row names. A device track's id is not in
  the collection, so its deck stays empty.
- The mod looks at the decks every 0.25 s. When a deck holds a device track (`MovableItemBasicInfo` browse type 2) and
  rekordbox shows phrases (`shouldLoadSongStruct`), it reads the device row's analysis path, reads that track's `.EXT`
  on a worker thread, unmasks its `PSSI` and writes the file as a copy to `<configDir>/usbphrases/deckN.EXT`.
- An export masks `PSSI` from byte 18 to the tag's end: XOR with `"Kanzen-niRikaishita"[i % 19] ^ 0x80` plus the low
  byte of the entry count. A mood above 20 means the tag is masked. This is rekordbox's own
  `analyzer::ConvUSB_ScrambleSongStruct`, which was checked byte for byte against the mod's version.
- Then, on the main thread, it calls `loadSongStructInfoFromDb(0xFFFFFF00 + deck)`. While that runs, the main
  controller's `getRowDataTrack` (vtable slot `0x48`) answers that made-up id with a row pointing at the copy, so
  rekordbox reads and shows the phrases itself.
- Outside that call the made-up ids get no row. Every phrase write goes `writeSongStructInfoToDb` →
  `DatabaseIF::setSongStructInfo` → `DatabaseMediator::setSongStructInfo`, which asks the same slot for the row first and
  writes nothing without one. So a phrase edit on such a deck shows but is never saved, and nothing reaches `master.db`
  or the collection's analysis files. Every other id goes to rekordbox's own function unchanged.
- Turning phrase display off clears the deck's phrases (rekordbox does that). Turning it back on feeds them again.
  Loading another track replaces them, and a collection track gets its own.

Addresses: the symbols in `mods/usbphrases.cpp`. The field offsets are recipes from
`DatabaseMediator::getSongStructInfo`, `SongStructInformation::getSongId` / `getTotalNumberOfPhrase` and
`DjplayDB::createObject`. The item's `+0` / `+4` are constants, because an `ldp` reads them and no recipe kind decodes
that. In 7.2.20 (macOS) the controller's `getRowDataTrack` moved to slot `0x50` (the recipe follows it); the other
fields are unchanged.

## Windows

The same code; what MSVC lays out differently:

| | macOS 7.2.19 | Windows 7.2.19 | Windows 7.2.20 |
|---|---|---|---|
| controller `getRowDataTrack` slot | `0x48` (7.2.20: `0x50`) | `0x40` (one destructor slot) | `0x48` |
| row type / analysis path | `+0x50` / `+0x3f0` | `+0x50` / `+0x3f0` | the same |
| "analysed here" flag | byte `+0x402` bit 0 | dword `+0x400` mask `0x10000` | the same |
| `SongStructInformation` song id / phrase count | `+0x48` / `+0x64` | `+0x38` / `+0x54` | the same |
| item track id | `+0x20` | `+0x20` | the same |

- The flag is a `win:` constant (`usbphrases win:row flags dword`) because MSVC tests the dword, not the byte.
- `SingletonHolder<SongStructInfoManager>`'s global comes from its verified `get()` (an `Addr` with a holder finder).
- The copy's path is written with backslashes.
- The test commands (`command-usbphrases` in the config folder: `state`, `edit N`) work on both. `edit` uses
  `SongStructInformation::setPhraseId` and `saveSongStructInfoIfNeeded`: 7.2.19 0x141918530 / 0x141919760, 7.2.20
  0x14199fb10 / 0x1419a0d40. They are declared under the file name `usbphrases_testcommand`, so a build without them
  switches off only the command, not the mod.
- Verified addresses: `tools/win/verify/usbphrases.out.json` (7.2.19; `DatabaseMediator::getSongStructInfo`
  0x141f8bb60, `setSongStructInfo` 0x141f8b9b0 with the same guard, `DatabaseIF::getRowDataTrack` 0x141f33610). The
  scanner finds all of it in 7.2.20 (`getSongStructInfo` 0x142018490); `O_rowType` and the flag word are anchored on
  `DatabaseMediator::overwrite_export` (0x141fbfc10) because `getSongStructInfo` reads the row on the stack.
- Emulator test `tools/win/emu/test_usbphrases.py` (passes on 7.2.19 and 7.2.20): the fields against the code that
  reads them; rekordbox's own `ConvUSB_ScrambleSongStruct` (found by its key in `.rdata`) masks a tag exactly as the mod
  unmasks it; the slot swap; a collection load untouched; a stick track read, unmasked, copied and fed through
  rekordbox's own `loadSongStructInfoFromDb` and `getSongStructInfo`; rekordbox's own `setSongStructInfo` writing
  nothing for the made-up id; switching tracks, a track without `PSSI`, display off and on, eject.
- On the test PC with a real USB stick (2026-11-27), rekordbox 7.2.19:
  - Both test tracks showed their phrases on deck 3 (6 and 7, and switching between them worked).
  - A real analysis' `PSSI` showed all 30 phrases.
  - A collection track on deck 4 was left alone.
  - `edit 3` (rekordbox's own `setPhraseId`, which writes at once through `writeSongStructInfoToDb`)
    found no row: nothing was written.
  - `master.db`, its `-wal`/`-shm` and all 632 collection analysis files were identical from right after the start
    to the end, the eject included.
  - Ejecting the stick in rekordbox unloaded the deck and its phrases.
- The same stick on rekordbox 7.2.20 (after a replug):
  - Stick tracks showed 7 and 30 phrases (the real `PSSI`) on deck 3; a collection track on deck 4 was left alone.
  - `edit 3` found no row; `master.db`, `-wal`/`-shm` and the 632 analysis files were identical from after startup to
    before quitting, across both loads and the edit.
  - The bar under a deck's overview needs rekordbox's *Phrase (full waveform)* (`showPhraseFull`); with only
    *Phrase (enlarged waveform)* (`showPhrase`) the phrases are fed but drawn only in the enlarged waveform.
- rekordbox lists only removable drives, fixed drives on a removable parent (a USB disk) and mapped network drives as
  devices (`RemovableDeviceDetector`, 0x141e6b9a0). A FAT32 VHD and a mapped share of the stick's files were not
  listed, so a test needs a real stick.
- rekordbox's eject also ejects the stick from Windows. To use it again, plug it in again.

## Tested

On macOS (arm64), with a FAT32 disk image registered as a USB device and two exported tracks:

- rekordbox 7.2.19: their exported `PSSI` held the high and low moods (7 and 6 phrases), masked as an export writes it.
  Both tracks showed their phrases on deck 3, and switching between them worked. Phrase display off and on again
  worked. A collection track on another deck kept its own 30 phrases.
- rekordbox 7.2.20 (2026-11-27): the same; 6 and 7 phrases on deck 3, a collection track on deck 4 untouched.
- A real analysis' `PSSI` (30 phrases, high mood, from a track rekordbox analysed on this Mac), masked the way an
  export masks it and put on the stick: rekordbox showed all 30 (cut off at the track's end).
- A phrase edit plus a save was refused (the log says so), on both versions. `master.db`, its `-wal`/`-shm` and the
  collection's analysis files were byte-identical before and after.

Phrase analysis of local files on the test Mac wrote no `PSSI` (it ran, 0.2 s). A Spotify track's analysis did, so the
real tag above came from that one. A stick exported from a library with real phrase analysis is still to be tried.
