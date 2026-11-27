# badge

The **PLATENDOOS** chip next to the rekordbox logo, so a modded rekordbox is easy to tell apart from the plain one.
Click it for a small window with the versions and a **Preferences…** button: the Preferences window has every mod
with its switch and options, next to rekordbox's own preferences.

<ModInfo name="badge" group="Quieter in the background" off="badge=0" />

<figure class="shot mid"><img src="/img/chip.webp" alt="The PLATENDOOS chip next to the rekordbox logo"></figure>

## What it shows

- **Hover**: the versions (and *untested rekordbox build: some mods may be off* on a rekordbox version platendoos
  wasn't checked on).
- **Click**: a small window with the platendoos and rekordbox versions, how many mods are on, what waits for a
  restart (with a **Restart rekordbox** button), **Open log**, **Open config file** and **Preferences…**, which opens
  the Preferences window (below).
- **Orange pulse**: the config now says something else than what is running, about a mod's switch or about one of the
  few options that are read once at startup. The tooltip and the window say *Restart rekordbox to apply:* and list
  what changes.
- It steps aside the moment rekordbox shows a message in its bottom bar, and comes back when the message is gone.

## The Preferences window

<figure class="shot"><img src="/img/about.webp" alt="The Preferences window: areas such as Decks & sound, Stems and Analysis on the left; on the right the Stems area with a card per feature, each option a line with a switch, a list or a field"><figcaption>The Preferences window: an area on the left, its features as cards on the right.</figcaption></figure>

On the left, under **platendoos**, are the areas: **Decks & sound**, **Stems**, **Grids, keys & cues**, **Spotify**,
**Track list**, **Hardware**, **Speed** and **General** (settings of no one mod, the chip's own switch, the
versions, **Open log** and **Open config file**). Under **rekordbox** are rekordbox's own
preferences, shown the same way: see [prefs](./prefs). An area shows an orange dot when something in it waits for a restart. On the right the area's features, one card each:

- The card's **switch** turns the feature (the mod: hover the title for its name in `config.txt`) on or off. That applies after a
  restart of rekordbox. Features with nothing to set are listed together at the bottom, a switch each.
- Each **option** is a line: what it does, and a switch, a choice, a number or a field to set it. A blue dot marks
  one you changed, with a *reset* link back to its default. Hover a line for its name in `config.txt`.
- Options apply within a few seconds, without a restart. The few that are read once, when rekordbox starts, say
  *after a restart* (the parts of betterfx and audiomem, the graphics card switch, `spotifyapi_slots`).
- Rarely needed options are folded under **More options**, logging and test switches under **Fault-finding**.
- **Restart rekordbox.** When something you changed needs a restart, an orange bar at the top says what is waiting,
  with a **Restart rekordbox** button: rekordbox quits the way its own Quit does and opens again by itself a few
  seconds later. While a deck is playing, the button asks once more first. The same button is on **General** and in the chip's
  small window.
- The **search box** at the top of the sidebar finds a setting by any word of its name or description, across all areas, rekordbox's own
  preferences included.
- In a narrow window the areas become a strip of tabs and the lines stack, so it stays usable at any size.

Everything is written to [config.txt](/guides/settings) as you change it: a new line goes right under the option's
own entry in that file, and *reset* removes it again. So you can use the window, the file, or both.

## Options

| Option | Default | What it does |
|---|---|---|
| `badge` | `1` | `0` hides the chip (restart). |

## Related

[prefs](./prefs) · [First steps](/getting-started/first-steps) · [Settings and config.txt](/guides/settings)
