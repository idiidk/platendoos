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
  needs. Spleeter jumps to the playhead (`isRandomAccessible`); Demucs3 goes strictly in order (stemcache asks for
  the playhead's chunks itself, see stems on demand below).
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
  correlation 0.7-0.97 per slot, so the slots line up. Seams at chunk edges (rekordbox's context is 93 ms for
  Spleeter, 23 ms for Demucs3): since 0.7.4 (macOS) `wideInput` reads more around each chunk with
  `AudioPartSeparator::getStreamAt`, up to what two model runs reach anyway (1.75 segments: 1.35 s each side with
  Demucs3, 0.94 s with Spleeter), checked against rekordbox's own input where they overlap (and not used when only
  silence comes back where the track goes on); such chunks are stored under `<engine>-htdemucs-<size>-wide-<n>`.
  Measured in-process on 6 Spotify tracks (75 s each, against the excerpt separated in one call): error power within
  23 ms of a chunk edge against the error elsewhere +3 dB with Demucs3's context, -0.3 to +2.4 dB with Spleeter's,
  -1 to -7 dB widened; overall agreement 0-1 dB better. A quick window (one segment, 0.5 s margins) is as close
  as a chunk. Windows: not yet (getStreamAt isn't in its tables). autocue still separates with 93 ms (its stores stay
  under the plain key, which decks no longer read).
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
  With Demucs3 selected rekordbox separates in order from the start, so a late press waited for everything before
  it (a tester: 5-8 s at 2:00). The separator doesn't care which model it runs: its run takes the priority list
  first and the chunks land in a SparseSet. So stemcache makes the deck reader's call itself: for a deck whose
  stems are needed (a stem control on, or asked for on demand), `AudioPartDataManager::requestSeparateAt(uri,
  1 and 3, playhead, 12 s, true)`, once when the control is used (before the held separation is let go) and then
  every half second while the deck plays. Positions are the separator's 44.1 kHz samples (`getPlayingTime` ms x
  44.1). A finished separation is left alone (the manager checks). Measured on 7.2.19 with Demucs3 selected
  (HTDemucs behind it, M2 Pro), all four stems muted at 2:06 of an unseparated track: before, chunks 1, 2, 3, ...
  in order and silence after ~7 s; with it, chunk 11 (the playhead's; Demucs3 chunks are 10.95 s) and 12 first,
  13 asked for as the deck went on, silence within 1 s. The test command `separateat <deck> <seconds>` (config
  dir `command-stemcache`) asks for those chunks with every model, Spleeter's too.
  Windows: `requestSeparateAt` (0x142d07160) and `DjEngineIF::getPlayingTime` (0x14253bb80) are verified for
  7.2.19 in `tools/win/verify/stems.out.json`, the scanner finds both in 7.2.20 (same code, checked);
  `test_stemcache.py` checks what the DLL passes them (66 checks on both builds). Not run on a Windows machine.
  There the stem controls are only followed with `stemcache_ondemand` on, so without it this doesn't apply.
  The tick looks every 0.1 s and asks at once when the playhead jumped (more than 1.5 s off where it should be),
  else twice a second; it asks for the playhead's chunk and, only in its last 5 s, the next one, because the
  priority list is sorted and a chunk asked for at another playhead would otherwise come first. Since 0.7.5
  (macOS) it asks for the next 30 s (`kAhead`): a tester's MacBook Air M3 went back to the track's start after
  the playhead's chunk and the playhead sometimes passed the end of the stems. `keepFirstNearPlayheads`, after
  every chunk on the separator's thread, moves chunks of the priority list (separator +0x240, a sorted
  `juce::Array<int>`; the in-order list at +0x250, the lock at +0x260) that lie outside every deck's playhead
  to 30 s on back into the in-order list. Live (M2 Pro): mute at 2:00, chunks 11, 12, 13, then 4, 5, 6, then 14
  as the playhead moved on; a jump forward moved 4 chunks back and the new playhead's chunk came next. Since
  2026-11-27 for every deck with a track, stem control or not (with `stemcache_ondemand` only a track that was
  asked for), and of all four models. betterstems' wait for an analysis' beat stage (up to 8 s,
  `betterstems_grid_first`) ends while a deck's stems are needed now (service `stemcache.stemsNeeded`: a stem
  control on a deck with a track); not yet seen live (the test tracks were analysed already).
- [x] Stems at the playhead at once (`stemcache_quick`, macOS, 2026-11-27). A chunk is 11-12 s and the deck gets
  it whole: two runs of HTDemucs (0.5 s on an M2 Pro) after whatever chunk was under way. Nothing in rekordbox
  needs whole chunks, though (7.2.19):
  - `AudioPartSeparator::OperationAtIndex` writes each stem to its `AudioPartOutputStream` (separator +0x2a0, one
    per stem in the order of the engine's outputs) with `setPosition(sample * 8)` + `write`, adds the range to two
    sets (+0x218 `SparseSet<unsigned>` in samples, +0x228 `SparseSet<long long>` in bytes), and `run` hands copies
    of both to `AudioPartDataManager::update(uri, model, bytes, samples)`: the track's `AudioPartData` takes them
    (what `isReadyPosition` / `numContains` ask) and its listeners are told. Then
    `AudioPartSeparatorManager::update` (a pool job: progress).
  - `AudioPartSeparator::getStreamAt(position, L, R, count)` reads any stretch of the track (zeros before 0 and
    after the end, in blocks of 512 through a `PcmReaderTemplate<Float2>`: `setPosition`, `isReady`, waiting up
    to 10 s for a stream). The track's length is +0x208, the engine's handle +0x358 (what `PreOperation` gets the
    address of: the way from an engine call to its separator).
  - A streaming track's stems are in memory, an array per stem that grows (`AudioPartData::writeData`): a write
    at the end appends, one beyond it fills the gap, one inside is copied, and one that starts inside and ends
    beyond is refused. `OperationAtIndex` then returns false and `run` ends the whole separation.
  - The deck: `StretchableAudioSource` around a `MultiPagingAudioSource`, the mix and one child source per stem,
    each with pages of 1 s read through an `AudioPartReader` (a `FileReader`: `isReadyToRead`, `fileRead`). A
    stem's page is read only when the data has all of it. Which pages are looked at when the playhead enters a new
    page (from it outwards, 12 s back to 26 s ahead), so once a second; the update message only serves ranges that
    were asked for with `ensureBufferedRange(Range<int>)` and weren't ready
    (`MultiPagingAudioSource::onAudioPartUpdateMessage`, message thread). `ensureBufferedRange` buffers the stems
    for a range now when all are there for all of it (it keeps scratch arrays per calling thread: audio, message,
    other). A deck's source: `IDjPlayerControl` slot 0x2b0 gives its `DjPlayerUnitState`, whose `refAudioSource()`
    returns a counted pointer (`RefCountedObjEx`).

  So (`quickAtPlayhead`): from inside an engine call, on the separator's thread (betterstems gives its caller a
  turn before each segment, service `stemcache.betweenSegments`; also after a chunk and when a held separation
  is asked for), a deck that needs stems and has none from 1.1 s behind its playhead to 2 s ahead gets one window:
  one segment of the model (7.8 s, `betterstems.segment`), its audio taken from the call's own input when it lies
  in that chunk and else read with `getStreamAt`, placed half a second before the first sample without stems and
  kept inside one chunk's input. Published is the window without half a second at each end (except where it starts
  or ends with the chunk's input), written from there to the chunk's end (silence after the window's samples, not
  announced: no write may end inside a chunk, see the memory array above) and announced as rekordbox does it
  (`publishQuick`). Then the deck is told on the main thread (`buffersAtPlayhead`): `ensureBufferedRange` for the
  stretch around its playhead that has stems, a page short of where they end; the same from the tick whenever that
  stretch has grown, and when a stem control is used (stems that were there already took up to a second before).
  The chunk itself is separated as always afterwards; `chunkDone` lays the window into its stems before they go to
  rekordbox and the cache: the published samples as they were, a 93 ms fade to the chunk's own on each side
  inside the unpublished margins. A second window in a chunk (a jump inside it) is cut where the first one's
  published part is and faded into it before it goes out. A window for a chunk that is itself past its first
  segment isn't made, nor one whose deck has jumped on while its audio was read.
  Measured (M2 Pro, HTDemucs 0.26 s a segment, all four stems muted, `rec <s> 50`): on demand, mute at 1:03 of an
  unseparated track: silent after 0.33 s (window out after 0.32 s, picked up in 20 ms); a jump into a part not
  separated yet: 0.37-0.57 s (a segment under way is finished first); a mute where the chunk was there already
  (the playhead's chunk goes first): 0.08 s, before the deck was told up to a second. `stemcache_log=1` says per
  chunk how far a window was from the chunk's own stems (-20 to -45 dB per stem against all of them: the same
  music twice through the model). A Spotify track played for the first time: all 30 chunks after six windows
  (before the write rule above was followed the separation ended at the first chunk after a short window).
  Speed setting (Spleeter: the decks' readers wait inside `fileRead`, see the comment above `fileRead` in
  stemcache.cpp): all four stems muted, jumps between unseparated places silent after 0.8 s and 1.35 s, each with
  one window at the playhead and one for the old page a reader was still waiting in; before the readers were
  followed, 2.2 s.
  Not done: Windows (the fields and functions have no table entries; the code is `#ifndef _WIN32`), rekordbox's own
  engines (their chunk is one call: no turn in between), 3-stem mode (written for it, not run).
  `tools/dev/stemtest.mm`: `rec <s> <ms>` for a level per <ms>, `reads` / `reads dump` for what the decks read of
  the stems (`AudioPartReader`'s two slots, logged).
  Windows: the same code. `DjSystem::getPlayerControl`, the wrapper's three functions and its player field (+0x50;
  slots 0x290 / 0x298 / 0x2a0, one destructor slot less than mac) are in `tools/win/verify/stems.out.json` for
  7.2.19; the scanner finds them all in 7.2.20 (checked by disassembly). There the slots are only swapped once
  the option is switched on. `tools/win/emu/test_stemcache.py` runs the DLL against both builds: the swap, the
  three replacements through rekordbox's own forwarders and originals, which deck a control belongs to, and the
  wait with rekordbox's `threadShouldExit` (59 checks). Run on a Windows PC on 2026-11-26 (7.2.19): a load
  logged the four mute resets for the right deck, the first chunk was separated and the rest waited, and a click on
  the on-screen VOCAL mute started the separation; with `stemcache_keep=chosen` a deck's track went to memory only.
  `tools/dev/stemtest.mm` (hot-loaded) drives all of this without a mouse: load, play, seek, the on-screen stem
  buttons, the isolator and FX setters, and `rec`, which taps each mixer channel's audio in the process
  (`EnvelopeGeneratorArray::processBlock`) and logs its level per half second.
- [x] rekordbox's own model only when it is needed (`stemcache_lazy_model`, macOS and Windows, 2026-11-27). rekordbox
  initialises the selected engine in the background some 20 s after it starts (`AudioPartSeparator::initialize`,
  twice: for the 3-stem and the 4-stem model, with `AudioPartLibrary::load` filling the table again each time), and
  the engine's `Initialize` loads its model: 680 MB for Demucs3 (`hdemucs.onnx` plus the STFT models; the app's
  footprint before and after one more call), 1.9 GB once it has separated something, and its `Finalize` gives next
  to nothing back. With betterstems that model never runs. The engine's functions are in
  `AudioPartLibrary::s_library` (a row of 0x58 per engine: type, the library, then `Initialize`, `Finalize`,
  `IsInitialized`, `PreOperation`, `PostOperation`, `CancelOperation`, the two `OperationAtIndex`), which the
  separators point at (+0x1f8). So when `Initialize` is called and betterstems is expected to separate (service
  `betterstems.expected`: on, its model there, not failed), stemcache's wrapper doesn't pass the call on, and puts
  its own functions in the row's other four places (found by the four that are its wrappers already): `IsInitialized`
  says yes, `PreOperation` gives a handle of the mod's own (`StandIn`), `PostOperation` / `CancelOperation` /
  `Finalize` only reach the engine when it was loaded. Whenever the engine itself must separate (`engineOp`: no
  HTDemucs when a track starts, or a chunk it didn't do), `Initialize` runs then with what rekordbox passed and the
  separation gets the engine's own handle. Tested: idle footprint 60 s after starting 2811 MB without, 1897 MB with;
  separation, a mute, unloading mid-separation and loading again on the stand-in handle; the test command
  `ownengine 1` (chunks go to rekordbox's engine): "model is needed after all: loaded in 2.3 s", four chunks from
  Demucs3 (1.5 s each), unload, `ownengine 0`, HTDemucs again. Speed setting (Spleeter): held back as well, and
  stems on demand with it as above. Switching *STEMS Analysis Process* in *Preferences* while the model is held
  back (both ways, through the prefs mod's `toggle`): rekordbox finalizes the engine (`finalizeWrapper`: nothing
  to close) and initialises the other one, which is held back in its turn ("rekordbox's demucs3 model isn't
  loaded ..."); a deck's stems afterwards as before. Windows: `AudioPartLibrary::s_library` is 0x145e029e0 in
  7.2.19 (`tools/win/verify/stems.out.json`, found in 7.2.20 as well), the same rows and the same code
  (`holdEngine`); `tools/win/emu/test_stemcache.py` checks the stand-ins against both builds (89 checks), and on
  the Windows PC (7.2.19, speed setting) the idle private memory went from 5.4 GB to 4.5 GB, with stems on demand
  working and Spleeter loaded on demand in 2.2 s with `ownengine 1`. Intel Mac: the build's x86_64 half resolves
  the same symbols (recipes checked), not run on an Intel Mac.
- Other mods use the cache through two services: `stemcache.cached` (a chunk's stems from disk, if there; only
  looks) and `stemcache.store` (stores a chunk separated elsewhere, with betterstems' model and layout). autocue
  separates tracks itself with betterstems and stores the chunks, so a deck gets them from disk.
