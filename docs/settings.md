# Settings: how a mod has them

Everything a mod lets people set goes through one interface in the core, `core/settings.h`. A mod declares each
setting once, in its own source file, and reads it through that declaration. From the declarations the core keeps
the values in step with the config file, writes the file's list of options, gives the settings window everything it
shows, and knows which changes wait for a restart. The docs' option tables are made from the same declarations.
Nothing lists the settings a second time: no wording file for the window, no generated header.

## Declaring and reading

```cpp
namespace opt {
const Setting keep{"stemcache_keep", "all", {.label = "Save stems to disk", .help = "Which separated stems are stored.",
                   .choices = {{"all", "Everything"}, {"chosen", "Only what I separate in the stems window"}, {"none", "Nothing"}}}};
const Setting maxGb{"stemcache_max_gb", 10, {.label = "Disk space for stems", .unit = "GB", .min = 0, .max = 2000, .step = 1}};
const Setting onDemand{"stemcache_ondemand", false, {.label = "Separate only when a stem control is used"}};
}  // namespace opt

if (opt::onDemand.on()) ...        // opt::maxGb.number(), opt::keep.text(), .integer()
```

- The first argument is the key in `config.txt`, the second the default (`bool`, `int`, `double` or text: that
  decides the kind of control), the third what the setting is to the people who set it (`SettingAbout`). Only
  `.label` is needed.
- A read is always what the file says now, else the default; any thread. The file is re-read every two seconds.
- `...AtStart()` gives the value as it was when rekordbox started, for a part that reads its setting once. Mark such
  a setting `.restart = true`: the window then says *after a restart*, and a change counts in *Restart rekordbox to
  apply*.
- `set()` / `reset()` write the file (main thread). `onChange(fn)` calls `fn` on the main thread after the value
  changed, whoever changed it.
- Another file's setting is read by its key: `setting("gpu").on()`. A key nobody declared reads as off / 0 / empty and
  is logged once.
- A mod says what it is itself in its `RBM_MOD`: `.title`, `.about`, `.area` (the window's sidebar) and `.order`
  within the area, and `.untested = "win"` (or `"mac"`) while it hasn't run on that platform: it is off there until
  the config says `<mod>=1` (core's `offUnlessAsked`; the patcher and the config list read the same field).
- The sidebar's areas are declared by the mods shown in them, not by the core:
  `const settings::Area kArea{"stems", "Stems", "How stems are separated...", "<path d=\"...\"/>", 20};` (id, title,
  what it is, an SVG icon drawn in 16x16, order). Mods that share an area may each declare it, so each still builds
  without the others; the declarations must say the same (the build checks). A mod naming an area nobody declared is
  shown under *More*.
- `.rekordboxPage = "Audio/Configuration"` (rekordbox's category id, `/` page id, or the category alone): the mod's
  card is shown on that page of rekordbox's own preferences, under rekordbox's settings, and its area keeps a line
  that leads there (outputsync: *Speakers and headphones* under Audio). Without rekordbox's pages in the window (prefs
  off) it stays in its area. A single setting can do the same (`SettingAbout::rekordboxPage`: betterfx's EQ switch
  sits on Controller > Mixer, the rest of its card on Controller > Effect); it is shown there under its mod's card
  head. rekordbox's page names are only known at run time: the prefs mod logs a name this rekordbox doesn't have
  (`prefs: <who> asks for rekordbox's page ...`).

What `SettingAbout` can say: `label`, `help`, `unit`, `min`/`max`/`step`, `choices` (a fixed set: a segmented
control or a drop-down), `list` (a drop-down of entries a mod gives at run time, `settings::list(name, fill)`: the
audio inputs of linein), `slider`, `wide`, `restart`, `advanced` (folded under *More options*), `debug` (under
*Fault-finding*), `line` + `column` (settings that share the lines of a table: linein's line per deck), `page`
(shown on another mod's page, or `"about"`: the General page), `platform` (`"mac"` / `"win"` in a file both build),
`needs` (what the setting only matters with: `"key"` a switch that is on, `"!key"` one that is off, `"key=value"`,
`"key!=value"`; the window greys the line out otherwise and says what it waits for).

A button on a mod's page: `settings::action(mod, id, label, help, run)` (outputsync's *Refresh audio devices*).
A switch whose state lives in the mod, not in the config file, for something that runs while it is on:
`settings::liveSwitch({.mod, .id, .label, .help, .on, .set, .why, .whileOpen})` (outputsync's metronome). `why()`
says why it can't be used now (the window greys it out with that line); `whileOpen` switches it off when the
Preferences window closes.

Keep a declaration a plain initializer with literals only: `tools/build/settings_parse.py` reads the declarations
from the sources (per platform and edition, following `#ifdef _WIN32` branches) for the website's option tables
(`website/scripts/settings_gen.py`), the patcher (`patcher_gen.py`) and the config list for a platform, without
building anything. `compile.sh` and `tools/win/build.sh` run `settings_parse.py --check` first: a declaration it
can't read (a name instead of a literal, a `Setting` made any other way), an area declared differently in two files,
an area or a `needs` key nobody declares, stops the build with the file and line.

The window's JSON is written with `webui::Json` (core/webui.h), which does the escaping.

## What the core does with them

- **The config file.** `config.txt` in the mods' folder holds `key=value` lines. A new file gets the commented list
  of every mod and setting (`settings::configList()`, written once at startup when the file has none); setting a
  value uncomments or adds its line under that entry, a reset comments it out again. A byte-order mark at the start
  of the file (Notepad) is skipped.
- **The Preferences window** (`mods/badge.cpp`, `ui/settings.html`; the chip itself opens a small window, `ui/about.html`, with the versions, what waits for a restart and a *Preferences…* button). The badge mod sends the page one JSON state built from
  the registry: the areas, a page per mod (title, what it is, its switch and state, its settings and buttons), the
  settings of the General page, the lists, and what waits for a restart. The page only knows how to draw a kind of
  setting. Its messages (`set`, `unset`, `mod`, `action`, `restart`) name declared settings only.
- **Restart.** `settings::pendingKeys()` gives the `.restart` settings whose value differs from the one rekordbox
  started with; the chip pulses and the window shows its bar with the *Restart rekordbox* button (`plat::restartApp`).

## Pages that aren't a mod's settings

`settings::source({id, state, message, shown})` adds a part to the window that isn't made of declared settings. The
window gets `state()` (JSON) under `id` in what it is sent, the source gets the page's messages whose `cmd` is `id`,
and `shown(open)` says when the window opens and closes.

The **prefs** mod is the one source there is: rekordbox's own preferences (`mods/prefs.cpp`). While the window is
open it keeps a Preferences component of its own that is never put on screen, the content of rekordbox's
Preferences dialog made the way the dialog makes it. It selects each category and page in it once and reads the
rows rekordbox built: labels, toggle buttons with their radio group, drop-downs, sliders, buttons, rekordbox's "?"
help texts and plan locks, each with where it sits in its row. The page lays these out again as cards and rows
(the rules are in `ui/settings.html`: a radio group becomes one choice, a row with a single control becomes one line,
and so on). Nothing about rekordbox's settings is written down in the mod: a new rekordbox version's pages, and
rekordbox's translations, come through as they are.

A change in the window is done to the hidden control (a click for a toggle or a button, the chosen id for a
drop-down, the value and the end of a drag for a slider), so rekordbox's own code applies it. rekordbox saves its
settings when its dialog closes; the mod saves them the same way two seconds after a change and when the window
closes. Rows the window can't show (lists, text fields) and the Plan, Keyboard and DVS categories are a line with
a button that opens rekordbox's own dialog at that page.

Test commands (`command-prefs` in the config folder): `scan` writes every page as read to `prefs-state.json`,
`msg <json>` is handled as a message of the page.
