"""Reads rekordbox's binary: its symbols (rekordbox isn't stripped), sections and the few instructions the mods
locate things by, in the arm64 slice (the default) or the x86_64 one. Used by patch_binary.py and the address
tools; core/symbols.mm does the same at runtime.

    b = Binary(open(path, "rb").read())               # arm64; Binary(data, CPU_X86_64) for the Intel slice
    b.sym("__ZN2db10DatabaseIF16isCollectionSongERKN4juce6StringERNS_12RowDataTrackE")  -> address
    b.calls(caller, target)          -> addresses of the "bl target" inside caller, in order (x86_64: call rel32)
    b.jumps(caller, target)          -> addresses of the "b target" (tail calls) inside caller, in order (jmp rel32)
    b.adrs(caller, target, reg)      -> "adr xreg, target" (x86_64: "lea <any register>, [rip + target]")
x86_64 code is found by a byte scan for the opcode and the exact rel32, without decoding instructions: for hooks.def's
sites that finds the same ones a disassembler does (docs/intel-mac.md).
"""
import bisect
import struct

TEXT_BASE = 0x100000000
LC_SEGMENT_64, LC_SYMTAB, LC_DYSYMTAB, LC_UUID = 0x19, 0x2, 0xB, 0x1B
CPU_ARM64, CPU_X86_64 = 0x0100000C, 0x01000007
ARCH_NAMES = {CPU_ARM64: "arm64", CPU_X86_64: "x86_64"}


def slices(data):
    """The architectures in the file, in its order: [(cputype, offset of the slice)]."""
    if struct.unpack_from("<I", data, 0)[0] == 0xFEEDFACF:  # thin
        return [(struct.unpack_from("<i", data, 4)[0], 0)]
    assert struct.unpack_from(">I", data, 0)[0] == 0xCAFEBABE, "expected a fat or 64-bit Mach-O binary"
    n = struct.unpack_from(">I", data, 4)[0]
    return [struct.unpack_from(">iiI", data, 8 + 20 * i)[::2] for i in range(n)]


def slice_offset(data, cpu):
    for c, off in slices(data):
        if c == cpu:
            return off
    raise SystemExit("no %s slice" % ARCH_NAMES.get(cpu, hex(cpu)))


def arm64_slice(data):
    return slice_offset(data, CPU_ARM64)


class Binary:
    def __init__(self, data, cpu=CPU_ARM64):
        self.data = data
        self.cpu = cpu
        self.base = slice_offset(data, cpu)
        self.segments, self.sections = {}, {}
        self.uuid = None
        ncmds, self.sizeofcmds = struct.unpack_from("<II", data, self.base + 16)
        off = self.base + 32
        symtab = dysymtab = None
        self.stubs_info = None
        for _ in range(ncmds):
            cmd, size = struct.unpack_from("<II", data, off)
            if cmd == LC_SEGMENT_64:
                seg = data[off + 8:off + 24].rstrip(b"\0").decode()
                vmaddr, vmsize, fileoff, filesize = struct.unpack_from("<QQQQ", data, off + 24)
                self.segments[seg] = (vmaddr, vmsize, fileoff, filesize)
                nsects = struct.unpack_from("<I", data, off + 64)[0]
                for s in range(nsects):
                    so = off + 72 + 80 * s
                    sect = data[so:so + 16].rstrip(b"\0").decode()
                    addr, ssize, soff = struct.unpack_from("<QQI", data, so + 32)
                    self.sections[(seg, sect)] = (addr, ssize, soff)
                    if (seg, sect) == ("__TEXT", "__stubs"):
                        self.stubs_info = (addr, ssize) + struct.unpack_from("<II", data, so + 68)  # reserved1, 2
            elif cmd == LC_SYMTAB:
                symtab = struct.unpack_from("<IIII", data, off + 8)
            elif cmd == LC_DYSYMTAB:
                dysymtab = struct.unpack_from("<II", data, off + 56)  # indirectsymoff, nindirectsyms
            elif cmd == LC_UUID:
                self.uuid = data[off + 8:off + 24].hex().upper()
            off += size
        self.symbols, self.stubs = {}, {}
        starts = set()
        text_lo, text_hi = self.text_range()
        if symtab:
            symoff, nsyms, stroff, _ = symtab
            for i in range(nsyms):
                strx, ntype, sect, _, value = struct.unpack_from("<IBBhQ", data, self.base + symoff + 16 * i)
                if ntype & 0xE0 or (ntype & 0x0E) != 0x0E or not value:  # stabs, or not defined in a section
                    continue
                end = data.index(b"\0", self.base + stroff + strx)
                name = data[self.base + stroff + strx:end].decode("latin1")
                self.symbols.setdefault(name, value)
                if text_lo <= value < text_hi:
                    starts.add(value)
        self.starts = sorted(starts)
        # Imported functions are called through __stubs: stub i belongs to indirect symbol reserved1 + i.
        if symtab and dysymtab and self.stubs_info:
            addr, size, first, stub_size = self.stubs_info
            for i in range(size // stub_size):
                symi = struct.unpack_from("<I", data, self.base + dysymtab[0] + 4 * (first + i))[0]
                if symi & 0xC0000000:
                    continue
                strx = struct.unpack_from("<I", data, self.base + symoff + 16 * symi)[0]
                end = data.index(b"\0", self.base + stroff + strx)
                self.stubs[data[self.base + stroff + strx:end].decode("latin1")] = addr + stub_size * i

    def text_range(self):
        addr, size, _ = self.sections[("__TEXT", "__text")]
        return addr, addr + size

    def fo(self, va):
        for vmaddr, vmsize, fileoff, filesize in self.segments.values():
            if vmaddr <= va < vmaddr + filesize:
                return self.base + fileoff + va - vmaddr
        raise ValueError("address %#x not in the file" % va)

    def u32(self, va):
        return struct.unpack_from("<I", self.data, self.fo(va))[0]

    def sym(self, name):
        """A symbol's address; "stub:_name" is the __stubs entry rekordbox calls an imported function through."""
        return self.stubs.get(name[5:]) if name.startswith("stub:") else self.symbols.get(name)

    def function_end(self, start):
        """Where the function at start ends: the next symbol in __text."""
        i = bisect.bisect_right(self.starts, start)
        return self.starts[i] if i < len(self.starts) else self.text_range()[1]

    def insns(self, fn):
        for va in range(fn, self.function_end(fn), 4):
            yield va, self.u32(va)

    def code(self, fn):
        o = self.fo(fn)
        return self.data[o:o + self.function_end(fn) - fn]

    def calls(self, caller, target):
        if self.cpu == CPU_X86_64:
            return self.rel32(caller, target, 0xE8)
        return [va for va, w in self.insns(caller) if bl_target(va, w) == target]

    def jumps(self, caller, target):
        if self.cpu == CPU_X86_64:
            return self.rel32(caller, target, 0xE9)
        return [va for va, w in self.insns(caller) if b_target(va, w) == target]

    def adrs(self, caller, target, reg):
        if self.cpu == CPU_X86_64:
            return self.leas(caller, target)
        return [va for va, w in self.insns(caller) if w & 0x9F00001F == 0x10000000 | reg and adr_target(va, w) == target]

    # ---- x86_64: byte scans ----

    def rel32(self, caller, target, op):
        """x86_64: every <op> rel32 (E8 call, E9 jmp) inside caller that reaches target."""
        code, out = self.code(caller), []
        i = code.find(op.to_bytes(1, "little"))
        while 0 <= i <= len(code) - 5:
            if caller + i + 5 + struct.unpack_from("<i", code, i + 1)[0] == target:
                out.append(caller + i)
            i = code.find(op.to_bytes(1, "little"), i + 1)
        return out

    def leas(self, caller, target):
        """x86_64: every "lea reg, [rip + target]" (48 / 4C 8D, ModRM mod 00 rm 101) inside caller, at its REX byte."""
        code, out = self.code(caller), []
        i = code.find(b"\x8D", 1)
        while 0 < i <= len(code) - 6:
            if code[i - 1] in (0x48, 0x4C) and code[i + 1] & 0xC7 == 0x05 and \
                    caller + i + 6 + struct.unpack_from("<i", code, i + 2)[0] == target:
                out.append(caller + i - 1)
            i = code.find(b"\x8D", i + 1)
        return out


# ---- arm64 ---------------------------------------------------------------------------------

def sext(v, bits):
    return v - (1 << bits) if v & (1 << (bits - 1)) else v


def bl_target(va, w):
    return va + 4 * sext(w & 0x3FFFFFF, 26) if w & 0xFC000000 == 0x94000000 else None


def b_target(va, w):
    return va + 4 * sext(w & 0x3FFFFFF, 26) if w & 0xFC000000 == 0x14000000 else None


def adr_target(va, w):
    return va + sext(((w >> 5) & 0x7FFFF) << 2 | (w >> 29) & 3, 21)


def adrp_target(va, w):
    return (va & ~0xFFF) + (sext(((w >> 5) & 0x7FFFF) << 2 | (w >> 29) & 3, 21) << 12)


def bl(src, dst):
    return 0x94000000 | (((dst - src) >> 2) & 0x3FFFFFF)


def b(src, dst):
    return 0x14000000 | (((dst - src) >> 2) & 0x3FFFFFF)


def adrp(src, dst, rd):
    imm = ((dst & ~0xFFF) - (src & ~0xFFF)) >> 12
    return 0x90000000 | ((imm & 3) << 29) | (((imm >> 2) & 0x7FFFF) << 5) | rd
