"""Patch an unmodified rekordbox so it loads librbmods.dylib and routes the call sites listed in hooks.def through it:
every slice the file has (arm64, and x86_64 for Intel Macs, docs/intel-mac.md).

    python3 patch_binary.py <in binary> <out binary>

patcher/macho.h is the same in C++ for the patcher app (byte-identical output): change both.

Call sites are found by name (see hooks.def); a hook whose site can't be found is left out and reported, so a
rekordbox update that changes one function only loses the hooks inside it. A hook's x86_64 sites are its HOOK_X86
lines if it has any, else the same as arm64's.

Hook i gets a 16-byte stub in the unused header padding of __TEXT, just before __text:
    arm64:   movz x17, #i ; adrp x16, slot ; ldr x16, [x16, slot] ; br x16
    x86_64:  mov r11d, i ; jmp qword ptr [rip + slot] ; int3 x 4
The single pointer slot (8 bytes of padding between __DATA's sections, else the unused tail of __DATA) holds the
dylib's dispatcher, which jumps to entry i of its hook table. x16/x17 are the intra-procedure-call scratch
registers, free at every call site; r11 is free at every x86_64 call site (docs/intel-mac.md). A marker in front of
the stubs (16-aligned, ending where they start) tells the dylib where the slot is and which hooks were patched:
    arm64:   "RBMHOOKS", slot address (8), hook count (4), 0 (4), one byte per hook (1 = patched)
    x86_64:  "RBMHOOKS", slot address (8), hook count (4), 1 (4), one byte per hook (1 = patched), 0-3 bytes of
             padding to a multiple of 4, then per hook the original target as a 32-bit offset from __TEXT's address
             (0 = not patched), so the dylib can run rekordbox stock without scanning x86 code for the sites
"""
import os
import re
import struct
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "tools", "dev"))
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "tools", "build"))
from rbbin import ARCH_NAMES, CPU_ARM64, CPU_X86_64, Binary, adrp, b as branch, bl, slices  # noqa: E402
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


def load_sites(macro):
    """name -> the hook's HOOK_OR or HOOK_X86 sites, in file order (see hooks.def). HOOK_X86's caller can be ""."""
    sites = {}
    pattern = HOOK_RE.replace("HOOK", macro, 1).replace('"([^"]+)"', '"([^"]*)"')
    for line in hooksdef.lines(REPO):
        m = re.match(pattern, line)
        if m:
            site = (m.group(2), int(m.group(3)), int(m.group(4)), m.group(5), int(m.group(6)))
            sites.setdefault(m.group(1), []).append(site)
    return sites


def load_alternatives():
    """name -> the hook's HOOK_OR sites, in file order (see hooks.def)."""
    return load_sites("HOOK_OR")


def hook_sites(hook, alts, x86):
    """The sites to try for a hook, in order: arm64 its HOOK line then its HOOK_OR lines; x86_64 its HOOK_X86 lines if
    it has any, else the same."""
    if hook[0] in x86:
        return x86[hook[0]]
    return [hook[1:]] + alts.get(hook[0], [])


def find_hook(b, sites):
    """The hook's call site (the first of its sites that is there) and that site's kind, or why none can be used
    (the reason of its first site)."""
    why = None
    for s in sites:
        found, w = find_site(b, *s)
        if found:
            return found, None, s[4]
        why = why or w
    return None, why, None


def find_site(b, caller, nth, count, target, kind):
    """The call site's address and target, or the reason it can't be used."""
    if not caller:
        return None, "not on %s (hooks.def)" % ARCH_NAMES[b.cpu]
    fn, orig = b.sym(caller), b.sym(target)
    if fn is None:
        return None, "no function %s" % caller
    if orig is None:
        return None, "no target %s" % target
    hits = b.calls(fn, orig) if kind == -1 else b.jumps(fn, orig) if kind == -2 else b.adrs(fn, orig, kind)
    if len(hits) != count:
        return None, "%d calls to the target instead of %d: the function changed" % (len(hits), count)
    site = hits[nth]
    if b.cpu == CPU_ARM64 and kind >= 0 and b.u32(site + 4) != 0xD503201F:
        return None, "no nop after the adr"
    return (site, orig), None


def find_slot(b):
    """8 zero bytes of file-backed padding between two of __DATA's sections, else the unused tail of __DATA after
    its last section (zero-filled memory)."""
    sects = sorted((a, s) for (seg, _), (a, s, off) in b.sections.items() if seg == "__DATA" and off)
    for (a, s), (na, _) in zip(sects, sects[1:]):
        slot = (a + s + 15) & ~15
        if slot + 8 <= na and b.data[b.fo(slot):b.fo(slot) + 8] == bytes(8):
            return slot
    if "__DATA" in b.segments:
        vmaddr, vmsize = b.segments["__DATA"][:2]
        slot = (max(a + s for (seg, _), (a, s, _) in b.sections.items() if seg == "__DATA") + 15) & ~15
        if slot + 8 <= vmaddr + vmsize:
            return slot
    raise SystemExit("no padding for the dispatch slot in __DATA")


def marker_size(cpu, n):
    """The marker's size, a multiple of 16 (it ends where the stubs start)."""
    size = 24 + n if cpu == CPU_ARM64 else 24 + ((n + 3) & ~3) + 4 * n
    return (size + 15) & ~15


def patch_slice(d, b, hooks, alts, x86):
    """Patches the slice b reads (parsed from the original file) in d. Returns its per-hook flags, the hooks left
    out ("<hook> (<why>)") and those deliberately not patched on this architecture (HOOK_X86 lines without a site)."""
    fo = b.fo
    put = lambda va, w: struct.pack_into("<I", d, fo(va), w)
    arm = b.cpu == CPU_ARM64
    n = len(hooks)

    # Room: the marker and the stubs end where __text starts; the load commands (plus ours) must end before.
    text = b.text_range()[0]
    stubs = text - STUB_SIZE * n
    marker = stubs - marker_size(b.cpu, n)
    name = DYLIB + b"\0"
    cmdsize = (24 + len(name) + 7) & ~7
    cmds_end = b.base + 32 + b.sizeofcmds
    assert cmds_end + cmdsize <= fo(marker), "no room for the hooks in the header padding"
    assert d[cmds_end:fo(text)] == bytes(fo(text) - cmds_end), "header padding not empty: the input must be an unpatched rekordbox"
    slot = find_slot(b)
    text_base = b.segments["__TEXT"][0]

    patched, origs, skipped, off = bytearray(n), [0] * n, [], []
    for i, hook in enumerate(hooks):
        hname = hook[0]
        stub = stubs + STUB_SIZE * i
        if arm:
            put(stub, 0xD2800011 | (i << 5))                                             # movz x17, #i
            put(stub + 4, adrp(stub + 4, slot, 16))
            put(stub + 8, 0xF9400000 | (((slot & 0xFFF) // 8) << 10) | (16 << 5) | 16)  # ldr x16, [x16, #off]
            put(stub + 12, 0xD61F0200)                                                   # br x16
        else:
            # mov r11d, i ; jmp qword ptr [rip + slot] ; int3 x 4
            jmp = struct.pack("<i", slot - (stub + 12))
            d[fo(stub):fo(stub) + 16] = b"\x41\xBB" + struct.pack("<I", i) + b"\xFF\x25" + jmp + b"\xCC" * 4
        sites = hook_sites(hook, alts, {} if arm else x86)
        if all(not s[0] for s in sites):
            off.append(hname)
            continue
        found, why, kind = find_hook(b, sites)
        if not found:
            skipped.append("%s (%s)" % (hname, why))
            continue
        site, orig = found
        if arm:
            if kind == -1:
                put(site, bl(site, stub))
            elif kind == -2:
                put(site, branch(site, stub))
            else:
                put(site, adrp(site, stub, kind))
                put(site + 4, 0x91000000 | ((stub & 0xFFF) << 10) | (kind << 5) | kind)  # add reg, reg, #off
        elif kind in (-1, -2):
            struct.pack_into("<i", d, fo(site) + 1, stub - (site + 5))                     # call / jmp rel32
        else:
            struct.pack_into("<i", d, fo(site) + 3, stub - (site + 7))                     # lea reg, [rip + rel32]
        patched[i] = 1
        origs[i] = orig - text_base

    m = fo(marker)
    head = MAGIC + struct.pack("<QII", slot, n, 0 if arm else 1) + bytes(patched)
    if not arm:
        head += bytes(-len(head) % 4) + struct.pack("<%dI" % n, *origs)
    d[m:m + len(head)] = head

    # Append LC_LOAD_DYLIB.
    ncmds, sizeofcmds = struct.unpack_from("<II", d, b.base + 16)
    struct.pack_into("<IIIIII", d, cmds_end, 0xC, cmdsize, 24, 2, 0x10000, 0x10000)
    d[cmds_end + 24:cmds_end + 24 + len(name)] = name
    struct.pack_into("<II", d, b.base + 16, ncmds + 1, sizeofcmds + cmdsize)
    return patched, skipped, off


def main(src_path, dst_path):
    hooks = load_hooks()
    alts = load_alternatives()
    x86 = load_sites("HOOK_X86")
    assert not set(alts) - {h[0] for h in hooks}, "HOOK_OR without its HOOK"
    assert not set(x86) - {h[0] for h in hooks}, "HOOK_X86 without its HOOK"
    src = open(src_path, "rb").read()
    d = bytearray(src)
    archs = [cpu for cpu, _ in slices(src)]
    if not set(archs) & {CPU_ARM64, CPU_X86_64}:
        raise SystemExit("no arm64 or x86_64 slice")
    report = []
    for cpu in (c for c in archs if c in ARCH_NAMES):
        b = Binary(src, cpu)
        report.append((cpu, b.uuid) + patch_slice(d, b, hooks, alts, x86))

    open(dst_path, "wb").write(d)
    for cpu, uuid, patched, skipped, off in sorted(report, key=lambda r: r[0] != CPU_ARM64):
        want = len(hooks) - len(off)
        print("%s: patched %d of %d hooks (rekordbox build %s)" % (ARCH_NAMES[cpu], sum(patched), want, uuid))
        for s in skipped:
            print("  left out: " + s)
        if off:
            print("  not on %s (hooks.def): %s" % (ARCH_NAMES[cpu], ", ".join(off)))


if __name__ == "__main__":
    main(*sys.argv[1:3])
