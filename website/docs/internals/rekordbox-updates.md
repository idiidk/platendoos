# When rekordbox updates

::: info For developers
This page is about checking and fixing platendoos for a new rekordbox version. If you just updated rekordbox, run the
patcher again: see [Installation](/getting-started/installation#after-a-rekordbox-update).
:::

What survives a rekordbox update by itself, how to check a new version before anyone runs it, and how to fix
what broke. The macOS side is described here; the Windows side (a table per rekordbox build) is in
[Windows internals](./windows), under *Finding things in rekordbox.exe*.

## What the mods depend on

Nothing in the mods is a fixed address. Three kinds of things are looked up in rekordbox:

| Kind | Written as | Found by | When it isn't found |
|---|---|---|---|
| Functions, vtables, globals | `Addr A_x{"<mangled symbol>"}` | the symbol's name, when rekordbox starts | the mod that uses it is switched off and logged |
| Fields inside rekordbox's objects | `Offset O_x{0x64, "<function>", "ldr x20", 9}` | "the number in the 9th `ldr` based on x20 inside that function": the field's offset as rekordbox's own code uses it | the mod is switched off and logged |
| Call sites the mods take over | `HOOK(name, caller, nth, count, target, kind)` in `hooks.def` | "the nth of the count calls to target inside caller", when the app is patched | that hook is left out, and what depends on it doesn't run |

So an update that only moves code changes nothing. A field that moved is picked up at its new offset (7.2.20
moved five effect fields by 8 bytes; betterfx followed without a change). What breaks is a function that was
rewritten: a symbol that no longer exists, or a caller that no longer makes the same calls.

The log's first line says how it went:

    rbmods: rekordbox build 4AF5D59D5EC937DCBD893FBC84D970AD (checked): 420 addresses, 167 fields

- "tested": the build the expected values were written for (`kTestedBuild` in `core/image_mac.mm`, rekordbox
  7.2.19). There a recipe that gives another value than expected is only logged ("field recipe … out of date").
- "checked": a later build every mod was run on (`checkedBuild()`, 7.2.20). Same rules as an untested build, but
  the chip and the patcher don't warn about it. Add a build there once the tests below have passed on it.
- "untested": any other build. A mod with anything unresolved is switched off; the PLATENDOOS chip says
  "untested rekordbox build: some mods may be off" and its window lists every mod and whether it is on.

## Checking a new version without installing it

rekordbox's installer can be unpacked instead of run, so the installed rekordbox and the library stay as they
are until the new version is known to work.

1. Unpack the installer: `pkgutil --expand-full <installer>.pkg <folder>` gives
   `<folder>/rekordbox.pkg/Payload/rekordbox.app`.
2. Check the field recipes against it:

       python3 tools/dev/recipes.py check <rekordbox.app>/Contents/MacOS/rekordbox

   A recipe that gives another value is a moved field (fine: the mods read the new value) or a function that
   changed (look at it: `recipes.py find <binary> <function regex> <offset>` lists the instructions that use an
   offset, each with its recipe).
3. Check the hooks:

       python3 patch_binary.py <rekordbox.app>/Contents/MacOS/rekordbox /tmp/rekordbox.patched

   It prints `patched 149 of 149 hooks` and a `left out:` line for each hook it couldn't place. 7.2.20: 128 of 135
   at first, the other 7 in three functions rekordbox had rewritten.
4. The patcher app does the same check with its own (C++) patcher:

       PLATENDOOS_SOURCE=<rekordbox.app> "platendoos patcher.app/Contents/MacOS/platendoos patcher" --check

## Fixing a hook whose call site moved

When the call a hook takes over still exists but the code around it changed (another caller, another count),
give the hook a second site under the first:

    HOOK(stem_sep4, "<caller in 7.2.19>", 0, 1, "<target>", -1)
    HOOK_OR(stem_sep4, "<caller in 7.2.20>", 0, 1, "<target>", -1)

`HOOK_OR` is another site for the `HOOK` above it, tried when that one isn't found. It is no hook of its own:
the mod's code stays the same, and both rekordbox versions keep working. Both patchers read it
(`patch_binary.py` and `patcher/macho.h`; they must give the same result, change both).

When the call itself is gone, the mod needs new code for the new rekordbox: keep the old path for the old
build and let the mod say which it found.

## Running the new version next to the old one

    ./build.sh <unpacked rekordbox.app> ~/Applications/platendoos-<version>.app

builds a patched copy of the unpacked rekordbox under its own name; `~/Applications/platendoos.app` and the
installed rekordbox aren't touched. Before the first start, copy rekordbox's database folder
(`~/Library/Pioneer/rekordbox`) somewhere: a newer rekordbox may update the library, and going back to the old
version afterwards isn't tested. Never run two copies at once.

`./install.sh <that app>` then swaps a rebuilt library into it, as for the normal copy.

Test at least: the log's first lines (every mod loaded, no "switched off"), one analysis of a track whose result
is known (same BPM and key as before), one deck load of a Spotify track (playable, waveforms, grid, stems), the
track menu, the windows. To try the patcher app's install without touching the real copy:
`PLATENDOOS_SOURCE=<rekordbox.app> PLATENDOOS_DEST=<a test folder>` with `--install`.

## rekordbox 7.2.20

Checked and run on 2026-11-19, next to an installed 7.2.19.

- macOS: every address and field resolved. Seven hooks had lost their site: `Service::getPlaylistTracks` and
  `getLookupTracks` were rewritten (spotifyapi's six hooks), and `AudioPartSeparator::start` was split, with
  the call stemcache takes over now in `checkAndPrepare`. With `HOOK_OR` lines for those, 7.2.19 and 7.2.20
  both patch every hook (135 that day, 149 now). All 33 mods load; a re-analysis gives the same result as on 7.2.19; a deck load is
  playable in 0.1 s; Spotify tracks get stems; the patcher app's check and install pass. Not timed: that
  several Spotify lists really load at once.
- Windows: see *Status (rekordbox 7.2.20)* in [Windows internals](./windows). All 27 mods load with the table made
  for it.
