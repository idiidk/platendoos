# Windows internals

::: info For developers
How the Windows version works inside and how a new rekordbox build is mapped. For using platendoos on Windows, see
[Windows notes](/guides/windows).
:::

The same mods run in rekordbox for Windows. This explains what's different and how to keep both working.

## One codebase

- Shared code is plain C++20: `core/*.cpp`, `mods/*.cpp`, `engines/*.cpp`. It talks to the operating system only
  through `core/plat.h` (threads, timers, paths, files, memory patching) and a few small backend headers.
- Platform code sits next to it, by file name: `*_mac.mm` is built only on macOS (`compile.sh`), `*_win.cpp` only
  for Windows (`tools/win/build.sh`). The rest of the `.mm` files are mac-only mods (midipoll, screeninfo,
  fontcache, and for now outputsync and betterfx).
- Backends behind headers: `engines/dsp.h` (Accelerate / portable C++), `engines/beatthis_backend.h` and
<!--@public win-audiofile-->
  `mods/betterstems_backend.h` (Core ML / ONNX Runtime), `mods/audiofile.h` (AudioToolbox / Media Foundation,
  and `mods/aiff.h` for AIFF, which Windows doesn't read),
<!--@/public-->
  `mods/stemcodec.h` (AudioConverter AAC + LZFSE / Media Foundation), `mods/flx4_overlay.h` (AppKit / Win32),
  `core/webui.h` (the tool windows: WKWebView / WebView2, pages in `ui/`).
- rekordbox's own C++ ABI differs: Itanium on mac, MSVC on Windows. Shared code avoids assuming either:
  `rbm::deleteObject` (virtual destructor), `rbm::dynCast` (dynamic_cast by typeinfo Addrs), vtable slots as
  `Offset` "vt:" recipes, never literal slot numbers. rekordbox's `std::` members (deque, map, string, function)
  have different layouts; code that reads them needs both.

## Finding things in rekordbox.exe

rekordbox for Windows is stripped (no symbols, no PDB), but the mac build of the same version names everything,
and its universal binary has an x86-64 slice. The mods keep naming rekordbox's functions by their **mac
symbols** (`Addr`, `Offset`, `hooks.def` stay as they are); for each Windows build a table maps those names
to that build:

    rbmods/rekordbox-<TimeDateStamp>-<SizeOfImage>.tbl      (next to rbmods.dll; format in core/image_win.cpp)

The table is made by matching the two builds (tools in `tools/win/match/`):

1. `feat.py` extracts per function: calls, imports, strings, constants, data references (mac from symbols,
   Windows from `.pdata` plus leaf functions).
2. `vtables.py` pairs classes by RTTI name (MSVC RTTI is in the exe): vtables slot by slot, and typeinfos.
3. `match2.py` anchors on unique strings, constants, data refs and Windows-only `__FUNCTION__` names, then
   propagates through the call graph; `verify/pins.json` (verified pairs) goes first. `HOLDOUT=1` +
   `fnnames.py` measures precision against the `__FUNCTION__` ground truth (~90%: not good enough alone).
4. Every address the mods use is then **verified** by comparing disassembly side by side (`show.py`,
   `xref.py`; sub-agents did this for 7.2.19): results in `verify/*.out.json`, field offsets included.
5. `tools/win/maketable.py <rekordbox.exe>` writes the table from verified entries only (plus RTTI vtables,
   typeinfos, vtable slots and singleton holders derived from them). A missing entry switches its mod off
   (the log says which); a wrong one would crash, which is why nothing unverified goes in.

**A rekordbox update without a table**: the patcher makes one itself (`patcher/retable.h`) from signatures of
the verified build, so the mods carry over as far as the new build's code is unchanged:
- `build/makesigs <rekordbox.exe> <its .tbl> > tools/win/sigs/rekordbox-<build>.sig` (about 3 seconds; the C++
  port of `tools/win/makesigs.py`, same output; build it with `clang++ -std=c++17 -O2 -I/opt/homebrew/include
  -L/opt/homebrew/lib -lcapstone -o build/makesigs tools/win/makesigs.cpp`, capstone from Homebrew). It reads
  `~/rbwin/win_chunks.txt`: `python3 -I tools/win/winchunks.py` writes it from win.pkl. The Python original
  (about 14 minutes; capstone, pefile, numpy) stays as the reference; the line format is at its top. Regenerate
  the signatures whenever the table changes. Patterns are whole instructions; they mask rip-relative and call targets, and field/stack
  displacements, immediates and short jumps are "loose" (a `*` before the byte): compared first, ignored when the
  exact pattern isn't found, the match unique either way.
  - Register-insensitive pass: the compiler may pick other registers in another build for the same code
    (`mov [r14+0x148],rsi` against `mov [rbp+0x138],rsi`). The signatures therefore mark the bytes that carry
    registers (`%` before a ModRM or SIB byte, `^` before a REX prefix). Exact and loose passes compare them
    exactly; only when both find nothing does a third pass ignore the loose bytes and compare ModRM/SIB under
    0xC0 and REX under 0xF8 (the addressing mode, scale and the W bit stay). It still needs a unique match.
    Hooks that read a caller's register (`hookreg`) never use this pass.
    For fields, makesigs adds one more `offin` line per read: a window that starts at the read and is unique in
    its function at that level (what comes before the read may differ between builds).
  - Names: RTTI (vtables, typeinfos), unique patterns, references in unique windows, and fallbacks: windows around
    calls in up to 3 callers (grown both ways, and backwards only), vtable slots by bytes (or rank among
    look-alikes; a moved subobject's vtable is searched among the class's). A name's lines vote; a name inside
    another .pdata function is rejected.
  - Fields: the instructions reading them (several lines must agree), unchanged when the function's code is, else
    `offseq` (the reads through the same register around it, as differences) or `offrel` (its distance to a found
    field read through the same register). `tools/win/sigfields.txt` pins a field to one instruction by regex.
    A field whose Mac function Windows inlined or split up (flx4's `O_tempoRange`, `O_tempoFader`) is looked for in
    the functions its verification entry names by address (`0x14...` in "win_fn" and "why"): name only functions
    that read it there, and write other addresses as rvas.
  - Hooks: only when all their sites are found and call the found target; else the k-th call of the target in a
    found caller (`hookcall`; for a hook that reads a caller's register, only where its own pattern matches).
    A `lea` site (a function's address taken for a callback table: spotifydl's `sp_https_get`) is found by its
    bytes like a call; the function whose address it takes gets a signature of its own (an `@<rva>` anchor), and a
    site that takes another function's address is left out.
  - A field read in a function the table has no address for (the three `ListViewSorter::compareElements` fields
    of multisort) is found when its verification entry names that function as `"win_fn"` and
    `tools/win/sigfields.txt` pins the instruction: small displacements like 8 and 0x10 are otherwise also stack
    slots in the same function.
- Constants MSVC compiles differently from the Mac go in the table as `win:` fields (verified in
  `tools/win/verify/*.out.json`), so a generated table finds them in the new build. `tools/win/sigguards.txt` is for
  constants still hard-coded in `#ifdef _WIN32` code: one not found unchanged leaves the hooks listed there out.
  **A mod that needs a raw Windows offset should make it a `win:` field, or add a guard line.**
- `tools/win/build.sh` ships the .sig files in `rbmods\sigs`. The patcher's `find()` scans rekordbox.exe when
  `rbmods\` has no table for its build (a few seconds), says how many parts it found, and the install writes the
  table into the installed `rbmods\`; mods missing a hook show as off, missing addresses and fields switch their
  mod off at run time (`checkBuild()`). `tools/dev/retable.cpp` runs the same scanner on the Mac:
  `clang++ -std=c++17 -O2 -o build/retable tools/dev/retable.cpp && build/retable <exe> <sig> > <tbl>`.
- On the verified build itself the scanner reproduces the table exactly, apart from 5 `TrackListSorter::compare*`
  functions nothing calls (checked again on 2026-11-19: 14062 of 14067 parts, no other difference). On 7.2.18 (fields moved ±0x10/0x20, vtables shrank) it finds everything the mods use
  except spotifyfix's token hook (7.2.18's Spotify client doesn't call that setter): 20 of 21 mods on, tested on
  the PC (track loading, analysis with key/phrase/similarity, bettergrid, betterkey).
- Limits: small updates carry over; a compiler change or a big refactor won't (then do the full matching above,
  from the previous Windows build). Hand-coded offsets are only safe when listed in sigguards.txt (flx4's
  `+568` is checked at run time). makesigs writes no `orig` lines (the 7.2.19 table has none).

**A build whose code changed where a mod hooks it** (7.2.20: see its status below) gets a shipped table: what the
scanner finds, plus the few lines checked by hand for the changed code, kept with their reasons in
`tools/win/verify/rekordbox-<build>.add`:

    build/retable <rekordbox.exe> tools/win/sigs/rekordbox-<verified build>.sig \
      | python3 -I tools/win/addtable.py - tools/win/verify/rekordbox-<build>.add > tools/win/tables/rekordbox-<build>.tbl

(The `.add` file is a source file, kept with the verification files; `tools/win/tables/` and `tools/win/sigs/`
aren't in git: they go into the build-inputs release.) `addtable.py` only fills gaps (a line the scanner finds itself must agree, anything else is an error). Run it
again whenever the signatures change (a new Addr, Offset or hook): a shipped table is used as it is, the patcher
doesn't scan a build that has one, so a stale one leaves the new mod off on that build. The 7.2.19 table and its
signatures stay the reference; verification entries in `tools/win/verify/*.out.json` are 7.2.19 addresses only
(`maketable.py` reads them all), which is why another build's hand-checked lines live in its `.add` file.

To try the patcher on a rekordbox that isn't installed (a new version unpacked from its installer with 7-Zip, next
to the installed one): `PLATENDOOS_SOURCE=<its rekordbox.exe>` with `PLATENDOOS_DEST=<a test folder>` and
`PLATENDOOS_FLX4=0`, then `"platendoos patcher.exe" --check` / `--install` (redirect the output to a file: it is a
window program), and start it with `platendoos.exe --rekordbox <its rekordbox.exe>`.

Corrections found later (a verdict that turned out wrong) go into `verify/overrides.json` ("drop": item -> why),
so a regenerated table never brings them back. `tools/win/coverage.py <table> [-v]` shows per mod what the table
still lacks; `tools/win/match/missing.py` turns that into new verification batches.

Table details that differ from mac:
- A vtable's mac symbol (`__ZTV...`) maps to the Windows vtable minus 0x10, so `symbol + 0x10` (mac's address
  point) is the Windows vptr. Slots come from "vt:" recipes, computed by maketable: MSVC has one deleting
  destructor where mac has two, puts a group of overloaded virtuals in reverse order, and has no virtual-base
  offset words before the slots (SyncManager on mac has one, so its address point is +0x18 there).
- `SingletonHolder<T>::get()` takes no argument on Windows (its holder is a global it uses itself); the holder
  Addr becomes that global, and the mods' `get(holder)` calls simply pass an ignored argument.
- `hook ... icall` sites call through the import table (juce's CriticalSection::enter/exit are inlined into
  EnterCriticalSection/LeaveCriticalSection calls); their original is whatever the import slot holds.

Safety rules in core/image_win.cpp: a field the table lacks switches its mod off (never the mac value); a mod
whose hooks aren't all in place gets the installed ones taken out again and is off (which mod a hook belongs to
is generated by `tools/win/hookmods.py` at build time); every hook site is checked against the expected call
before it is patched.

After a rekordbox update: download the new Windows installer (its URL is in winget-pkgs:
`manifests/a/AlphaTheta/rekordbox/<version>`), install the same mac version, re-run 1-5. Most pins and
verified pairs carry over by name; only the changed functions need a new look.

## Hooks and loading

- No file of rekordbox is changed. `platendoos.exe` starts rekordbox suspended and has its main thread load
  `rbmods.dll` (an APC, which runs once the loader has initialised the process, before rekordbox's own code).
- `rbmods.dll` loads the table, patches each `hooks.def` call site (`call rel32` / `jmp rel32` / `call [rip]`,
  checked against the expected target first) to a thunk next to rekordbox's code that jumps to
  `rbm_hook_<name>`, and hooks rekordbox's entry point once to create its main-thread window (outside the
  loader lock) before rekordbox's own startup.
- A hook whose mod isn't built for Windows (the mac-only mods) links to `rbm_hook_missing`, and its site is left
  alone. hooks_win.cpp compares the two addresses at run time: the compiler folds `&rbm_hook_x == &rbm_hook_missing`
  to false (they're different functions to it), which once patched those sites to an empty function.
- Hooks that need a register of rekordbox's caller (mac: the arm64 trampolines moving x19/x20 into an extra
  argument) get a `hookreg <name> <register> <argument>` table line; the thunk then moves that register.
- C++ hooks have the same signature on both: MSVC's x64 convention is what clang-cl compiles them to.
  Careful with hooks on member functions returning a class by value: the hidden return pointer comes after
  `this` on Windows.

## Building

    brew install llvm lld xwin
    xwin --accept-license splat --output ~/rbwin/xwin
    tools/win/build.sh          # -> build/win/platendoos/{platendoos.exe, rbmods.dll, rbmods/}

Cross-compiled with clang-cl in the MSVC ABI, static CRT. SQLite comes from rekordbox's own `sqlite3.dll`.
`RBM_JOBS=<n> tools/win/build.sh` compiles at most n files at a time (the default starts them all at once, which
fails with "fork failed" when the Mac is close to its process limit).

## Testing without Windows

`tools/win/emu/` runs the DLL's compiled code on the Mac: a small x86-64 emulator (Python + Unicorn) maps the real
rekordbox.exe and rbmods.dll, runs the DLL's C runtime and static initialisers (the real table loads, every `Addr`
and `Offset` resolves), and a per-mod test calls the mod's functions against rekordbox's real code and RTTI, with
Python stand-ins for what needs a live app (JUCE components, the engine, windows):

    python3 tools/win/emu/run.py            # every test against each rekordbox.exe under $RBWIN with a table

`tools/win/emu/README.md` in the source tree explains writing a test and what runs for real.

## Paths on Windows

| | |
|---|---|
| Config | `%APPDATA%\rekordbox-mods\config.txt` |
| Log | `%LOCALAPPDATA%\rekordbox-mods\rekordbox-mods.log` |
| Caches | `%LOCALAPPDATA%\rekordbox-mods\cache\` |

## Writing mod code that works on both

rekordbox for Windows is MSVC-compiled; the mods' calls into it and hooks must follow MSVC's x64 ABI there:

- A member function returning a class by value (juce::String, Rectangle...) takes the hidden result pointer
  after `this` (Itanium: first). Use `rbm::memberString(addr, self, args...)` for String results; hooks that
  replace such a call take `(this, result*, args...)` on Windows (see columns.cpp's paint_getString).
- A class passed by value (juce::String, std::function) is passed by address of a temporary that the CALLEE
  destroys on Windows (Itanium: the caller). Build it in raw storage and don't destroy it after the call
  (core.cpp's menuAddItem, flx4's StrArg).
- `long` is 4 bytes on Windows: rekordbox's `long`/`unsigned long` parameters (mangled l/m) are 32-bit there.
- A member function overriding a virtual of a non-primary base takes that base's subobject as `this` on
  Windows; mac's `__ZThn<N>` thunks become such functions. Call through `rbm::thisDelta(addr)` (from the table's
  `adj` lines; 0 on mac). Offsets from verification are relative to the primary object.
- RTTI: `rbm::dynCast` works on both; code that reads typeinfo names must read MSVC's complete object locator
  on Windows (core.cpp's isA, flx4's className).
- rekordbox's std:: members have MSVC layout on Windows; we compile with MSVC's STL there, so std:: casts match
  (libc++ static_asserts must be guarded).
- Objects rekordbox allocates and frees: sizes (`S_x`) are per build; make them Offsets (betterkey's S_pcmReader).
- Code with no Windows equivalent yet goes behind `#ifdef _WIN32` with a log line saying the feature is off.
- A hook that has no site on Windows switches its whole mod off (half-hooked mods are unsafe): keep a mod's
  hooks together, and keep a hook whose Windows site is missing but optional out of the mod's Windows build.

## Windows (tool windows) and the badge

<!--@public win-windows-->
The mods' windows (analyzer, stemcache, autocue, genres, the about window) are one HTML page each in
<!--@/public-->
ui/, shown by core/webui.h (WKWebView on macOS, WebView2 on Windows; WebView2 needs no install on Windows 11).
The mod builds the page's state as JSON (`web::post`) and handles its commands (`onMessage`); the page's look
and anything purely visual (filters, dates, dialogs) are the page's. On Windows a window is owned by rekordbox's
main window (the largest of its top-level windows; not the foreground one, which can be a short-lived popup that
takes its owned windows with it).

The badge's chip is drawn by mods/badge_chip.h's AppKit code on both: on Windows from PNGs that build.sh renders
with it at the display scales (100-300 %) and embeds (tools/win/embed_pngs.py).

For scripted tests, `rbm::devCommand(mod)` reads a line from `<configDir>/command-<mod>`: `about` (badge),
<!--@public win-devcmd-->
`open [playlist id]` (stemcache), as analyzer's `command` file.
<!--@/public-->
- The command files are in `%APPDATA%` (Roaming), the log in `%LOCALAPPDATA%`.
- analyzer's `load <row> [deck]` counts rows from 0 (`rows [n]` logs them).
- From cmd, put a space before `>`: in `echo load 3 2> file` the `2>` is a redirect, so the deck number is lost.
  The trailing space this leaves is trimmed.

A slower PC needs longer waits, so on Windows the analyzer's per-track timeout is 180 s (macOS: 90) and
bettergrid waits 60 s for its result (macOS: 20). The analyzer's start and analysis clocks of a queued row only
start when rekordbox starts analysing it.

## Speed

What the models cost on Windows.

Measured on the x64 PC (i5-9600K, 6 cores; Radeon RX 580; 16 GB) on 2026-11-19 with the unpacked rekordbox 7.2.20,
<!--@public win-speed-track-->
a never-loaded, never-analysed Spotify track loaded on deck 2. Times are seconds from the test's load command, which
<!--@/public-->
the analyzer reads once a second (rekordbox itself calls the deck playable 0.0-0.1 s after it starts loading).
"All cores" is the whole PC's CPU use.

| Case | Track | Playable | Waveform | Beat grid | Key | Stems done | CPU | Audio, UI |
|---|---|---|---|---|---|---|---|---|
| GPU (the defaults), nothing playing | 4:56 | 0.8 | 6.9 | 11.2 | 12.3 | ~50 (HTDemucs) | 40-73% for 6 s, then 16-25% | UI answers within 0.2 s |
| GPU, first load after a launch | 7:00 | 1.0 | 8.1 | 14.4 | 15.8 | ~55 (HTDemucs) | the same | the same |
| `gpu=0`, HTDemucs and Beat This! on the CPU (the defaults until 2026-11-19; now `betterstems_slow=1` `bettergrid_slow=1`), deck 1 playing | 4:05 | 0.6 | 7.4 | 17.9 | 19.0 | 117 (HTDemucs) | 94-100% for 6 s, 62% for 7 s, then 70-89% until the stems are done | one audio glitch 1.9 s after the load; UI within 0.3 s |
| `gpu=0` `betterstems=0`, Beat This! on the CPU, deck 1 playing | 6:35 | 0.7 | 9.2 | 31.4 | 32.6 | ~30 (rekordbox's) | 97-100% for 23 s, 62% for 7 s | no glitch; UI within 0.25 s |
| `gpu=0` `betterstems=0` `bettergrid=0`, deck 1 playing | 6:07 | 0.8 | 7.8 | 8.4 | 9.2 | ~20 (rekordbox's) | 97-99% for 8 s, 75-80% for 6 s | no glitch; UI within 0.15 s |
| `gpu=0`, the defaults since 2026-11-19, deck 1 playing | 6:45 | 0.6 | 7.7 | 7.9 | 9.6 | ~22 (rekordbox's) | 91-99% for 9 s, 75-80% for 8 s | no glitch; UI within 0.35 s |

What the numbers say:

- **The models loading at launch** (core's `startupTask`, one after another): Beat This! 0.7-1.2 s, HTDemucs 1.1-1.7 s,
  the style model 0.1-0.2 s on the GPU; 0.4 s, 0.8 s and 0.1 s on the CPU. All three are loaded before rekordbox's
  window shows (the launch takes 13-14 s to the reopened playlist on 7.2.20, 14-20 s on 7.2.19).
- **No first-run cost on DirectML.** The same track took Beat This! 3.0 s as the first run after a launch and 2.9 s
  after; HTDemucs's first segment 0.52 s against 0.43-0.45 s. So the macOS launch warm-ups (seconds there) aren't
  needed for speed on Windows. One segment and one chunk are still run at launch on a GPU, to measure it (below).
- **Beat This! holds the beat grid back.** rekordbox's own grid is there about half a second after the waveforms on this PC; the
  analysis then waits for Beat This! (`bettergrid_wait`, 60 s on Windows). On the RX 580 that is 0.41 s per chunk
  (30 s of audio), about 0.9 s per minute of track: the final grid of a 5-minute track is saved 3-5 s later than without
  bettergrid. A deck doesn't wait for it: while the analysis waits, quickdeck shows rekordbox's own grid
  (`bettergrid_rb_first`), and the combined one replaces it. Measured on 7.2.19 with a 4-minute track: "deck 2
  shows with the waveforms 4.3 s after the analysis started", "with rekordbox's grid 4.4 s", "with the beat grid
  8.5 s" (Beat This! 4.1 s). On the CPU (hw/2 threads) it took 12.7 s for a 4-minute track and 22.9 s for a 6.5-minute one while
  rekordbox's stems engine had the other cores: the grid 18-31 s after the load, against 8 s.
- **HTDemucs on the CPU** needs 2.6-5 s per segment (7.8 s of audio) and kept the six cores at 70-89% for two
  minutes for one 4-minute track, next to the playing deck. On the RX 580 a segment takes 0.44-0.49 s (a track in
  about 50 s, the beat grid first: `betterstems_grid_first`). rekordbox's own engine separates a track in 15-30 s
  on the CPU, at 75-100% of all cores.
- **Even without the heavy mods** a load on a PC like this one fills every core for 8-9 s (rekordbox's analysis and
  its stems engine together) and 75-80% for another 6-8 s. That is rekordbox's own; the mods no longer add to it
  when there is no GPU.

What the mods do about it (Windows only):

- **betterstems uses HTDemucs only where it is fast**: on a GPU (DirectML) that separates a segment within 1.0 s.
  The segment is timed when the model is loaded at launch (one of silence, a second one if the first is over the
  limit; the log says "a segment takes 0.47 s on the GPU (DirectML)"). With `gpu=0`, without a DirectX 12 GPU, or
  on a slower GPU, the model isn't kept and rekordbox's own engine separates (the log says so).
  `betterstems_slow=1` uses HTDemucs anyway, on half the cores (2-8 threads; it was all but two).
- **bettergrid doesn't make a deck wait for a slow Beat This!**: where it runs on the CPU, or on a GPU that needed
  over 1.0 s for one chunk at launch ("model loaded (1.1s, GPU (DirectML), 0.41 s a chunk)"), a track that is on
  a deck when its analysis reaches the beat stage keeps rekordbox's grid, which shows at once; the log says
  "Beat This! is slow on this PC (...)". Tracks analysed from the Analysis window or the collection still wait for
  Beat This!. `bettergrid_slow=1` waits on a deck too. (An analyzer run with `analyze_direct=0` analyses on decks
  3/4: those count as deck loads.)
- **The style model** (genres) uses half the cores at most on the CPU (1-4 threads; it was all but two, up to 8).
  A run takes 0.4-0.5 s there.
- No chunk or segment is run at launch on the CPU: the answer is known, and it would keep the cores busy for
  seconds while rekordbox starts.

Both "slow GPU" rules ran on the PC with the limits lowered to 0.1 s in a test build (the RX 580 then counts as
slow: HTDemucs released, rekordbox's engine separated, the grid at once). They have not run on a real integrated
GPU.

How it was measured: `C:\rb\mon.ps1` (task `rbmon`) writes the mods' log lines with a clock time and the CPU use
once a second to `C:\rb\tlog.txt`; `C:\rb\uiprobe.ps1` (task `rbuiprobe`, in the desktop session) sends rekordbox's
main window an empty message every 50 ms and logs answers slower than 100 ms to `C:\rb\ui.txt`; both stop when
`C:\rb\mon.stop` exists. Audio: Windows' event log `Microsoft-Windows-Audio/GlitchDetection`
(`wevtutil sl Microsoft-Windows-Audio/GlitchDetection /e:true`, off again afterwards). A deck is started with a
click on its play button (`C:\rb\click.txt`, task `rbclick`); the analyzer's `load <uri> <deck>` loads the track.
`C:\rb\launchload.ps1 <task> <seconds> "<command>"` starts rekordbox and writes an analyzer command once the
playlist is open.
(The event log alone proves little, and the "one audio glitch" in the table above was never heard or recorded:
see the next section for a recording-based check.)

### A deck load while another deck plays

Does loading a track on deck 2 make deck 1's sound drop out, and is it worse with the mods? Measured on the same
PC on 2026-11-19 with rekordbox 7.2.19: **no dropout, plain or with the mods**, in 42 recorded loads. What the mods did
<!--@public win-freeze-->
add was a longer freeze of rekordbox's window at the moment a new track's waveforms appear; that is now
<!--@/public-->
as short as plain rekordbox's.

How rekordbox plays there: JUCE "Windows Audio" (WASAPI, shared mode), the PC's digital output, 44.1 kHz, a
buffer of 256 samples (5.8 ms); Windows mixes in periods of 10.16 ms (448 frames). rekordbox's audio thread runs
at priority 24 (Windows' "Pro Audio" class); everything else in the process at 8.

How a dropout is found: `rbglitch.exe` (tools/win/glitch) records the output device through WASAPI loopback while
deck 1 plays a local file (rekordbox's Demo Track 1: a Spotify track is taken off the deck when another device
of the account starts playing). `analyze.py` then looks for packets Windows marked silent, runs of zeros, and
places where the recording stops matching the file or jumps in time. The recording equals the file to -80 dB,
so anything wrong stands out. The check itself was checked: stopping rekordbox's threads for 50 ms gives 38.6 ms
of zeros, three silent packets and the track 38.6 ms late; 20 ms gives 15.1 ms; 5 ms gives nothing (the buffers
cover it). Windows' own glitch log (`Microsoft-Windows-Audio/GlitchDetection`) was on as well. The same tool
sends rekordbox's window an empty message every 20 ms (the freeze of the window is the longest answer), samples
the CPU time of every thread every 50 ms, and presses the keys: the track is found with the list's search box
and loaded with Shift+Right, in plain rekordbox and with the mods alike.

Each run: deck 1 playing, 6 s of recording, the load on deck 2, 39 s more. Tracks of 6-7 minutes.

| Case | Runs | Dropouts | Glitch log | Longest freeze of the window | CPU, first 10 s (busiest second) | Waveforms and grid on the deck |
|---|---|---|---|---|---|---|
| plain rekordbox, analysed track | 6 | 0 | 0 | 61-143 ms | 12-21% (24-37%) | 0.6-1.0 s |
| plain rekordbox, never-analysed track | 6 | 0 | 0 | 112-152 ms, once, when the analysis is shown | 25-30% (34-38%) | 8.3-10.3 s (21 s for one of 18 minutes) |
<!--@public win-dropout-rows-->
| mods, GPU, new track, before | 8 | 0 | 0 | 280-690 ms (started below normal), 348-392 ms (started normally) | 40-52% (63-85%) | 6.9-9.1 s, the combined grid 12.6-16.1 s |
| mods, GPU, new track, `worker_priority` only | 6 | 0 | 0 | 246-294 ms | 37-52% (64-79%) | 5.5-8.8 s |
| mods, GPU, new track, now | 4 | 0 | 0 | 119-157 ms | 37-44% (68-71%) | 5.4-8.7 s, the combined grid 9.6-14.5 s |
| mods, `gpu=0`, new track, now | 2 | 0 | 0 | 197-223 ms | 89-91% (100%) | 8.1-8.8 s |
<!--@/public-->

("Started below normal": Windows' Task Scheduler starts a program one priority class down unless the task says
otherwise, which the test tasks `platendoos` and `rbplain` do; the first 20 runs were made that way, nine of
plain rekordbox's too (its two started normally: 112 and 118 ms). The tasks `platendoosT` and `rbplainN` start it as a double click does.)

Where the freeze came from. With **quickdeck** a deck is redrawn when the waveforms are ready and again when
rekordbox's own grid is, 0.15 s later on this PC; each redraw is rekordbox's own "analysis completed" reload
and keeps its window busy for 0.13-0.15 s. Plain rekordbox does it once, at the end. And at that same moment
Beat This! prepares the track (resampling and the mel spectrogram, `plat::parallelFor`: six threads, every
core at 100% for half a second) at the same priority as the thread that draws: the thread sampler shows the
mods' threads taking 2.1-2.4 core-seconds of that 0.6 s, and each redraw took 0.18-0.22 s (0.23-0.56 s when
started below normal).

What was changed (Windows only):

- **The mods' threads run one step below normal priority** (core/plat_win.cpp): every thread the mods start
  (found as it starts, by its start address inside rbmods.dll), their thread-pool jobs, and ONNX Runtime's
  workers for the mods' models (engines/ort.cpp starts those itself for that). rekordbox's own threads, the
  drawing one first of all, get a core before the mods' work does; the mods still use every core nothing else
  wants, and the analysis is not slower for it. `worker_priority=0` in config.txt puts them back at normal
  priority, `-2` two steps down; the default is `-1` on a PC with four or more logical cores and `0` on a
  smaller one. The log says which: "rbmods: the mods' threads run at one step below normal priority".
- **quickdeck shows the waveforms and rekordbox's grid in one redraw** (mods/quickdeck.cpp): the waveforms
  wait up to 0.25 s for the grid (`quickdeck_merge`, seconds; `0`: no wait). The log line then reads "deck 2
  shows with the waveforms and rekordbox's grid".

With `gpu=0` the cores are full for ten seconds (rekordbox's own stems engine on three TensorFlow threads, which
stemcache starts for a Spotify track, next to the analysis) and the sound still does not drop: the audio thread
outranks all of it. The window's 0.2 s there is that engine against the redraw; it is rekordbox's own and was
left alone.

To repeat it: `tools/win/glitch/build.sh`, copy `build/win/rbglitch.exe` and `grun.ps1` to `C:\rb`, a desktop
task `rbglitch` running `conhost --headless C:\rb\rbglitch.exe` (with its own console window the tool takes the
keys meant for rekordbox), then from the Mac `setup.sh <task>` (starts rekordbox, plays the demo track on deck
1, opens the playlist), `case.sh <name> <title>` per load, `thr.py <run> <from s> <to s>` for the threads.
Results land in `~/rbwin/glitch`. Row numbers of a Spotify playlist are no use for picking a track: the list
loads in pieces and re-sorts.

## Status (rekordbox 7.2.19, table 6A97B2E4-6573000)

| Mod | Windows |
|---|---|
| analyzer, artcache, betterkey, fastwave, flx4, framerate, keepmaster, parallel, spotifylog, streamio, tfpool | on, tested on an x64 PC. streamio's phrase passes read the kept samples there too: the reader is rewound through its pcm interface (+0x250, slot 0x28: `win:` fields), byte-identical ANLZ files, phrase stage 0.2-0.3 s against 0.7-0.9 s. (7.2.18/7.2.17 have the interface at +0x270: the generated table finds it.) analyzer's fast mode and forced key re-detection use Windows table fields read from CBeatAnalyzeThread::analyze() (tested on the PC). Since 2026-11-17 also: "Re-analyze track(s)" in the track list's menu (list_showMenu/list_popupShow, getSelectedRowDataTracks 0x1414af380), rekordbox's Analysis Setting dialog skipped during runs (both AnalysisSettingWindow::show calls in analyzeCollectionLoad; tested on the PC), and Spotify's streaming limit from the SP_LOG call (MSVC moved both mac call sites into one helper, 0x14118e780, whose SP_LOG copy takes (?, ?, String*, ?, int): its own hook signature; Spotify.log is read only without the hook). flx4's tempo pickup (DjEngineIF::getTempo 0x14253bd50; the tempo range, UiPlayer +0x50c, inlined getter) and rekordbox's own deck numbers in blue (Label's Font +0x210, Font copy/getHeight; optional: a build without them leaves the numbers alone) since 2026-11-17: built, not yet tried with an FLX4 on the PC. Since 2026-11-19 a deck a side switches to keeps its tempo there too (`flx4_keep_tempo`, `keepTempo` after `UiPlayer::initializeDevice`): the tempo fader position rekordbox remembers for a deck is UiPlayer +0x5d4 on Windows (mac +0x604; tools/win/verify/flx4ui.out.json, the same in 7.2.19 and 7.2.20), a required field. Run on the PC without a controller (it has no MIDI device, so `flx4_inject` can't reach `switchDeck`): a test-only build ran the same two calls for a loaded deck that had followed BEAT SYNC to -6.25 % and was the master, its remembered position at +2.50 %: without the fix rekordbox set +2.50 % a message later, with it the deck stayed at -6.25 % ("flx4: deck 1 keeps its tempo"). Not yet tried with an FLX4 on the PC. The playlist the analyzer reopens at launch: rekordbox's own first selection (the collection tree's) comes after it and left the Spotify playlist as a name over an empty list on a spinner (every launch on the PC, both rekordbox versions); the analyzer opens it again while the list is empty and nothing else was selected (4 tries, 6 s apart; tested on 7.2.20 and 7.2.19) |
| betterstems, beat/key engines | on, tested: ONNX Runtime (rekordbox's own onnxruntime.dll). betterstems and bettergrid run on the GPU through DirectML, which that dll is built with (it exports `OrtSessionOptionsAppendExecutionProvider_DML`; rekordbox ships DirectML.dll); CPU if that fails or with `gpu=0`; a GPU run that fails later (driver reset, out of memory) moves that session to the CPU and is retried (tested with a faked failure). On the PC (RX 580): an HTDemucs segment 0.44 s against 2.4 s, Beat This! 4.5-5 s a track against 14-16 s, same grids. betterkey's CNN is our own C++ (CPU). Since 2026-11-19 HTDemucs is used only on a GPU that is fast enough, and a slow Beat This! isn't waited for on a deck (see "Speed" above; `betterstems_slow`, `bettergrid_slow`) |
| stemcache | on, tested: GetProcAddress import-call hooks; lossless chunks (LZFSE); AAC chunks through Media Foundation (max 192 kbps, encoder delay measured at run time): tested on the PC both ways with Mac chunks (tools/dev/codectest.cpp) |
| bettergrid | on, tested: hooks AnalyzeJobController::GetResult (the grid copy after it is inlined). The collection's BPM following the saved grid (2026-11-18) is macOS only: `rekordboxDBController::modTrackBpm` and `DatabaseMediator::mainDBController` (for bettergrid) aren't in the table |
| keycolumn (columns) | on: the 4 inlined getColumnIndexByID copies skip unknown ids, so they need no hook, and our column isn't in rekordbox's saved layout: its place (left neighbour, width, shown) is kept in the config as `column<id>=` instead. Sorting passes MSVC's single-inheritance member function pointer (just the address). Tested on the PC (values, sorting both ways, place kept) |
| timing | on, tested (the analyzer's row lines show the stages): the seek-table marker sits on the inlined createSeekTable's builder call |
| spotifyapi | on, tested (a 50-track playlist fills in seconds; plain rekordbox shows a spinner for over 20 s): no hooks for the exception-unwind exits (see spotifyapi.cpp) |
| spotifyfix | on, tested: hooks the token request and runs rekordbox's (inlined) callback on another thread |
| spotifystart | on, 7.2.19 x64 only (build and code bytes checked; raw patch, not in hooks.def). The Client's 1-thread job pool could deadlock: a restart job (body 0x2fa5840: shutdownClient, createClient, clear queue) waits for `Client+0x81` (SDK starting) to clear while the SDK needs websocket jobs on that pool. The job is skipped while the flag is set. On the PC: 8 of 8 restarts ready in ~2 s, plain 7.2.19 stalled 2 of 4. A second pool thread also cured the stall but crashed in the SDK: don't. |
<!--@private win-row-a-->
| badge | on, tested: the chip is a child window on rekordbox's main window (GDI+), the about window HTML |
<!--@private win-row-b-->
| spotifydl | on (2026-11-13), not yet run: its hook is the address of `HttpsHalHelper::getCallback` taken in the Client constructor (`lea rax,[rip+fn]`, table hook kind `lea`: the patcher changes the displacement) |
| spotifyfix's logout guard | on (2026-11-13), not yet run: `spotify::Service::requestLogOut` (vtable slot 0xb0) and `spotify::DownloadConnectionJob::run` (slot 0x8), with spotifydl's hook as the gate |
| quickdeck | ported 2026-11-13 (tools/win/verify/quickdeck.out.json): MSVC inlined notifyColorZoom/notify3BandsZoom into analyze(), so the mod calls the listener's slots itself (`win:` fields 0x50/0x70); getWaveNormalizeCoef returns its 6-byte struct through a pointer; the callee destroys the by-value BigInteger of handleDBMessageTrackStatusWhenAnalyzeCompleted. Tested on the PC: the deck shows the waveforms early. Not seen there yet: the Beat This! early grid (Beat This! finished after rekordbox's beat job) and the reload at the phrase stage (Windows deck loads show no stage timing) |
| multisort | ported 2026-11-13 (tools/win/verify/multisort.out.json): ListViewSorter's chain holds plain 8-byte code addresses from +0x10 (mac: 16-byte Itanium member pointers), direction at +0x60; the table's O_sorterChain is the chain start on Windows. The ordinary columns' setSortColumnId call is the 3rd by address on Windows (2nd on mac). Tested on the PC (Key then BPM). The other sorted columns drawn blue with their arrow (2026-11-17): MSVC has no thunk, `drawTableHeaderColumn` (0x1416047d0) is itself slot 2 of the LookAndFeelMethods vtable at +152 in both browse look-and-feels, found by scanning from the primary vtable; tested on the PC (Key then BPM) |
| betterfx | not ported (betterfx's recipes and hooks are arm64-specific). tfpool's warm-up and stemcache's status-bar message are mac-only too (`windows-optional` hooks) |
| autocue, genres | on, tested on the PC: autocue wrote 220 cues there (2026-11-13), genres' style model runs on the GPU through DirectML (a demo track in 1.0 s, `detect <file>` in command-genres; 2026-11-17). autocue's Claude Code option (`plat::run` in plat_win.cpp, finding claude.exe / claude.cmd) compiles but has not run on Windows yet |
| tidymenu | on (2026-11-19), tested on the PC in 7.2.19 and 7.2.20 (tools/win/verify/tidymenu.out.json): the same juce functions as on mac (`PopupMenu::addItem(Item)` 0x142bb0700, `addSubMenu(String, PopupMenu, bool)` 0x142baef90, `Item`'s copy constructor 0x142bb0810 and destructor 0x140fba300, `~PopupMenu` 0x142bb0be0). MSVC's `PopupMenu::Item` is 0x90 bytes (its `std::function` is 0x40), `isEnabled` +0x84, `isSeparator` +0x86 (mac 0x70 / +0x64 / +0x66); a by-value Item, String or PopupMenu is destroyed by the function called, so the mod doesn't destroy them after `addItem` / `addSubMenu` there. The scanner finds all of it in 7.2.20 (unchanged code). Run with `tidymenu_log=1` on a Spotify track: 13 of 23 items shown, the *platendoos* submenu with the six window items; its *Genres window* (7.2.19) and *Stems window* (7.2.20) opened on the one track, *Re-analyze track* re-analysed it, rekordbox's own items from the copy work (*Laden* > *Deck1* loaded the track); `tidymenu_group=0` gives the flat long items again |
| xfadelock | on (2026-11-20), built and checked offline, not yet run on the PC (tools/win/verify/xfadelock.out.json); its emulator test (`tools/win/emu`: the slot search, the fields, lock and pickup) passes on 7.2.19 and 7.2.20. MSVC gives the three methods the mod swaps no slot in the primary vtable: each overrides a virtual of a secondary base and sits only in that base's vtable, taking that subobject as `this` (mac's `__ZThn<N>_` thunks). `DjMixerUnit::setCrossFaderPosition` 0x142da6770 is slot 13 of the mixer control interface's vtable (subobject 344; mac 384), `CrossFaderComp::eventAbsoluteValueChanged` 0x14186bd40 slot 3 of the device listener's (256), `sliderValueChanged` 0x14186b580 slot 1 of the Slider::Listener's (240); `handleAsyncUpdate` 0x14186bf30 (504; mac 552) names the field recipes, `DjEngineIF::setCrossFader` 0x14253ec50. The mod finds each slot at run time: the method's address in rekordbox's `.rdata`, taken only in the vtable whose RTTI locator (the word before the vtable) names that class and the subobject offset the table's `adj` line gives (each address is in exactly one slot in 7.2.19 and 7.2.20). Fields of the whole object: the crossfader's DeviceComponent +0x298 (mac 0x2c8), its Slider +0x230 (0x260), the position it last set +0x2d8 (0x308). The button is a child window of rekordbox's main window drawn with GDI+ (mods/xfadelock_win.cpp). The scanner finds all of it in 7.2.20 (the same code and offsets) |
| fontcache, midipoll, screeninfo | mac-only: fixes for macOS APIs (Core Text, CoreMIDI, NSScreen) |
| outputsync | mac-only for now (Core Audio device latencies) |

## Status (rekordbox 7.2.20, table 6AFC1795-66AA000)

Tested on the x64 PC on 2026-11-19 from the unpacked installer in `C:\rb\rb720` (not installed), next to the
installed 7.2.19, with a test install in `C:\rb\platendoos720`.

- With only what the scanner finds (an unknown build, 14049 of 14067 parts): 24 of the 27 mods on, 85 of 135
  hooks. Off: spotifyapi, spotifyfix, stemcache, because 7.2.20's code changed at their hooks.
- With the shipped table (`tools/win/tables/rekordbox-6AFC1795-66AA000.tbl` = the scan + 11 hand-checked lines
  from `tools/win/verify/rekordbox-6AFC1795-66AA000.add`): all 27 mods on, 104 of 135 hooks, 360 addresses, 109 fields, the same as 7.2.19.
<!--@public win-720-run-->
  Run there: a Spotify playlist re-analysed (bettergrid, betterkey, genres, streamio), deck loads of Spotify
  tracks with HTDemucs stems (stemcache's `stem_sep4` hit), multisort (Key
<!--@/public-->
  then BPM, blue header), a download through spotifydl's hook, the client token handed over by spotifyfix, and
  34 playlist requests through spotifyapi's lock hooks on six threads (counted with cdb breakpoints on the sites).
- Not seen executing on 7.2.20 (checked by disassembly only): spotifyapi's lookup pair (`sp_lookup_enter/exit`:
  nothing tried made rekordbox ask for tracks by id), spotifyfix's second token site and its logout guard (no
  logout was made; unlike 7.2.19, 7.2.20 doesn't call `requestLogOut` at start).
- What changed in 7.2.20: `Service::getPlaylistTracks` / `getLookupTracks` rewritten (the 401 handling logs now),
  `AudioPartSeparator::start` split up, `requestLogOut` logs, the Client's fields moved by 0x40-0x60 (its callback
  table from +0x88 to +0xc8), and a newer Spotify library (1.37.0, was 1.26.1) with new `[SpotifyLifecycle]`
  log lines.
- The build of 2026-11-19 afternoon (core's `startupTask`, the beat-stage hold-back of the stems, "Genre first", the
  rules under "Speed") ran there and on the installed 7.2.19: 27 mods, 104 of 135 hooks; a track analysed from the
<!--@public win-analysis-run-->
  Analysis window (18.9 s for a never-loaded 7.8-minute track: stream wait 3.4 s, audio pass 8.7 s, beat job 7.2 s,
<!--@/public-->
  all of it Beat This!); "Genre first" filled the empty Genre field 8-12 s after the analysis for 12 of 13 tracks (the
  model was unsure about one); "betterstems: waited 4.2 s for an analysis' beat stage" when a deck load and its
  analysis overlap.
- 2026-11-19 evening: the table has one field more (110; flx4's remembered tempo fader position, UiPlayer +0x5d4,
  which the scanner finds in 7.2.20 by itself: 14050 of 14068 parts). The 7.2.20 copy was not started with it: the
  field was checked by disassembly there (the same code as 7.2.19, where it ran).
<!--@public win-night-crash-->
- 2026-11-19 night: a Spotify download started from one of the mods' own threads crashed 7.2.20
<!--@/public-->
  (reproduced on the PC: access violation reading 0x1ed at rekordbox.exe+0x2fee7a1). 7.2.20's
  `DownloadManager::syncDownload` (0x142fee380; 7.2.19's 0x142f34570 has no such loop) waits up to 30 s
  (300 x `Sleep(100)`) calling `getCurrentThread()` and reading its stop flag (`Thread+0x1ed` on Windows, mac
  +0x1ad) without a null check: null on the mods' own threads. Fixed as on the Mac by core/jucethread.cpp's
  `JuceThreadScope` (pseudo-mod "jucethread"; 4 lines in the .add file: `getCurrentThread` 0x142b30a50,
  `getCurrentThreadHolder` 0x142b30e40 with its result through rcx, the name String at `Thread+0x8`, the flag at
  +0x1ed). Windows inlines `getCurrentThreadId`: the code calls `GetCurrentThreadId()` (zero-extended, as
<!--@public win-night-run-->
  `ThreadLocalValue::get` 0x140f162e0 does). Run on the PC: 19 Spotify downloads from the mods' threads, one after the other,
<!--@/public-->
  each, no crash, "rbmods: the mods' threads stand in for juce threads where rekordbox needs one"; 374 addresses,
  112 fields, the same 27 mods. Its four names are verified in 7.2.19 too since the same night (tools/win/verify/tidymenu.out.json: the same code), so jucethread is on there as well and the scanner finds them in 7.2.20 (the .add lines went); all mods load.
- 2026-11-19 late: tidymenu ported (see its row above). Both builds then: 28 mods (the 27 + tidymenu), 104 hooks in
  place, 378 addresses, 115 fields; the 7.2.20 table is the scan (14064 of 14082 parts) + 11 hand-checked lines.
- 2026-11-20: xfadelock ported (see its row above): 5 addresses, 3 fields and 4 `adj` lines more in both editions' 7.2.19 tables; the scanner finds all 12 in 7.2.20 (14072 of 14090 parts, full edition), no hand-checked lines needed.
- spotifystart (the 7.2.19 start-up stall) does nothing on 7.2.20 ("not this rekordbox build, nothing patched"),
  and isn't needed: 4 plain and 9 modded starts of 7.2.20 all had "SDK Initialization successful" about 0.3 s
  after `createClient`, no "timed out waiting for client initialization" (plain 7.2.19 stalled 2 of 4).

Where Windows inlined a mac hook's target, the Windows site is a different call with its own hook signature
(`#ifdef _WIN32` in the mod), recorded in tools/win/verify/win2.out.json ("why"). A table may list several
`hook` lines for one name (the same call in each inlined copy). A hook a mod can do without on Windows is named
in a `// windows-optional: ...` comment in its source (tools/win/hookmods.py), so the mod stays on.
