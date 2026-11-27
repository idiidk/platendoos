"""usbphrases (mods/usbphrases.cpp) on Windows, in the emulator.

Real code: the DLL's tick (the controller's slot swap, the deck poll, start: the device row's analysis path, the
worker that reads the stick's .EXT, unmasks its PSSI and writes the copy, the feed), the replacement getRow; and
from rekordbox.exe: SingletonHolder<SongStructInfoManager>::get with the manager's own constructor (the four decks'
SongStructInformation objects), SongStructInfoManager::getInformation, SongStructInformation::
loadSongStructInfoFromDb / clearSongStructInfo / getSongId / getTotalNumberOfPhrase, DatabaseIF::getSongStructInfo
and DatabaseMediator::getSongStructInfo / setSongStructInfo (the checks on the controller's row: its slot, the row's
type, the "analysed here" flag word and the analysis path), RowDataTrack's constructor and destructor, juce::String,
and analyzer::ConvUSB_ScrambleSongStruct (found by the mask key it uses), which masks the test's PSSI as an export
does.
Python stand-ins for rekordbox: the DatabaseIF singleton, DatabaseMediator::mainDBController (a controller whose
vtable answers getRowDataTrack for one collection track and records any write), DatabaseIF::getRowDataTrack for the
device's rows, PlayerAccessor::getMovableItemBasicInfo (the decks' items), shouldLoadSongStruct, CAnalyzerIF (its
getSongStructInfo parses the file it is given, the copy the mod wrote, in Python; its setSongStructInfo records),
the stick's file (plat::readFile), the worker and main-thread queue (plat::background / onMain run at once).
Not covered: rekordbox's own .EXT parser and the deck's drawing, a real device's export.pdb, timing between the
worker and the deck: that needs a Windows machine.
"""
import re
import struct

import capstone
import msvc
from juce import Juce

M = "usbphrases.cpp"
NS = "`anonymous namespace'::"
MAGIC = 0xFFFFFF00
KEY = bytes(c ^ 0x80 for c in b"Kanzen-niRikaishita")
COLL_ID, COLL_PATH = 1234, "C:\\Users\\dj\\AppData\\Roaming\\Pioneer\\rekordbox\\share\\PIONEER\\USBANLZ\\abc\\def\\ANLZ0000.DAT"
DEV, STICK = 3, "E:\\PIONEER\\USBANLZ\\P053\\0001D21F\\ANLZ0000.DAT"
TRACKS = {1: (1, [(1, 1), (33, 2), (65, 3), (129, 2), (161, 4), (225, 5)], 257),   # id: (mood, [(beat, kind)], end beat)
          2: (3, [(1, 1), (17, 2), (49, 3), (81, 8), (113, 9), (145, 5), (177, 10)], 200),
          5: None}                                                                    # an EXT without a PSSI


def pssi_raw(mood, phrases, end):
    """An unmasked PSSI tag as rekordbox's analysis writes it (crate-digger's layout)."""
    n = len(phrases)
    t = bytearray(32 + 24 * n)
    t[0:4] = b"PSSI"
    struct.pack_into(">IIIH", t, 4, 32, len(t), 24, n)
    struct.pack_into(">H", t, 18, mood)
    struct.pack_into(">H", t, 26, end)
    for i, (beat, kind) in enumerate(phrases):
        struct.pack_into(">HHH", t, 32 + 24 * i, i + 1, beat, kind)
    return bytes(t)


def anlz(tags):
    """A PMAI file (an ANLZ .EXT) holding tags."""
    body = b"".join(tags)
    head = bytearray(28)
    head[0:4] = b"PMAI"
    struct.pack_into(">II", head, 4, 28, 28 + len(body))
    return bytes(head) + body


def other_tag():  # (a tag before PSSI, as a real .EXT has: the mod walks past it)
    t = bytearray(24)
    t[0:4] = b"PWV3"
    struct.pack_into(">II", t, 4, 24, 24)
    return bytes(t)


def find_pssi(data):
    o = struct.unpack_from(">I", data, 4)[0]
    while o + 12 <= len(data):
        ln = struct.unpack_from(">I", data, o + 8)[0]
        if data[o:o + 4] == b"PSSI":
            return data[o:o + ln]
        if ln < 12:
            break
        o += ln
    return None


def run(t):
    r = t.r
    e, exe = r.emu, r.exe
    S = lambda n: r.sym(NS + n, M)
    A = lambda n: exe.base + r.table_value("addr", n)
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
    dis = lambda a, n=0x200: list(md.disasm_lite(bytes(e.mem_read(a, n)), a))
    KSTRUCT = "__ZN2db16DatabaseMediator17getSongStructInfoEjRN8analyzer14SongStructInfoE"
    O = lambda name: r.offset(NS + name, M)

    # ---- the table: what the DLL resolved ------------------------------------------------------------------------
    for addr, name in [("A_deckItem", "__ZN6browse14PlayerAccessor23getMovableItemBasicInfoEN6djplay9ePlayerIDE"),
                       ("A_infoHolder", "SingletonHolder<SongStructInfoManager>"),
                       ("A_infoMgrGet", "__ZN4juce15SingletonHolderIN6djplay21SongStructInfoManagerENS_15CriticalSectionELb0EE3getEv"),
                       ("A_getInfo", "__ZN6djplay21SongStructInfoManager14getInformationEi"),
                       ("A_loadFromDb", "__ZN6djplay21SongStructInformation24loadSongStructInfoFromDbEj"),
                       ("A_shouldLoad", "__ZNK6djplay21SongStructInformation20shouldLoadSongStructEv"),
                       ("A_clear", "__ZN6djplay21SongStructInformation19clearSongStructInfoEv"),
                       ("A_getRow", "__ZN2db10DatabaseIF15getRowDataTrackEjRNS_12RowDataTrackENS_12_browse_typeEl"),
                       ("A_mainCtl", "__ZN2db16DatabaseMediator16mainDBControllerEv"),
                       ("A_setPhraseId", "__ZN6djplay21SongStructInformation11setPhraseIdEii"),
                       ("A_saveIfNeeded", "__ZN6djplay21SongStructInformation26saveSongStructInfoIfNeededEv")]:
        t.eq(r.addr(NS + addr, M), A(name), f"{addr} resolved from the table")
    want = {"V_ctlGetRow": f"{KSTRUCT}|ldr|1", "O_rowType": f"{KSTRUCT}|ldr|2", "O_rowFlagsWord": f"{KSTRUCT}|win:row flags dword|0",
            "O_rowAnalysis": f"{KSTRUCT}|add|5", "O_itemId": "__ZN6djplay8DjplayDB12createObjectEPN2db20MovableItemBasicInfoE|ldr|2",
            "O_infoSong": "__ZNK6djplay21SongStructInformation9getSongIdEv|ldr|0",
            "O_infoCount": "__ZNK6djplay21SongStructInformation22getTotalNumberOfPhraseEv|ldr|0"}
    val = {}
    for name, key in want.items():
        val[name] = r.table_value("off", key)
        t.eq(O(name), (val[name], True), f"{name} = {val[name]:#x} from the table")
    V, ROWTYPE, FLAGS, PATH = val["V_ctlGetRow"], val["O_rowType"], val["O_rowFlagsWord"], val["O_rowAnalysis"]
    SONG, COUNT = val["O_infoSong"], val["O_infoCount"]

    # ---- independent of the table: rekordbox's getSongStructInfo, getSongId and getTotalNumberOfPhrase say the same --
    gssi = A(KSTRUCT)
    ins = dis(gssi, 0xa0)
    ops = [f"{mn} {op}" for _, _, mn, op in ins]
    row_at = next((int(m.group(1), 16) for o in ops if (m := re.match(r"lea rcx, \[rsp \+ (0x[0-9a-f]+)\]$", o))), None)
    t.check(row_at is not None, "getSongStructInfo builds the row on its stack", ops[:12])
    t.check(any(re.match(r"mov r\w+, qword ptr \[rcx \+ %#x\]$" % V, o) or o == "call qword ptr [rax + %#x]" % V for o in ops),
            f"getSongStructInfo calls the controller through slot {V:#x}", ops)
    t.check(f"mov rax, qword ptr [rsp + {row_at + ROWTYPE:#x}]" in ops, f"...checks the row's type at +{ROWTYPE:#x}", ops)
    t.check(f"test dword ptr [rsp + {row_at + FLAGS:#x}], 0x10000" in ops, f"...and bit 16 of the flag word at +{FLAGS:#x}", ops)
    t.check(f"lea rdx, [rsp + {row_at + PATH:#x}]" in ops, f"...and passes the analysis path at +{PATH:#x}", ops)
    for name, fn, off in [("getSongId", "__ZNK6djplay21SongStructInformation9getSongIdEv", SONG),
                          ("getTotalNumberOfPhrase", "__ZNK6djplay21SongStructInformation22getTotalNumberOfPhraseEv", COUNT)]:
        o2 = [f"{mn} {op}" for _, _, mn, op in dis(A(fn), 0x30)]
        t.check(f"mov ebx, dword ptr [rbx + {off:#x}]" in o2, f"{name} reads +{off:#x} under the lock", o2)
    calls = [(a, op) for a, _, mn, op in ins if mn == "call"]
    after = [int(op, 16) for a, op in calls if a > next(a for a, _, mn, op in ins if mn == "test" and "0x10000" in op) and op.startswith("0x")]
    an_get, an_gssi = after[0], after[1]

    # ---- analyzer::ConvUSB_ScrambleSongStruct: the function that uses the key "Kanzen-niRikaishita" ------------------
    rd0, rd1 = exe.section(".rdata")
    rdata = bytes(e.mem_read(rd0, rd1 - rd0))
    keys = [rd0 + m.start() for m in re.finditer(rb"Kanzen-niRikaishita\0", rdata)]
    tx0, tx1 = exe.section(".text")
    text = bytes(e.mem_read(tx0, tx1 - tx0))
    users = []
    for m in re.finditer(rb"\xc5\xf8\x10[\x05\x0d\x15\x1d\x25\x2d\x35\x3d]|\x0f\x10[\x05\x0d\x15\x1d\x25\x2d\x35\x3d]", text):
        i = m.end()
        a = tx0 + i + 4 + struct.unpack_from("<i", text, i)[0]
        if a in keys:
            users.append(tx0 + m.start())
    pdata = exe.pe
    pdata.parse_data_directories(directories=[3])
    funcs = sorted((exe.base + f.struct.BeginAddress, exe.base + f.struct.EndAddress) for f in pdata.DIRECTORY_ENTRY_EXCEPTION)
    fn_of = lambda a: next(((b, en) for b, en in funcs if b <= a < en), None)
    scrambles = sorted({fn_of(a) for a in users if fn_of(a) and fn_of(a)[1] - fn_of(a)[0] < 0x400})
    t.check(1 <= len(scrambles) <= 2, "small functions masking with the key (ConvUSB_ScrambleSongStruct, ...VocalDetect)", [hex(b) for b, _ in scrambles])

    def masked(raw, fn):  # rekordbox's own mask, as its export writes the tag
        p = e.alloc(len(raw) + 64, data=raw)
        e.call(fn, p)
        return bytes(e.mem_read(p, len(raw)))

    raw1 = pssi_raw(*TRACKS[1])
    # ConvUSB_ScrambleSongStruct(unsigned char* tag): the one of them that takes a PSSI tag alone and changes it
    scramble = next((b for b, _ in scrambles if masked(raw1, b) != raw1), scrambles[0][0])
    masked1 = masked
    masked = lambda raw: masked1(raw, scramble)
    m1 = masked(raw1)
    t.check(m1[:18] == raw1[:18] and m1 != raw1 and struct.unpack_from(">H", m1, 18)[0] > 20,
            "rekordbox's ConvUSB_ScrambleSongStruct masks from byte 18 (mood > 20 afterwards)")
    t.eq(m1, raw1[:18] + bytes(b ^ ((KEY[(i - 18) % 19] + raw1[0x11]) & 0xFF) for i, b in enumerate(raw1) if i >= 18),
         "...with the key the mod undoes (XOR key[i % 19] + the count's low byte)")
    stick = {}
    for tid, spec in TRACKS.items():
        tags = [other_tag()] + ([masked(pssi_raw(*spec))] if spec else [])
        stick[STICK.replace("0001D21F", "%08X" % tid)[:-3] + "EXT"] = anlz(tags)

    # ---- rekordbox's side ---------------------------------------------------------------------------------------
    st = {"qpc": 10 ** 9, "items": [0, 0, 0, 0], "orig": [], "dev_rows": [], "writes": [], "an_get": [], "an_set": [],
          "show": True, "reads": []}
    e.set_api("QueryPerformanceCounter", lambda e: e.w64(e.arg(0), st["qpc"]) or 1)
    e.set_api("GetSystemTimePreciseAsFileTime", lambda e: e.w64(e.arg(0), 133000000000000000 + st["qpc"]))
    j = Juce(r)
    r.dev_files()
    files = r.files_written
    dbif, mediator, analyzer = e.alloc(0x100), e.alloc(0x1000), e.alloc(0x100)
    e.w64(dbif, mediator)
    ctl = e.alloc(0x100)

    def orig_get_row(e):  # the controller's own getRowDataTrack(id, row): one collection track
        st["orig"].append(e.arg(1) & 0xFFFFFFFF)
        if e.arg(1) & 0xFFFFFFFF != COLL_ID:
            return 0
        row = e.arg(2)
        e.w32(row + ROWTYPE, 1)
        e.w32(row + FLAGS, e.r32(row + FLAGS) | 0x10000)
        e.w64(row + PATH, j.string(COLL_PATH))
        return 1

    def recorder(i):
        def f(e):
            st["writes"].append(i)
            return 0
        return f

    slots = [recorder(i) for i in range(0x500 // 8)]
    slots[V // 8] = orig_get_row
    e.w64(ctl, e.vtable(slots))
    orig_slot = e.r64(e.r64(ctl) + V)

    def dev_row(e):  # DatabaseIF::getRowDataTrack(id, row, browse type, device)
        tid, row, kind, dev = e.arg(1) & 0xFFFFFFFF, e.arg(2), e.arg(3) & 0xFFFFFFFF, e.arg(4) & 0xFFFFFFFF
        st["dev_rows"].append((tid, kind, dev))
        if kind != 2 or dev != DEV:
            return 0
        e.w32(row + ROWTYPE, 2)
        e.w64(row + PATH, j.string(STICK.replace("0001D21F", "%08X" % tid)))
        return 1

    entry_vt = e.vtable([lambda e: None])

    def analyzer_gssi(e):  # CAnalyzerIF::getSongStructInfo(path, info): what rekordbox's parser gives, from the file
        path = j.text(e.r64(e.arg(1)))
        st["an_get"].append(path)
        ext = path[:-3] + "EXT"
        norm = lambda p: p.replace("\\", "/").lower()
        f = next((v for k, v in files.items() if norm(k) == norm(ext)), None)
        tag = find_pssi(f) if f else None
        if path == COLL_PATH:   # the collection track's own analysis (rekordbox's, never masked)
            tag = pssi_raw(2, [(1, 1), (65, 2), (129, 3)], 300)
        if not tag or struct.unpack_from(">H", tag, 18)[0] > 20:
            st["an_bad"] = path
            return 0
        n = struct.unpack_from(">H", tag, 16)[0]
        info = e.arg(2)
        e.mem_write(info + 8, struct.pack("<HH", struct.unpack_from(">H", tag, 18)[0], n))   # mood, count
        e.w8(info + 0xc, tag[30])
        e.mem_write(info + 0xe, struct.pack("<H", struct.unpack_from(">H", tag, 26)[0]))      # end beat
        arr = e.alloc(8 * n)
        for i in range(n):
            ent = e.alloc(0x20)
            e.w64(ent, entry_vt)   # (polymorphic: the caller's OwnedArray deletes them through slot 0)
            beat, kind = struct.unpack_from(">HH", tag, 32 + 24 * i + 2)
            e.mem_write(ent + 8, struct.pack("<HH", kind, beat))
            e.w64(arr + 8 * i, ent)
        e.w64(info + 0x18, arr)
        e.w32(info + 0x20, n)
        e.w32(info + 0x24, n)
        return 1

    # plat::background runs the worker as an emulated thread (TrySubmitThreadpoolCallback isn't there: std::thread);
    # plat::onMain posts to core's main-thread window (startMainThread, as at rekordbox's start), whose real window
    # procedure pump() calls with WM_RBM_RUN
    e.enable_threads()
    e.call(r.sym("rbm::plat::startMainThread"))
    main_wnd = next(h for h, w in e.windows.items() if w.get("class") == "platendoos.main")

    def pump():
        e.run_threads()
        e.send_message(main_wnd, 0x8000 + 0x5242)

    def read_file(e):
        path = msvc.read_string(e, e.arg(0))
        st["reads"].append(path)
        if path not in stick:
            return 0
        msvc.write_string(e, e.arg(1), stick[path])
        return 1

    e.hook(A("__ZN4juce15SingletonHolderIN2db10DatabaseIFENS_15CriticalSectionELb0EE3getEv"), lambda e: dbif)
    e.hook(A("__ZN2db16DatabaseMediator16mainDBControllerEv"), lambda e: ctl)
    e.hook(A("__ZN2db10DatabaseIF15getRowDataTrackEjRNS_12RowDataTrackENS_12_browse_typeEl"), dev_row)
    e.hook(A("__ZN6browse14PlayerAccessor23getMovableItemBasicInfoEN6djplay9ePlayerIDE"), lambda e: st["items"][e.arg(0) & 3])
    e.hook(A("__ZNK6djplay21SongStructInformation20shouldLoadSongStructEv"), lambda e: int(st["show"]))
    e.hook(an_get, lambda e: st["an_get"].append("get") or analyzer)
    e.hook(an_gssi, analyzer_gssi)
    e.hook(r.sym("rbm::deckCount"), lambda e: 4)
    rf = r.sym("rbm::plat::readFile")   # (Rbmods hooks it already, for the table: only its handler is replaced, as a
    e.hooks[rf] = ("rbm::plat::readFile", read_file)   # second e.hook on one address would run both handlers)
    st["an_get"].clear()

    # the manager (its own constructor reads lighting files: a fake one) and the decks' SongStructInformation objects,
    # empty as constructed (lock at +8, the phrase array at +0x48, its listeners at +0x68: all zero)
    # the phrase names loadSongStructInfoFromDb copies (static juce::Strings indexed by kind, which rekordbox's static
    # initialisers would set): empty strings, so its own String::operator= copies something
    load = A("__ZN6djplay21SongStructInformation24loadSongStructInfoFromDbEj")
    names = set()
    for _, _, mn, op in dis(load, 0x500):
        m = re.match(r"r\w+, \[r\w+ \+ (0x[0-9a-f]+)\]$", op)
        if mn == "lea" and m and 0x1000000 < int(m.group(1), 16) < exe.size:
            names.add(exe.base + int(m.group(1), 16))
    t.check(len(names) >= 3, "loadSongStructInfoFromDb's phrase name tables found", [hex(x) for x in names])
    for tab in names:
        for k in range(16):
            if not e.r64(tab + 8 * k):
                e.w64(tab + 8 * k, j.empty)

    mgr = e.alloc(0x40)
    for i in range(4):
        e.w64(mgr + 8 + 8 * i, e.alloc(0x100))
    e.hook(A("__ZN4juce15SingletonHolderIN6djplay21SongStructInfoManagerENS_15CriticalSectionELb0EE3getEv"), lambda e: mgr)
    infos = [e.call(A("__ZN6djplay21SongStructInfoManager14getInformationEi"), mgr, i) for i in range(4)]
    t.check(all(infos) and len(set(infos)) == 4, "rekordbox's getInformation: four decks' SongStructInformation")
    song = lambda d: e.r32(infos[d] + SONG)
    count = lambda d: e.r32(infos[d] + COUNT)

    def item(tid, kind=2, dev=DEV):
        it = e.alloc(0x40)
        e.w32(it, kind)
        e.w32(it + 4, dev)
        e.w32(it + 8, 1)
        e.w32(it + 0x20, tid)
        return it

    def tick(n=1):
        for _ in range(n):
            st["qpc"] += 3 * 10 ** 6   # 0.3 s
            e.call(S("tick"))
            pump()

    def beats(d):  # the deck's phrases as rekordbox holds them: entry +0x10 = beat - 1
        arr = e.r64(infos[d] + COUNT - 0xc)
        return [e.r32(e.r64(arr + 8 * i) + 0x10) + 1 for i in range(count(d))]

    # ---- 1. the slot swap (first tick) ----------------------------------------------------------------------------
    vt = e.r64(ctl)
    before = e.mem_read(vt, 0x500)
    tick()
    after = e.mem_read(vt, 0x500)
    t.eq([i for i in range(0x500 // 8) if before[8 * i:8 * i + 8] != after[8 * i:8 * i + 8]], [V // 8],
         f"first tick: exactly the controller's slot {V // 8} (+{V:#x}) swapped")
    t.eq(e.r64(vt + V), S("getRow"), "...for the mod's getRow")
    t.eq(e.r64(S("g_origGetRow")), orig_slot, "...keeping rekordbox's as the original")
    t.eq(st["writes"], [], "nothing else of the controller called")

    # ---- 2. a collection track: rekordbox's own load, untouched ------------------------------------------------------
    e.call(A("__ZN6djplay21SongStructInformation24loadSongStructInfoFromDbEj"), infos[0], COLL_ID)
    t.eq(st["orig"], [COLL_ID], "a collection id goes to rekordbox's getRowDataTrack")
    t.eq(st["an_get"][-1:], [COLL_PATH], "...and rekordbox reads its own analysis file (type 1, flag, path from its row)")
    st["orig"].clear()

    # ---- 3. a device track on deck 3 -----------------------------------------------------------------------------------
    st["items"][2] = item(1)
    n = len(r.log)
    tick()
    t.eq(st["dev_rows"][-1:], [(1, 2, DEV)], "deck 3's device row asked for: id 1, browse type 2, device 3")
    t.eq(st["reads"][-1:], [STICK.replace("0001D21F", "00000001")[:-3] + "EXT"], "the stick's .EXT read (the row's analysis path, .DAT -> .EXT)")
    copy = [p for p in files if p.replace("\\", "/").endswith("/usbphrases/deck3.EXT")]
    t.check(len(copy) == 1, "the copy written as <configDir>/usbphrases/deck3.EXT", list(files))
    tag = find_pssi(files[copy[0]]) if copy else None
    t.eq(tag, raw1, "...its PSSI unmasked: byte for byte what rekordbox's analysis wrote")
    t.eq(count(2), 0, "not fed in the same tick")
    tick()
    t.eq(st["orig"], [], "the feed's made-up id never reaches rekordbox's getRowDataTrack")
    t.eq(st["an_get"][-1].replace("/", "\\").lower(), "c:\\users\\dj\\appdata\\roaming\\rekordbox-mods\\usbphrases\\deck3.dat",
         "rekordbox's own getSongStructInfo accepted the made-up row (type, flag word, path) and reads the copy")
    t.check("\\" in st["an_get"][-1] and "/" not in st["an_get"][-1], "...a path with backslashes", st["an_get"][-1])
    t.eq(song(2), MAGIC + 2, "deck 3 holds the made-up song id 0xFFFFFF02")
    t.eq(beats(2), [b for b, _ in TRACKS[1][1]], "deck 3 holds the stick's phrases (their beats)")
    t.check(any("usbphrases: deck 3: 6 phrases of device 3's track 1" in l for l in r.log[n:]), "the log says so", r.log[n:])
    tick(3)
    t.eq(len([p for p in st["an_get"] if p.lower().endswith("deck3.dat")]), 1, "fed once, not again while shown")

    # ---- 4. outside a load the made-up id gets no row: rekordbox's setSongStructInfo writes nothing --------------------
    tx = [m.start() for m in re.finditer(rb"\xe8", text)]
    ctl_fn = A("__ZN2db16DatabaseMediator16mainDBControllerEv")
    callers = {fn_of(tx0 + i) for i in tx if tx0 + i + 5 + struct.unpack_from("<i", text, i + 1)[0] == ctl_fn}
    sets = []
    for f in callers:
        if not f or f[0] == gssi:
            continue
        o = [f"{mn} {op}" for _, _, mn, op in dis(f[0], min(f[1] - f[0], 0x200))]
        if any(x.startswith("call qword ptr [rax + %#x]" % V) for x in o) and any(x.startswith("bts") and x.endswith(", 0x10") for x in o) \
                and any(x == "call %#x" % an_get for x in o):
            sets.append((f[0], o))
    t.eq(len(sets), 1, "rekordbox's DatabaseMediator::setSongStructInfo found (controller slot, analyzer, flag bit 16 set)")
    if sets:
        set_fn, o = sets[0]
        idx = next(i for i, x in enumerate(o) if x == "call %#x" % an_get)
        an_set = int(o[idx + 4].split()[1], 16) if o[idx + 4].startswith("call 0x") else None
        if an_set:
            e.hook(an_set, lambda e: st["an_set"].append(j.text(e.r64(e.arg(1)))) or 1)
        n = len(r.log)
        info = e.alloc(0x40)
        res = e.call(set_fn, mediator, MAGIC + 2, info) & 0xFF
        t.eq((res, st["writes"], st["an_set"]), (0, [], []), "an edit's save of deck 3: no row, nothing written (no analyzer write, no flag update)")
        t.check(any("no row for deck 3's phrases outside a load" in l for l in r.log[n:]), "the log says the save found no row", r.log[n:])
        # a collection track's save: the swapped slot hands its id to rekordbox's own getRowDataTrack (the rest of
        # setSongStructInfo is rekordbox's database code, not run here)
        st["orig"].clear()
        row = e.alloc(0x500)
        res = e.call(e.r64(e.r64(ctl) + V), ctl, COLL_ID, row) & 0xFF
        t.eq((res, st["orig"], e.r32(row + ROWTYPE)), (1, [COLL_ID], 1),
             "outside a load a collection id still gets rekordbox's own row (so its saves go through)")
        st["orig"].clear()

    # ---- 5. another device track on the same deck, one without phrases, then a collection track ----------------------
    st["items"][2] = item(2)
    tick(2)
    t.eq(beats(2), [b for b, _ in TRACKS[2][1]], "deck 3, another stick track: its own phrases")
    t.eq(song(2), MAGIC + 2, "...under the same made-up id")
    st["items"][2] = item(5)
    n = len(r.log)
    tick(2)
    t.eq(count(2), 0, "a stick track without a PSSI: the earlier phrases cleared, none shown")
    t.check(any("no phrases in" in l and "no PSSI" in l for l in r.log[n:]), "...the log says no PSSI", r.log[n:])
    st["items"][2] = item(2)
    tick(2)
    t.eq(count(2), 7, "back to track 2: fed again")
    e.call(A("__ZN6djplay21SongStructInformation24loadSongStructInfoFromDbEj"), infos[2], COLL_ID)
    st["items"][2] = item(COLL_ID, kind=1, dev=0)
    tick(2)
    t.eq(song(2), COLL_ID, "a collection track loaded on deck 3: rekordbox's own phrases stay")

    # ---- 6. phrase display off: nothing read --------------------------------------------------------------------------
    st["show"] = False
    e.call(A("__ZN6djplay21SongStructInformation19clearSongStructInfoEv"), infos[0])   # (rekordbox's load of a
    st["items"][0] = item(1)                                                          # track it has no phrases for)
    nr = len(st["reads"])
    tick(2)
    t.eq(st["reads"][nr:], [], "phrases not shown: deck 1's stick track not read")
    st["show"] = True
    tick(2)
    t.eq(beats(0), [b for b, _ in TRACKS[1][1]], "shown again: deck 1 fed")
    t.check(not st.get("an_bad"), "rekordbox's parser never got a masked or missing tag", st.get("an_bad"))

    # ---- 7. the "edit" test command: rekordbox's own setPhraseId and its write find no row for the made-up id ------
    n, nw = len(r.log), len(st["writes"])
    r.commands["usbphrases"] = ["edit 1"]
    tick(1)
    t.check(any("usbphrases: edit: deck 1 (song ffffff00)" in l for l in r.log[n:]), "edit 1: rekordbox's setPhraseId on deck 1", r.log[n:])
    t.check(any("no row for deck 1's phrases outside a load" in l for l in r.log[n:]), "...its write asked for the row and got none", r.log[n:])
    t.eq((st["writes"][nw:], st["an_set"]), ([], []), "...nothing else of the controller called, no analysis file written")
    st["items"][0] = 0
    tick(1)
    t.eq(count(0), 0, "deck 1 ejected: the device track's phrases cleared")
