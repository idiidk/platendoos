# Troubleshooting and FAQ

## First, look at the log

Everything the mods do is logged:

| Platform | Log |
|---|---|
| macOS | `~/Library/Logs/rekordbox-mods.log` |
| Windows | `%LOCALAPPDATA%\rekordbox-mods\rekordbox-mods.log` |

The PLATENDOOS chip's window opens it (**Open log**). The first lines say which rekordbox build it is, how the mods
found their way around in it, and which mods loaded or were switched off and why:

```text
rbmods: rekordbox build 4AF5D59D5EC937DCBD893FBC84D970AD (checked): 420 addresses, 167 fields
```

`tested` is the build platendoos was made for (7.2.19), `checked` a later one every mod was run on (7.2.20),
`untested` any other.

## Common problems

### The patcher won't open (macOS)

It isn't notarised. Open *System Settings > Privacy & Security* and choose **Open Anyway**, or right-click the app and
choose *Open*.

### Windows warns about the patcher

It isn't signed, so SmartScreen may warn: **More info > Run anyway**.

### rekordbox hangs at start, or has no sound (macOS)

macOS is asking whether platendoos may use the microphone, and rekordbox's start stands still until that's answered;
after two minutes it goes on without sound. Look for the prompt (it can be behind other windows) and click **Allow**.
It's asked once after an install: to macOS a newly signed copy of rekordbox is a new app. If you clicked *Don't
Allow*, allow it in *System Settings > Privacy & Security > Microphone*.

### A mod is off after a rekordbox update

On macOS the mods find rekordbox's code by name. After an update, a mod whose code changed switches itself off, and the
rest keep working. The chip says *untested rekordbox build: some mods may be off*; its window shows that mod as
*off: not available in this build*, and the log says what wasn't found. Run the patcher again (on Windows it then makes
a table for the new build); if the mod stays off, it needs a new platendoos release. See
[When rekordbox updates](/internals/rekordbox-updates).

### The patcher says it can't install

It says why: rekordbox not found, rekordbox open (Windows: close it, then *Check again*), a rekordbox that was already
changed (reinstall it from rekordbox.com), not enough disk space, a version that's too different, or the patcher's
own files missing (download or unzip it again, whole). Nothing is changed in any of these cases.

### I switched a mod off in the config and nothing happened

A mod's on/off switch applies at the next start of rekordbox. The chip pulses orange until you restart; its window
lists the mods that will change. Other options apply within a couple of seconds of saving.

### Spotify tracks don't get stems

<!--@public trouble-stems-->
They need [stemcache](/mods/stemcache) on (`stemcache_spotify=1`) and rekordbox's STEMS feature (*Preferences >
Extensions > STEMS*). A Spotify track gets its stems on a deck; the stems window doesn't separate Spotify tracks
ahead of time, since their stems are never stored on disk.
<!--@/public-->

### "streaming limit reached"

Spotify allows about 30 track downloads a minute. The mods wait it out (up to a minute) instead of failing, and try a
track that still fails again at the end of the run.

### A beat grid is at half or double tempo

Analyse with the [genres](/mods/genres) mod on (the analysis window's *Genre first*): the genre picks which tempo is
meant. If rekordbox's *Cloud Analysis* (*Preferences > Analysis*) found the track online, it kept rekordbox's grid and
skipped Beat This!: turn Cloud Analysis off and re-analyse. Locked grids are never touched.

### Clicks or dropouts (macOS)

See [Performance and dropouts](/guides/performance): keep audiomem on, close other apps when the log reports swap in
use, and try a buffer size of 256 or 512.

### Scratching a paused deck with a jog wheel feels choppy

[framerate](/mods/framerate) lowers the frame rate when it sees no input, and a controller's jog wheel isn't input it
sees. Set `framerate=0`.

### The cache drive was unplugged

Nothing new is recorded on it while it's gone; what is staged on the internal disk stays and moves once it's back. A
USB stick that hangs needs to be replugged. See [Caches and external drives](/guides/caches).

### Stems or grids look wrong on Windows

Set `gpu=0` and restart rekordbox: the models then stay off the graphics card. See [Windows notes](/guides/windows).

## FAQ

**Does platendoos change my rekordbox?** No. On macOS it is a separate copy in `~/Applications`; on Windows it starts
your installed rekordbox with the mods loaded. The only change, and only when ticked, is the DDJ-FLX4 mapping on
Windows, which is put back on uninstall.

**Does it change my library?** It works on your library like rekordbox does, when you ask: analysis results, grids
(bettergrid), genres you apply, cues you write, playlists you create. Keep backups of your rekordbox library, as with
any tool that changes it.

**Can I run plain rekordbox and platendoos at the same time?** Don't: they share the same library.

**Can I turn everything off?** Untick all mods in the patcher (*All off*), or use plain rekordbox.

**Is it official?** No. It's an unofficial hobby project, not made by or affiliated with AlphaTheta / Pioneer DJ or
Spotify.

**Can I use it commercially?** No. platendoos is CC BY-NC-SA 4.0, and two of its models are non-commercial too. See
[Licence and credits](/internals/licence).

## Reporting a problem

Open an issue on [GitHub](https://github.com/idiidk/platendoos/issues) with:

- your platform, rekordbox version and platendoos version (the chip's tooltip or window shows both);
- what you did, what you expected, and what happened;
- the part of the log around it, and the log's first lines. Look it over first: it contains file paths and track
  titles from your computer.
- for an install problem, the output of `--check` (see [Installation](/getting-started/installation#from-the-command-line)).

Turning one mod off at a time (and restarting) is the quickest way to find which one is involved.
