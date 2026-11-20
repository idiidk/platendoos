# badge

The **PLATENDOOS** chip next to the rekordbox logo, so a modded rekordbox is easy to tell apart from the plain one.
Click it for a window with rekordbox's and platendoos's versions, every mod and whether it's on, and buttons to open
the log and the config.

<ModInfo name="badge" group="Quieter in the background" off="badge=0" />

<figure class="shot mid"><img src="/img/chip.webp" alt="The PLATENDOOS chip next to the rekordbox logo"></figure>

## What it shows

- **Hover**: the versions (and *untested rekordbox build: some mods may be off* on a rekordbox version platendoos
  wasn't checked on).
- **Click**: the chip's window, with every mod's state and **Open log** / **Open config**. See
  [First steps](/getting-started/first-steps#the-chip-s-window).
- **Orange pulse**: the config now says something else about a mod's switch than what is running. The tooltip and the
  window say *Restart rekordbox to apply:* and list the mods that change.
- It steps aside the moment rekordbox shows a message in its bottom bar, and comes back when the message is gone.

## Options

| Option | Default | What it does |
|---|---|---|
| `badge` | `1` | `0` hides the chip (restart). |

## Related

[First steps](/getting-started/first-steps) · [Settings and config.txt](/guides/settings)
