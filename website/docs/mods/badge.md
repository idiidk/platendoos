# badge

The **PLATENDOOS** chip next to the rekordbox logo, so a modded rekordbox is easy to tell apart from the plain one.
Click it for the settings window: the versions, every mod with its switch, and each mod's options.

<ModInfo name="badge" group="Quieter in the background" off="badge=0" />

<figure class="shot mid"><img src="/img/chip.webp" alt="The PLATENDOOS chip next to the rekordbox logo"></figure>

## What it shows

- **Hover**: the versions (and *untested rekordbox build: some mods may be off* on a rekordbox version platendoos
  wasn't checked on).
- **Click**: the settings window (below), with **Open log** and **Open config file**.
- **Orange pulse**: the config now says something else about a mod's switch than what is running. The tooltip and the
  window say *Restart rekordbox to apply:* and list the mods that change.
- It steps aside the moment rekordbox shows a message in its bottom bar, and comes back when the message is gone.

## The settings window

The list on the left has every mod with a lamp for its state (green: on; grey: off; orange: waiting for a restart,
or not available in this rekordbox build) and how many of its options you have set. Select a mod to see what it
does and its options on the right:

- A **switch** at the top turns the mod itself on or off. That applies after a restart of rekordbox; the window says
  which mods are waiting for one.
- Each **option** shows its name (as it is called in `config.txt`), what it does, and a switch, a list or a field
  to set it. An option you have set shows its value in blue and a *reset* link that puts it back to its default.
  Options apply within a few seconds, without a restart.
- Options that are only there to find faults (logging, test switches, experiments) sit under their own heading,
  folded until you open it.
- The **search box** finds mods and options by name or by what their description says.

Everything is written to [config.txt](/guides/settings) as you change it: a new line goes right under the option's
own entry in that file, and *reset* removes it again. So you can use the window, the file, or both.

## Options

| Option | Default | What it does |
|---|---|---|
| `badge` | `1` | `0` hides the chip (restart). |

## Related

[First steps](/getting-started/first-steps) · [Settings and config.txt](/guides/settings)
