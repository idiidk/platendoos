"""JUCE values in rekordbox.exe's own Windows layout, for fake components: juce::String, juce::var (with rekordbox's
real VariantType tables, so its own var code copies, converts and destroys them) and juce::Value (a
juce::SimpleValueSource with its real vtable holding the var).

    j = juce.Juce(r)                       # r: an initialised rbmods.Rbmods
    s = j.string("Audio")                  # a juce::String (its text pointer), made by rekordbox's own constructor
    j.text(s)                              # -> "Audio"
    vs = j.value_source(True)              # a ValueSource holding var(true): what a Value field points at
    j.set(vs, 0.25)                        # its var now a double
"""
import struct

import capstone

from emu import EmuError

_FLAGS = ("void", "undefined", "int", "int64", "bool", "double", "string", "object", "array", "binary", "method")


class Juce:
    def __init__(self, r):
        self.r, self.e, self.exe = r, r.emu, r.exe
        A = lambda n: self.exe.base + r.table_value("addr", n)
        self.A = A
        self._ctor = A("__ZN4juce6StringC1ENS_16CharPointer_UTF8E")
        self.empty = self.string(None)                 # String(): rekordbox's own empty string
        self.types = self._var_types(A("__ZN4juce3varC1Ev"))
        self.svs_vtable = A("__ZTVN4juce17SimpleValueSourceE+0x10")
        self.strings = {}

    # -- juce::String: a pointer to UTF-8 text, a StringHolder {refCount, allocatedBytes} 16 bytes before it --
    def string(self, text):
        """A juce::String made by rekordbox's String(CharPointer_UTF8) (None: String()). Returns the String's value
        (its text pointer); the reference count is raised so nothing rekordbox's code does frees it."""
        slot = self.e.alloc(8)
        if text is None:
            self.e.call(self.A("__ZN4juce6StringC1Ev"), slot)
        else:
            self.e.call(self._ctor, slot, self.e.alloc_cstr(text))
        p = self.e.r64(slot)
        if text:   # (the empty string is a static: its count is left alone)
            self.e.w32(p - 16, self.e.r32(p - 16) + 1000)
        return p

    def text(self, p):
        return self.e.cstr(p) if p else ""

    def refcount(self, p):
        return self.e.r32(p - 16)

    # -- juce::var: {const VariantType* type; ValueUnion value} --
    def _var_types(self, var_ctor):
        """The VariantType tables (16 flag bytes, then the functions: toInt +0x10 ... toBool +0x30, cleanUp +0x58,
        createCopy +0x60), found next to the void one var::var() stores."""
        md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
        i = next(md.disasm(self.e.mem_read(var_ctor, 8), var_ctor))
        if i.mnemonic != "lea" or "rip" not in i.op_str:
            raise EmuError(f"var::var() isn't lea rax,[rip+x]: {i.mnemonic} {i.op_str}")
        void = i.address + i.size + int(i.op_str.split("rip + ")[1].rstrip("]"), 16)
        text = self.exe.section(".text")
        types = {}
        data = self.e.mem_read(void - 0x4000, 0x8000)
        for o in range(0, 0x8000 - 0x80, 16):
            fl = data[o:o + 16]
            if fl[12:] != b"\0\0\0\0" or any(x > 1 for x in fl[:12]) or sum(fl[:11]) != 1:
                continue
            ptrs = struct.unpack_from("<13Q", data, o + 16)
            if all(text[0] <= p < text[1] for p in ptrs):
                types.setdefault(_FLAGS[fl[:11].index(1)], void - 0x4000 + o)
        if types.get("void") != void:
            raise EmuError("var types: the void one isn't var::var()'s")
        for k in ("bool", "int", "double", "string"):
            if k not in types:
                raise EmuError(f"no {k} VariantType found")
        return types

    def write_var(self, a, value):
        e = self.e
        if value is None:
            e.w64(a, self.types["void"]), e.w64(a + 8, 0)
        elif isinstance(value, bool):
            e.w64(a, self.types["bool"]), e.w64(a + 8, int(value))
        elif isinstance(value, int):
            e.w64(a, self.types["int"]), e.w64(a + 8, value & 0xFFFFFFFF)
        elif isinstance(value, float):
            e.w64(a, self.types["double"]), e.wf64(a + 8, value)
        elif isinstance(value, str):
            e.w64(a, self.types["string"]), e.w64(a + 8, self.string(value))
        else:
            raise EmuError(f"var of {value!r}")

    def read_var(self, a):
        e = self.e
        t = e.r64(a)
        kind = next((k for k, v in self.types.items() if v == t), None)
        return {"void": None, "bool": lambda: bool(e.r8(a + 8)), "int": lambda: e.r32(a + 8) - (1 << 32 if e.r32(a + 8) >> 31 else 0),
                "double": lambda: e.f64(a + 8), "string": lambda: self.text(e.r64(a + 8))}.get(kind, lambda: kind)() if kind != "void" else None

    # -- juce::Value: its first word is the ValueSource* (Value::toString / getValue call its slot 1, getValue) --
    def value_source(self, value):
        """A juce::SimpleValueSource (real vtable; ReferenceCountedObject count at +8 held up; its var at +0x30)."""
        vs = self.e.alloc(0x60)
        self.e.w64(vs, self.svs_vtable)
        self.e.w32(vs + 8, 1000)
        self.write_var(vs + 0x30, value)
        return vs

    def set(self, vs, value):
        self.write_var(vs + 0x30, value)

    def get(self, vs):
        return self.read_var(vs + 0x30)
