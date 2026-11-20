# How it works

## One library, loaded into rekordbox

The mods are compiled into one library: `librbmods.dylib` on macOS, `rbmods.dll` on Windows. It is loaded into
rekordbox when it starts, and a few dozen specific calls inside rekordbox are sent through the mods (listed in
`hooks.def`).

- **macOS:** the patcher (or `build.sh`) copies rekordbox and patches the copy's program so that it loads the library
  and sends those calls to the mods. The copy is signed for that Mac.
- **Windows:** `platendoos.exe` starts rekordbox suspended, has its main thread load `rbmods.dll` before rekordbox's
  own code runs, and patches the calls in memory. No file of rekordbox is changed.

## Finding things by name

The mods find everything they need in rekordbox **by name** when it starts, rather than relying on fixed addresses,
so they have a decent chance of surviving an update. Three kinds of things are looked up:

| Kind | Written as | Found by | When it isn't found |
|---|---|---|---|
| Functions, vtables, globals | `Addr A_x{"<mangled symbol>"}` | the symbol's name, when rekordbox starts | the mod that uses it is switched off and logged |
| Fields inside rekordbox's objects | `Offset O_x{0x64, "<function>", "ldr x20", 9}` | a recipe: "the number in the 9th `ldr` based on x20 inside that function", the field's offset as rekordbox's own code uses it | the mod is switched off and logged |
| Call sites the mods take over | `HOOK(name, caller, nth, count, target, kind)` in `hooks.def` | "the nth of the count calls to target inside caller", when the app is patched | that hook is left out, and what depends on it doesn't run |

So an update that only moves code changes nothing, and a field that moved is picked up at its new offset (7.2.20 moved
five effect fields by 8 bytes; betterfx followed without a change). What breaks is a function that was rewritten.

## Windows: a table per build

rekordbox for Windows has no names in it. The mods keep naming rekordbox's functions by their **mac** symbols, and for
each Windows build a table maps those names to that build, made by matching it against the mac build of the same
version. For a build without a table, the patcher makes one from signatures of a verified build. See
[Windows internals](./windows).

## The same source on both

The mods are the same source on both platforms: plain C++20, with platform code behind small interfaces in `core/`
(`plat`, `webui`, the image and hook layers). `*_mac.mm` files are built only on macOS, `*_win.cpp` only for
Windows. The models run through backends: Core ML on macOS, ONNX Runtime (rekordbox's own `onnxruntime.dll`, with
DirectML) on Windows.

## The windows are web pages

<!--@public hiw-windows-->
The mods' windows (analysis, stems, auto cues, genres, the chip's window, the patcher) are
<!--@/public-->
HTML pages in `ui/`, shown in the system's web view (WKWebView on macOS, WebView2 on Windows). The mod builds the
page's state as JSON and handles its commands.

## Mods talk through services

<!--@public hiw-services-->
Mods never link to each other. A mod provides a service by name (`spotifyaudio.read`,
<!--@/public-->
`genres.forTrack`, …) and another asks for it; when it's not there, the feature that needs it says so. That's why
every mod can be switched off on its own.

## Project layout

| Folder | What's in it |
|---|---|
| `core/` | The shared base that finds things in rekordbox, hooks into it and gives the mods their tools (`*_mac.mm` / `*_win.cpp` hold the platform parts). |
| `mods/` | The mods, one file (or a few) per feature. Each mod's options are described at the top of its file. |
| `ui/` | The mods' windows, one HTML page each, shared by macOS and Windows. |
| `patcher/` | The platendoos patcher (shared logic and its `ui/patcher.html` window, with a macOS and a Windows part). |
| `engines/` | The analysis engines the mods use (Beat This! for beat grids, a CNN for keys). |
| `models/` | The trained models (not committed: made by `tools/models/` or fetched). |
| `tools/` | Building (`build/`), making the models (`models/`), development (`dev/`), checking key detection (`keytest/`), the Windows build and rekordbox.exe mapping (`win/`). |
| `docs/` | Longer write-ups of how some of the mods work. |
| `trailer/` | The scripts that render the trailer from recordings of rekordbox. |
| `website/` | This site. |
