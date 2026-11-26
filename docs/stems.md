# Stems: plan

rekordbox 7.2.19 separates a track into vocal, inst, bass and drums ("STEMS", *Preferences > Extensions*). This is
what we found about how it does that, and what platendoos does or will do about it.

## How rekordbox does it

- `rbxfrm::audiopart`: `AudioPart` (settings, model choice), `AudioPartSeparatorManager`, one `AudioPartSeparator`
  (a `juce::Thread`) per track, `AudioPartDataManager` / `AudioPartData` (the separated audio, in memory or in
  `rekordbox6/.cache/.spleeter|.demucs3/<name>_{other,vocal,drum,bass}.raw`).
- The engine is a plugin: `AudioPartLibrary::load` dlopens `Frameworks/libSpleeter.dylib` or `libDemucs3.dylib` and
  looks up 8 C functions (`<prefix>Initialize`, `Finalize`, `IsInitialized`, `PreOperation`, `PostOperation`,
  `CancelOperation`, `OperationAtIndexOtherWithBass`, `OperationAtIndexSeparatedBass`).
- `MODEL`: 0 Spleeter 3 stems, 1 Demucs3 3 stems, 2 Spleeter 4 stems (the default), 3 Demucs3 4 stems.
  *STEMS Analysis Process* (setting `PartAnalysisQuality`, 0 = quality) picks Demucs3.
- `OperationAtIndex(handle, chunk, inL, inR, out0L, out0R, ... out3L, out3R)`: planar float, 44.1 kHz. Outputs:
  0 inst ("other"), 1 vocal, 2 drums, 3 bass (3-stem: inst = other + bass; `OtherWithBass` takes no last pair, and
  rekordbox leaves those two argument slots as they were, on both platforms: not NULL, never read them).
  Chunks: Spleeter 519168 samples (11.8 s) + 4096 each side; Demucs3 483052 + 1024 each side.
- Separation starts when a track loads, chunk by chunk from the start, on the CPU. Playback waits for the chunk it
  needs. Spleeter jumps to the playhead (`isRandomAccessible`); Demucs3 goes strictly in order.
- The stems are thrown away ~100 ms after a track leaves the last deck: every load separates it again.

Measured outside rekordbox with its own libraries (M2 Pro): Spleeter with 5 threads ~6 s per 5-minute track,
Demucs3 ~33 s (threads don't help), ~1.8 GB. Quality (published SDR): Spleeter ~5.8 dB, Hybrid Demucs v3 ~7.7-8,
HTDemucs v4 ~9, RoFormer models ~10-12.

## Plan

1. **Quality setting** (no code). Switch *STEMS Analysis Process* to quality: Demucs3 instead of Spleeter, about
   2 dB better, ~33 s per track instead of ~6. With the stem cache (2) that cost is paid once per track.
2. **stemcache**: keep separated stems on disk. Wrap the engine's `OperationAtIndex`: key = hash of the input
   chunk + model; on a hit, copy the stems from disk instead of running the model; on a miss, run it and store the
   result. Keyed on the audio itself, so it doesn't depend on track identity and can't serve the wrong stems if
   decoding changes. Stores every stem of the chunk (3 or 4, per model), compressed, with a size cap
   (LRU). To check: decoding is bit-identical across loads (Spotify too), 3-stem vs 4-stem modes.
3. **prestems**: *Separate stems* in the stems window of a playlist: runs rekordbox's own separation for each
   track in the background (so the chunks hash the same as on a deck) and fills the stem cache. One track at a
   time, with progress and stop.
4. **betterstems**: replace the engine's model with HTDemucs v4 (MIT) on the GPU / Neural Engine (CoreML or MLX),
   behind the same `OperationAtIndex` interface. It has to run overlapping windows itself to avoid seams at chunk
   edges (rekordbox gives only 23-93 ms of context). Biggest job; depends on converting HTDemucs and on it being
   fast enough on Apple Silicon. Cheaper first experiment: give rekordbox's own Demucs3 ONNX session the CoreML
   execution provider.

## Status

- [ ] 1 quality setting: needs the user (rekordbox downloads the Demucs model from *Preferences*).
- [x] 2 stemcache (`mods/stemcache.cpp`; the chunk format is `mods/stemcodec.h`, with backends `stemcodec_mac.mm`
  and `stemcodec_win.cpp`). New chunks are AAC-LC, one stream per stem, 256 kbps by default (`stemcache_quality`;
  `lossless`, 192 and 128 also work): about 1.5 MB per chunk, ~35 MB per 5-minute track. Windows encodes through
  Media Foundation, which stops at 192 kbps, so 256 is stored as 192 there. Chunks made earlier, lossless (16-bit,
  mid/side, order-2 prediction, byte planes, LZFSE: ~108 MB per track), are still read. When the encoder falls
  behind (8 chunks waiting), a chunk is stored lossless rather than skipped. Decoding is identical across loads
  (AIFF, WAV checked): reloading a track gets every chunk from disk. A 16-minute track's stems were ready 6.8 s
  after loading instead of ~20 s. The key is the SHA-256 of the input as 16-bit samples (so a Spotify track's first
  load, from the stream, and a later one hit the same chunk). Config: `stemcache_max_gb`
  (default 10, least recently used chunks go first), `stemcache_min_free_gb` (default 5: nothing new is stored
  while the disk has less free), `stemcache_dir`, `stemcache=0`.
- [x] 3 prestems, in stemcache: *Separate stems* in the stems window (a playlist's menu: *Open stems window*).
  `AudioPart::load(100, uri, nullptr)` (decks are map keys; 100 is never a real deck) runs rekordbox's own
  separation, one track at a time; done when as many chunks went through as `PreOperation`'s length gives, then
  `AudioPart::unload(100)`. 50 tracks in ~6 min (Spleeter). Not usable as a "done" signal:
  `isSeparatedSeparatableUri` (stays false on a made-up deck), `getSeparatedRate` (returns a struct),
  `syncRequestSeparate` (only prepares the track's data).
- [x] Spotify tracks. rekordbox doesn't separate them (`isSeparatableUri` is false), so stemcache answers yes for
  them (`stemcache_spotify=0` turns this off) and treats a Spotify track as a downloaded one for where the stems
  go. The separator reads the track through rekordbox's own Spotify reader (kept up with the download by
  `core/spotifyread.cpp`), so its stems arrive during the first load. Their chunks are never stored on disk: the
  last ones are kept in memory (`stemcache_memory_mb`), so the track loaded again in the same session gets them
  at once. Which track a separation is of comes from the separator's own uri, which rekordbox asks about when it
  starts (`stem_sep4`) a few hundred bytes before the engine handle it passes to `PreOperation`. The stems
  window can't separate Spotify tracks ahead of time (there is no deck to keep them); they show *On a deck*.
  Where the mods can't read the stream, rekordbox's "no STEMS for Spotify tracks" message is replaced by one that
  says so (macOS only).
- [x] 4 betterstems (`mods/betterstems.cpp`, model by `tools/models/htdemucs_coreml.sh` on macOS,
  `tools/models/htdemucs_onnx.py` on Windows). The network runs behind `betterstems_backend.h`: CoreML on macOS
  (`betterstems_mac.mm`, FLOAT16 on the GPU, the Neural Engine compile fails), ONNX Runtime on Windows
  (`betterstems_ort.cpp`, fp32, DirectML where there is a GPU). HTDemucs is converted without its STFT/iSTFT
  (those are in `betterstems.cpp`, as Demucs does them). Matches PyTorch to 46-59 dB per stem. stemcache's engine
  wrapper calls it (service `betterstems.separate`) instead of the engine (any of the four models), over the whole
  input chunk (context included); it cuts that into overlapping 7.8 s segments cross-faded with triangular
  weights, as Demucs does, and the stems are the middle of the result. The chunks are kept apart from rekordbox's
  by the cache key: folder `<engine>-<model>-<3|4>`, the model being the weights' name and size
  (`htdemucs-<size>` on macOS, `htdemucs-onnx-<size>` on Windows). Without the model rekordbox's engine separates.
  ~1 s per 11.8 s chunk in rekordbox (0.5 s standalone). Checked against Spleeter's stems of the same chunks:
  correlation 0.7-0.97 per slot, so the slots line up. Still open: seams at chunk edges (each chunk gets only
  rekordbox's 93 ms of context); `AudioPartSeparator::getStreamAt` could give it more.
- [x] Which stems are kept (`stemcache_keep`, *Save stems to disk* in the stems window): `all` as before; `chosen` stores
  only the chunks of tracks the stems window's run separates (the run's current track, or one in `tracks.txt` from
  an earlier run); `none` nothing new. What isn't stored goes to the in-memory store Spotify tracks use
  (`stemcache_memory_mb`). Which track a chunk belongs to comes from the separation's uri (as for Spotify tracks).
  Checked live with `stemcache_log=1`: a deck's track "0 stored", the run's track 11 chunks stored, both served
  from the right place on the next load.
- [x] Stems on demand (`stemcache_ondemand=1`). What rekordbox does, found on 7.2.19 (macOS):
  - A deck plays the unseparated track while its stems aren't there, whatever the stem buttons say (all four muted
    on screen: the channel's level unchanged). The buttons are accepted meanwhile (`PartMuteSoloComp::setPartActive`
    only refuses a mute when the stem's source isn't loaded).
  - `AudioPartSeparator::run` takes chunks from two lists: one with priority (+0x240) and the rest in order (+0x250).
    `AudioPartData::isReadyPosition`, which the deck's stem reader asks about the playhead, moves the chunk it
    needs to the priority list (`requestSeparateAt(pos, n, true)`), but only where
    `AudioPart::isRandomAccessible(MODEL)` says so: the Spleeter models (0, 2), not Demucs3 (1, 3).
  - `AudioPartSeparator::cancel()` / `stop()` wait for the separator's thread without a time limit
    (`stopThread(-1)`), so anything that keeps that thread must watch `threadShouldExit`.
  - Every stem control ends in `IDjPlayerControl::setPartMuteState / setPartIsoValue / setPartFxState` on the
    deck's `DjPlayerUnit` (reached through a `DjPlayerStreamingEventSender` wrapper, the player at +0x40); a load
    resets the four mutes (four calls with false). The isolator's centre is 0.5.

  So the engine wrapper separates the first chunk a track needs and then keeps the separator's thread inside that
  call until the track is asked for: one of the three setters taking a stem away on a deck holding the track, or
  such a control being on already. The setters' slots are swapped in the vtable the player behind a deck's wrapper
  points at (found on the running objects; the slot numbers are the ones the wrapper's own three functions pass
  their calls on through). Released after a chunk, the
  separator's next pick is the playhead's chunk: a mute at 1:40 into an unseparated track was silent 0.9-1.0 s
  after the press (HTDemucs, M2 Pro; the playhead's chunk took 0.57 s). Holding before the first chunk instead
  cost one stale chunk first (2 s). Unloading the deck or quitting while it waits ends the wait (checked).
  With Demucs3 selected rekordbox separates in order from the start, so a late press waits for everything before
  it.
  Windows: the same code. `DjSystem::getPlayerControl`, the wrapper's three functions and its player field (+0x50;
  slots 0x290 / 0x298 / 0x2a0, one destructor slot less than mac) are in `tools/win/verify/stems.out.json` for
  7.2.19; the scanner finds them all in 7.2.20 (checked by disassembly). There the slots are only swapped once
  the option is switched on. `tools/win/emu/test_stemcache.py` runs the DLL against both builds: the swap, the
  three replacements through rekordbox's own forwarders and originals, which deck a control belongs to, and the
  wait with rekordbox's `threadShouldExit` (59 checks). Not run on a Windows machine: threads, the engine wrapper
  around the wait, and what the separator does when it ends.
  `tools/dev/stemtest.mm` (hot-loaded) drives all of this without a mouse: load, play, seek, the on-screen stem
  buttons, the isolator and FX setters, and `rec`, which taps each mixer channel's audio in the process
  (`EnvelopeGeneratorArray::processBlock`) and logs its level per half second.
- Other mods use the cache through two services: `stemcache.cached` (a chunk's stems from disk, if there; only
  looks) and `stemcache.store` (stores a chunk separated elsewhere, with betterstems' model and layout). autocue
  separates tracks itself with betterstems and stores the chunks, so a deck gets them from disk.
