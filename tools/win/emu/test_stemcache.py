"""stemcache's stems on demand (mods/stemcache.cpp) on Windows, in the emulator.

Real code: the DLL's followStemControls (finds the vtable on the decks' objects and swaps three slots), its three
replacements (setPartMute, setPartIso, setPartFx), deckOfControl, wantStems / isWanted, waitForStems (the wait a
separator thread sits in), rbm::engine / deckCount / deckPath; and from rekordbox.exe: DjPlayerStreamingEventSender's
three forwarding functions (how every caller reaches the player), DjPlayerUnit's own setPartMuteState /
setPartIsoValue / setPartFxState (the originals the replacements call), juce::Thread::threadShouldExit.
Also separateAtPlayhead (the playhead's chunk first): what it hands
AudioPartDataManager::requestSeparateAt (a stand-in that records its arguments: track, model, position in 44.1 kHz
samples, count, priority) for DjEngineIF::getPlayingTime's position (a stand-in), from a stem control and directly.
And rekordbox's own model only when it is needed: holdEngine on rekordbox's real table of engine functions
(AudioPartLibrary::s_library, the Demucs3 row filled as AudioPartLibrary::load leaves it, with Python functions for the
engine's own), the four stand-ins it puts there, engineNeeded and what Finalize does after it.
Python stand-ins for rekordbox: the DjEngineIF singleton, DjEngineIF::getDjSystem, DjSystem::getPlayerControl (hands
out the four decks' wrappers), DjEngineIF::getFilePath (a deck's track), juce::Thread::getCurrentThread (a thread
object whose "should exit" flag the test sets), juce::String's destructor, and Sleep (counts the waits).
Not covered: threads (the wait runs on the test's one thread), the engine wrapper that calls the wait, and what
rekordbox's separator does when the wait ends: that needs a Windows machine.
"""
import struct

import msvc
from emu import F64

M = "stemcache.cpp"
SENDER = "__ZN6rbxfrm28DjPlayerStreamingEventSender"
MUTE = SENDER + "16setPartMuteStateENS_9audiopart4PARTEb"
ISO = SENDER + "15setPartIsoValueENS_9audiopart4PARTEd"
FX = SENDER + "14setPartFxStateENS_9audiopart4PARTEb"
CONTROL = 344  # DjPlayerUnit: its IDjPlayerControl subobject (the vtable's RTTI offset), as verified
TRACKS = ["C:\\music\\one.mp3", "C:\\music\\two.mp3", "", "C:\\music\\four.mp3"]


def run(t):
    r = t.r
    e, exe = r.emu, r.exe
    S = lambda n: r.sym(n, M)
    A = lambda n: exe.base + r.table_value("addr", n)
    V = lambda meth: r.table_value("off", f"__ZTVN6rbxfrm28DjPlayerStreamingEventSenderE|vt:{meth}|0")

    # ---- the table: what the DLL resolved ----------------------------------------------------------------------
    for addr, name in [("A_senderVt", "__ZTVN6rbxfrm28DjPlayerStreamingEventSenderE"), ("A_playerControl", "__ZNK8DjSystem16getPlayerControlEi"),
                       ("A_djSystem", "__ZNK8djengine10DjEngineIF11getDjSystemEv"), ("A_currentThread", "__ZN4juce6Thread16getCurrentThreadEv"),
                       ("A_threadShouldExit", "__ZNK4juce6Thread16threadShouldExitEv")]:
        t.eq(r.addr(addr, M), A(name), f"{addr} resolved from the table")
    slots = {}
    for name, meth in [("V_setPartMute", MUTE), ("V_setPartIso", ISO), ("V_setPartFx", FX)]:
        slots[name] = V(meth)
        t.eq(r.offset(name, M), (slots[name], True), f"{name} = {slots[name]:#x} from the table")
    control_at = r.table_value("off", f"{MUTE}|ldr x0|0")
    t.eq(r.offset("O_senderControl", M), (control_at, True), f"O_senderControl = {control_at:#x} from the table")

    # ---- independent of the table: rekordbox's own code says the same -------------------------------------------
    # Each of the wrapper's three functions is: mov rcx,[rcx+disp8]; mov rax,[rcx]; jmp [rax+disp32]
    sender_vts = msvc.class_vtables(exe, A("__ZTIN6rbxfrm28DjPlayerStreamingEventSenderE"))
    player_vts = msvc.class_vtables(exe, A("__ZTIN6rbxfrm12DjPlayerUnitE"))
    t.check(CONTROL in player_vts, "DjPlayerUnit has a vtable for a base at +344 (its control interface)")
    for name, meth in [("V_setPartMute", MUTE), ("V_setPartIso", ISO), ("V_setPartFx", FX)]:
        code = e.mem_read(A(meth), 14)
        ok = code[:3] == b"\x48\x8b\x49" and code[4:7] == b"\x48\x8b\x01" and code[7:10] == b"\x48\xff\xa0"
        t.check(ok, f"{name}: the wrapper's function is a plain forwarder")
        if ok:
            t.eq(code[3], control_at, f"{name}: the forwarder reads the player at +{control_at:#x}")
            t.eq(struct.unpack("<I", code[10:14])[0], slots[name], f"{name}: the forwarder jumps through slot {slots[name]:#x}")
            t.eq(e.r64(sender_vts[0] + slots[name]), A(meth), f"{name}: it sits in the same slot of the wrapper's own vtable")

    # ---- rekordbox's side: four decks, each a wrapper around a player ---------------------------------------------
    engine, system = e.alloc(0x400), e.alloc(0x100)
    channels = r.table_value("off", "__ZN8djengine10DjEngineIF6unloadENS_15EnPlayerChannelE|ldr x20|0")  # (core's O_engineChannels)
    e.w32(engine + channels, 4)
    players = [msvc.fake_instance(e, exe, None, 0x800, player_vts) for _ in range(4)]
    wrappers = [msvc.fake_instance(e, exe, None, 0x100, sender_vts) for _ in range(4)]
    for w, p in zip(wrappers, players):
        e.w64(w + control_at, p + CONTROL)
    control_vt = e.r64(players[0] + CONTROL)
    originals = [e.r64(control_vt + slots[n]) for n in ("V_setPartMute", "V_setPartIso", "V_setPartFx")]
    before = e.mem_read(control_vt, 101 * 8)
    strings = []

    def file_path(e):  # DjEngineIF::getFilePath(channel) -> juce::String, through the hidden pointer after `this`
        text = TRACKS[e.arg(2) & 3].encode() + b"\0"
        holder = e.alloc(16 + len(text))
        e.w32(holder, 1000)  # (the reference count: never freed by rekordbox's code)
        e.w64(holder + 8, len(text))
        e.mem_write(holder + 16, text)
        e.w64(e.arg(1), holder + 16)
        strings.append(holder)
        return e.arg(1)

    thread = e.alloc(0x400)
    state = {"thread": thread, "sleeps": 0, "exit_after": None, "want_after": None, "clock": 1000000}
    # juce::Thread's "should exit" flag: where rekordbox's own threadShouldExit reads it (movzx eax, byte [rcx+disp32]; ret)
    code = e.mem_read(A("__ZNK4juce6Thread16threadShouldExitEv"), 8)
    t.check(code[:3] == b"\x0f\xb6\x81" and code[7] == 0xC3, "threadShouldExit is the plain flag read")
    exit_flag = struct.unpack("<I", code[3:7])[0]

    def sleep(e):  # (the emulator's clock stands still: Sleep moves it on, or sleep_for would never return)
        state["sleeps"] += 1
        state["clock"] += (e.arg(0) & 0xFFFFFFFF) * 10000 + 10000
        if state["exit_after"] is not None and state["sleeps"] >= state["exit_after"]:
            e.w8(thread + exit_flag, 1)
        if state["want_after"] is not None and state["sleeps"] >= state["want_after"]:
            want(TRACKS[3], "test")
        if state["sleeps"] > 500:
            raise RuntimeError("the wait never ended")
        return 0

    e.hook(A("__ZN4juce15SingletonHolderIN8djengine10DjEngineIFENS_15CriticalSectionELb0EE3getEv"), lambda e: engine)
    e.hook(A("__ZNK8djengine10DjEngineIF11getDjSystemEv"), lambda e: system)
    e.hook(A("__ZNK8DjSystem16getPlayerControlEi"), lambda e: wrappers[e.arg(1) & 0xFFFFFFFF] if (e.arg(1) & 0xFFFFFFFF) < 4 else 0)
    e.hook(A("__ZNK8djengine10DjEngineIF11getFilePathENS_15EnPlayerChannelE"), file_path)
    e.hook(A("__ZN4juce6StringD1Ev"), lambda e: None)
    e.hook(A("__ZN4juce6Thread16getCurrentThreadEv"), lambda e: state["thread"])
    playing_ms = [1000, 120000, 0, 61500]  # where each deck plays
    requests = []

    def request_at(e):  # AudioPartDataManager::requestSeparateAt(const String&, MODEL, long long, int, bool): static
        text = e.mem_read(e.r64(e.arg(0)), 64).split(b"\0")[0].decode()
        requests.append((text, e.arg(1) & 0xFFFFFFFF, e.arg(2), e.arg(3) & 0xFFFFFFFF, e.arg(4) & 0xFF))
        return 0

    e.hook(A("__ZN6rbxfrm9audiopart20AudioPartDataManager17requestSeparateAtERKN4juce6StringENS0_5MODELExib"), request_at)
    e.hook(A("__ZNK8djengine10DjEngineIF14getPlayingTimeENS_15EnPlayerChannelE"), lambda e: playing_ms[e.arg(1) & 3])
    for api in ("Sleep", "SleepEx"):
        e.set_api(api, sleep)

    def clock(e):  # QueryPerformanceCounter, at the emulator's frequency of 10 MHz
        e.w64(e.arg(0), state["clock"])
        return 1

    e.set_api("QueryPerformanceCounter", clock)

    def std_string(text):
        s = e.alloc(32)
        msvc.write_string(e, s, text)
        return s

    def wanted(text):
        return bool(e.call(S("isWanted"), std_string(text)) & 0xFF)

    def want(text, why):
        e.call(S("wantStems"), std_string(text), e.alloc_cstr(why))

    def stem_state(deck):
        return e.r32(S("g_stemState") + 4 * deck)

    def through_wrapper(deck, name, part, value):
        """As every caller in rekordbox: a virtual call on the deck's wrapper, which rekordbox's own code passes on."""
        w = wrappers[deck]
        return e.call(msvc.vcall_target(e, w, slots[name] // 8), w, part, value)

    # ---- off: nothing is touched on Windows ------------------------------------------------------------------------
    e.w32(S("g_onDemand"), 0)
    e.call(S("followStemControls"))
    t.eq(e.mem_read(control_vt, 101 * 8), before, "stemcache_ondemand off: the player's vtable is left alone")

    # ---- on: the three slots, and only those -----------------------------------------------------------------------
    e.w32(S("g_onDemand"), 1)
    e.w8(S("g_logChunks"), 1)
    e.call(S("followStemControls"))
    after = e.mem_read(control_vt, 101 * 8)
    mine = [S("setPartMute"), S("setPartIso"), S("setPartFx")]
    changed = [i for i in range(101) if after[8 * i:8 * i + 8] != before[8 * i:8 * i + 8]]
    t.eq(changed, sorted(slots[n] // 8 for n in slots), "exactly the three slots of the control interface's vtable changed")
    for n, fn, orig, o in zip(("V_setPartMute", "V_setPartIso", "V_setPartFx"), mine, originals, ("o_setPartMute", "o_setPartIso", "o_setPartFx")):
        t.eq(e.r64(control_vt + slots[n]), fn, f"{n}: the slot holds the mod's replacement")
        t.eq(e.r64(S(o)), orig, f"{o} is rekordbox's function that was there")
    t.eq(e.mem_read(sender_vts[0], 101 * 8), e.mem_read(e.r64(wrappers[1]), 101 * 8), "the wrapper's own vtable is untouched")
    t.check(not any("can't be followed" in line for line in r.log), "no complaint in the log")

    # ---- a load resets the mutes: nothing asked for ----------------------------------------------------------------
    for part in range(4):
        through_wrapper(0, "V_setPartMute", part, 0)
    t.eq(stem_state(0), 0, "deck 1: four mutes set to off leave no control on")
    t.check(not wanted(TRACKS[0]), "... and its track isn't asked for")

    # ---- a mute, through rekordbox's forwarder, on deck 2 ----------------------------------------------------------
    with e.trace(players[1], 0x800) as tr:
        through_wrapper(1, "V_setPartMute", 2, 1)
    t.eq(stem_state(1), 1 << 2, "deck 2: muting stem 2 sets its bit")
    t.eq(stem_state(0), 0, "deck 1 is not touched by deck 2's control")
    t.check(wanted(TRACKS[1]), "deck 2's track is asked for")
    t.check(not wanted(TRACKS[0]), "deck 1's track is not")
    t.check(CONTROL + 0x100 in tr.read_offsets(), "rekordbox's own setPartMuteState ran on the player (it read its core pointer)")
    span = 5 * 44100
    MODELS = (0, 1, 2, 3)  # (whichever the track is separated with: rekordbox doesn't find the others)
    t.eq(requests, [(TRACKS[1], m, 120000 * 441 // 10, span, 1) for m in MODELS],
         "the mute asked rekordbox for deck 2's chunk at its playhead (2:00), every model, with priority")
    requests.clear()
    through_wrapper(1, "V_setPartMute", 2, 0)
    t.eq(stem_state(1), 0, "deck 2: unmuting clears the bit")
    t.eq(requests, [], "an unmute asks for nothing")
    t.check(wanted(TRACKS[1]), "... the track stays asked for (it is on the deck)")

    # ---- the isolator: a double, centre 0.5 -------------------------------------------------------------------------
    through_wrapper(3, "V_setPartIso", 1, F64(0.5))
    t.eq(stem_state(3), 0, "deck 4: an isolator knob at its centre is not a control that is on")
    t.check(not wanted(TRACKS[3]), "... and asks for nothing")
    n = len(r.log)
    through_wrapper(0, "V_setPartIso", 1, F64(0.2))
    t.eq(stem_state(0), 1 << 5, "deck 1: an isolator knob at 0.2 sets its bit")
    t.check(wanted(TRACKS[0]), "deck 1's track is asked for")
    t.check(any("isolator, stem 1 = 0.2" in line for line in r.log[n:]), "the value arrived as the double it is (the log says 0.2)")
    through_wrapper(0, "V_setPartIso", 1, F64(0.5))
    t.eq(stem_state(0), 0, "back at the centre clears it")

    # ---- the FX: taken off a stem ------------------------------------------------------------------------------------
    through_wrapper(1, "V_setPartFx", 3, 1)
    t.eq(stem_state(1), 0, "deck 2: the FX on a stem (the default) is not a control that is on")
    through_wrapper(1, "V_setPartFx", 3, 0)
    t.eq(stem_state(1), 1 << 11, "deck 2: the FX taken off stem 3 sets its bit")
    through_wrapper(1, "V_setPartFx", 3, 1)

    # ---- an empty deck: nothing to ask for --------------------------------------------------------------------------
    through_wrapper(2, "V_setPartMute", 0, 1)
    t.eq(stem_state(2), 1, "deck 3 (empty): the bit is kept for the track that comes")
    t.check(not any("stems of  asked for" in line for line in r.log), "... and no track is asked for")
    t.eq(requests, [(TRACKS[0], m, 1000 * 441 // 10, span, 1) for m in MODELS] + [(TRACKS[1], m, 120000 * 441 // 10, span, 1) for m in MODELS],
         "deck 1's isolator (0:01) and deck 2's FX taken off (2:00) asked; centred knobs, FX back on and the empty deck 3 didn't")
    t.check(all(q[0] != "" for q in requests), "no request for an empty deck")
    requests.clear()
    e.call(S("separateAtPlayhead"), 3, std_string(TRACKS[3]), 0xFFFFFFFF)
    t.eq(requests, [(TRACKS[3], m, 61500 * 441 // 10, span, 1) for m in MODELS], "separateAtPlayhead: deck 4 at 1:01.5")
    requests.clear()
    e.call(S("separateAtPlayhead"), 0, std_string(TRACKS[0]), 30000)
    t.eq([q[2] for q in requests], [30000 * 441 // 10] * 4, "the test command's form: at the position given (0:30)")
    requests.clear()
    through_wrapper(2, "V_setPartMute", 0, 0)

    # ---- the wait a separator thread sits in ------------------------------------------------------------------------
    t.eq(e.call(A("__ZNK4juce6Thread16threadShouldExitEv"), thread) & 0xFF, 0, "rekordbox's threadShouldExit: not yet")
    state["sleeps"] = 0
    e.call(S("waitForStems"), std_string(TRACKS[1]))
    t.eq(state["sleeps"], 0, "a track that is asked for doesn't wait")
    e.call(S("waitForStems"), std_string(""))
    t.eq(state["sleeps"], 0, "a separation whose track isn't known doesn't wait")
    state["thread"] = 0
    e.call(S("waitForStems"), std_string(TRACKS[3]))
    t.eq(state["sleeps"], 0, "not on a juce thread: no wait")
    state["thread"] = thread
    state.update(sleeps=0, want_after=7)
    e.call(S("waitForStems"), std_string(TRACKS[3]))
    t.eq(state["sleeps"], 7, "a track nobody asked for waits until it is asked for")
    t.eq(e.r32(S("g_held")), 0, "... and the count of waiting separations is back at 0")
    state.update(sleeps=0, want_after=None, exit_after=5)
    e.call(S("waitForStems"), std_string("C:\\music\\other.mp3"))
    t.eq(state["sleeps"], 5, "the wait ends when rekordbox tells the thread to stop (its own threadShouldExit)")
    e.w8(thread + exit_flag, 0)
    state.update(sleeps=0, exit_after=None)
    e.w32(S("g_onDemand"), 0)
    # (with the option off the engine wrapper doesn't call the wait at all; a wait under way ends on its next turn)
    e.w32(S("g_onDemand"), 1)

    # ---- rekordbox's own model only when it is needed ----------------------------------------------------------------
    LIBS = A("__ZN6rbxfrm9audiopart16AudioPartLibrary9s_libraryE")
    t.eq(r.addr("A_libraries", M), LIBS, "A_libraries resolved from the table")
    # rekordbox's own code names the same table: AudioPartSeparatorManager::isInitialized is
    # ... imul rcx,rax,0x58; lea rax,[rip+table]; add rax,rcx (the row), then the row's pointers from +0x10 on
    code = e.mem_read(A("__ZN6rbxfrm9audiopart25AudioPartSeparatorManager13isInitializedENS0_5MODELE"), 0x40)
    at = code.find(b"\x48\x6b\xc8\x58\x48\x8d\x05")
    t.check(at > 0, "isInitialized multiplies the model's row by 0x58 and takes the table's address")
    if at > 0:
        lea_end = A("__ZN6rbxfrm9audiopart25AudioPartSeparatorManager13isInitializedENS0_5MODELE") + at + 11
        t.eq(lea_end + struct.unpack("<i", code[at + 7:at + 11])[0], LIBS, "... which is the table's entry")
    engine_calls = []

    def engine_fn(name, result):
        def fn(e):
            engine_calls.append((name, e.arg(0)))
            return result
        return e.py_func(fn, name)

    own = {0x20: engine_fn("Finalize", 7), 0x28: engine_fn("IsInitialized", 0), 0x38: engine_fn("PostOperation", 1), 0x40: engine_fn("CancelOperation", 1)}
    W = lambda which: e.call(S("engineWrapper"), 1, which)  # this file's functions for Demucs3 (row 1)
    mine = {0x18: W(0), 0x30: W(1), 0x48: W(2), 0x50: W(3)}
    stand_ins = {0x20: W(4), 0x28: W(5), 0x38: W(6), 0x40: W(7)}
    t.eq(len(set(list(mine.values()) + list(stand_ins.values()))), 8, "eight different functions for the engine")
    row = LIBS + 0x58
    row_before, other_row = e.mem_read(row, 0x58), e.mem_read(LIBS, 0x58)

    def fill():  # the row as AudioPartLibrary::load leaves it: wrap()'s four, and the engine's own
        e.w64(row + 0x10, 0x1234)
        for off, fn in list(mine.items()) + list(own.items()):
            e.w64(row + off, fn)

    def slots():
        return {off: e.r64(row + off) for off in range(0x18, 0x58, 8)}

    model_dir = e.alloc_cstr("C:\\models\\demucs3_model")
    hold = lambda wanted: e.call(S("holdEngine"), 1, wanted, model_dir, 0xFFFFFFFF, 1, 0, 4, 0) & 0xFF
    fill()
    as_loaded = slots()
    t.eq(hold(0), 0, "not asked to hold: Initialize goes to the engine")
    t.eq(slots(), as_loaded, "... and the row is left as rekordbox filled it")
    t.eq(hold(1), 1, "asked to hold: Initialize is held back")
    t.eq(slots(), {**as_loaded, **stand_ins}, "the row has the four stand-ins, the other four as they were")
    t.eq(e.mem_read(LIBS, 0x58), other_row, "the other engine's row is untouched")
    t.eq(e.call(e.r64(row + 0x28)) & 0xFF, 1, "IsInitialized says yes while the model is held back")
    t.eq(engine_calls, [], "... without asking the engine")
    fill()  # rekordbox initialises the engine a second time: the table is filled again first
    t.eq(hold(0), 1, "the second Initialize is held back too")
    t.eq(slots(), {**as_loaded, **stand_ins}, "... and the stand-ins are back in the row")
    e.call(e.r64(row + 0x38), 0x5555)
    e.call(e.r64(row + 0x40), 0x6666)
    t.eq(engine_calls, [("PostOperation", 0x5555), ("CancelOperation", 0x6666)], "a handle that isn't a stand-in goes to the engine's own PostOperation / CancelOperation (kept from the first time, not the stand-ins themselves)")
    engine_calls.clear()
    # the engine has to separate after all: its Initialize, with what rekordbox passed
    init_args = []

    def engine_init(e):
        init_args.append((e.cstr(e.arg(0)), e.arg(1) & 0xFFFFFFFF, e.arg(2) & 0xFFFFFFFF, e.arg(3) & 0xFF, e.arg(4) & 0xFFFFFFFF, e.arg(5) & 0xFFFFFFFF))
        return 0

    engines = S("`anonymous namespace'::g_engines")  # Engine: prefix, inSamples, outSamples, init, pre, op[2], version (0x50 each)
    t.eq(e.cstr(e.r64(engines + 0x50)), "demucs3", "g_engines[1] is Demucs3 (the layout this test reads)")
    init_before = e.r64(engines + 0x50 + 0x10)
    e.w64(engines + 0x50 + 0x10, e.py_func(engine_init, "Initialize"))
    e.call(S("engineNeeded"), 1)
    t.eq(init_args, [("C:\\models\\demucs3_model", 0xFFFFFFFF, 1, 0, 4, 0)], "engineNeeded runs the engine's Initialize with rekordbox's arguments")
    e.call(S("engineNeeded"), 1)
    t.eq(len(init_args), 1, "... once")
    own[0x28] = engine_fn("IsInitialized", 1)
    fill()
    t.eq(hold(0), 0, "an Initialize after the model was loaded goes to the engine (as rekordbox would have)")
    t.eq(e.call(e.r64(row + 0x28)) & 0xFF, 1, "IsInitialized now asks the engine")
    t.eq(engine_calls, [("IsInitialized", engine_calls[0][1] if engine_calls else 0)], "... once")
    engine_calls.clear()
    t.eq(e.call(e.r64(row + 0x20)), 7, "Finalize reaches the engine once its model was loaded")
    t.eq([c[0] for c in engine_calls], ["Finalize"], "... and nothing else")
    engine_calls.clear()
    # held back and never loaded: Finalize doesn't reach the engine
    fill()
    t.eq(hold(1), 1, "held back again after a Finalize")
    e.call(e.r64(row + 0x20))
    t.eq(engine_calls, [], "Finalize of a model that was never loaded doesn't reach the engine")
    e.w64(engines + 0x50 + 0x10, init_before)
    e.mem_write(row, row_before)
