"""linein (mods/linein.cpp, mods/linein_win.cpp): an audio input played through a deck, on Windows, in the emulator.

Real code: the DLL's init (the slot swap on RekordboxDjPlayerUnit's real vtable), manage (findUnits on fake engine
objects laid out with the Windows offsets, the options, findInput and the "name #2" rule, startCapture's back-off,
detach, retire, the kernels), unitProcess (render: the windowed-sinc resampler and its control loop, the gain and
fade-in, the thru flag), tick / command ("list", "state", logDecks), and the whole WASAPI half: sys::inputs (device
list, names, formats), sys::open (the capture thread started through the DLL's own std::thread and _beginthreadex,
the wait for it), the capture thread's run (opening, the raw-stream fallback, the mix format, event-driven packet
fetching and every sample format's conversion into the ring, the lost device), sys::stop (stop event, join).
The thread runs as an emulated thread (Emu.enable_threads): cooperatively, when the test lets it (a packet's event
set) or when the main thread waits for it. From rekordbox.exe: DjEngineIF::getDjSystem, DjSystem::enableInputThru
(with its inlined DjUnitAudioGraph / RekordboxDjPlayerUnit parts: the vbtable magic check, the units array, the thru
byte; the BPM manager it calls is a fake object), core's engine() / deckCount() / modEnabled.
Python stand-ins (each records its calls): the DjEngineIF singleton, isLoaded / isLoading, MainComponent::getInstance,
PlayerPanelL::updateDVSThroughMode, rekordbox's processBlock (the original the replacement calls), the BPM
manager's calls inside enableInputThru, the config (rbm::cfgStr / cfgNum / cfgHas), the clock (QueryPerformanceCounter,
GetSystemTimePreciseAsFileTime) and the Windows audio API: CoCreateInstance and fake COM objects (vtables in emulator
memory with Python methods) for IMMDeviceEnumerator, IMMDeviceCollection, IMMDevice, IPropertyStore, IAudioClient /
IAudioClient2, IAudioSessionControl2 and IAudioCaptureClient; CoTaskMemAlloc'd formats, PropVariantClear.
Not covered (needs a Windows PC): real WASAPI (its formats, periods and timing, the raw stream option, the
microphone privacy switch), real threads running at once (the audio thread against the capture thread and the main
thread: the lock and the atomics are only exercised one at a time here), MMCSS, a real device unplugged, rekordbox's
real processBlock with the input (its copy, BPM detection, gain smoothing) and the deck display.
"""
import math
import struct
import uuid

import msvc

M = "linein.cpp"
MW = "linein_win.cpp"
NS = "`anonymous namespace'::"
TABLE_BUILD_1919 = "6A97B2E4-6573000"
PB = "__ZN6rbxfrm21RekordboxDjPlayerUnit12processBlockERN4juce11AudioBufferIfEERNS1_10MidiBufferE"
VT = "__ZTVN6rbxfrm21RekordboxDjPlayerUnitE"

# tools/win/verify/linein.out.json (7.2.19)
VERIFIED_ADDR = {"A_djSystem": 0x1425400c0, "A_enableThru": 0x142d75f30, "A_unitVtable": 0x1457dda68 - 0x10,
                 "A_mainComponent": 0x14182a9c0, "A_showThru": 0x1419048b0}
VERIFIED_OFF = {"V_unitProcess": 0x50, "O_sysGraph": 0x58, "O_graphCount": 0x51c, "O_graphUnits": 0x510, "O_unitThru": 0x208,
                "O_bufChannels": 0x10, "O_bufClear": 0x120, "O_procRate": 0x30, "O_mainHolder": 0x4c0, "O_holderPlayers": 0xe0,
                "O_playersPanel": 0x148}
ADDRS = {"A_djSystem": "__ZNK8djengine10DjEngineIF11getDjSystemEv", "A_enableThru": "__ZN8DjSystem15enableInputThruEib",
         "A_unitVtable": VT, "A_mainComponent": "__ZN13MainComponent11getInstanceEv",
         "A_showThru": "__ZN6djplay15PlayerComponent12PlayerPanelL20updateDVSThroughModeEib"}

# the Windows layout the test builds (verified offsets above)
SYS_GRAPH, GRAPH_VB, GRAPH_UNITS, GRAPH_COUNT = 0x58, 0x418, 0x510, 0x51c
UNIT_RATE, UNIT_THRU, UNIT_BPM, UNIT_BPMMODE = 0x30, 0x208, 0x358, 0x368
BUF_CH, BUF_CLEAR = 0x10, 0x120
# the mod's structs (PDB types): Deck {unit, feeding +8, busy +9, switchedOff +10, Reader +16 {cap +0, chL +24, chR +28,
# gain +32, extra +40, synced +48, pos +56, played +64, ..., underruns +128, resyncs +132}, thruOurs +82672, showing +82680,
# why +82712}, 82744
DECK_SIZE, R = 82744, 16
CAP_RATE, CAP_CH, CAP_BLOCK, CAP_RING, CAP_WRITTEN, CAP_LOST, CAP_STARTED = 72, 88, 92, 96, 120, 132, 133
KRING = 1 << 15

S_OK, E_FAIL, E_NOINTERFACE, E_ACCESSDENIED = 0, 0x80004005, 0x80004002, 0x80070005
AUDCLNT_E_DEVICE_INVALIDATED, AUDCLNT_E_DEVICE_IN_USE, AUDCLNT_S_BUFFER_EMPTY = 0x88890004, 0x8889000A, 0x08890001
G = lambda s: uuid.UUID(s).bytes_le
IID = {"unknown": G("00000000-0000-0000-C000-000000000046"), "enum": G("A95664D2-9614-4F35-A746-DE8DB63617E6"),
       "client": G("1CB9AD4C-DBFA-4c32-B178-C2F568A703B2"), "client2": G("726778CD-F60A-4eda-82DE-E47610CD78AA"),
       "capture": G("C8ADBD64-E71E-48a0-A4DE-185C395CD317"), "session2": G("bfb7ff88-7239-4fc9-8fa2-07c950be9c6d"),
       "session": G("F4B1A599-7266-4319-A8CA-E70ACB11E8CD")}
CLSID_ENUM = G("BCDE0395-E52F-467C-8E3D-C4579291692E")
PKEY_NAME = G("a45c254e-df1c-4efd-8020-67d146a850e0") + struct.pack("<I", 14)
PKEY_FORMAT = G("f19f064d-082c-4e27-bc73-6882a1bb8e4c") + struct.pack("<I", 0)
SUB_PCM, SUB_FLOAT = G("00000001-0000-0010-8000-00aa00389b71"), G("00000003-0000-0010-8000-00aa00389b71")


def f32(x):
    return struct.unpack("<f", struct.pack("<f", x))[0]


# ---- the Windows audio side: devices and their COM objects -----------------------------------------------------------
class Device:
    """An audio input. fmt: f32 | f32plain | i16 | i24 | i24in32 | i32 | bad."""

    def __init__(self, uid, name, rate=48000, nch=2, fmt="f32", period=100000, raw=True, init_hr=0, present=True):
        self.uid, self.name, self.rate, self.nch, self.fmt, self.period = uid, name, rate, nch, fmt, period
        self.raw, self.init_hr, self.present = raw, init_hr, present
        self.opens = 0            # GetDevice calls (an open attempt each)
        self.inits = []           # (share mode, flags, buffer hns, raw) of Initialize calls
        self.streams = []         # its Stream per Activate

    def wfx(self):
        bits, valid, ext, isfloat = {"f32": (32, 32, True, True), "f32plain": (32, 32, False, True), "i16": (16, 16, False, False),
                                     "i24": (24, 24, True, False), "i24in32": (32, 24, True, False), "i32": (32, 32, True, False),
                                     "bad": (8, 8, False, False)}[self.fmt]
        tag = 0xFFFE if ext else (3 if isfloat else 1)
        align = self.nch * bits // 8
        b = struct.pack("<HHIIHHH", tag, self.nch, self.rate, self.rate * align, align, bits, 22 if ext else 0)
        if ext:
            b += struct.pack("<HI", valid, 3) + (SUB_FLOAT if isfloat else SUB_PCM)
        return b

    def pack(self, frames):
        """frames: [[value per channel]] in the device's own units (floats for f32, ints for the others)."""
        out = bytearray()
        for fr in frames:
            for v in fr:
                if self.fmt in ("f32", "f32plain"):
                    out += struct.pack("<f", v)
                elif self.fmt == "i16":
                    out += struct.pack("<h", v)
                elif self.fmt == "i24":
                    out += struct.pack("<i", v)[:3]
                elif self.fmt == "i24in32":
                    out += struct.pack("<i", v << 8)
                elif self.fmt == "i32":
                    out += struct.pack("<i", v)
        return bytes(out)

    def expect(self, v):
        """What the ring should hold for one sample."""
        if self.fmt in ("f32", "f32plain"):
            return f32(v)
        if self.fmt == "i16":
            return f32(f32(v) / 32768.0)
        if self.fmt == "i24":
            return f32(f32(v << 8) / 2147483648.0)
        if self.fmt == "i24in32":
            return f32(f32(v << 8) / 2147483648.0)
        return f32(f32(v) / 2147483648.0)

    @property
    def stream(self):
        return self.streams[-1] if self.streams else None


class Stream:
    """One IAudioClient (and its capture client): the packets waiting in it."""

    def __init__(self, dev):
        self.dev = dev
        self.packets = []         # (bytes, frames, flags)
        self.ev = None            # SetEventHandle
        self.started = self.stopped = False
        self.raw = False
        self.initialized = False
        self.invalid = False      # GetNextPacketSize fails as for an unplugged device
        self.fetched = 0          # frames handed out
        self.held = None          # the packet between GetBuffer and ReleaseBuffer


class Audio:
    """The fake Windows audio API: CoCreateInstance(MMDeviceEnumerator) and the COM objects behind it."""

    def __init__(self, t):
        self.t, self.e = t, t.r.emu
        self.devices = []
        self.objects = {}         # address -> [name, refs]
        self.freed = []           # CoTaskMemFree'd pointers
        self.taskmem = set()      # CoTaskMemAlloc'd by the fakes
        self.calls = []           # (what, ...) of interest
        e = self.e
        e.set_api("CoCreateInstance", self._create)
        e.set_api("CoTaskMemFree", self._free)
        e.set_api("PropVariantClear", lambda e: e.mem_write(e.arg(0), b"\0" * 24) or 0)
        e.set_api("CoInitializeEx", lambda e: self.calls.append(("CoInitializeEx", e.arg(1) & 0xFFFFFFFF)) or 0)
        e.set_api("CoUninitialize", lambda e: self.calls.append(("CoUninitialize",)) or 0)
        # avrt.dll (MMCSS): LoadLibraryW gives a fake module, GetProcAddress its two functions (Emu: by e.api name)
        self.mmcss = []
        e.set_api("LoadLibraryW", lambda e: 0xA7A70000 if e.wstr(e.arg(0)).lower() == "avrt.dll" else 0)
        e.set_api("FreeLibrary", lambda e: self.calls.append(("FreeLibrary", e.arg(0))) or 1)

        def av_set(e):
            self.mmcss.append(("set", e.wstr(e.arg(0)), e.current.tid if e.current else 1))
            e.w32(e.arg(1), 7)
            return 0xAB000 + len(self.mmcss)
        e.set_api("AvSetMmThreadCharacteristicsW", av_set)
        e.set_api("AvRevertMmThreadCharacteristics", lambda e: self.mmcss.append(("revert", e.arg(0), e.current.tid if e.current else 1)) or 1)

    # -- COM plumbing --
    def obj(self, name, methods, ifaces, data=None):
        """A COM object: IUnknown + methods (callables taking (e, self_info)); ifaces: the IIDs QueryInterface gives."""
        e = self.e
        p = e.alloc(16)
        info = {"name": name, "refs": 1, "ifaces": set(ifaces) | {IID["unknown"]}, "data": data, "ptr": p}
        self.objects[p] = info

        def qi(e):
            iid = e.mem_read(e.arg(1), 16)
            if iid in info["ifaces"]:
                info["refs"] += 1
                e.w64(e.arg(2), p)
                return S_OK
            e.w64(e.arg(2), 0)
            return E_NOINTERFACE

        def addref(e):
            info["refs"] += 1
            return info["refs"]

        def release(e):
            info["refs"] -= 1
            if info["refs"] < 0:
                self.t.check(False, f"{name} released once too often")
            return max(info["refs"], 0)
        slots = [qi, addref, release] + [(lambda m: (lambda e: m(e, info)))(m) for m in methods]
        e.w64(p, e.vtable(slots))
        return p

    def live(self):
        """COM objects still referenced (leaks once the mod let go of everything)."""
        return sorted(i["name"] for i in self.objects.values() if i["refs"] > 0)

    def taskmem_alloc(self, data):
        p = self.e.alloc(len(data) + 16, data=data)
        self.taskmem.add(p)
        return p

    def _free(self, e):
        if e.arg(0):
            self.freed.append(e.arg(0))

    def _create(self, e):
        clsid, iid, out = e.mem_read(e.arg(0), 16), e.mem_read(e.arg(3), 16), e.arg(4)
        self.calls.append(("CoCreateInstance",))
        if clsid != CLSID_ENUM or iid != IID["enum"]:
            e.w64(out, 0)
            return E_NOINTERFACE
        e.w64(out, self.obj("IMMDeviceEnumerator", [self._enum_endpoints, lambda e, i: E_FAIL, self._get_device,
                                                     lambda e, i: S_OK, lambda e, i: S_OK], [IID["enum"]]))
        return S_OK

    # -- IMMDeviceEnumerator --
    def _enum_endpoints(self, e, info):   # (flow, state mask, IMMDeviceCollection**)
        self.calls.append(("EnumAudioEndpoints", e.arg(1) & 0xFFFFFFFF, e.arg(2) & 0xFFFFFFFF))
        devs = [d for d in self.devices if d.present]

        def count(e, i):
            e.w32(e.arg(1), len(devs))
            return S_OK

        def item(e, i):
            n = e.arg(1) & 0xFFFFFFFF
            if n >= len(devs):
                e.w64(e.arg(2), 0)
                return E_FAIL
            e.w64(e.arg(2), self.device_obj(devs[n]))
            return S_OK
        e.w64(e.arg(3), self.obj("IMMDeviceCollection", [count, item], []))
        return S_OK

    def _get_device(self, e, info):       # (LPCWSTR id, IMMDevice**)
        uid = e.wstr(e.arg(1))
        d = next((d for d in self.devices if d.uid == uid and d.present), None)
        self.calls.append(("GetDevice", uid))
        if not d:
            e.w64(e.arg(2), 0)
            return 0x80070490   # E_NOTFOUND
        d.opens += 1
        e.w64(e.arg(2), self.device_obj(d))
        return S_OK

    # -- IMMDevice --
    def device_obj(self, d):
        def activate(e, i):           # (REFIID, clsctx, params, void**)
            if e.mem_read(e.arg(1), 16) != IID["client"]:
                return E_NOINTERFACE
            s = Stream(d)
            d.streams.append(s)
            e.w64(e.arg(4), self.client_obj(s))
            return S_OK

        def props(e, i):
            e.w64(e.arg(2), self.props_obj(d))
            return S_OK

        def get_id(e, i):
            e.w64(e.arg(1), self.taskmem_alloc((d.uid + "\0").encode("utf-16-le")))
            return S_OK
        return self.obj(f"IMMDevice {d.name}", [activate, props, get_id, lambda e, i: S_OK], [])

    def props_obj(self, d):
        def get_value(e, i):          # (REFPROPERTYKEY, PROPVARIANT*)
            key, pv = e.mem_read(e.arg(1), 20), e.arg(2)
            e.mem_write(pv, b"\0" * 24)
            if key == PKEY_NAME:
                e.w32(pv, 31)                         # VT_LPWSTR
                e.w64(pv + 8, e.alloc_wstr(d.name))
            elif key == PKEY_FORMAT:
                blob = d.wfx()
                e.w32(pv, 65)                         # VT_BLOB
                e.w32(pv + 8, len(blob))
                e.w64(pv + 16, e.alloc(len(blob), data=blob))
            return S_OK
        return self.obj("IPropertyStore", [lambda e, i: S_OK, lambda e, i: E_FAIL, get_value, lambda e, i: E_FAIL,
                                           lambda e, i: E_FAIL], [])

    # -- IAudioClient(2), IAudioSessionControl2, IAudioCaptureClient --
    def client_obj(self, s):
        d = s.dev
        R = lambda e, i: E_FAIL

        def initialize(e, i):         # (share mode, flags, REFERENCE_TIME buffer, periodicity, WAVEFORMATEX*, session guid)
            d.inits.append((e.arg(1) & 0xFFFFFFFF, e.arg(2) & 0xFFFFFFFF, e.arg(3), s.raw))
            self.calls.append(("Initialize", d.name, s.raw, e.mem_read(e.arg(5), 18)))
            hr = d.init_hr(s.raw) if callable(d.init_hr) else d.init_hr
            if hr == 0:
                s.initialized = True
            return hr

        def mix_format(e, i):
            e.w64(e.arg(1), self.taskmem_alloc(d.wfx()))
            return S_OK

        def period(e, i):
            if e.arg(1):
                e.w64(e.arg(1), d.period)
            if e.arg(2):
                e.w64(e.arg(2), 30000)
            return S_OK

        def start(e, i):
            s.started = True
            self.calls.append(("Start", d.name))
            return S_OK

        def stop(e, i):
            s.stopped = True
            self.calls.append(("Stop", d.name))
            return S_OK

        def set_event(e, i):
            s.ev = e.arg(1)
            return S_OK

        def get_service(e, i):
            iid = e.mem_read(e.arg(1), 16)
            if iid == IID["capture"]:
                e.w64(e.arg(2), self.capture_obj(s))
                return S_OK
            if iid == IID["session2"]:
                e.w64(e.arg(2), self.obj("IAudioSessionControl2", [R] * 13 + [duck], [IID["session"], IID["session2"]]))
                return S_OK
            e.w64(e.arg(2), 0)
            return E_NOINTERFACE

        def duck(e, i):
            self.calls.append(("SetDuckingPreference", d.name, e.arg(1) & 0xFF))
            return S_OK

        def props(e, i):              # IAudioClient2::SetClientProperties(AudioClientProperties*)
            cb, other, cat, opts = struct.unpack("<IiII", e.mem_read(e.arg(1), 16))
            self.calls.append(("SetClientProperties", d.name, cb, cat, opts))
            if not d.raw:
                return 0x88890021   # AUDCLNT_E_RAW_MODE_UNSUPPORTED
            s.raw = bool(opts & 1)
            return S_OK
        return self.obj(f"IAudioClient {d.name}", [initialize, R, R, R, R, mix_format, period, start, stop, R, set_event,
                                                     get_service, R, props, R], [IID["client"], IID["client2"]])

    def capture_obj(self, s):
        e = self.e

        def get_buffer(e, i):         # (BYTE**, UINT32* frames, DWORD* flags, u64* pos, u64* qpc)
            if not s.packets:
                e.w64(e.arg(1), 0), e.w32(e.arg(2), 0)
                return AUDCLNT_S_BUFFER_EMPTY
            data, frames, flags = s.packets[0]
            p = e.alloc(max(1, len(data)), data=data)
            e.w64(e.arg(1), p), e.w32(e.arg(2), frames), e.w32(e.arg(3), flags)
            s.held = frames
            return S_OK

        def release(e, i):
            n = e.arg(1) & 0xFFFFFFFF
            self.t.check(s.held == n, "ReleaseBuffer gets the frames GetBuffer gave", f"{n} vs {s.held}") if s.held != n else None
            if s.packets and s.held is not None:
                s.packets.pop(0)
                s.fetched += n
            s.held = None
            return S_OK

        def next_size(e, i):
            if s.invalid:
                return AUDCLNT_E_DEVICE_INVALIDATED
            e.w32(e.arg(1), s.packets[0][1] if s.packets else 0)
            return S_OK
        return self.obj("IAudioCaptureClient", [get_buffer, release, next_size], [IID["capture"]])

    def push(self, dev, frames, silent=False, run=True):
        """A packet from the device (frames: per-channel values in its own units), its event set, the thread run."""
        s = dev.stream
        s.packets.append((dev.pack(frames), len(frames), 2 if silent else 0))
        if run:
            self.wake(dev)

    def wake(self, dev):
        if dev.stream and dev.stream.ev:
            self.e.set_event(dev.stream.ev)
        self.e.run_threads()


# ---- rekordbox's side ---------------------------------------------------------------------------------------------------
class Rekordbox:
    def __init__(self, t):
        self.t, self.r = t, t.r
        r, e, exe = t.r, t.r.emu, t.r.exe
        self.e, self.exe = e, exe
        self.A = A = lambda n: exe.base + r.table_value("addr", n)
        self.config = {}
        self.qpc = 1000000000       # 10 MHz ticks (the emulator's frequency): 100 s
        self.loaded = set()
        self.thru_shown = []        # updateDVSThroughMode (panel, deck, on)
        self.processed = []         # the original processBlock: (unit, buffer, midi, L copy, R copy, isClear, thru)
        self.bpm_calls = []
        e.set_api("QueryPerformanceCounter", lambda e: e.w64(e.arg(0), self.qpc) or 1)
        e.set_api("GetSystemTimePreciseAsFileTime", lambda e: e.w64(e.arg(0), 133000000000000000 + self.qpc))
        # the config: core's readers, from self.config
        e.hook(r.sym("rbm::cfgStr"), self._cfg_str)
        e.hook(r.sym("rbm::cfgStrAtStart"), self._cfg_str)
        e.hook(r.sym("rbm::cfgNum"), self._cfg_num)
        e.hook(r.sym("rbm::cfgHas"), lambda e: int(e.cstr(e.arg(0)) in self.config))
        # the engine: DjEngineIF (getDjSystem runs for real: [engine+0xb0]), DjSystem, its graph and the four units
        self.engine, self.system, self.graph = e.alloc(0x400), e.alloc(0x200), e.alloc(0x600)
        e.w64(self.engine + 0xb0, self.system)
        e.w32(self.engine + r.table_value("off", "__ZN8djengine10DjEngineIF6unloadENS_15EnPlayerChannelE|ldr x20|0"), 4)
        e.hook(A("__ZN4juce15SingletonHolderIN8djengine10DjEngineIFENS_15CriticalSectionELb0EE3getEv"), lambda e: self.engine)
        e.hook(A("__ZNK8djengine10DjEngineIF8isLoadedENS_15EnPlayerChannelE"), lambda e: int((e.arg(1) & 3) in self.loaded))
        e.hook(A("__ZNK8djengine10DjEngineIF9isLoadingENS_15EnPlayerChannelE"), lambda e: 0)
        e.w64(self.system + SYS_GRAPH, self.graph)
        # (the graph's virtual base ExistenceConfirmable: vbtable at +0x418, its offset word, the magic 0x5e715e28)
        vb = e.alloc(16)
        e.w32(vb + 4, 0x100)
        e.w64(self.graph + GRAPH_VB, vb)
        e.w32(self.graph + GRAPH_VB + 0x100, 0x5e715e28)
        self.vtable = A(VT + "+0x10")
        self.units = []
        self.bpm_obj = e.alloc(0x40)
        bvt = [lambda e: self.bpm_calls.append(("slot", 0))] * 2 + [lambda e: self.bpm_calls.append(("slot2", e.arg(1)))] + \
              [lambda e: None] * 20 + [lambda e: 120.0] + [lambda e: None] * 10
        e.w64(self.bpm_obj, e.vtable(bvt))
        for d in range(4):
            u = e.alloc(0x400)
            e.w64(u, self.vtable)
            e.wf64(u + UNIT_RATE, 44100.0)
            bpm = e.alloc(0x200)
            e.w64(bpm + 0x190, self.bpm_obj)
            e.w64(bpm + 0x188, e.alloc(0x40))
            e.w64(u + UNIT_BPM, bpm)
            e.w32(u + UNIT_BPMMODE, 0)
            self.units.append(u)
        self.set_units(self.units)
        # enableInputThru's direct calls (the BPM manager's): stand-ins
        for c in direct_calls(e, A("__ZN8DjSystem15enableInputThruEib")):
            e.hook(c, lambda e, c=c: self.bpm_calls.append(("call", c)))
        # the decks' display
        self.main, holder, players, self.panel = e.alloc(0x800), e.alloc(0x200), e.alloc(0x200), e.alloc(0x100)
        e.w64(self.main + 0x4c0, holder), e.w64(holder + 0xe0, players), e.w64(players + 0x148, self.panel)
        e.hook(A("__ZN13MainComponent11getInstanceEv"), lambda e: self.main)
        e.hook(A(ADDRS["A_showThru"]), lambda e: self.thru_shown.append((e.arg(0), e.arg(1) & 0xFFFFFFFF, e.arg(2) & 0xFF)))
        # rekordbox's processBlock: the original the replacement calls
        self.orig_pb = A(PB)
        e.hook(self.orig_pb, self._process)

    def set_units(self, units, count=None):
        e = self.e
        arr = e.alloc(8 * max(1, len(units)))
        for i, u in enumerate(units):
            e.w64(arr + 8 * i, u)
        e.w64(self.graph + GRAPH_UNITS, arr)
        e.w32(self.graph + GRAPH_COUNT, len(units) if count is None else count)

    def _cfg_str(self, e):   # std::string cfgStr(const char* key, const char* def): the result through rcx
        key = e.cstr(e.arg(1))
        msvc.write_string(e, e.arg(0), self.config.get(key, e.cstr(e.arg(2)) if e.arg(2) else ""))
        return e.arg(0)

    def _cfg_num(self, e):
        v = self.config.get(e.cstr(e.arg(0)))
        return float(v) if v not in (None, "") else e.farg(1)

    def _process(self, e):
        u, b, m = e.arg(0), e.arg(1), e.arg(2)
        n, ch = e.r32(b + 4), e.r64(b + BUF_CH)
        left = struct.unpack(f"<{n}f", e.mem_read(e.r64(ch), 4 * n)) if ch and n else ()
        right = struct.unpack(f"<{n}f", e.mem_read(e.r64(ch + 8), 4 * n)) if ch and n else ()
        self.processed.append((u, b, m, left, right, e.r8(b + BUF_CLEAR), e.r8(u + UNIT_THRU)))

    def take_shown(self):
        out, self.thru_shown = self.thru_shown, []
        return [(d, on) for p, d, on in out]


def direct_calls(e, fn, limit=0x300):
    import capstone
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
    out = []
    for i in md.disasm(e.mem_read(fn, limit), fn):
        if i.mnemonic == "int3":
            break
        if i.mnemonic == "call" and i.op_str.startswith("0x"):
            out.append(int(i.op_str, 16))
    return out


class Buffer:
    """A juce::AudioBuffer<float> as rekordbox's processBlock gets it: numChannels, numSamples, channels +0x10, isClear +0x120."""

    def __init__(self, e, nch=2, n=128):
        self.e, self.n = e, n
        self.p = e.alloc(0x200)
        self.chans = [e.alloc(4 * 2048) for _ in range(nch)]
        arr = e.alloc(8 * nch)
        for i, c in enumerate(self.chans):
            e.w64(arr + 8 * i, c)
        e.w32(self.p, nch), e.w32(self.p + 4, n)
        e.w64(self.p + BUF_CH, arr)
        self.midi = e.alloc(0x40)
        self.fill(0.0)

    def fill(self, v, clear=1):
        for c in self.chans:
            self.e.mem_write(c, struct.pack("<f", v) * self.n)
        self.e.w8(self.p + BUF_CLEAR, clear)

    def ch(self, i):
        return struct.unpack(f"<{self.n}f", self.e.mem_read(self.chans[i], 4 * self.n))


# ---- the test -------------------------------------------------------------------------------------------------------------
def run(t):
    r = t.r
    e, exe = r.emu, r.exe
    S = lambda n: r.sym(NS + n, M)
    SW = lambda n: r.sym(n, MW)
    A = lambda n: exe.base + r.table_value("addr", n)
    is1919 = exe.build == TABLE_BUILD_1919

    # ---- 1. the table ---------------------------------------------------------------------------------------------------
    for name, key in ADDRS.items():
        t.eq(r.addr(name, M), A(key), f"{name} = the table's {key[:60]}")
        if is1919:
            t.eq(r.addr(name, M), VERIFIED_ADDR[name], f"{name} = linein.out.json's verified {VERIFIED_ADDR[name]:#x}")
    for name, want in VERIFIED_OFF.items():
        v, ok = r.offset(name, M)
        t.check(ok, f"{name} resolved from the table")
        t.eq(v, want, f"{name} = linein.out.json's verified {want:#x}")
    t.eq(A(VT + "+0x10"), A(VT) + 0x10, "the table's vtable symbol is the vtable - 0x10 (init adds 0x10)")
    # independent of the table: processBlock is slot 10 of RekordboxDjPlayerUnit's primary vtable, and only there
    vts = msvc.class_vtables(exe, A("__ZTIN6rbxfrm21RekordboxDjPlayerUnitE"))
    where = {off: s for off, vt in vts.items() if (s := msvc.vtable_slots(exe, vt, A(PB)))}
    t.eq(where, {0: [10]}, "RTTI: processBlock(AudioBuffer<float>&) only in slot 10 (+0x50) of the vtable at offset 0")
    t.eq(vts.get(0), A(VT + "+0x10"), "...that vtable is the table's")

    # ---- switched on as core does: on by default since the PC test of 2026-11-27 (no .untested), linein=0 switches it off ---
    rb = Rekordbox(t)
    au = Audio(t)
    e.enable_threads()
    r.dev_files()
    en = lambda: bool(e.call(r.sym("rbm::modEnabled"), e.alloc_cstr("linein")) & 0xFF)
    t.check(en(), "core: linein is on by default on Windows")
    rb.config["linein"] = "0"
    t.check(not en(), "core: linein=0 switches it off")
    rb.config["linein"] = "1"
    t.check(en(), "core: linein=1 switches it on again")
    e.w8(r.sym("g_on", M), 1)

    # ---- 2. init: the slot swap ------------------------------------------------------------------------------------------
    vt = rb.vtable
    before = e.mem_read(vt, 76 * 8)
    e.call(S("init"))
    after = e.mem_read(vt, 76 * 8)
    changed = [i for i in range(76) if before[8 * i:8 * i + 8] != after[8 * i:8 * i + 8]]
    t.eq(changed, [10], "init patches exactly slot 10 (+0x50) of RekordboxDjPlayerUnit's vtable")
    t.eq(e.r64(vt + 0x50), S("unitProcess"), "...with the mod's unitProcess")
    t.eq(e.r64(S("g_origProcess")), rb.orig_pb, "...and keeps rekordbox's processBlock as the original")
    t.eq(e.r8(S("g_patched")), 1, "g_patched")
    t.check(any(n == "VirtualProtect" for n, _ in e.calls), "the slot written through VirtualProtect (plat::patchMemory)")

    deck = lambda i: S("g_deck") + DECK_SIZE * i
    unit_of = lambda i: e.r64(deck(i))
    feeding = lambda i: e.r8(deck(i) + 8)
    cap_of = lambda i: e.r64(deck(i) + R)
    thru_ours = lambda i: e.r8(deck(i) + 82672)
    why = lambda i: msvc.read_string(e, deck(i) + 82712)

    def manage(seconds=1.0):
        rb.qpc += int(seconds * 1e7)
        e.call(S("manage"))

    def block(i, buf, thru_reset=False):
        """One engine block of deck i through its unit's vtable slot (as rekordbox's audio thread calls it)."""
        u = rb.units[i]
        if thru_reset:
            e.w8(u + UNIT_THRU, 0)
        n = len(rb.processed)
        e.call(msvc.vcall_target(e, u, 10), u, buf.p, buf.midi)
        return rb.processed[n:]

    # ---- findUnits through manage(): nothing configured ---------------------------------------------------------------
    manage()
    t.eq([unit_of(i) for i in range(4)], rb.units, "findUnits: the four units from DjSystem +0x58 -> graph +0x510 (count +0x51c)")
    t.eq(au.calls, [], "nothing asked for: no device listed or opened")
    rb.set_units(rb.units[:2])
    manage()
    t.eq([unit_of(i) for i in range(4)], rb.units[:2] + [0, 0], "a graph with two units: decks 3 and 4 have none")
    rb.set_units(rb.units)
    manage()

    # ---- 3. unitProcess with no input --------------------------------------------------------------------------------
    buf = Buffer(e)
    buf.fill(0.25, clear=0)
    raw_before = e.mem_read(buf.p, 0x200) + e.mem_read(buf.chans[0], 512) + e.mem_read(buf.chans[1], 512)
    e.w8(rb.units[2] + UNIT_THRU, 0)
    got = block(2, buf)
    t.eq([(u, b, m) for u, b, m, *_ in got], [(rb.units[2], buf.p, buf.midi)], "no input: the original called once with the same three arguments")
    t.eq(e.mem_read(buf.p, 0x200) + e.mem_read(buf.chans[0], 512) + e.mem_read(buf.chans[1], 512), raw_before, "...the buffer untouched")
    t.eq(e.r8(rb.units[2] + UNIT_THRU), 0, "...the thru flag untouched")
    other = e.alloc(0x400)
    e.w64(other, rb.vtable)
    n = len(rb.processed)
    e.call(msvc.vcall_target(e, other, 10), other, buf.p, buf.midi)
    t.eq([x[0] for x in rb.processed[n:]], [other], "a unit that is no deck's (another processor of the class): passed on as it is")

    # ---- the devices -----------------------------------------------------------------------------------------------------
    turntable = Device("{0.0.1.00000000}.{aaaa}", "USB AUDIO CODEC", rate=48000, nch=2, fmt="f32", period=100000)
    second = Device("{0.0.1.00000000}.{bbbb}", "USB AUDIO CODEC", rate=44100, nch=2, fmt="i16", period=100000)
    mic = Device("{0.0.1.00000000}.{0001}", "Microphone (Realtek)", rate=48000, nch=1, fmt="i16")
    au.devices = [second, mic, turntable]

    # 7. list
    def command(c):   # the test command through tick (command() is inlined there), manage() not due
        r.commands["linein"] = [c]
        e.wf64(r.sym("managedAt", M), rb.qpc / 1e7)
        e.call(S("tick"))

    n = len(r.log)
    command("list")
    lines = r.log[n + 1:]
    t.check(any("linein: 3 audio inputs" == l for l in lines), "list: '3 audio inputs'", lines)
    t.check(any("'Microphone (Realtek)'  1 channel, 48000 Hz  UID {0.0.1.00000000}.{0001}" in l for l in lines), "list: the mic, 1 channel, its rate and UID", lines)
    t.check(any("'USB AUDIO CODEC'  2 channels, 48000 Hz  UID {0.0.1.00000000}.{aaaa}" in l for l in lines) and
            any("'USB AUDIO CODEC' #2  2 channels, 44100 Hz  UID {0.0.1.00000000}.{bbbb}" in l for l in lines),
            "list: two of the same name, by UID: the second is '#2'", lines)
    t.check(("EnumAudioEndpoints", 1, 1) in au.calls, "inputs(): EnumAudioEndpoints(eCapture, DEVICE_STATE_ACTIVE)")
    t.eq(au.live(), [], "inputs(): every COM object it got is released")
    t.eq(sorted(au.freed), sorted(au.taskmem), "inputs(): every endpoint ID string freed (CoTaskMemFree)")
    t.check(au.calls.count(("CoInitializeEx", 2)) == au.calls.count(("CoUninitialize",)) and ("CoInitializeEx", 2) in au.calls,
            "inputs(): COM apartment-threaded on the calling thread, and uninitialised again")

    # ---- 6. manage: deck 1 plays the turntable -------------------------------------------------------------------------
    rb.config.update({"linein_1": "USB AUDIO CODEC", "linein_2": "  usb audio codec #2 ", "linein_2_on": "0"})
    au.calls.clear()
    n = len(r.log)
    manage()
    log = r.log[n:]
    t.eq(turntable.opens, 1, "linein_1 'USB AUDIO CODEC': the first of that name (by UID) opened")
    t.eq(second.opens, 0, "linein_2_on=0: deck 2's device is not opened")
    t.check(any("deck 2's input is off: switched off" in l for l in log), "...and the log says deck 2 is switched off", log)
    t.check(feeding(0) and cap_of(0), "deck 1 is fed from a capture")
    t.check(not feeding(1), "deck 2 is not")
    cap = cap_of(0)
    t.eq((e.f64(cap + CAP_RATE), e.r32(cap + CAP_CH), e.r32(cap + CAP_BLOCK), e.r8(cap + CAP_STARTED)), (48000.0, 2, 480, 1),
         "the capture: 48000 Hz, 2 channels, a 480-frame block (the 10 ms period), started")
    s = turntable.stream
    t.check(s and s.raw and s.initialized and s.started and s.ev, "the stream: raw, initialised, started, with an event", vars(s) if s else None)
    t.check(turntable.inits and turntable.inits[0][:3] == (0, 0x40000, 1000000), "Initialize(shared, EVENTCALLBACK, 100 ms)", turntable.inits)
    t.check(("SetDuckingPreference", "USB AUDIO CODEC", 1) in au.calls, "ducking switched off for its session")
    t.check(any("listening to 'USB AUDIO CODEC' (2 input channels, 48000 Hz, 480-frame blocks)  {0.0.1.00000000}.{aaaa}" in l for l in log), "logged: listening", log)
    t.check(any("deck 1 plays 'USB AUDIO CODEC' (channels 1-2, +0.0 dB, 48000 Hz into the engine's 44100 Hz)" in l for l in log), "logged: deck 1 plays it", log)
    t.eq(e.r8(rb.units[0] + UNIT_THRU), 1, "rekordbox's own DjSystem::enableInputThru(deck 0, true) set the unit's thru flag")
    t.eq(rb.take_shown(), [(0, 1)], "updateDVSThroughMode(panel, deck 0, true): INPUT THROUGH shown")
    t.check(thru_ours(0), "deck 1's thru is the mod's")
    th = [x for x in e.threads.values()]
    t.eq([x.state for x in th], ["blocked"], "one capture thread, waiting for its event")
    t.eq(au.mmcss, [("set", "Pro Audio", th[0].tid)] if th else [], "the capture thread runs at MMCSS 'Pro Audio' (on itself)")
    t.check(("CoInitializeEx", 0) in au.calls, "the capture thread: COM multithreaded")

    # ---- 4. the audio: a 1 kHz / 1.5 kHz sine at 48 kHz into a 44.1 kHz engine ------------------------------------------
    AL, AR, FL, FR = 0.5, 0.25, 1000.0, 1500.0
    k = [0]

    def sine_frames(dev, frames, al=AL, ar=AR):
        out = []
        for _ in range(frames):
            x = k[0] / dev.rate
            out.append([al * math.sin(2 * math.pi * FL * x), ar * math.sin(2 * math.pi * FR * x)])
            k[0] += 1
        return out

    def play(dev, seconds, deck_i=0, writer=True, nblock=128, engine_rate=44100.0, thru_reset_every=0):
        """Real time, simulated: the device's packets (one per period) and the engine's blocks in time order."""
        b = Buffer(e, n=nblock)
        out_l, out_r, flags = [], [], []
        period = dev.period / 1e7
        t0 = rb.qpc / 1e7
        tp, tb, end = t0 + period, t0 + nblock / engine_rate, t0 + seconds
        i = 0
        while min(tp, tb) < end:
            if writer and tp <= tb:
                rb.qpc = int(tp * 1e7)
                au.push(dev, sine_frames(dev, int(round(period * dev.rate))))
                tp += period
            else:
                rb.qpc = int(tb * 1e7)
                b.fill(0.0, clear=1)
                i += 1
                got = block(deck_i, b, thru_reset=bool(thru_reset_every and i % thru_reset_every == 0))
                l, rr = b.ch(0), b.ch(1)
                pl = got[-1] if got else None
                rd_ = deck(deck_i) + R
                c_ = e.r64(rd_)
                flags.append((e.r8(b.p + BUF_CLEAR), e.r8(rb.units[deck_i] + UNIT_THRU), len(got),
                              pl is not None and pl[3] == l and pl[4] == rr,
                              # reader: underruns, starts, pos; the capture's written
                              e.r32(rd_ + 128), e.r32(rd_ + 132), e.f64(rd_ + 56), e.r64(c_ + CAP_WRITTEN) if c_ else 0, rb.qpc))
                out_l += l
                out_r += rr
                tb += nblock / engine_rate
            if not writer and tp <= tb:
                tp += period
        rb.qpc = int(end * 1e7)
        return out_l, out_r, flags

    L, Rr, fl = play(turntable, 1.5, thru_reset_every=50)
    back = [(i, fl[i - 1][6], fl[i][6]) for i in range(1, len(fl)) if fl[i][6] < fl[i - 1][6]]
    t.eq(back, [], "the reader never goes back to input it played already (pos after each block)")
    und = [i for i in range(1, len(fl)) if fl[i][4] > fl[i - 1][4]]
    t.eq(und, [], "a steady input (480-frame packets every 10 ms): no underrun after the start "
                  "(blocks where one was counted, 2.9 ms each)")
    first = next((i for i, v in enumerate(L) if v != 0.0), None)
    t.check(first is not None, "deck 1 got audio")
    if first is not None:
        t.check(first >= 128 and all(v == 0.0 for v in L[:first]), f"before enough is written: silence ({first} frames)")
        b0 = first // 128 * 128
        t.check(abs(L[b0]) < 1e-6 and abs(Rr[b0]) < 1e-6, "the first block fades in from 0", (L[b0], Rr[b0]))
        t.check(all(f[1] == 1 for f in fl), "the thru flag is 1 after every block (also after rekordbox switched it off)")
        t.check(all(f[2] == 1 for f in fl), "the original called once per block")
        t.check(all(f[3] for f in fl), "the original gets the buffer already filled (called after)")
        t.check(all(f[0] == 1 for f in fl[:first // 128]) and all(f[0] == 0 for f in fl[first // 128:]),
                "isClear stays set while nothing is played, cleared once the input is in the buffer")
        steady = L[int(0.5 * 44100):], Rr[int(0.5 * 44100):]
        for name, xs, amp, freq in (("left (input ch 1)", steady[0], AL, FL), ("right (input ch 2)", steady[1], AR, FR)):
            rms = math.sqrt(sum(v * v for v in xs) / len(xs))
            t.near(rms, amp / math.sqrt(2), f"{name}: level {amp} (rms)", tol=amp * 0.01)
            f = freq_of(xs, 44100.0)
            t.near(f, freq, f"{name}: {freq:.0f} Hz at the engine's 44.1 kHz from a 48 kHz input", tol=freq * 0.002)
            d2 = max(abs(xs[j + 1] - 2 * xs[j] + xs[j - 1]) for j in range(1, len(xs) - 1))
            bound = amp * (2 * math.pi * freq / 44100.0) ** 2
            t.check(d2 < bound * 1.15 + 1e-3, f"{name}: no discontinuity (largest second difference {d2:.4f}, a sine's {bound:.4f})")
    t.check(e.r8(deck(0) + 10), "rekordbox switching the flag off is noted (switchedOff)")
    rb.bpm_calls.clear()
    manage()
    t.eq(rb.take_shown(), [(0, 1)], "manage: the thru switched on again through rekordbox (and shown)")
    t.eq(e.r8(deck(0) + 10), 0, "...switchedOff cleared")

    # state + logDecks
    n = len(r.log)
    command("state")
    lines = r.log[n + 1:]
    t.check(any("linein: deck 1: plays 'USB AUDIO CODEC'; rekordbox's thru flag 1" == l for l in lines), "state: deck 1 plays, thru 1", lines)
    t.check(any("linein: deck 2: no input (switched off); rekordbox's thru flag" in l for l in lines), "state: deck 2 switched off", lines)
    t.check(any("linein: deck 3: no input; rekordbox's thru flag 0" == l for l in lines), "state: deck 3 nothing", lines)
    dl = [l for l in lines if l.startswith("linein: deck 1 'USB AUDIO CODEC': read at")]
    t.check(dl and "0 underruns, 1 start" in dl[0] and "peak -6.0 dBFS" in dl[0], "state: deck 1's reader: 0 underruns, 1 start, peak -6 dBFS", dl)

    # gain: +6 dB
    rb.config["linein_1_gain"] = "6"
    manage()
    L, Rr, fl = play(turntable, 0.5)
    xs = L[int(0.1 * 44100):]
    t.near(math.sqrt(sum(v * v for v in xs) / len(xs)), AL * 10 ** (6 / 20) / math.sqrt(2), "linein_1_gain=6: level x1.995", tol=0.01)
    rb.config.pop("linein_1_gain")
    manage()
    play(turntable, 0.2)

    # the writer stops: the rest is played, then silence (no old audio again)
    L, Rr, fl = play(turntable, 0.3, writer=False)
    back = [(i, round(fl[i - 1][6]), round(fl[i][6])) for i in range(1, len(fl)) if fl[i][6] < fl[i - 1][6]]
    t.eq(back[:3], [], f"writer stopped: the reader doesn't go back to input it played ({len(back)} times in 0.3 s; block, pos before, after)")
    last = max((i for i, v in enumerate(L) if v != 0.0), default=-1)
    t.check(0 < last < len(L) - 1 and all(v == 0.0 for v in L[last + 1:]), "writer stopped: the ring's rest is played, then silence", last)
    if 0 < last:
        n_played = last + 1
        t.check(n_played < 0.05 * 44100, f"...within the distance kept ({n_played / 44.1:.1f} ms)")
        played = L[:n_played]
        f = freq_of(played, 44100.0) if n_played > 200 else FL
        t.near(f, FL, "...what is played after the writer stopped is still the sine going on (no old audio repeated)", tol=FL * 0.01)
        # (the fade is as long as the input that was left when it ran out: at most one engine block, here a few
        # dozen frames, down to nothing; a stop at full level would be a step of up to AL)
        tail = max(abs(v) for v in L[max(0, last - 1):last + 1])
        t.check(tail < 0.1 * AL, "...and fades out at the end instead of stopping at full level (a click)",
                f"last 2 frames before silence peak at {tail:.3f} of {AL}")
    rd = deck(0) + R
    t.check(e.r32(rd + 128) >= 1, "the underrun is counted")
    # the writer comes back: a fresh start
    L, Rr, fl = play(turntable, 0.5)
    t.check(any(v != 0.0 for v in L), "the writer back: the deck plays again")
    t.check(e.r32(rd + 132) >= 2, "...after a new start (resync)")

    # ---- 5. sample formats ---------------------------------------------------------------------------------------------
    rb.config.update({"linein_1": "Fmt", "linein_1_on": "1"})
    for fmt in ("f32", "f32plain", "i16", "i24", "i24in32", "i32"):
        for nch in (1, 2, 8):
            dev = Device("{fmt}." + fmt + str(nch), "Fmt", rate=44100, nch=nch, fmt=fmt)
            au.devices = [dev]
            manage()
            c = cap_of(0)
            if not t.check(c and feeding(0) and dev.stream and dev.stream.started, f"{fmt} x{nch}: opened and read", why(0)):
                continue
            t.eq(e.r32(c + CAP_CH), nch, f"{fmt} x{nch}: {nch} channels kept")
            if fmt.startswith("f32"):
                frames = [[((i * 8 + j) % 13 - 6) / 7.0 for j in range(nch)] for i in range(5)]
            elif fmt == "i16":
                frames = [[[-32768, 32767, 1, -1, 12345, -23456, 0, 16384][(i + j) % 8] for j in range(nch)] for i in range(5)]
            elif fmt in ("i24", "i24in32"):
                frames = [[[-8388608, 8388607, 1, -1, 1234567, -2345678, 0, 4194304][(i + j) % 8] for j in range(nch)] for i in range(5)]
            else:
                frames = [[[-2147483648, 2147483647, 1, -1, 123456789, -234567890, 0, 1 << 30][(i + j) % 8] for j in range(nch)] for i in range(5)]
            w0 = e.r64(c + CAP_WRITTEN)
            au.push(dev, frames[:3], run=False)
            au.push(dev, [[1] * nch, [2] * nch] if not fmt.startswith("f32") else [[0.9] * nch] * 2, silent=True, run=False)
            au.push(dev, frames[3:])
            w1 = e.r64(c + CAP_WRITTEN)
            t.eq(w1 - w0, 7, f"{fmt} x{nch}: three packets, 7 frames written")
            ring = e.r64(c + CAP_RING)
            got = [struct.unpack(f"<{nch}f", e.mem_read(ring + 4 * nch * ((w0 + i) % KRING), 4 * nch)) for i in range(7)]
            want = [tuple(dev.expect(v) for v in fr) for fr in frames[:3]] + [tuple([0.0] * nch)] * 2 + \
                   [tuple(dev.expect(v) for v in fr) for fr in frames[3:]]
            t.eq([list(x) for x in got], [list(x) for x in want], f"{fmt} x{nch}: the ring holds the samples as floats, the silent packet as 0")
    # a format it can't read, and mono on both sides
    bad = Device("{fmt}.bad", "Fmt", fmt="bad")
    au.devices = [bad]
    n = len(r.log)
    manage()
    t.check(not feeding(0) and any("deck 1's input is off: 'Fmt': its audio isn't in a format this can read" in l for l in r.log[n:]),
            "an 8-bit device: not opened, 'its audio isn't in a format this can read'", r.log[n:])
    t.eq(rb.take_shown()[-1:], [(0, 0)], "...deck 1 detached: updateDVSThroughMode(deck 0, false)")
    t.eq(e.r8(rb.units[0] + UNIT_THRU), 0, "...and rekordbox's thru off again (enableInputThru false)")
    t.check(any("deck 1 plays tracks again" in l for l in r.log[n:]), "...logged: plays tracks again")

    mono = Device("{mono}", "Mono In", rate=44100, nch=1, fmt="f32")
    au.devices = [mono]
    rb.config.update({"linein_1": "Mono In", "linein_1_ch": "1"})
    manage()
    t.eq((e.r32(deck(0) + R + 24), e.r32(deck(0) + R + 28)), (0, 0), "linein_1_ch=1 on a mono input: channel 1 on both sides")
    k[0] = 0
    L, Rr, fl = play(mono, 0.4)
    t.check(any(v != 0 for v in L) and L == Rr, "mono: the same audio on left and right")
    rb.config["linein_1_ch"] = "3-4"
    manage()
    t.eq((e.r32(deck(0) + R + 24), e.r32(deck(0) + R + 28)), (0, 0), "'3-4' on a mono device: its first channel(s) instead")
    rb.config.pop("linein_1_ch")

    # ---- 6. the device goes away, comes back; lost stream; "#2"; a failed open's back-off ---------------------------
    rb.config.update({"linein_1": "USB AUDIO CODEC", "linein_2": "USB AUDIO CODEC #2", "linein_2_on": "1"})
    turntable.present = second.present = True
    au.devices = [second, turntable]
    manage()
    t.check(cap_of(0) and cap_of(1) and cap_of(0) != cap_of(1), "two decks, two captures")
    t.eq(msvc.read_string(e, cap_of(1) + 8), second.uid, "'USB AUDIO CODEC #2': the second of that name by UID")
    t.eq(e.f64(cap_of(1) + CAP_RATE), 44100.0, "...44.1 kHz 16-bit")
    rb.take_shown()
    nthreads = len([x for x in e.threads.values() if x.state != "done"])
    t.eq(nthreads, 2, "two capture threads running")
    # unplugged
    second.present = False
    n = len(r.log)
    old = second.stream
    manage()
    log = r.log[n:]
    t.check(not feeding(1) and cap_of(1) == 0, "deck 2's device gone: deck 2 detached")
    t.eq([x for x in rb.take_shown() if x[0] == 1], [(1, 0)], "...updateDVSThroughMode(deck 1, false)")
    t.check(any("deck 2 plays tracks again ('USB AUDIO CODEC' went away or changed)" in l for l in log), "...logged", log)
    t.check(any("stopped listening to 'USB AUDIO CODEC'" in l for l in log), "...its capture stopped", log)
    t.check(old.stopped, "...the stream stopped (IAudioClient::Stop)")
    t.eq(len([x for x in e.threads.values() if x.state != "done"]), 1, "...its thread ended (joined)")
    t.check(feeding(0), "deck 1 plays on")
    second.present = True
    manage()
    t.check(feeding(1) and cap_of(1), "plugged in again: deck 2 plays it again")
    t.eq([x for x in rb.take_shown() if x[0] == 1], [(1, 1)], "...INPUT THROUGH shown again")
    # a stream that stops delivering (AUDCLNT_E_DEVICE_INVALIDATED) while still listed
    n = len(r.log)
    second.stream.invalid = True
    au.wake(second)
    t.check(e.r8(cap_of(1) + CAP_LOST), "a stream failing with DEVICE_INVALIDATED: the capture is marked lost")
    t.check(any("'USB AUDIO CODEC' stopped delivering audio (0x88890004)" in l for l in r.log[n:]), "...logged", r.log[n:])
    t.eq(len([x for x in e.threads.values() if x.state != "done"]), 1, "...its thread ended by itself")
    opens = second.opens
    manage()
    t.eq(second.opens, opens + 1, "manage: the lost capture is let go of and the device opened anew")
    t.check(feeding(1) and not e.r8(cap_of(1) + CAP_LOST), "...deck 2 plays the new one")
    # a device that can't be opened: tried again after 10 s, then 20, 40, 80, 120
    busy = Device("{busy}", "Busy One", init_hr=AUDCLNT_E_DEVICE_IN_USE)
    au.devices = [second, turntable, busy]
    rb.config["linein_3"] = "Busy One"
    n = len(r.log)
    attempts = []
    for sec in range(0, 300):
        o = busy.opens
        manage()
        if busy.opens != o:
            attempts.append(sec)
    gaps = [b - a for a, b in zip(attempts, attempts[1:])]
    # (manage() once a simulated second: a wait of exactly 80 s can be missed by a rounding of the clock, then it's 81)
    t.check(attempts[:1] == [0] and len(gaps) == 5 and all(w <= g <= w + 1 for g, w in zip(gaps, [10, 20, 40, 80, 120])),
            "a failed open: tried again after 10, 20, 40, 80, then every 120 s (not every second)", attempts)
    t.check(any("deck 3's input is off: 'Busy One': another program has it to itself (exclusive mode)" in l for l in r.log[n:]), "...logged once with why", r.log[n:])
    t.eq(len([l for l in r.log[n:] if "deck 3's input is off" in l]), 1, "...only once")
    t.eq(len([x for x in e.threads.values() if x.state != "done"]), 2, "...its threads all ended (joined)")
    busy.present = False
    manage()
    busy.present = True
    o = busy.opens
    manage()
    t.eq(busy.opens, o + 1, "unplugged and back: tried again at once")
    # microphone privacy switch off: E_ACCESSDENIED
    denied = Device("{denied}", "Denied", init_hr=E_ACCESSDENIED)
    au.devices = [second, turntable, denied]
    rb.config["linein_3"] = "Denied"
    n = len(r.log)
    manage()
    t.check(any("Settings > Privacy & security > Microphone" in l for l in r.log[n:]), "E_ACCESSDENIED: the log says where to allow it", r.log[n:])
    t.check(any("deck 3's input is off: 'Denied': Windows doesn't let rekordbox use audio inputs" in l for l in r.log[n:]), "...and why deck 3 is off")
    t.eq(len(denied.inits), 1, "...not tried again as an ordinary stream (raw refused the same way)")
    rb.config.pop("linein_3")
    # a device without raw mode: opened as an ordinary stream
    noraw = Device("{noraw}", "No Raw", raw=False)
    au.devices = [second, turntable, noraw]
    rb.config["linein_4"] = "No Raw"
    manage()
    t.check(feeding(3) and noraw.stream and noraw.stream.started and not noraw.stream.raw, "no raw mode: opened as an ordinary stream")
    t.eq(len(noraw.streams), 2, "...on a second IAudioClient")
    # raw refused at Initialize (not access / in use): again without
    rawfail = Device("{rawfail}", "Raw Fails", init_hr=lambda raw: 0x88890008 if raw else 0)   # AUDCLNT_E_UNSUPPORTED_FORMAT
    au.devices = [second, turntable, rawfail]
    rb.config["linein_4"] = "Raw Fails"
    manage()
    t.check(feeding(3) and rawfail.stream.started and not rawfail.stream.raw and [i[3] for i in rawfail.inits] == [True, False],
            "raw refused at Initialize: initialised again as an ordinary stream", rawfail.inits)
    # not connected, track loaded, switched off
    rb.config["linein_4"] = "Nothing Like It"
    n = len(r.log)
    manage()
    t.check(not feeding(3) and any("deck 4 plays tracks again ('Nothing Like It' isn't connected)" in l for l in r.log[n:]),
            "a device that isn't connected: deck 4 detached, logged", r.log[n:])
    rb.config.pop("linein_4")
    rb.loaded.add(1)
    n = len(r.log)
    manage()
    t.check(not feeding(1) and any("deck 2 plays tracks again (a track is loaded)" in l for l in r.log[n:]), "linein_empty: a track loaded on deck 2 takes it back", r.log[n:])
    t.check(any("stopped listening to 'USB AUDIO CODEC'" in l for l in r.log[n:]), "...and the device nobody reads is stopped")
    rb.loaded.discard(1)
    rb.config["linein_empty"] = "0"
    rb.loaded.add(1)
    manage()
    t.check(feeding(1), "linein_empty=0: deck 2 plays its input with a track loaded")
    rb.loaded.discard(1)
    rb.config["linein_1_on"] = "0"
    n = len(r.log)
    manage()
    t.check(not feeding(0) and any("deck 1 plays tracks again (switched off)" in l for l in r.log[n:]), "linein_1_on=0: deck 1 detached", r.log[n:])
    t.eq(why(0), "switched off", "...why: switched off")

    # ---- everything off: every thread ends, every COM object is released -----------------------------------------------
    for k_ in [k_ for k_ in rb.config if k_.startswith("linein_")]:
        rb.config.pop(k_)
    manage()
    manage(10)
    t.eq([x for x in e.threads.values() if x.state != "done"], [], "nothing asked for: every capture thread ended")
    sets = sorted(x[2] for x in au.mmcss if x[0] == "set")
    reverts = sorted(x[2] for x in au.mmcss if x[0] == "revert")
    t.check(sets and sets == reverts, "every capture thread reverted its MMCSS class on its way out", au.mmcss[-4:])
    t.eq(au.calls.count(("FreeLibrary", 0xA7A70000)), len(sets), "...and freed avrt.dll")
    t.eq(au.live(), [], "every COM object the mod got is released")
    t.eq(sorted(set(au.taskmem) - set(au.freed)), [], "every CoTaskMemAlloc'd format and ID freed")
    t.eq([feeding(i) for i in range(4)], [0, 0, 0, 0], "no deck fed")


def freq_of(xs, rate):
    """Frequency from the rising zero crossings (linearly interpolated)."""
    cross = []
    for j in range(1, len(xs)):
        if xs[j - 1] < 0 <= xs[j]:
            cross.append(j - 1 + (-xs[j - 1]) / (xs[j] - xs[j - 1]))
    if len(cross) < 3:
        return None
    return (len(cross) - 1) / ((cross[-1] - cross[0]) / rate)
