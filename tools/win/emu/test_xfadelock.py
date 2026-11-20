"""xfadelock (mods/xfadelock.cpp, mods/xfadelock_win.cpp) on Windows, in the emulator.

Real code: the DLL's init (vtableSlot's .rdata/RTTI scan of the real rekordbox.exe, the slot swaps), the
replacements (mixerSetCtl, midiL, sliderChangedL), the lock's click path (the button's window procedure, clicked,
setLocked), tick and the button placement (buttonRect, update), rbm::engine, dynamic_cast (the DLL's
__RTDynamicCast on rekordbox's RTTI), and rekordbox's own DjMixerUnit::setCrossFaderPosition (the original the
mixer replacement calls).
Python stand-ins for rekordbox: the on-screen CrossFaderComp's originals (eventAbsoluteValueChanged,
sliderValueChanged: they store the position and set the engine, as rekordbox does), DjEngineIF::setCrossFader (it
calls the mixer through its patched vtable slot), juce::Slider / juce::Component, MainComponent::getInstance, the
DjEngineIF singleton, the mixer's input graphs (they record what the audio gets) and its MultiTimer calls.
"""
import struct

import msvc
from emu import F32

M = "xfadelock.cpp"
FREE, LOCKED, PICKUP = 0, 1, 2
# (method, class vtable, replacement, original's static, subobject offset, slot) as tools/win/verify/xfadelock.out.json
SLOTS = [
    ("__ZN11DjMixerUnit21setCrossFaderPositionEf", "__ZTI11DjMixerUnit", "A_mixerVt", "mixerSetCtl", "o_mixerSetCtl", 344, 13),
    ("__ZN14CrossFaderComp25eventAbsoluteValueChangedEPN6djplay12DeviceObjectEi", "__ZTI14CrossFaderComp", "A_compVt", "midiL", "o_midiL", 256, 3),
    ("__ZN14CrossFaderComp18sliderValueChangedEPN4juce6SliderE", "__ZTI14CrossFaderComp", "A_compVt", "sliderChangedL", "o_sliderL", 240, 1),
]
DEVICE, SLIDER, AT = 0x298, 0x230, 0x2D8   # CrossFaderComp fields (whole object), the verified Windows offsets
WM_LBUTTONDOWN = 0x201


class Rekordbox:
    """The Python side of rekordbox: a crossfader component, its slider, the engine and the mixer."""

    def __init__(self, t):
        self.t = t
        r = self.r = t.r
        e, exe = r.emu, r.exe
        self.e = e
        A = lambda n: exe.base + r.table_value("addr", n)
        self.A = A
        self.audio = []          # floats the mixer's input graphs got (what you'd hear)
        self.engine_calls = []   # DjEngineIF::setCrossFader values
        self.midi_orig = []      # rekordbox's eventAbsoluteValueChanged calls: (listener, obj, value)
        self.slider_orig = []    # rekordbox's sliderValueChanged calls: (listener, slider, proportion)
        self.set_value = []      # juce::Slider::setValue: (slider, value, notification)
        self.timers = []
        # the mixer (a DjMixerUnit with its real vtables) and one input graph
        self.mixer_vts = msvc.class_vtables(exe, A("__ZTI11DjMixerUnit"))
        self.mixer = msvc.fake_instance(e, exe, None, 0x800, self.mixer_vts)
        self.ctl = self.mixer + 344
        graph = e.alloc(0x500)
        e.w64(graph + 0x420, e.vtable([lambda e: 0, self._graph_set]))
        vec = e.alloc(8)
        e.w64(vec, graph)
        e.w64(self.ctl + 0x158, vec)
        e.w64(self.ctl + 0x160, vec + 8)
        # the crossfader component, its slider, the crossfader's DeviceComponent and an assign button's
        self.comp_vts = msvc.class_vtables(exe, A("__ZTI14CrossFaderComp"))
        self.comp = msvc.fake_instance(e, exe, None, 0x400, self.comp_vts)
        tdc, tdo = A("__ZTIN6djplay15DeviceComponentE"), A("__ZTIN6djplay12DeviceObjectE")
        self.dc_vts = msvc.class_vtables(exe, tdc)
        self.obj_in_dc = msvc.base_offset(exe, tdc, tdo)
        self.device = msvc.fake_instance(e, exe, None, 0x200, self.dc_vts)
        self.assign = msvc.fake_instance(e, exe, None, 0x200, self.dc_vts)
        self.slider = e.alloc(0x300)
        self.lo, self.hi = -1.0, 1.0
        self.value = 0.0
        e.w64(self.comp + DEVICE, self.device)
        e.w64(self.comp + SLIDER, self.slider)
        e.wf64(self.comp + AT, 0.5)
        # the component tree (screen bounds in JUCE logical pixels)
        self.main = e.alloc(0x100)
        self.other = e.alloc(0x100)
        self.assignB = e.alloc(0x100)
        self.children = {self.main: [self.other, self.comp], self.comp: [self.slider, self.assignB]}
        self.bounds = {self.comp: (500, 900, 300, 40), self.slider: (540, 910, 200, 20), self.assignB: (760, 905, 30, 30),
                       self.other: (0, 0, 10, 10), self.main: (0, 0, 1600, 950)}
        self.engine = e.alloc(0x100)
        self.main_instance = self.main
        hook = e.hook
        hook(A("__ZN13MainComponent11getInstanceEv"), lambda e: self.main_instance)
        hook(A("__ZNK4juce9Component21getNumChildComponentsEv"), lambda e: len(self.children.get(e.arg(0), [])))
        hook(A("__ZNK4juce9Component17getChildComponentEi"), lambda e: self.children.get(e.arg(0), [0])[e.arg(1) & 0xFFFFFFFF])
        hook(A("__ZNK4juce9Component15getScreenBoundsEv"), self._bounds)
        hook(A("__ZNK4juce9Component9isShowingEv"), lambda e: 1)
        hook(A("__ZNK4juce6Slider8getValueEv"), lambda e: self.value)
        hook(A("__ZNK4juce6Slider10getMinimumEv"), lambda e: self.lo)
        hook(A("__ZNK4juce6Slider10getMaximumEv"), lambda e: self.hi)
        hook(A("__ZN4juce6Slider8setValueEdNS_16NotificationTypeE"), self._set_value)
        hook(A("__ZN4juce15SingletonHolderIN8djengine10DjEngineIFENS_15CriticalSectionELb0EE3getEv"), lambda e: self.engine)
        hook(A("__ZN8djengine10DjEngineIF13setCrossFaderEd"), lambda e: self.set_crossfader(e.farg(1)))
        hook(A("__ZN14CrossFaderComp25eventAbsoluteValueChangedEPN6djplay12DeviceObjectEi"), self._midi_orig)
        hook(A("__ZN14CrossFaderComp18sliderValueChangedEPN4juce6SliderE"), self._slider_orig)
        self._hook_timers(A("__ZN11DjMixerUnit21setCrossFaderPositionEf"))

    def _hook_timers(self, fn):
        """rekordbox's mixer starts / stops a MultiTimer on [this+0x38] at the ends: recorded instead of run."""
        import capstone
        md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
        prev = []
        for i in md.disasm(self.e.mem_read(fn, 0x200), fn):
            if i.mnemonic == "ret":
                break
            if i.mnemonic == "call" and i.op_str.startswith("0x") and any(p.startswith("lea rcx") for p in prev[-4:]):
                self.e.hook(int(i.op_str, 16), lambda e: self.timers.append(e.arg(1)) or 0, "MultiTimer")
            prev.append(f"{i.mnemonic} {i.op_str}")

    # rekordbox stand-ins
    def _graph_set(self, e):
        self.audio.append(e.farg32(1))
        return 0

    def _bounds(self, e):
        e.mem_write(e.arg(1), struct.pack("<iiii", *self.bounds.get(e.arg(0), (0, 0, 0, 0))))
        return e.arg(1)

    def proportion(self):
        return (self.value - self.lo) / (self.hi - self.lo)

    def _set_value(self, e):
        s, v, n = e.arg(0), e.farg(1), e.arg(2) & 0xFFFFFFFF
        self.set_value.append((s, v, n))
        changed = v != self.value
        self.value = v
        if n != 0 and changed:   # juce: listeners hear of it (sync here) -> CrossFaderComp::sliderValueChanged
            self.vcall(self.comp + 240, 1, self.comp + 240, s)

    def set_crossfader(self, v):
        """DjEngineIF::setCrossFader: the mixer control interface's setCrossFaderPosition (patched slot 13)."""
        self.engine_calls.append(v)
        self.vcall(self.ctl, 13, self.ctl, F32(v))
        return 0

    def _midi_orig(self, e):
        this, obj, value = e.arg(0), e.arg(1), e.arg(2) & 0xFFFFFFFF
        self.midi_orig.append((this, obj, value))
        if msvc_dyn_is(self, obj, self.device):  # the crossfader: as rekordbox, the engine and its own copy
            v = value / 16383
            self.set_crossfader(v)
            e.wf64(this - 256 + AT, v)
            self.value = self.lo + v * (self.hi - self.lo)   # (handleAsyncUpdate: the slider follows, silently)
        return None

    def _slider_orig(self, e):
        this, s = e.arg(0), e.arg(1)
        p = self.proportion()
        self.slider_orig.append((this, s, p))
        e.wf64(this - 240 + AT, p)
        self.set_crossfader(p)
        return None

    def vcall(self, obj, slot, *args):
        return self.e.call(msvc.vcall_target(self.e, obj, slot), *args)

    # what rekordbox's callers do
    def mixer_set(self, v):
        self.audio.clear()
        self.vcall(self.ctl, 13, self.ctl, F32(v))
        return self.audio[-1] if self.audio else None

    def midi(self, value, assign=False):
        """A controller's crossfader (or an assign button's) message: CrossFaderComp's device listener, slot 3."""
        dc = self.assign if assign else self.device
        n = len(self.midi_orig)
        with self.e.trace(self.comp, 0x400) as tr:
            self.vcall(self.comp + 256, 3, self.comp + 256, dc + self.obj_in_dc, value)
        return len(self.midi_orig) > n, tr

    def drag(self, proportion):
        """The on-screen fader dragged: juce sets the slider, then calls the slider listener, slot 1."""
        self.value = self.lo + proportion * (self.hi - self.lo)
        n = len(self.slider_orig)
        with self.e.trace(self.comp, 0x400) as tr:
            self.vcall(self.comp + 240, 1, self.comp + 240, self.slider)
        return len(self.slider_orig) > n, tr

    def at(self):
        return self.e.f64(self.comp + AT)


def msvc_dyn_is(rb, obj, dc):
    return obj == dc + rb.obj_in_dc


def run(t):
    r = t.r
    e, exe = r.emu, r.exe
    S = lambda n: r.sym(n, M)
    A = lambda n: exe.base + r.table_value("addr", n)

    # ---- the table: what the DLL resolved -------------------------------------------------------------------
    for addr, name in [("A_mixerVt", "__ZTV11DjMixerUnit"), ("A_compVt", "__ZTV14CrossFaderComp"),
                       ("A_mixerSet", SLOTS[0][0]), ("A_compMidi", SLOTS[1][0]), ("A_compSlider", SLOTS[2][0])]:
        t.eq(r.addr(addr, M), A(name), f"{addr} resolved from the table")
    for name, want in [("O_xfDevice", DEVICE), ("O_xfSlider", SLIDER), ("O_xfAt", AT)]:
        t.eq(r.offset(name, M), (want, True), f"{name} = {want:#x} from the table")

    # ---- the slots, found in Python from the exe's RTTI (independent of the DLL) ------------------------------
    expect = {}
    for fn_name, td_name, vt_addr, mine, orig, delta, slot in SLOTS:
        fn = A(fn_name)
        vts = msvc.class_vtables(exe, A(td_name))
        t.eq(vts.get(0), r.addr(vt_addr, M) + 0x10, f"{td_name}'s primary vtable is {vt_addr} + 0x10")
        t.eq(r.table_value("adj", fn_name), delta, f"table adj of {fn_name[:40]}...")
        where = {off: msvc.vtable_slots(exe, vt, fn) for off, vt in vts.items()}
        where = {off: s for off, s in where.items() if s}
        t.eq(where, {delta: [slot]}, f"{fn_name[:40]}... is only in slot {slot} of the subobject-{delta} vtable")
        expect[mine] = (vts[delta] + 8 * slot, fn)

    # ---- vtableSlot (the DLL's) before init: the right slot, and nothing for another subobject or class --------
    # (clang dropped the Addr& for its value: vtableSlot(vt value, fn, delta) is how init calls it in this build)
    vslot = S("vtableSlot")
    for fn_name, td_name, vt_addr, mine, orig, delta, slot in SLOTS:
        fn = A(fn_name)
        t.eq(e.call(vslot, r.addr(vt_addr, M), fn, delta), expect[mine][0], f"vtableSlot finds {mine}'s slot")
        t.eq(e.call(vslot, r.addr(vt_addr, M), fn, 0), 0, f"vtableSlot: nothing at subobject 0 for {mine}")
    t.eq(e.call(vslot, r.addr("A_compVt", M), A(SLOTS[0][0]), 344), 0, "vtableSlot: the mixer's method isn't CrossFaderComp's")

    # ---- init: the swaps -----------------------------------------------------------------------------------------
    e.call(S("init"))
    t.check(any("xfadelock: ready" in l for l in r.log), "init logs ready", r.log[-3:])
    t.check(not any("could not patch" in l for l in r.log), "init patched all three", r.log[-3:])
    t.eq(e.r64(S("kDeviceListener")), 256, "kDeviceListener (thisDelta)")
    t.eq(e.r64(S("kSliderListener")), 240, "kSliderListener (thisDelta)")
    for mine, (slot_addr, fn) in expect.items():
        t.eq(e.r64(slot_addr), S(mine), f"slot now holds {mine}")
    for _, _, _, mine, orig, _, _ in SLOTS:
        t.eq(e.r64(S(orig)), expect[mine][1], f"{orig} = rekordbox's original")

    rb = Rekordbox(t)

    # ---- unlocked: everything passes through ----------------------------------------------------------------------
    t.near(rb.mixer_set(0.2), 0.2, "unlocked: the mixer gets 0.2 (through rekordbox's original)")
    passed, tr = rb.midi(3277)
    t.check(passed, "unlocked: controller move reaches rekordbox")
    t.eq(tr.read_offsets(), [DEVICE], "midiL reads only the device field (+0x298)")
    t.eq(tr.write_offsets(), [], "midiL writes nothing in the component")
    t.near(e.f64(S("g_hw")), 3277 / 16383, "the controller's position is remembered")
    t.near(rb.audio[-1], 3277 / 16383, "the mixer follows the controller", 1e-6)
    passed, tr = rb.drag(0.3)
    t.check(passed, "unlocked: on-screen drag reaches rekordbox")
    t.eq(tr.read_offsets(), [], "unlocked sliderChangedL doesn't read the component")
    t.near(rb.proportion(), 0.3, "unlocked: the slider stays where dragged")

    # ---- lock: a click on the button --------------------------------------------------------------------------------
    e.windows[0x500] = {"title": "rekordbox", "rect": (100, 50, 1700, 1000), "parent": None, "visible": True}
    e.call(S("tick"))
    buttons = [h for h, w in e.windows.items() if w.get("class") == "platendoos.xfadelock"]
    t.eq(len(buttons), 1, "tick made the button window")
    btn = buttons[0] if buttons else None
    if btn:
        t.eq(e.windows[btn]["rect"], (640, 863, 654, 877), "button right of the slider, centred on it (client coords)")
        t.check(e.windows[btn]["visible"], "button shown")
    rb.engine_calls.clear()
    rb.audio.clear()
    if btn:
        e.send_message(btn, WM_LBUTTONDOWN)
    else:
        e.call(S("clicked"))
    t.eq(e.r32(S("g_state")), LOCKED, "click: locked")
    t.check(any("locked at the centre" in l for l in r.log), "lock logged")
    t.near(rb.proportion(), 0.5, "lock: on-screen slider to the centre")
    t.check(rb.set_value and rb.set_value[-1][2] == 2, "lock: slider set with sendNotificationSync", rb.set_value[-1:])
    t.check(rb.engine_calls and abs(rb.engine_calls[-1] - 0.5) < 1e-9, "lock: DjEngineIF::setCrossFader(0.5)", rb.engine_calls)
    t.check(rb.audio and all(abs(a - 0.5) < 1e-7 for a in rb.audio), "lock: the mixer at the centre", rb.audio)

    # ---- locked ------------------------------------------------------------------------------------------------------
    t.near(rb.mixer_set(0.9), 0.5, "locked: mixer asked for 0.9 gets 0.5")
    t.near(rb.mixer_set(0.1), 0.5, "locked: mixer asked for 0.1 gets 0.5")
    passed, tr = rb.midi(16383)
    t.check(not passed, "locked: controller move dropped")
    t.eq(tr.read_offsets(), [DEVICE], "locked midiL reads only +0x298")
    t.eq(tr.write_offsets(), [], "locked midiL writes nothing in the component")
    t.near(e.f64(S("g_hw")), 1.0, "locked: controller position still remembered (for the pickup)")
    passed, _ = rb.midi(0, assign=True)
    t.check(passed, "locked: an assign button's message still passes")
    rb.set_value.clear()
    passed, tr = rb.drag(0.8)
    t.check(passed, "locked: the drag still reaches rekordbox's listener")
    t.check(SLIDER in tr.read_offsets(), "locked sliderChangedL reads the slider field (+0x230)", tr.read_offsets())
    t.check(rb.set_value and rb.set_value[0][2] == 0, "locked drag: slider put back silently (dontSendNotification)", rb.set_value)
    t.near(rb.slider_orig[-1][2], 0.5, "locked drag: rekordbox reads the centre")
    t.near(rb.audio[-1], 0.5, "locked drag: the mixer stays at the centre")
    rb.value = rb.lo + 0.7 * (rb.hi - rb.lo)   # something else moved the on-screen fader (automix...)
    e.call(S("tick"))
    t.near(rb.proportion(), 0.5, "locked tick: on-screen fader back at the centre")

    # ---- unlock with the controller at 100%: pickup -----------------------------------------------------------------
    if btn:
        e.send_message(btn, WM_LBUTTONDOWN)
    else:
        e.call(S("clicked"))
    t.eq(e.r32(S("g_state")), PICKUP, "unlock, controller away from the centre: pickup")
    t.check(any("once it passes the centre (the controller's is at 100%)" in l for l in r.log), "pickup logged", r.log[-1:])
    t.near(rb.mixer_set(0.3), 0.3, "pickup: the mixer passes again (only the controller waits)")
    rb.mixer_set(0.5)
    passed, tr = rb.midi(int(0.9 * 16383))
    t.check(not passed, "pickup: 100% -> 90% dropped (hasn't reached the centre)")
    t.eq(tr.read_offsets(), [DEVICE, AT], "pickup midiL reads +0x298 and the position +0x2d8")
    passed, _ = rb.midi(int(0.4 * 16383))
    t.check(passed, "pickup: 90% -> 40% crosses the centre: picked up")
    t.eq(e.r32(S("g_state")), FREE, "picked up: free")
    t.check(any("picked up at 40%" in l for l in r.log), "pickup logged", r.log[-1:])
    passed, _ = rb.midi(int(0.3 * 16383))
    t.check(passed, "free: follows the controller")
    t.near(rb.audio[-1], int(0.3 * 16383) / 16383, "free: the mixer follows")

    # ---- pickup follows an on-screen drag (where rekordbox has it now) ----------------------------------------------
    e.call(S("clicked"))                      # lock (controller at 30%)
    rb.midi(int(0.9 * 16383))                 # controller to 90% while locked
    e.call(S("clicked"))                      # unlock: pickup
    t.eq(e.r32(S("g_state")), PICKUP, "unlock with the controller at 90%: pickup")
    rb.drag(0.75)                              # the on-screen fader dragged to 75%
    t.near(rb.at(), 0.75, "rekordbox's position is the drag's")
    passed, _ = rb.midi(int(0.85 * 16383))
    t.check(not passed, "pickup at 75%: 85% not there yet")
    passed, _ = rb.midi(int(0.76 * 16383))
    t.check(passed, "pickup at 75%: 76% is there (within 2%)")
    t.eq(e.r32(S("g_state")), FREE, "picked up at the dragged position")

    # ---- unlock with the controller at the centre: free at once -----------------------------------------------------
    e.call(S("clicked"))
    rb.midi(8192)
    e.call(S("clicked"))
    t.eq(e.r32(S("g_state")), FREE, "unlock with the controller at the centre: free at once")
    t.check(any("unlocked: the crossfader follows the fader again" in l for l in r.log), "free unlock logged", r.log[-1:])

    # ---- the mod switched off (xfadelock=0): a lock left on does nothing ---------------------------------------------
    e.call(S("clicked"))
    t.eq(e.r32(S("g_state")), LOCKED, "locked again")
    e.w8(S("g_on"), 0)
    t.near(rb.mixer_set(0.2), 0.2, "mod off: the mixer passes")
    passed, _ = rb.midi(1000)
    t.check(passed, "mod off: controller passes")
    rb.set_value.clear()
    rb.drag(0.25)
    t.eq(rb.set_value, [], "mod off: a drag isn't put back")
    e.w8(S("g_on"), 1)

    # ---- the button at 150% display scale -----------------------------------------------------------------------------
    if btn:
        e.dpi = 144
        e.call(S("tick"))
        t.eq(e.windows[btn]["rect"], (1010, 1320, 1031, 1341), "button at 150%: JUCE logical pixels scaled")
        e.dpi = 96
