# uimon: measuring rekordbox's UI

How long does the main thread stand still when something is clicked, where does that time go, and how long does a
repaint take to reach the screen? These are the tools the 2026-11-26 UI audit used (mods relayout, quickframe and
framerate's numbers come from them). Everything runs inside the running platendoos through `tools/dev/hotload.sh`:
no debugger, no Accessibility or screen-recording permission.

```sh
tools/dev/uimon/uimon.sh start             # the stall monitor and the command channels
python3 tools/dev/uimon/bench.py before    # Preferences, a drop-down, repaint-to-frame latency
python3 tools/dev/uimon/bench.py before layout    # + window resize, panel toggles, the deck layout switch
tools/dev/uimon/uimon.sh snap shot1        # /tmp/rbm-uimon/shot1.png: what the window shows
tools/dev/uimon/uimon.sh stop              # before quitting or restarting rekordbox
```

Files and output live in `/tmp/rbm-uimon/`.

| | |
|---|---|
| `mon.mm` | The monitor (its header lists the log lines and commands). A background thread posts a job to the main thread every 5 ms and logs when it waited 10 ms or more (`STALL`), a run-loop observer logs turns of 8 ms or more (`BUSY`), and `-[CAMetalLayer nextDrawable]` is wrapped to time every frame. The command channel drives the UI with synthesized NSEvents and in-process calls. |
| `cmd2.mm` | A second channel (`cmd2.txt`): `rows <n>`, `load <row> <deck>`, `play|pause|unload <deck>`, `state`, `pl <playlist id>`. |
| `snap.mm` | Paints MainComponent with JUCE's `createComponentSnapshot` and writes a PNG. The parts Metal draws itself (waveforms, level meters) stay black. |
| `layer.mm` | Logs or sets the Metal layer's `maximumDrawableCount` and `displaySyncEnabled`. |
| `meas.py` | `run(label, commands, seconds, sample=False)`: sends commands, collects the stalls in that window, optionally takes a `sample` of the process into `samples/`. |
| `lat.py` | Repaints one button a dozen times and reports the time to the next frame's start and to its presented time. |
| `bench.py` | The benchmark. |
| `incl.py`, `callers.py`, `callees.py`, `under.py` | Read a `sample` file: inclusive time per function on the main thread (`python3 incl.py x.sample.txt main 40`), who calls a function, what a function calls, and everything below one function (`python3 under.py x.sample.txt "juce::Component::sendLookAndFeelChange" [words to keep]`). |
| `laf.mm` | Times `sendLookAndFeelChange()` on the main window four times (rekordbox's "refresh everything", which some option clicks trigger). |

## Things to know

- A hot-loaded experiment's `rbm_experiment()` runs **on the main thread**, inside the mods' tick: do the work
  there directly, and never sleep in it (start a `std::thread` for anything that waits; `mon.mm` does).
- `sample` slows the app down: a Preferences open took 687 ms while sampled, 115 ms without. Take stall times from
  runs without it and proportions from the sample. A sample tick is about 1.5 to 1.7 ms, not 1 ms.
- Synthesized events are not HID input: `plat::idleSeconds()` doesn't see them, so the framerate mod thinks nobody is
  there. To measure "in use", add `framerate_idle_after=100000` to the config for the run (it is read live).
- With `displaySyncEnabled` off, a frame's `presentedTime` is when it was handed to the window server, not when it
  was on the glass.
- `unload` goes through the engine (`DjEngineIF::unload`): the deck's title stays on screen. `load` of an uncached
  Spotify track downloads and caches it.
- Clicks are in window points from the top left; the positions in `bench.py` belong to one layout and window size.
- The first run after a launch is slower (cold). Give rekordbox a minute after it starts.
