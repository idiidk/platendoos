# Windows emulator tests

Tests the Windows mod code (rbmods.dll) on a Mac, without Windows: a small x86-64 emulator (Python +
[Unicorn](https://www.unicorn-engine.org/)) maps the real rekordbox.exe and the cross-built rbmods.dll, runs the
DLL's C runtime and static initialisers (so `core/image_win.cpp` reads the real address table and every `rbm::Addr`
and `rbm::Offset` resolves as in rekordbox), and lets a test call the mod's own compiled functions against
rekordbox's real code and data.

    python3 -m pip install unicorn pefile capstone     # once (llvm-pdbutil comes with brew's llvm)
    tools/win/build.sh                                 # the DLL under test: build/win/platendoos/rbmods.dll + .pdb
    python3 tools/win/emu/run.py                       # every test, every rekordbox.exe found
    python3 tools/win/emu/run.py xfadelock -v          # one test, each check and the mod's log
    python3 tools/win/emu/run.py --exe path/to/rekordbox.exe

rekordbox.exe: `--exe`, else `$RBEMU_EXE` (paths separated by `:`), else each `rekordbox.exe` up to three folders
deep under `$RBWIN` (default `~/rbwin`) whose build has a table next to the DLL (`rbmods/rekordbox-<build>.tbl`).
A run takes a few seconds per build.

## Files

| | |
|---|---|
| `emu.py` | The emulator: PE mapping, import stubs, TEB/PEB/TLS, Windows x64 calls, Python hooks, memory helpers, fake windows, PDB symbols (via `llvm-pdbutil`, cached in `build/win/emu-cache/`) |
| `msvc.py` | MSVC ABI helpers: RTTI (complete object locators, a class's vtable per subobject, base offsets), fake instances of real classes, `std::string` |
| `rbmods.py` | rekordbox.exe + rbmods.dll, initialised; reads the mods' `Addr` / `Offset` statics |
| `run.py` | The runner: `test_<name>.py` per mod, each run against each rekordbox build |
| `test_xfadelock.py` | xfadelock (the first test: see its docstring for what runs for real and what stands in) |
| `test_stemcache.py` | stemcache's stems on demand: the slot swap on the decks' objects, the three replacements, the wait |
| `juce.py` | JUCE values in rekordbox's Windows layout: juce::String (rekordbox's own constructor), juce::var (its real VariantType tables), juce::Value (a SimpleValueSource with its real vtable) |
| `test_prefs.py` | prefs: the hidden Preferences component read and operated through the mod's settings source and tick, on a fake component tree with the exe's real RTTI and JUCE getters; the click wait (kClickTime); rekordbox's Preferences command opening the window (perform's slot swap) |
| `test_linein.py` | linein: the processBlock slot swap, manage() on fake engine objects (Windows offsets), the resampler through the swapped slot, and the whole WASAPI half (its capture thread as an emulated thread) against fake COM objects |

## Writing a test

`test_<mod>.py` defines `run(t)`. `t.r` is an initialised `Rbmods` (`t.r.emu`, `t.r.exe`, `t.r.dll`);
`t.check(cond, what)` / `t.eq(got, want, what)` / `t.near(...)` record results.

    S = lambda n: t.r.sym(n, "mymod.cpp")            # a symbol of the mod's object file (PDB)
    A = lambda n: t.r.exe.base + t.r.table_value("addr", n)   # a rekordbox address, straight from the table
    e = t.r.emu
    e.hook(A("__ZNK4juce6Slider8getValueEv"), lambda e: 0.5)  # rekordbox function -> Python (return: rax/xmm0)
    obj = msvc.fake_instance(e, t.r.exe, A("__ZTI14CrossFaderComp"), 0x400)  # real vtables at their offsets
    e.call(S("init"))                                 # the mod's init, as compiled
    with e.trace(obj, 0x400) as tr:                   # which fields the code reads and writes
        e.call(msvc.vcall_target(e, obj + 256, 3), obj + 256, arg, 42)
    t.eq(tr.read_offsets(), [0x298], "reads the device field")

- Arguments: ints / pointers go to rcx, rdx, r8, r9, then the stack; `F32(x)` / `F64(x)` (or a Python float) to
  xmm0-3 by position. `ret="f32"` / `"f64"` reads xmm0.
- In a hook, `e.arg(i)` / `e.farg(i)` / `e.farg32(i)` are the call's arguments; a hook may `e.call` back into
  emulated code (rekordbox's listeners calling the mod, the mod calling rekordbox).
- `e.py_func(fn)` makes a function pointer for a fake vtable or callback; `e.vtable([...])` a whole fake vtable.
- `msvc.type_descriptor(exe, ".?AVName@")` finds a class's RTTI by its decorated name (a trailing single `@`: a class
  in an anonymous namespace), `msvc.class_names` lists its bases; `t.r.dev_files()` stands in for a mod's test-command
  files (`t.r.commands[mod]` lines for `devCommand`, `t.r.files_written` for `plat::writeFileAtomic`).
- `e.windows` holds fake windows (title, screen rect, parent, visible) for `EnumWindows`, `CreateWindowExW`,
  `SetWindowPos` and friends; `e.send_message(hwnd, msg)` calls a window's real window procedure.

## What runs and what stands in

Runs for real: rekordbox.exe's code and data at its preferred base (RTTI, vtables, any function a test calls), the
DLL's code including its static CRT (malloc, the STL, `__RTDynamicCast`), the table loader, the mods' static
initialisers, and whatever mod functions a test calls.

Stands in: Win32 (kernel32 heap/TLS/locks/module queries and a handful of user32 window calls, in `emu.py`); any
other import records the call and returns 0 (`Emu(unknown="trap")` raises instead; `run.py -v` lists them).
`CreateThread` raises (no threads) unless the test calls `emu.enable_threads()`: then threads are emulated
cooperatively (each on its own stack and TLS slots; one runs at a time, until it waits for an event or ends): the
main thread's `WaitForSingleObject` / `WaitForMultipleObjects` run them until what it waits for is signalled, and
`emu.run_threads()` / `emu.set_event(h)` let a test run them at a point it chooses (`CreateEventW`, `SetEvent`,
`ResumeThread`, `ExitThread`, `GetExitCodeThread` work then; a wait's timeout never elapses by itself:
`run_threads(timeouts=True)`). A C++ throw (`RaiseException`: no unwinding), with the C++ type and
a stack. `rbm::logf` is captured in Python (`t.r.log`) and `rbm::plat::readFile` reads the table from the host.
DllMain is not run (no hooks.def call sites are patched, no mod threads start): a test calls the mod's `init` and
its functions directly. Drawing (GDI+), WebView2 and JUCE's message loop don't run; rekordbox functions that would
need a live app (JUCE components, the engine) are Python stand-ins in the test.

## Pitfalls

- A `static` (internal) function may not have the signature in the source: clang can drop or replace arguments
  (xfadelock's `vtableSlot` takes the `Addr`'s value instead of the `Addr&`). Check its disassembly before calling
  it directly; the mod's public paths (`init`, the vtable slots it patched) are safe.
- Unicorn bakes hooks into translated code: `emu.hook` and `emu.trace` flush the translation cache for that, so
  add hooks through them, not `emu.uc.hook_add`.
- The Python found by `python3` depends on the folder (pyenv): install unicorn for the one that runs the tests.
