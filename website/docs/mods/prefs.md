# prefs

rekordbox's own preferences in the platendoos Preferences window: the same settings as in rekordbox's Preferences
dialog, shown as cards and rows next to the mods' settings, with one search over all of them.

<ModInfo name="prefs" group="Quieter in the background" off="prefs=0" />

<figure class="shot"><img src="/img/prefs.webp" alt="The Preferences window showing rekordbox's View > Display Type page: Language, Tooltips, Browse font size and line space, Key display format and Waveform as rows with a switch, a slider or a choice each"><figcaption>rekordbox's View > Display Type page in the Preferences window.</figcaption></figure>

rekordbox's own **Preferences** command (⌘, its menu item and the gear in the top bar) opens this window too, at
the category rekordbox's dialog showed last. Plan, Keyboard and DVS, which the window can't show, open rekordbox's
own dialog as before; so does the command when the [badge](./badge) is off.

rekordbox's Preferences dialog is ten categories, most with tabs, and pages that scroll a long way: a heading, a
list of radio buttons, the next heading. Finding a setting means knowing which tab it is on.

With this mod the [Preferences window](./badge#the-preferences-window) (the PLATENDOOS chip) has a second group in its
sidebar, **rekordbox**, with rekordbox's categories. Click one and its pages appear under it: no tabs. A page shows
what rekordbox's page has, tidied:

- a list of radio buttons is **one choice** (a row of buttons, or a drop-down when there are many),
- a checkbox is a **switch**, a drop-down and a slider keep their kind, with their caption on the left,
- a section that holds a single setting is **one line**, and such lines are grouped into a card,
- rekordbox's **?** help texts are a small mark next to the setting: hover it, or click it to read the text in place,
- settings that need a paid plan say *plan*, and greyed-out ones are greyed out here too.

**Search** (the box at the top) looks through rekordbox's settings and the mods' at once: the results say which
page each one is on, and you can set them right there.

## How it sets things

Nothing is copied or guessed. While the window is open the mod keeps rekordbox's own Preferences page in the
background, never shown, and reads from it what there is to set, in the language rekordbox is set to. When you
change something in the window, the mod operates rekordbox's own control: a click on its switch, a choice in its
list, a move of its slider. So rekordbox applies the setting itself, exactly as if you had done it in its dialog,
and saves it to its own settings.

A new rekordbox version's settings show up by themselves, since the pages are read from rekordbox each time.

## What stays in rekordbox's dialog

Some things can't be shown this way. They appear as a line with an **Open in rekordbox** button, which opens
rekordbox's own Preferences on that page:

- **Plan** (your account), **Keyboard** (the shortcut editor) and **DVS** (its signal scopes),
- sections with a list or a text field in them: the colour names under *Advanced > Browse*, the video transition
  lists, the linked video folder.

Every page also has a link at its top right, *Open this page in rekordbox's Preferences*. rekordbox's dialog itself
is unchanged.

The mods' own section in rekordbox's dialog ([outputsync](./outputsync)'s *Speakers and headphones* under Audio)
isn't repeated here: its settings are in the window's **Decks & sound** area.

## On Windows

On by default on Windows too since 2026-11-27, when it ran on a PC: every page, the gear and *File > Preferences*
open the window, changes are saved to rekordbox's own settings file, and *Open rekordbox's dialog* works. On Windows
the window has a taskbar entry of its own (grouped with rekordbox) and can go behind rekordbox's window; the gear
brings it back. `prefs=0` in [config.txt](/guides/settings) leaves rekordbox's preferences out of the window.

## Options

| Option | Default | What it does |
|---|---|---|
| `prefs` | `1` | `0` leaves rekordbox's preferences out of the window, `1` shows them (restart). |

## Related

[badge](./badge) (the window) · [Settings and config.txt](/guides/settings)
