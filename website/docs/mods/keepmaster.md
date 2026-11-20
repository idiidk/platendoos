# keepmaster

Changing the sync master never changes the tempo of a deck you hear.

<ModInfo name="keepmaster" group="Browsing and playing" />

## Why

rekordbox picks the sync master again on every audio block, by priority (a deck whose channel fader is down ranks
lower), and every deck with BEAT SYNC on follows the new master's BPM at once. So:

- **Fade out the master** while a deck at another tempo plays, and the synced decks jump to that tempo.
- **A stopped master stays master.** Cue the next track on it and the synced decks that are still playing follow its
  tempo.

## What it does

- rekordbox may still move master by itself (for example when you pull the master's channel fader down), but only
  while every other playing deck already runs at the master's tempo (or half or double: what a synced deck does), so
  the move changes nothing you hear.
- When the master **stops** (pause, end, eject), master goes straight to a deck that's playing at the same tempo,
  as the MASTER button would.
- If moving would change a playing deck's tempo, master stays where it is.
- **MASTER** on a deck still works as always.

## Options

| Option | Default | What it does |
|---|---|---|
| `keepmaster` | `1` | `0` gives rekordbox's own behaviour back. |

## Related

[flx4](./flx4) (keeps a deck's tempo when a side switches) · [keycolumn](./keycolumn)
