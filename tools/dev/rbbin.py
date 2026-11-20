"""Reads rekordbox's arm64 binary: its symbols (rekordbox isn't stripped), sections and the few arm64
instructions the mods locate things by. Used by patch_binary.py and the address tools; core/symbols.mm does
the same at runtime.

    b = Binary(open(path, "rb").read())
    b.sym("__ZN2db10DatabaseIF16isCollectionSongERKN4juce6StringERNS_12RowDataTrackE")  -> address
    b.calls(caller, target)          -> addresses of the "bl target" inside caller, in order
    b.jumps(caller, target)          -> addresses of the "b target" (tail calls) inside caller, in order
"""
import bisect
import struct

TEXT_BASE = 0x100000000
LC_SEGMENT_64, LC_SYMTAB, LC_DYSYMTAB, LC_UUID = 0x19, 0x2, 0xB, 0x1B


def arm64_slice(data):
    if struct.unpack_from("<I", data, 0)[0] == 0xFEEDFACF:  # thin
        return 0
    assert struct.unpack_from(">I", data, 0)[0] == 0xCAFEBABE, "expected a fat or arm64 Mach-O binary"
    n = struct.unpack_from(">I", data, 4)[0]
    for i in range(n):
        cpu, _, off, size, _ = struct.unpack_from(">iiIII", data, 8 + 20 * i)
        if cpu == 0x0100000C:
            return off
    raise SystemExit("no arm64 slice")


class Binary:
    def __init__(self, data):
        self.data = data
        self.base = arm64_slice(data)
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

    def calls(self, caller, target):
        return [va for va, w in self.insns(caller) if bl_target(va, w) == target]

    def jumps(self, caller, target):
        return [va for va, w in self.insns(caller) if b_target(va, w) == target]

    def adrs(self, caller, target, reg):
        return [va for va, w in self.insns(caller) if w & 0x9F00001F == 0x10000000 | reg and adr_target(va, w) == target]


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
