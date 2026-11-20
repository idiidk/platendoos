"""Patch the arm64 slice of an unmodified rekordbox so it loads librbmods.dylib and routes the call sites
listed in hooks.def through it.

    python3 patch_binary.py <in binary> <out binary>

patcher/macho.h is the same in C++ for the patcher app (byte-identical output): change both.

Call sites are found by name (see hooks.def); a hook whose site can't be found is left out and reported, so a
rekordbox update that changes one function only loses the hooks inside it.

Hook i gets a 16-byte stub in the unused header padding of __TEXT, just before __text:
    movz x17, #i ; adrp x16, slot ; ldr x16, [x16, slot] ; br x16
The single pointer slot (8 bytes of padding between __DATA's sections) holds the dylib's dispatcher, which
jumps to entry i of its hook table. x16/x17 are the intra-procedure-call scratch registers, free at every call
site. A marker in front of the stubs tells the dylib where the slot is and which hooks were patched:
    "RBMHOOKS", slot address (8), hook count (4), 0 (4), one byte per hook (1 = patched)
"""
import os
import re
import struct
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "tools", "dev"))
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "tools", "build"))
from rbbin import Binary, adrp, b as branch, bl  # noqa: E402
import hooksdef  # noqa: E402  (hooks.def of the edition being built: RBM_EDITION)

STUB_SIZE = 16
MAGIC = b"RBMHOOKS"
DYLIB = b"@executable_path/librbmods.dylib"
REPO = os.path.dirname(os.path.abspath(__file__))
HOOK_RE = r'\s*HOOK\(\s*(\w+)\s*,\s*"([^"]+)"\s*,\s*(\d+)\s*,\s*(\d+)\s*,\s*"([^"]+)"\s*,\s*(-?\d+)\s*\)'


def load_hooks():
    hooks = []
    for line in hooksdef.lines(REPO):
        m = re.match(HOOK_RE, line)
        if m:
            hooks.append((m.group(1), m.group(2), int(m.group(3)), int(m.group(4)), m.group(5), int(m.group(6))))
    return hooks


def load_alternatives():
    """name -> the hook's HOOK_OR sites, in file order (see hooks.def)."""
    alts = {}
    for line in hooksdef.lines(REPO):
        m = re.match(HOOK_RE.replace("HOOK", "HOOK_OR", 1), line)
        if m:
            alts.setdefault(m.group(1), []).append((m.group(2), int(m.group(3)), int(m.group(4)), m.group(5), int(m.group(6))))
    return alts


def find_hook(b, alts, hname, caller, nth, count, target, kind):
    """The hook's call site (the first of its sites that is there) and that site's kind, or why none can be used."""
    found, why = find_site(b, caller, nth, count, target, kind)
    for a in alts.get(hname, []) if not found else []:
        found, _ = find_site(b, *a)
        if found:
            kind = a[4]
            break
    return found, why, kind


def find_site(b, caller, nth, count, target, kind):
    """The call site's address, or the reason it can't be used."""
    fn, orig = b.sym(caller), b.sym(target)
    if fn is None:
        return None, "no function %s" % caller
    if orig is None:
        return None, "no target %s" % target
    hits = b.calls(fn, orig) if kind == -1 else b.jumps(fn, orig) if kind == -2 else b.adrs(fn, orig, kind)
    if len(hits) != count:
        return None, "%d calls to the target instead of %d: the function changed" % (len(hits), count)
    site = hits[nth]
    if kind >= 0 and b.u32(site + 4) != 0xD503201F:
        return None, "no nop after the adr"
    return (site, orig), None


def find_slot(b):
    """8 zero bytes of file-backed padding between two of __DATA's sections."""
    sects = sorted((a, s) for (seg, _), (a, s, off) in b.sections.items() if seg == "__DATA" and off)
    for (a, s), (na, _) in zip(sects, sects[1:]):
        slot = (a + s + 15) & ~15
        if slot + 8 <= na and b.data[b.fo(slot):b.fo(slot) + 8] == bytes(8):
            return slot
    raise SystemExit("no padding for the dispatch slot in __DATA")


def main(src_path, dst_path):
    hooks = load_hooks()
    alts = load_alternatives()
    assert not set(alts) - {h[0] for h in hooks}, "HOOK_OR without its HOOK"
    d = bytearray(open(src_path, "rb").read())
    b = Binary(bytes(d))
    fo = b.fo
    put = lambda va, w: struct.pack_into("<I", d, fo(va), w)

    # Room: the marker and the stubs end where __text starts; the load commands (plus ours) must end before.
    text = b.text_range()[0]
    marker_size = (24 + len(hooks) + 15) & ~15
    stubs = text - STUB_SIZE * len(hooks)
    marker = stubs - marker_size
    name = DYLIB + b"\0"
    cmdsize = (24 + len(name) + 7) & ~7
    cmds_end = b.base + 32 + b.sizeofcmds
    assert cmds_end + cmdsize <= fo(marker), "no room for the hooks in the header padding"
    assert d[cmds_end:fo(text)] == bytes(fo(text) - cmds_end), "header padding not empty: the input must be an unpatched rekordbox"
    slot = find_slot(b)

    patched, skipped = bytearray(len(hooks)), []
    for i, (hname, caller, nth, count, target, kind) in enumerate(hooks):
        stub = stubs + STUB_SIZE * i
        put(stub, 0xD2800011 | (i << 5))                                             # movz x17, #i
        put(stub + 4, adrp(stub + 4, slot, 16))
        put(stub + 8, 0xF9400000 | (((slot & 0xFFF) // 8) << 10) | (16 << 5) | 16)  # ldr x16, [x16, #off]
        put(stub + 12, 0xD61F0200)                                                   # br x16
        found, why, kind = find_hook(b, alts, hname, caller, nth, count, target, kind)
        if not found:
            skipped.append("%s (%s)" % (hname, why))
            continue
        site, orig = found
        if kind == -1:
            put(site, bl(site, stub))
        elif kind == -2:
            put(site, branch(site, stub))
        else:
            put(site, adrp(site, stub, kind))
            put(site + 4, 0x91000000 | ((stub & 0xFFF) << 10) | (kind << 5) | kind)  # add reg, reg, #off
        patched[i] = 1

    d[fo(marker):fo(marker) + 24 + len(hooks)] = MAGIC + struct.pack("<QII", slot, len(hooks), 0) + bytes(patched)

    # Append LC_LOAD_DYLIB.
    ncmds, sizeofcmds = struct.unpack_from("<II", d, b.base + 16)
    struct.pack_into("<IIIIII", d, cmds_end, 0xC, cmdsize, 24, 2, 0x10000, 0x10000)
    d[cmds_end + 24:cmds_end + 24 + len(name)] = name
    struct.pack_into("<II", d, b.base + 16, ncmds + 1, sizeofcmds + cmdsize)

    open(dst_path, "wb").write(d)
    print("patched %d of %d hooks (rekordbox build %s)" % (sum(patched), len(hooks), b.uuid))
    for s in skipped:
        print("  left out: " + s)


if __name__ == "__main__":
    main(*sys.argv[1:3])
