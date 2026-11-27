"""MSVC x64 ABI helpers for emulated images: RTTI (complete object locators, vtables per subobject), fake instances
of real classes, std::string."""
import struct

from emu import EmuError


def _file_image(img):
    if not hasattr(img, "_mapped"):
        img._mapped = img.pe.get_memory_mapped_image()
    return img._mapped


def _find_all(data, needle, start, end, step):
    out, i = [], data.find(needle, start, end)
    while i >= 0:
        if (i - start) % step == 0:
            out.append(i)
        i = data.find(needle, i + 1, end)
    return out


def complete_object_locators(img, type_descriptor):
    """The COLs naming type_descriptor (an address in img), as the file has them: [(col, offset, cdOffset)]."""
    data = _file_image(img)
    lo, hi = (a - img.base for a in img.section(".rdata"))
    td_rva = type_descriptor - img.base
    out = []
    for i in _find_all(data, struct.pack("<I", td_rva), lo, hi, 4):
        c = i - 12
        sig, off, cd, td, chd, self_ = struct.unpack_from("<IIIIII", data, c)
        if sig == 1 and self_ == c and td == td_rva:
            out.append((img.base + c, off, cd))
    return out


def type_descriptor(img, decorated):
    """The RTTI type descriptor (in .data) named decorated, e.g. ".?AVLabel@juce@@". A name ending in a single '@'
    matches a class in an anonymous namespace (".?AVVerticalSeparator@" -> ".?AVVerticalSeparator@?A0x0a54aa24@@");
    there must be exactly one."""
    data = _file_image(img)
    lo, hi = (a - img.base for a in img.section(".data"))
    exact = not decorated.endswith("@") or decorated.endswith("@@")
    needle = decorated.encode() + (b"\0" if exact else b"?A0x")
    hits = []
    i = data.find(needle, lo, hi)
    while i >= 0:
        if i % 8 == 0:   # (name at +16 of an 8-aligned descriptor)
            hits.append(img.base + i - 16)
        i = data.find(needle, i + 1, hi)
    if len(hits) != 1:
        raise EmuError(f"type descriptor {decorated}: {len(hits)} found")
    return hits[0]


def class_vtables(img, type_descriptor):
    """{subobject offset: vtable address point} of the class (from the file: unpatched)."""
    data = _file_image(img)
    lo, hi = (a - img.base for a in img.section(".rdata"))
    out = {}
    for col, off, _ in complete_object_locators(img, type_descriptor):
        hits = _find_all(data, struct.pack("<Q", col), lo, hi, 8)
        if len(hits) == 1:
            out[off] = img.base + hits[0] + 8
        elif hits:
            raise EmuError(f"COL {col:#x} is before {len(hits)} vtables")
    return out


def vtable_slots(img, vtable, fn, limit=512):
    """The slot indexes of vtable (address point, file contents) holding fn; the vtable ends at the first word that
    doesn't point into .text."""
    data = _file_image(img)
    text = img.section(".text")
    out = []
    for i in range(limit):
        v = struct.unpack_from("<Q", data, vtable - img.base + 8 * i)[0]
        if not text[0] <= v < text[1]:   # (the next vtable's COL pointer, or other data: the end)
            break
        if v == fn:
            out.append(i)
    return out


def fake_instance(emu, img, type_descriptor, size, vtables=None):
    """A zeroed object of size bytes with the class's real vtables (from emulated memory: patched slots stay patched)
    at their subobject offsets, so dynamic_cast and virtual calls see a real instance of the class."""
    obj = emu.alloc(size)
    for off, vt in (vtables or class_vtables(img, type_descriptor)).items():
        if off + 8 <= size:
            emu.w64(obj + off, vt)
    return obj


def vcall_target(emu, obj, slot):
    """The function a virtual call of slot on obj (a subobject pointer) reaches, from emulated memory."""
    return emu.r64(emu.r64(obj) + 8 * slot)


# std::string (MSVC x64): union {char buf[16]; char* ptr} at 0, size at 16, capacity at 24 (15: the inline buffer).
def read_string(emu, s):
    size, cap = emu.r64(s + 16), emu.r64(s + 24)
    p = emu.r64(s) if cap > 15 else s
    return emu.mem_read(p, size).decode("utf-8", errors="replace")


def write_string(emu, s, text):
    b = text.encode() if isinstance(text, str) else bytes(text)
    if len(b) <= 15:
        emu.mem_write(s, b.ljust(16, b"\0"))
        cap = 15
    else:
        cap = len(b)
        if cap + 1 >= 4096:  # the STL's big blocks: 32-aligned, the raw pointer in the word before (it checks on free)
            raw = emu.alloc(cap + 1 + 0x27)
            p = (raw + 0x27) & ~0x1F
            emu.w64(p - 8, raw)
        else:
            p = emu.alloc(cap + 1)
        emu.mem_write(p, b + b"\0")
        emu.w64(s, p)
    emu.w64(s + 16, len(b))
    emu.w64(s + 24, cap)


def class_names(img, type_descriptor):
    """The decorated names of the class and all its bases, from its class hierarchy descriptor (the class first)."""
    data = _file_image(img)
    cols = [c for c in complete_object_locators(img, type_descriptor) if c[1] == 0]
    if not cols:
        raise EmuError("no primary COL for the class")
    chd = struct.unpack_from("<I", data, cols[0][0] - img.base + 16)[0]
    _, _, n, bca = struct.unpack_from("<IIII", data, chd)
    out = []
    for i in range(n):
        td = struct.unpack_from("<I", data, struct.unpack_from("<I", data, bca + 4 * i)[0])[0]
        out.append(data[td + 16:data.index(b"\0", td + 16)].decode())
    return out


def base_offset(img, type_descriptor, base_type_descriptor):
    """Where base_type_descriptor's subobject sits in an object of the class (its class hierarchy's mdisp)."""
    data = _file_image(img)
    cols = [c for c in complete_object_locators(img, type_descriptor) if c[1] == 0]
    if not cols:
        raise EmuError("no primary COL for the class")
    chd = struct.unpack_from("<I", data, cols[0][0] - img.base + 16)[0]
    _, _, n, bca = struct.unpack_from("<IIII", data, chd)
    for i in range(n):
        bcd = struct.unpack_from("<I", data, bca + 4 * i)[0]
        td, _, mdisp, pdisp, _ = struct.unpack_from("<IIiii", data, bcd)
        if img.base + td == base_type_descriptor:
            if pdisp != -1:
                raise EmuError("virtual base: not supported")
            return mdisp
    raise EmuError("not a base of the class")
