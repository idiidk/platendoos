# audiomem <span class="pd-tag">mac</span>

Keeps the memory rekordbox's audio works with in RAM, so playback doesn't click when the Mac runs short of memory.

<ModInfo name="audiomem" platform="mac" group="Browsing and playing" />

## Why

rekordbox's audio runs in blocks of 128 samples: 2.9 ms each (at its default buffer size). A block that isn't ready in
time is silence: a gap of 2.9 ms or more **on every deck**.

When other apps fill the memory, macOS compresses and swaps out what wasn't used for a few seconds, and rekordbox's
audio then has to wait for it to come back: 1 to 10 ms for a page, a missed block. Measured on a 16 GB Mac with 6 to 8
GB in swap: 9 to 23 gaps a minute with one deck just playing, and more right after a deck load, which allocates and
clears several hundred MB at once. **Plain rekordbox has the same gaps.** What the audio was waiting for:

- **rekordbox's FX history buffers** (nine in ten gaps): 214 buffers of 3 MB that rekordbox writes all the time,
  whether an FX is on or not.
- **The playing deck's own audio**: decoded into memory at the load, read minutes later.
- Rarely used parts of rekordbox's program, now and then.

## What it does

| Part | Option | What it does |
|---|---|---|
| FX buffers | `audiomem_fx` | Pins every FX history buffer that is being written (about 650 MB that rekordbox writes all the time anyway), so macOS can't take it away. |
| Deck audio | `audiomem_decks` | Four times a second, brings back what the next `audiomem_ahead` seconds (20) of every loaded deck need; 4 s on a deck that isn't playing, so pressing play is clean too. |
| Program | `audiomem_code` | Keeps rekordbox's own code and data (90 MB) in memory. |
| Deck loads | `audiomem_lazy` | rekordbox clears the new track's buffers on the main thread at a load, freezing the window for 0.1 to 0.3 s. With this they come from the system already empty, and each page is first touched when its audio is written, off the main thread: about 0.04 s instead of 0.12 s per load. |

Measured with 3 GB of other memory in use on top:

| | Gaps per minute, one deck playing | Gaps per deck load | Longest audio block |
|---|---|---|---|
| Every mod off | 32 | 10 | 37 ms |
| All mods, audiomem off | 30 | 14 | 21 ms |
| All mods, audiomem on (two runs) | 6 and 0 | 1 and 0.1 | 9 and 4 ms |

## When the log warns

When the log says `audiomem: N GB of swap in use`, close other apps before a set. A larger buffer size in
rekordbox's audio settings (256 or 512 instead of 128) gives more margin too: four in five of the waits are shorter
than 5.8 ms. The log also says, once a minute while it happens, how much deck audio had to be brought back.

## What it can't do

- A jump further than the 20 s ahead (a hot cue, needle search) can still meet audio that was swapped out.
- Small, rarely used objects all over rekordbox's memory can still be away when needed.

## Options

| Option | Default | What it does |
|---|---|---|
| `audiomem` | `1` | `0` switches the mod off (restart). |
| `audiomem_ahead` | `20` | Seconds of audio kept ready ahead of a playing deck. |
| `audiomem_fx` | `1` | `0` switches the FX buffer part off (restart). |
| `audiomem_decks` | `1` | `0` switches the deck audio part off (restart). |
| `audiomem_code` | `1` | `0` switches the program part off (restart). |
| `audiomem_lazy` | `1` | `0` switches the deck-load part off (restart). |

## Windows

macOS only for now. On Windows no dropout was found in any run, plain or with the mods: 42 recorded deck loads next
to a playing deck. Windows' shared audio there has a 10 ms period, which absorbs a 5 ms hold-up.

## Related

[Performance and dropouts](/guides/performance) · [Loading a track](/guides/loading-a-track) · [betterfx](./betterfx)
