# Intel Macs

rekordbox for macOS is a universal app: one file with an Apple Silicon (arm64) half and an Intel (x86_64) half.
platendoos was made for the arm64 half only. This page is the plan and the state of making it work in the Intel
half too, and what was measured to get there (2026-11-23, rekordbox 7.2.19 and 7.2.20).

**State: beta since 0.5.0 (released through the same patcher), tested only under Rosetta on an Apple Silicon
Mac, not yet on a real Intel Mac.** The patcher says so on an Intel Mac. See [Status](#status) at the end; keep
it current.

## What an Intel Mac runs

- The same `rekordbox` file: its x86_64 slice. Both slices have the full symbol table, so everything is still found
  by name. The engines rekordbox ships (`libSpleeter`, `libDemucs3`, TensorFlow, ONNX Runtime) are universal too.
- Intel Macs stop at macOS 26. rekordbox 7.2.20 asks for macOS 13 or later and a CPU with AVX2.
- Everything we build has to be universal: `librbmods.dylib`, the patcher app. A patched x86_64 slice with an
  arm64-only dylib doesn't start on Intel.

## Testing without an Intel Mac

Rosetta 2 runs rekordbox's x86_64 slice on an Apple Silicon Mac, and that is how this is developed:

    ./build.sh ~/rbwin/dl720/mac/rekordbox.pkg/Payload/rekordbox.app ~/Applications/platendoos-intel.app
    ROSETTA_ADVERTISE_AVX=1 arch -x86_64 ~/Applications/platendoos-intel.app/Contents/MacOS/rekordbox

(`open` on the same app runs its arm64 half: the regression check.) The mods' log starts with "x86_64: field
offsets read from the arm64 half of ..." when the Intel half runs. `tools/dev/hotload.sh` builds its experiment
for whichever half is running.

- Pages are 4 KB there, as on Intel (16 KB on Apple Silicon).
- Rosetta doesn't advertise AVX/AVX2 unless `ROSETTA_ADVERTISE_AVX=1` is set; rekordbox has AVX-gated code.
- The first start after the executable changed takes ~50 s: Rosetta translates rekordbox and TensorFlow.
- Core ML takes its Intel path under Rosetta (same results as arm64 within float precision), but on the Apple
  GPU. Speed says nothing about a real Intel Mac.
- macOS 27 is the last macOS with Rosetta.

What only a real Intel Mac can tell: Core ML speed and correctness on Intel and AMD GPUs, audio dropouts, the
microphone prompt, macOS 13 to 15.

## The patcher: hooks in the x86_64 slice

`patch_binary.py` and `patcher/macho.h` patch each slice the app has. The arm64 side is unchanged.

**Sites.** A hook is still "the nth of count calls to target inside caller" (hooks.def). In x86_64 code:

| kind | arm64 | x86_64 |
|---|---|---|
| -1 | `bl target` | `E8 rel32` (call) |
| -2 | `b target` (tail call) | `E9 rel32` (jmp) |
| n ≥ 0 | `adr xn, target; nop` | `lea reg, [rip+target]` (`48`/`4C 8D /r`, any register; only the rel32 changes) |

A plain byte scan of the caller for `E8`/`E9` + the exact rel32 finds the same sites as a disassembler (checked
on all 154 lines, both builds), so the patchers need no x86 decoder. `stub:_x` targets are the slice's `__stubs`
entries (indirect symbol table), as on arm64.

**nth is per architecture.** The compiler lays the same calls out in a different order in the two slices, and a
caller can have the same number of calls in another order. 135 of 147 hooks have the same count in x86_64, but
some of those would hit the wrong call with arm64's nth (`spotify_rev_item1`: 5 not 4, `spotify_rev_item2`: 2 not
5, `bfx_rollWrap`: 0 not 1, and `an_yield0`-`5`, the same six calls with the first one last), and 12 need another
target, count or kind. So hooks.def has lines for x86_64 only:

    HOOK_X86(name, caller, nth, count, target, kind)

A hook's x86_64 sites are its `HOOK_X86` lines if it has any, else its `HOOK` / `HOOK_OR` lines. Like `HOOK_OR`
it is no hook of its own (no index) and code that includes hooks.def doesn't see it unless it asks. A hook whose
x86_64 site doesn't exist gets `HOOK_X86(name, "", 0, 0, "", 0)`: never patched there. Today: nine hooks
re-ordered or re-counted (the above and `fastwave_check` / `fastwave_retry`), audiomem's eight on other targets
(below), one never patched (`screen_desc`); the other 129 use their arm64 line as it is (`sp_https_get` too: on x86_64 a kind ≥ 0
site is a `lea` into any register).

**The order check.** Because a wrong nth is silent, `tools/dev/hookorder.py <binary>` compares, for every hook,
the calls around the site in both slices of the same file (it pairs a caller's calls to the target one to one,
by the calls before and after them) and reports any hook whose x86_64 site isn't the same call as the arm64 one.
Run it for every new rekordbox version (docs/rekordbox-updates.md). It is a heuristic: a caller with a single
call to the target is the same call by definition.

**Stub** (16 bytes each, in the zero padding before `__text`, as on arm64; 7.2.20's x86_64 slice has 6432
bytes of padding):

    41 BB ii ii ii ii     mov  r11d, <hook index>
    FF 25 dd dd dd dd     jmp  qword ptr [rip + slot]     ; dd = slot - (stub + 12)
    CC CC CC CC           int3

r11 is free at every call site: no function in either build reads r11 before writing it. `al` (variadic calls),
the argument registers, rsp, rbp and the callee-saved registers are untouched.

**Slot.** 8 bytes of zero padding between `__DATA`'s sections (the gap before `__thread_data`, as on arm64);
if a build has none, the unused tail of `__DATA` after `__common`.

**Marker** (x86_64 slice; in front of the stubs, 16-aligned):

    "RBMHOOKS", slot address (8), hook count (4), 1 (4), one byte per hook (1 = patched), 0-3 bytes of padding
    to a multiple of 4, then per hook the original target as a 32-bit offset from __TEXT's address (0 = not patched)

The 1 (the arm64 marker has 0 there) says the table of originals follows. With it the dylib doesn't have to scan
`__text` for the patched sites to run rekordbox stock (a byte scan of x86 code could mistake data for a call).

**Load command and signing.** `LC_LOAD_DYLIB @executable_path/librbmods.dylib` is appended to the x86_64 header
the same way. The x86_64 slices use `LC_DYLD_INFO_ONLY` (lazily bound stubs), which changes nothing for us.
`codesign` re-signs both slices in one go; the arm64 code hash doesn't change when only the x86_64 slice does.

## The dylib

**Dispatch** (`core/hooks_mac.mm`): the stub's index in r11, r10 as scratch.

    _rbm_dispatch:        leaq _rbm_hook_table(%rip), %r10 ; jmpq *(%r10,%r11,8)
    _rbm_hook_default:    r10 = *rbm_hook_origs_ptr[r11] + g_slide ; jmpq *%r10
    _rbm_dispatch_stock:  movq _rbm_stock_targets(%rip), %r10 ; jmpq *(%r10,%r11,8)

**A hook's original** (`RBM_ORIG`, `kHookOrig`) is the target of the line that was patched: on x86_64 the
`HOOK_X86` target where there is one (`stub:___bzero` instead of `stub:_bzero`, for instance).

**Finding things** (`core/image_mac.mm`): the symbol table is read the same way. What reads arm64 instructions
needs an x86_64 answer:

- `callSite`: byte scan for `E8 rel32`.
- `singleton`: all eight juce::SingletonHolders have a symbol of their own in both slices
  (`__ZN2db10DatabaseIF15singletonHolderE` ...): use the symbol on both architectures.
- `adrpAdd` / `adrpAddBefore`: `lea reg, [rip+x]` before the site (`A_mediaNames` is the symbol
  `__ZL15aStrButtonNames` in x86_64; flx4's `A_vinylId` has no symbol).
- The build checks: the x86_64 slices have their own LC_UUIDs (7.2.19 `08B8BB79-9BBF-3D63-B6D6-9B1C5A362F0B`,
  7.2.20 `F5BE9C7D-3285-3E11-A3EB-F4B4FD401904`).

**Field offsets** (`rbm::Offset`, 141 instruction recipes + 35 `vt:` vtable recipes; the 13 `win:` ones are
Windows').

- `vt:` recipes read rekordbox's vtables, which have the same slot order in both slices: unchanged.
- Instruction recipes name arm64 instructions. Struct layouts are *not* the same in both slices
  (`std::function` is 48 bytes aligned to 16 on x86_64, 32/8 on arm64; `rbxfrm::Float2` is 16 bytes, 8 on arm64;
  `long double` is 16), so every class holding one shifts. Measured on both builds: 133 recipes have the same
  value in both slices, 6 differ, 2 are the same value in a form x86 has no single instruction for.
- So on x86_64 a recipe is resolved against the **arm64 slice of the same file on disk** (the running image's
  own path: the patched file's arm64 slice still has its code), and an Offset whose x86_64 value differs says so
  in its declaration: `Offset O_x{0x64, fn, kind, nth, X86{0x74}}`. The x86_64 value is then the arm64 result
  plus the declared difference (0x74 - 0x64), so a field that moves in an update still follows.
- `tools/dev/recipes.py check <binary> --x86` is the proof per build: for every recipe it looks for the value in
  the x86_64 code of the same function (as a displacement or immediate between the same calls) and lists what it
  can't confirm. Run it for every new rekordbox version. Nothing at run time notices a new difference, which is
  why the check is part of the update routine.

The six that differ (same in 7.2.19 and 7.2.20):

| Offset | arm64 | x86_64 |
|---|---|---|
| flx4 `O_labelFont` | 0x1b0 | 0x1e8 |
| outputsync `O_onClickF`, `O_onValueChangeF` | 0x118 | 0x120 (and the hard-coded `- 0x18` back to the std::function is `- 0x20`) |
| tidymenu `S_item` | 0x70 | 0x80 |
| tidymenu `O_itemEnabled` | 0x64 | 0x74 |
| tidymenu `O_itemSeparator` | 0x66 | 0x76 |

**Hooks written in assembly.** arm64 hooks that pass a caller's register on, or keep x8 (the struct-return
register) intact, are assembly. On x86_64 SysV a struct return is an ordinary first argument (rdi) and comes back
in rax, so most of them become plain C++ hooks:

| hook | arm64 | x86_64 |
|---|---|---|
| `an_poll`, `an_seek` (MARK_HOOK) | thread = x0 | C hook, thread = first argument |
| `an_phrase`, `an_embed` (MARK_HOOK) | thread = x20 | C hook; no register holds the thread |
| `key_specialInfo`, `beat_specialInfo` | x20 → argument 3 | C hook; no register |
| `an_begin` | x20 → argument 1 | C hook; no register to rely on |
| `grid_assign` | x20 → argument 2 | C hook; no register |
| `tf_madmom` | keeps x0-x8 | C hook: result pointer is argument 1 |
| `history_add` | x19 → argument 2 | two instructions: the manager is in **rbx** |
| `ring_zero0-2` (audiomem) | x19 | r12 in the constructors, r14 in `getMemory` |

Where arm64 took the analysis thread from x20, x86_64's `CBeatAnalyzeThread::analyze()` keeps it in a stack slot.
`analyze()` runs on that thread itself (`run()` tail-calls it), so the hooks ask
`juce::Thread::getCurrentThread()` instead.

**Other arm64-only spots:** multisort's member-function-pointer decoding (x86_64: `adj` unshifted, the virtual
flag in `ptr & 1`), flx4's read of a `__ZThn656_` thunk's instruction (x86_64: `add rdi, -0x290`), a Float2 audio
`stride = 2` (Float2: 4 on x86_64, as on Windows), audiomem's `kPage = 16384` (now the real page size), and
outputsync's juce objects, which it allocates with sizes written into the code: they are bigger in x86_64
(ToggleButton 0x220 for 0x1f8, Label 0x260 for 0x220, Slider 0x200 for 0x1a8, as every `operator new` before those
constructors in rekordbox says) and its way back from a `std::function`'s callable to the function is 0x20, not
0x18.

**In shared files** test `RBM_MAC_X86` (rbmod.h), not `__x86_64__`: the Windows build defines that too.

**Fine as they are:** the C++-typed hooks and calls. Class return types make the compiler put the result pointer
where each ABI wants it; non-trivial by-value arguments are pointers in both; every symbol the mods name exists
in the x86_64 slice except `_objc_msgSend$deviceDescription` (screeninfo).

## Models

The Core ML models are architecture-independent: one copy serves both slices. As x86_64 they load and predict on
every compute-unit setting, with results equal to arm64 (checked under Rosetta). Open for real hardware:

- Speed. Core ML's CPU path was 2.5-3x slower than ONNX Runtime's on the same (translated) CPU, and a 6-core
  Intel desktop CPU needs 2.4 s per HTDemucs segment with ONNX Runtime. Integrated Intel graphics are likely no
  better. So the x86_64 half judges speed as Windows does:
  - **Beat This!** (`engines/beatthis_mac.mm`): on the GPU one chunk of silence is timed when the model loads
    (`firstChunkTime`, shared with the ONNX Runtime backend; twice when the first is slow), on the CPU (`gpu=0`) it
    is slow by definition. Slow (over `kSlowChunk`, 1 s) means a track on a deck keeps rekordbox's grid at once
    (`bettergrid_slow=1`: wait anyway); other analyses wait up to `bettergrid_wait`, 60 s here as on Windows. No
    warm-up run at launch: the timing run is one.
  - **HTDemucs** (`mods/betterstems.cpp`, `fastEnough`): one segment of a test signal on the GPU at launch (twice
    when the first is slow), over `kSlowSegment` (1 s) rekordbox's engine separates; with `gpu=0` it doesn't load
    at all (`betterstems_slow=1`: HTDemucs anyway, on the CPU).
  - **Stems that are noise** (Windows and Intel Macs): every segment's stems are checked (finite, no source over
    twice the mix's level, the sources adding up to the mix within half its level, with a -40 dB floor); bad stems
    switch HTDemucs off for the session and rekordbox's engine separates.
  - Under Rosetta on this M-series Mac (its GPU): Beat This! 0.70 s a chunk, HTDemucs 1.00 s a segment, so both
    kept; `gpu=0`: Beat This! on the CPU, slow (the deck rule fired, and other tracks went past 20 s), HTDemucs
    not loaded.
- HTDemucs is exported for macOS 14; on 13 it doesn't load and rekordbox's engine separates.
- `(float)_Float16` conversions are a library call on x86_64 unless built with `-mf16c` (every CPU rekordbox 7
  accepts has F16C).
- Denormals: x86 is slow on them, but betterfx needs no guard: rekordbox's audio device callback and every
  juce::Thread set flush-to-zero (MXCSR FTZ) before any effect runs, as they do on arm64 (FPCR FZ);
  `tools/dev/betterfx_test.cpp` checks the filters' and the reverb's tails under FTZ.

## audiomem, screeninfo, betterfx

Off in the first round, on since the second:

- **audiomem**: its eight sites sit on other functions in x86_64. The zeroing is `___bzero`; RingBuffer's two
  destructors tail-call `delete[]` (`pop rbp; jmp`: kind -2); PageBuffer's chunks come from
  `posix_memalign(&p, 16, n)` and go back with `free` (the loop's call is the first of the destructor's nine, a
  tenth is its tail jump), so `rbm_hook_pagebuf_new` takes posix_memalign's arguments there. The `ring_zero`
  thunks take the RingBuffer from r12 (constructors) or r14 (`getMemory`). `kPage` is the real page size (4096).
  Under Rosetta: 214 FX buffers pinned (654 MB), deck buffers tracked and brought back.
- **screeninfo**: the call has no site of its own (JUCE keeps `objc_msgSend` in r14 and calls it five times), so
  rekordbox's one selector reference for `deviceDescription` (writable `__DATA`) points at `rbm_deviceDescription`,
  a method screeninfo adds to NSScreen. All five callers come there; only the repaint's (by return address) gets
  the cached dictionary, the rest AppKit's own. `screen_desc` stays unpatched in x86_64 (146 of 147 hooks).
- **betterfx**: nothing to port: every offset and layout it writes into its code is the same in both halves (the
  EQ self-check matches rekordbox's filters to 2e-16 under Rosetta), and no denormal guard is needed (above).
  `rbm_hook_bfx_rollWrap` reads the roll ring only once its state is found.

## Building

- `compile.sh` builds `librbmods.dylib` universal (`-arch arm64 -arch x86_64`, `-mmacosx-version-min=13.0`);
  `RBM_ARCHS=arm64` builds one architecture for a quick turn (not for an app whose x86_64 half is patched: that
  half then can't start).
- `tools/build/patcher_mac.sh` builds the patcher universal.
- `tools/dev/hotload.sh` builds for the architecture rekordbox runs as.
- `build.sh` patches both slices.

## For a new rekordbox version

On top of docs/rekordbox-updates.md: `python3 patch_binary.py` must report every hook found in both slices,
`tools/dev/hookorder.py` must be clean, and `tools/dev/recipes.py check <binary> --x86` must confirm every
recipe (or the Offset gets its `X86{}` value).

## Status

2026-11-23, on an Apple Silicon Mac, rekordbox 7.2.19 and 7.2.20:

| part | state |
|---|---|
| design, measurements | done (this page) |
| patcher: x86_64 sites, stubs, marker, `HOOK_X86`, `hookorder.py` | done: 146 of 147 hooks patched (`screen_desc` has no site there), arm64 output byte-identical to before, Python and C++ patchers identical, every site checked by disassembly on 7.2.19 and 7.2.20 |
| dylib: dispatch, image reader, Offsets | done: 444 addresses and 175 fields resolve in the x86_64 half; `recipes.py check --x86` confirms all 176 recipes on both builds |
| hooks in assembly, arm64-only spots in the mods | done, audiomem and screeninfo included (betterfx needed none) |
| universal build | done (`compile.sh`, `build.sh`, the patcher app) |
| run under Rosetta | **works**: the patched x86_64 half starts, the mods load, a Spotify track loads on a deck with waveforms, Beat This! grid, key, genre and HTDemucs stems; a 4-track analysis run, the genres and stems windows and a hot-loaded experiment work; quits cleanly. The same universal app runs natively as before |
| not yet tried under Rosetta | the track menu (tidymenu), multisort clicks, FLX4, outputsync's Preferences row, autocue's window, the patcher app's own install, the public edition, the stock path (app patched from another hooks.def), 7.2.19 |
| slow rules for the Core ML backends | done: timed at load on the GPU, `gpu=0` = slow; tried under Rosetta both ways (Models) |
| audiomem, screeninfo, betterfx on Intel | done, on under Rosetta: every mod loads; a deck load, stems from the cache, play/pause, audiomem `state`. The rebuilt universal app runs natively as before |
| patcher app on Intel, 7.2.19 | done under Rosetta: the patcher's x86_64 half (`--check`, `--install`) on the installed 7.2.19, 147 of 147 hooks; the result runs as Intel with all mods, and the clicks (track menu, crossfader lock, effects, Preferences row, chip window, playlist windows, stems of a selection) were tried by hand |
| quit | a crash at quit under Rosetta (twice): a mod's background thread locked a mutex that exit() had already destroyed. Fixed for every Mac: core's `quitting()` (NSApplicationWillTerminate) parks the mods' own long-running threads first |
| a real Intel Mac | nobody yet: speed, GPUs, dropouts, macOS 13 to 15 are unknown |
