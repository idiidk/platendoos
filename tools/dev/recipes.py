"""Finds and checks the recipes core/symbols.mm reads struct offsets with (rbm::Offset): "the immediate of the
nth instruction of a kind inside a function".

    python3 tools/dev/recipes.py find <binary> <symbol regex> <offset>   # instructions using offset, with their recipe
    python3 tools/dev/recipes.py check <binary>                          # every recipe in core/ and mods/ against its expected value

Kinds (as written in the sources): "ldr", "str" (any size; rbm decodes size and scale), "add" (add immediate),
"tb" (tbz/tbnz: the bit number), "movz" (a single movz), "mov" (movz + movk: a 32-bit constant built in one
register), each optionally with a register (the base, or the bit-tested / written one): "ldr x0" = loads based
on x0. "vt:<method symbol>" in vtable fn gives the method's slot offset from the address point (+0x10).
"""
import os
import re
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from rbbin import TEXT_BASE, Binary  # noqa: E402


def decode(w):
    """(kind, base register, immediate) of the instructions recipes use, else None."""
    if w & 0x3B000000 == 0x39000000:                          # load/store register (also FP), unsigned immediate
        size, opc = w >> 30, (w >> 22) & 3
        if w & 0x04000000 and opc >= 2:                       # 128-bit FP
            return ("str" if opc == 2 else "ldr", (w >> 5) & 31, ((w >> 10) & 0xFFF) << 4)
        return ("str" if opc == 0 else "ldr", (w >> 5) & 31, ((w >> 10) & 0xFFF) << size)
    if w & 0x7F800000 == 0x11000000:                          # add (immediate)
        return ("add", (w >> 5) & 31, ((w >> 10) & 0xFFF) << (12 if w & 0x400000 else 0))
    if w & 0x7E000000 == 0x36000000:                          # tbz / tbnz
        return ("tb", w & 31, (w >> 31) << 5 | (w >> 19) & 31)
    if w & 0x7F800000 == 0x52800000:                          # movz
        return ("movz", w & 31, ((w >> 5) & 0xFFFF) << 16 * ((w >> 21) & 3))
    return None


def matches(kind, k, rn):
    want = kind.split()
    return k == want[0] and (len(want) == 1 or rn == int(want[1][1:]))


def run(b, fn, kind, nth):
    """The recipe's value, or None."""
    if kind.startswith("vt:"):
        method = b.sym(kind[3:])
        if method is None:
            return None
        for i in range(1024):
            v = struct.unpack_from("<Q", b.data, b.fo(fn + 0x10 + 8 * i))[0]
            if (v & 0xFFFFFFFFF) + TEXT_BASE == method:   # chained-fixup rebase: target in the low 36 bits
                if nth == 0:
                    return 8 * i
                nth -= 1
        return None
    if kind == "mov":
        val, n = None, -1
        for va, w in b.insns(fn):
            if w & 0x7F800000 == 0x52800000:      # movz w
                rd, val, n = w & 31, ((w >> 5) & 0xFFFF) << 16 * ((w >> 21) & 3), n + 1
            elif w & 0x7F800000 == 0x72800000 and val is not None and (w & 31) == rd:  # movk w, same register
                val |= ((w >> 5) & 0xFFFF) << 16 * ((w >> 21) & 3)
                if n == nth:
                    return val
        return None
    n = 0
    for va, w in b.insns(fn):
        d = decode(w)
        if d and matches(kind, d[0], d[1]):
            if n == nth:
                return d[2]
            n += 1
    return None


def find(b, pattern, offset):
    for name, fn in sorted(b.symbols.items()):
        if not re.search(pattern, name) or not (b.text_range()[0] <= fn < b.text_range()[1]):
            continue
        counts = {}
        for va, w in b.insns(fn):
            d = decode(w)
            if not d:
                continue
            for kind in (d[0], "%s x%d" % (d[0], d[1])):
                if d[2] == offset:
                    print('%s  {"%s", "%s", %d}  (%d insns)' % (hex(va), name, kind, counts.get(kind, 0),
                                                              (b.function_end(fn) - fn) // 4))
                counts[kind] = counts.get(kind, 0) + 1


RECIPE_RE = r'Offset\s*\w*\s*\{\s*(0x[0-9a-fA-F]+|\d+)\s*,\s*"([^"]+)"\s*,\s*"([^"]+)"\s*,\s*(\d+)'


def check(b):
    here = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
    bad = 0
    for d in ("core", "mods"):
        for f in sorted(os.listdir(os.path.join(here, d))):
            src = open(os.path.join(here, d, f), errors="replace").read()
            for name, val in re.findall(r'^#define\s+(\w+)\s+("[^"\n]*")\s*$', src, re.M):  # #define FN "symbol"
                src = re.sub(r'(Offset\s*\w*\s*\{[^,]+,\s*)%s\b' % name, lambda m: m.group(1) + val, src)
            for m in re.finditer(RECIPE_RE, src):
                expected, sym, kind, nth = int(m.group(1), 0), m.group(2), m.group(3), int(m.group(4))
                if kind.startswith("win:"):  # a Windows table field (#ifdef _WIN32): tools/win checks those
                    print("win  %s/%s %s (%s)" % (d, f, hex(expected), kind))
                    continue
                fn = b.sym(sym)
                got = run(b, fn, kind, nth) if fn else None
                ok = got == expected
                bad += not ok
                print("%-4s %s/%s %s = %s%s" % ("ok" if ok else "FAIL", d, f, hex(expected), hex(got) if got is not None else None,
                                                "" if fn else " (no symbol %s)" % sym))
    sys.exit(1 if bad else 0)


if __name__ == "__main__":
    b = Binary(open(sys.argv[2], "rb").read())
    if sys.argv[1] == "find":
        find(b, sys.argv[3], int(sys.argv[4], 0))
    else:
        check(b)
