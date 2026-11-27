# usbphrases

A track loaded on a deck straight from a USB stick shows its phrases under the waveform, as a collection track does.

<ModInfo name="usbphrases" group="Browsing and playing" />

## Why

rekordbox shows a deck's phrase blocks (intro, up, chorus, down, outro, or the names of the low and mid moods) only
for tracks in your own collection. Load a track from a stick rekordbox exported, a friend's for example, and the
phrase bar stays empty, though the stick has the phrases: the export writes each track's phrase analysis next to its
waveforms.

## Using it

- Plug in the stick and load a track from it onto a deck, in Performance mode. Its phrases show within a second.
- They show when rekordbox shows phrases: its own phrase display options decide. Turn them off and on again and the
  phrases come back.
- Load another track and the phrases follow it. A track from your collection keeps its own phrases.
- Nothing is imported: nothing is added to or changed in your collection. Editing a phrase on such a deck shows on
  screen but isn't saved.
- A stick exported without phrase analysis has no phrases to show: the bar stays empty, as before.

## Options

| Option | Default | What it does |
|---|---|---|
| `usbphrases` | `1` | `0` switches the mod off (restart). |

## Windows

Works on rekordbox 7.2.19 and 7.2.20 for Windows, on by default: both were tested there with a real USB stick,
real phrase data, a collection track next to it and an edit that wasn't saved.

## How it works

The phrases are read from the stick's own analysis file for the track, unmasked the way rekordbox's export masked
them, and handed to rekordbox's own phrase loading under a made-up track id that only the mod answers, and only
while the phrases load. Details: `docs/usb-phrases.md` in the source.

## Related

[autocue](./autocue) · [keepmaster](./keepmaster)
