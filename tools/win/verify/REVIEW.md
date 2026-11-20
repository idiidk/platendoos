# Windows review of platendoos mods (rekordbox 7.2.19)

You review a group of mods (C++ files in the repository, branch `windows`) for running on rekordbox
for Windows, and make the code changes needed so that, on Windows, every mod either works or switches itself off
(never crashes rekordbox, never corrupts data). The mac build must keep working exactly as before.

## How the port works (read first)
- docs/windows.md (architecture), core/rbmod.h (Addr, Offset, RBM_ORIG, deleteObject, dynCast), core/plat.h,
  core/image_win.cpp (the table format, hook thunks, `hookreg`), hooks.def (all hook sites).
- Mods name rekordbox functions by MAC symbols (rbm::Addr) and fields by MAC recipes (rbm::Offset{mac_value, fn,
  kind, nth}). On Windows a per-build table (tools/win/tables/rekordbox-6A97B2E4-6573000.tbl) maps them; anything
  the table lacks switches that mod off. So: a field read through an Offset is safe; a LITERAL offset/size
  (e.g. `+ 0x98`, `S_x = 0x4e0`, `vt[0xb8/8]`) is NOT: it would be wrong on Windows.
- Windows facts found so far: ~/rbwin/verify/*.out.json (verified addresses, hook sites, field values with notes in
  "why"); vtables in ~/rbwin/vt.pkl. Tools: ~/rbwin/scripts/show.py, xref.py (see ~/rbwin/verify/BRIEF.md), capstone
  + pefile on ~/rbwin/x/rekordbox.exe (image base 0x140000000). Mac binary with symbols: ~/rbwin/mac/rekordbox_x86
  (x86_64) and the installed arm64 app "/Applications/rekordbox 7/rekordbox.app/Contents/MacOS/rekordbox".
- `python3 tools/dev/recipes.py find <mac arm64 bin> <fn regex> <offset>` finds a mac recipe for a field;
  `recipes.py check <bin>` checks all recipes. New Offsets must be added the same way the existing ones are
  (mac recipe + expected mac value); their Windows values then come from verification (write them to
  ~/rbwin/verify/r<group>.out.json in the verify format: {"item": name, "kind": "offset", "fn", "kind_recipe",
  "nth", "value": "0x..", "confidence", "why"}), which tools/win/maketable.py turns into table lines.
- We compile Windows with clang-cl against MSVC's STL, so std:: containers inside rekordbox objects have MSVC
  layout automatically, but a libc++-specific assumption (static_asserts, __ internals) must be guarded.
- ABI: MSVC x64. `this` in rcx; a member function returning a class by value takes the hidden return pointer in
  rdx (after this), a free function in rcx; `long` is 4 bytes; vtables: one deleting destructor slot (slot 0, takes
  a flags arg) instead of mac's two; virtual calls must use "vt:" Offset recipes, never literal slots. A hook
  replaces a call: rbm_hook_<name> gets exactly the arguments rekordbox passes at THAT Windows call site; check
  that the hook's C++ signature fits (by the verified site and target in the out.json files and disassembly).
  Some Windows sites pass a secondary-base `this` (e.g. DeviceMidi+0x300). arm64-only code (register
  trampolines, `#if __aarch64__`) needs an x64 equivalent or a clean switch-off on Windows; mac trampolines that
  pass x19/x20 become `hookreg <name> <reg> <arg>` table lines: find which Windows register holds that value at
  the site, and write it as {"item": "<hook>", "kind": "hookreg", "reg": "rbx", "arg": 2, "confidence", "why"}
  into your r<group>.out.json.
- Hooks/functions the verifiers found "inlined" or "absent" on Windows: that hook won't be installed, so the mod
  is off; if a mod could instead work with a different Windows hook, describe it (don't implement new hooks.def
  entries without saying so in your report).

## Rules
- Edit only your group's files (and add Offsets there). Keep mac behaviour identical; `./compile.sh` (mac) and
  `tools/win/build.sh` (Windows) must both still build. Don't run ./install.sh or touch rekordbox.
- Prefer turning literal offsets into Offset recipes over `#ifdef _WIN32` constants. Use `#ifdef _WIN32` only for
  real platform differences.
- If something can't be made right on Windows now, make the mod switch off on Windows with a log line
  (rbm::logf) saying why, rather than guessing.
- Final reply: per file, what you changed, what's still unverified, and the Windows values you found (also in
  ~/rbwin/verify/r<group>.out.json).
