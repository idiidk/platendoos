# Signatures for finding a Windows address table's entries in another rekordbox build (patcher/retable.h reads
# them): made from a build that has a verified table, so a new rekordbox version gets a table without the mac
# matching work, as far as its code is unchanged.
#   python3 -I tools/win/makesigs.py <rekordbox.exe> <its .tbl> [workdir] > tools/win/sigs/rekordbox-<build>.sig
#
# Patterns are hex bytes with ?? for bytes that move between builds (rip-relative displacements, call/jmp
# targets), with a * before the ones that may (field and stack displacements, immediates, short jumps: "loose"
# bytes, compared unless the exact pattern isn't found; the patterns are unique with them ignored where that can
# be had), then any number of |<dispPos>,<insEnd>,<hex>: a masked reference in the match must point at those bytes
# (a string literal: tells apart template copies whose code is the same). The bytes that carry registers are
# marked too, because another build's compiler may pick other registers for the same code: % before an
# instruction's ModRM byte or its SIB byte, ^ before its REX prefix (a masked or loose byte is never marked as
# well; VEX/EVEX instructions get no marks). The first two passes compare marked bytes exactly. A third pass, only
# when those found nothing, ignores the loose bytes and compares ModRM/SIB under mask 0xC0 (addressing mode and
# scale stay, registers go) and REX under 0xF8 (the W bit stays). The marks are only written here: a pattern isn't
# chosen for being unique at that level, the scanner just requires a unique match. A pattern must match exactly
# once in the new build's .text (or in its range, for the scoped lines). Names are resolved over several passes, so a scoped
# line's anchor may itself have been found by reference. Spaces in names and field keys are written as ~. Lines:
#   build <build the signatures were made from>
#   fn <name> <pattern>                         the function starts where the pattern matches
#   ref <name> <pattern> <dispPos> <insEnd>     the address an instruction in the match refers to (rel32 or rip
#                                               displacement at dispPos, relative to the instruction end insEnd)
#   refin <name> <anchor> <range> <pattern> <dispPos> <insEnd>   the same, the pattern unique in the anchor's
#                                               function (its .pdata extent; for a leaf [anchor, +range * 1.25 + 16))
#                                               only (anchor: a fn line's name; @<rva> names are only anchors)
#   vt <name> <delta> <decorated class> <offset>   a vtable by RTTI: the one whose locator has that subobject offset
#   ti <name> <decorated class>                 a type descriptor by RTTI
#   hook <name> <kind> <pattern> <sitePos> <target name|->   a hook site (the call/jmp at sitePos)
#   hookin <name> <kind> <anchor> <range> <pattern> <sitePos> <target name|->   the same within [anchor, +range)
#   hookcall <name> <kind> <anchor> <range> <target> <k> <n>   or (when the hook/hookin lines don't give every
#                                               site) the k-th of the n call/jmp rel32 to the target in the anchor's
#                                               function; for a hookreg hook only where one of its hook/hookin
#                                               patterns matches loosely at the site (the register bytes exact)
#   off <key> <value> <pattern> <dispPos> <dispSize>   a field read by an instruction: its displacement in the new
#                                               build (several lines for one key must agree)
#   offsame <key> <value> <fn name> <pattern>   or the same value when the function's code is unchanged
#   offrel <key> <value> <other key> <other value>   or, when nothing else finds it, the other field's new value
#                                               + the old difference (read through the same register, near it)
#   offseq <key> <value> <fn> <range> <gap> <d0,d1,...>   or, when no other line finds the field, the v for which
#                                               v+d0, v+d1, ... are dwords in that order in the function (each within
#                                               gap bytes of the one before): the field and the reads through the
#                                               same register around it, whose struct may have moved as a whole
#   offvt <key> <value> <vtable name> <method name>   a vtable slot: where the method is in the new vtable
#   offvtfn <key> <value> <vtable name> <pattern>   or the one slot of the new vtable whose function starts so
#   fnvt <name> <vtable> <pattern>              a virtual method: the one slot of the vtable whose function starts so
#                                               (vtable: a vt line's name, or rtti:<decorated class>:<offset>)
#   hooksites <name> <n>                        the hook's site count: with fewer found it's left out
#   fnvt <name> <vtable> <pattern> <k> <n>      or the k-th (slot order) of the n slots' functions starting so
#   vtslots <vtable name> <n>                   the vtable's slot count: when an offvt/offvtfn method is in several
#                                               slots (identical functions folded), the old slot is kept if it's one
#                                               of them and the count is unchanged
#   (A name may have several lines: the fn/ref one, then refin fallbacks through other found functions; the first
#   that resolves it counts.)
#   offany <key> <value> <pattern> <dispPos> <dispSize>   like off, for code that has identical copies: up to 8
#                                               matches, which must all agree
#   offadj[in|any] <key> <value> <fn name> <off / offin / offany tail>   a field of the whole object read through the
#                                               subobject the function gets as this: the function's adjustment (worked
#                                               out again from the new vtables) plus the displacement
#   adj <name> <delta>                          a this adjustment: worked out again from the new build's vtables (the
#                                               name is left out if that can't be done)
#   hookreg <name> <reg> <arg>                  kept while the hook is found (the register is read at the site, whose
#                                               bytes match exactly)
#   guard <hooks> <value> <pattern> <dispPos> <dispSize>   a constant a mod has in its source (tools/win/sigguards.txt):
#                                               if it isn't found unchanged, the hooks (comma-separated) are left out
# The verification data and sigfields are the edition's (RBM_EDITION; tools/win/verifydata.py), like the table's.
import sys, os, re, struct, pickle, bisect
import pefile, capstone, numpy as np

exe, tblfile = sys.argv[1], sys.argv[2]
work = sys.argv[3] if len(sys.argv) > 3 else os.path.expanduser('~/rbwin')
repo = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
pe = pefile.PE(exe, fast_load=True)
pe.parse_data_directories(directories=[pefile.DIRECTORY_ENTRY['IMAGE_DIRECTORY_ENTRY_EXCEPTION']])
base = pe.OPTIONAL_HEADER.ImageBase
build = '%08X-%X' % (pe.FILE_HEADER.TimeDateStamp, pe.OPTIONAL_HEADER.SizeOfImage)
text = next(s for s in pe.sections if s.Name.rstrip(b'\0') == b'.text')
T0 = text.VirtualAddress
TB = pe.__data__[text.PointerToRawData:text.PointerToRawData + text.Misc_VirtualSize]
rdata = [(s.VirtualAddress, s.VirtualAddress + s.Misc_VirtualSize) for s in pe.sections if s.Name.rstrip(b'\0') in (b'.rdata', b'.data')]
def img(rva, n):  # bytes of the image at an rva (any section)
    try: return pe.get_data(rva, n)
    except Exception: return b''

md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
md.detail = True
pdata = sorted((e.struct.BeginAddress, e.struct.EndAddress) for e in pe.DIRECTORY_ENTRY_EXCEPTION)
pbeg = [b for b, e in pdata]

def in_text(rva): return T0 <= rva < T0 + len(TB)

def fn_end(rva):
    """End of the function chunk starting at rva: its .pdata entry, else (a leaf) the first ret/jmp followed by padding."""
    i = bisect.bisect_right(pbeg, rva) - 1
    if i >= 0 and pdata[i][0] == rva: return pdata[i][1]
    end = rva
    for ins in md.disasm(TB[rva - T0:rva - T0 + 4096], rva):
        end = ins.address + ins.size
        if ins.mnemonic in ('ret', 'jmp', 'int3') and TB[end - T0] in (0xcc, 0x90): break
    return end

leafs = []  # table functions without a .pdata entry (no stack frame), sorted: set once the table is read
def chunk_start(rva):
    """Start of the .pdata chunk (or leaf table function) holding rva, or None."""
    i = bisect.bisect_right(pbeg, rva) - 1
    if i >= 0 and pdata[i][0] <= rva < pdata[i][1]: return pdata[i][0]
    j = bisect.bisect_right(leafs, rva) - 1
    return leafs[j] if j >= 0 and rva < fn_end(leafs[j]) else None

_ins_cache = {}
def insns(start, end):
    """(address, size, mask, insn, ripTarget) per instruction: mask = byte indexes that move between builds."""
    key = (start, end)
    if key in _ins_cache: return _ins_cache[key]
    out = []
    for ins in md.disasm(TB[start - T0:end - T0], start):
        m, tgt = set(), None
        rel = ins.group(capstone.CS_GRP_JUMP) or ins.group(capstone.CS_GRP_CALL)
        if ins.disp_size and any(o.type == capstone.x86.X86_OP_MEM and o.mem.base == capstone.x86.X86_REG_RIP for o in ins.operands):
            m.update(range(ins.disp_offset, ins.disp_offset + ins.disp_size))
            if ins.disp_size == 4: tgt = ins.address + ins.size + ins.disp
        if rel and ins.imm_size == 4:
            m.update(range(ins.imm_offset, ins.imm_offset + 4))
        out.append((ins.address, ins.size, m, ins, tgt))
    if len(_ins_cache) > 20000: _ins_cache.clear()
    _ins_cache[key] = out
    return out

def string_at(rva):
    """The string literal at rva (ASCII or UTF-16, at least 4 characters) as bytes to compare, or None."""
    if not any(lo <= rva < hi for lo, hi in rdata): return None
    d = img(rva, 48)
    n = 0
    while n < len(d) and 0x20 <= d[n] < 0x7f: n += 1
    if n >= 4 and (n == len(d) or d[n] == 0): return d[:min(n + 1, 32)]
    n = 0
    while n + 1 < len(d) and 0x20 <= d[n] < 0x7f and d[n + 1] == 0: n += 2
    if n >= 8 and (n + 1 >= len(d) or d[n:n + 2] == b'\0\0'): return d[:min(n + 2, 32)]
    return None

def loose_bytes(ins, m):
    """Byte indexes of an instruction that a small change elsewhere moves: non-rip displacements (fields, stack),
    immediates and rel8 jumps."""
    res = set()
    rel = ins.group(capstone.CS_GRP_JUMP) or ins.group(capstone.CS_GRP_CALL)
    if ins.disp_size in (1, 4) and ins.disp_offset: res.update(range(ins.disp_offset, ins.disp_offset + ins.disp_size))
    if ins.imm_size in (1, 2, 4) and ins.imm_offset and (not rel or ins.imm_size == 1):
        res.update(range(ins.imm_offset, ins.imm_offset + ins.imm_size))
    return res - m

def reg_bytes(ins):
    """{byte index: 1 (ModRM/SIB) or 2 (REX)} for the bytes of an instruction that carry registers."""
    res = {}
    raw = bytes(ins.bytes)
    if raw[0] in (0xc4, 0xc5, 0x62): return res  # VEX/EVEX: registers live in the prefix
    i = 0
    while i < len(raw) and raw[i] in (0x66, 0x67, 0xf0, 0xf2, 0xf3, 0x2e, 0x36, 0x3e, 0x26, 0x64, 0x65): i += 1
    if ins.rex and i < len(raw) and 0x40 <= raw[i] <= 0x4f and raw[i] == ins.rex: res[i] = 2
    if ins.modrm_offset:
        mo = ins.modrm_offset
        res[mo] = 1
        if (raw[mo] >> 6) != 3 and (raw[mo] & 7) == 4 and mo + 1 < len(raw): res[mo + 1] = 1
    return res

class Pat:
    def __init__(self, start, end, extra_mask=()):
        cs = chunk_start(start) or start
        self.start = start
        self.b = bytes(TB[start - T0:end - T0])
        self.mk = [False] * (end - start)
        self.lz = [False] * (end - start)
        self.rg = [0] * (end - start)  # 1 ModRM/SIB (mask 0xC0), 2 REX (mask 0xF8)
        self.cons = []  # (dispPos, insEnd, bytes)
        for a, sz, m, ins, tgt in insns(cs, end):
            for k in m:
                if start <= a + k < end: self.mk[a + k - start] = True
            for k in loose_bytes(ins, m):
                if start <= a + k < end: self.lz[a + k - start] = True
            for k, kind in reg_bytes(ins).items():
                if start <= a + k < end: self.rg[a + k - start] = kind
            if tgt is not None and start <= a and a + sz <= end:
                st = string_at(tgt)
                if st: self.cons.append((a + ins.disp_offset - start, a + sz - start, st))
        for x in extra_mask:
            if start <= x < end: self.mk[x - start] = True
    def text(self):
        return ''.join('??' if m else '*%02x' % v if l else '%%%02x' % v if r == 1 else '^%02x' % v if r == 2 else '%02x' % v
                       for v, m, l, r in zip(self.b, self.mk, self.lz, self.rg)) + \
               ''.join('|%d,%d,%s' % (p, e, d.hex()) for p, e, d in self.cons)
    def at(self, q, loose=0):  # matches at .text offset q? (level 1: the loose bytes may differ; 2: and the registers)
        b, mk, lz, rg = self.b, self.mk, self.lz, self.rg
        if q < 0 or q + len(b) > len(TB): return False
        for i in range(len(b)):
            if mk[i] or (loose and lz[i]) or TB[q + i] == b[i]: continue
            if loose >= 2 and rg[i] and not ((TB[q + i] ^ b[i]) & (0xC0 if rg[i] == 1 else 0xF8)): continue
            return False
        for p, e, d in self.cons:
            disp = struct.unpack_from('<i', TB, q + p)[0]
            if img(q + T0 + e + disp, len(d)) != d: return False
        return True
    def count(self, limit=2, loose=False):
        """Matches in .text (up to limit): the longest unmasked run is searched, the rest compared at each hit."""
        best, rs = (0, 0), 0
        for i, m in enumerate([x or (loose and y) or (loose >= 2 and z) for x, y, z in zip(self.mk, self.lz, self.rg)] + [True]):
            if m:
                if i - rs > best[1] - best[0]: best = (rs, i)
                rs = i + 1
        s, e = best
        if e - s < (6 if loose else 4): return limit  # too little to go on: not unique (or too slow to tell)
        needle, hits, p = self.b[s:e], 0, 0
        while True:
            p = TB.find(needle, p)
            if p < 0: return hits
            if self.at(p - s, loose):
                hits += 1
                if hits >= limit: return hits
            p += 1
    def count_in(self, lo, hi, limit=2, loose=False):
        hits = 0
        for q in range(lo - T0, min(hi - T0, len(TB) - len(self.b) + 1)):
            if loose < 2 and not (self.mk[0] or (loose and self.lz[0]) or TB[q] == self.b[0]): continue
            if self.at(q, loose):
                hits += 1
                if hits >= limit: return hits
        return hits

def whole_end(start, n):
    """The end of the last whole instruction within n bytes of start (a cut instruction would keep its displacement exact)."""
    end, lim = start, min(fn_end(start), start + n)
    for a, sz, _, _, _ in insns(start, lim):
        if a + sz <= lim: end = a + sz
    return end if end > start else lim

def unique_from(start, maxlen, minlen=16):
    """The shortest pattern from start (whole instructions) that's unique in .text, or None."""
    end = min(fn_end(start), start + maxlen)
    ins = [a + sz for a, sz, _, _, _ in insns(start, end)]
    for loose in (True, False):
        for stop in ins:
            if stop - start < minlen and stop != ins[-1]: continue
            pt = Pat(start, stop)
            if pt.count(loose=loose) == 1: return pt
    return None

def windows(site, size, after=96, before=160, back=False):
    """Growing windows of whole instructions around the instruction at site (after first, then before; back: the
    instructions before it first, the ones after only when there's nothing more before)."""
    cs = chunk_start(site)
    if cs is None: return
    L = insns(cs, min(fn_end(cs), site + size + after))
    idx = next((i for i, x in enumerate(L) if x[0] == site), None)
    if idx is None: return
    lo, hi = idx, idx + 1
    for step in range(80):
        yield L[lo][0], L[hi - 1][0] + L[hi - 1][1]
        if step % 2 == 0 and hi < len(L) and not back: hi += 1
        elif lo > 0 and site - L[lo - 1][0] <= before: lo -= 1
        elif hi < len(L): hi += 1
        else: return

def unique_around(site, size, extra_mask=()):
    """A window around the instruction at site that's unique in .text, or None."""
    for loose in (True, False):
        for start, stop in windows(site, size):
            if stop - start >= 12:
                pt = Pat(start, stop, extra_mask)
                if pt.count(loose=loose) == 1: return pt
    return None

found = {}    # table name -> rva, for the ones a line finds
anchors = {}  # chunk start rva -> anchor name, or None
_depth = [0]
def anchor_of(site):
    """A found function whose chunk holds site, or the chunk start found itself ('@<rva>' lines): by its own
    signature, a vtable slot, or (one level deep) a call or reference to it."""
    cs = chunk_start(site)
    if cs is None: return None
    if cs not in anchors:
        named = [n for n, a in found.items() if a == cs and not n.startswith('__ZT')]
        if named: anchors[cs] = named[0]
        else:
            name = '@%x' % cs
            u = unique_from(cs, 512)
            ok = False
            if u: out.append('fn %s %s' % (E(name), u.text())); ok = True
            if not ok: ok = emit_fnvt(name, cs)
            if not ok and _depth[0] == 0:
                _depth[0] += 1
                try: ok = emit_ref(name, cs, False) or emit_ref(name, cs, True)
                finally: _depth[0] -= 1
            anchors[cs] = name if ok else None
    return (anchors[cs], cs) if anchors[cs] else None

def scoped_window(site, size, extra_mask=(), back=False):
    """A window around the instruction at site that's unique within its chunk: (anchor, range, Pat)."""
    an = anchor_of(site)
    if not an: return None
    name, cs = an
    rng = fn_end(cs) - cs  # (the scan uses the new build's .pdata extent of the anchor; this for leafs)
    for loose in (True, False):
        for start, stop in windows(site, size, 64, 1 << 20, back):
            if stop - start >= 8:
                pt = Pat(start, stop, extra_mask)
                if pt.count_in(cs, cs + rng, loose=loose) == 1: return name, rng, pt
    return None

def scoped_after(site, size, extra_mask=(), maxins=40):
    """The shortest window that starts at the instruction at site (whole instructions after it only, so what the new
    build put before it can differ) and is unique within its chunk's function with the registers ignored (level 2):
    (anchor, range, Pat) or None. For a read that has no other window that survives other registers."""
    an = anchor_of(site)
    if not an: return None
    name, cs = an
    rng = fn_end(cs) - cs
    L = insns(chunk_start(site), min(fn_end(site), site + 24 * maxins))
    i0 = next((i for i, x in enumerate(L) if x[0] == site), None)
    if i0 is None: return None
    for hi in range(i0 + 1, min(len(L), i0 + maxins) + 1):
        stop = L[hi - 1][0] + L[hi - 1][1]
        if stop - site < 8: continue
        pt = Pat(site, stop, extra_mask)
        if pt.count_in(cs, cs + rng, loose=2) == 1: return name, rng, pt
    return None

def vt_slots(vt):
    res = []
    for k in range(1024):
        w = img(vt + 8 * k, 8)
        if len(w) < 8: break
        f = struct.unpack('<Q', w)[0] - base
        if not in_text(f): break
        res.append(f)
    return res
def all_vtables():
    """Every vtable with RTTI in the image: [(rva, 'rtti:<decorated>:<offset>')]. A locator is {1, offset, cdOffset,
    typeDescriptor rva, classDescriptor rva, its own rva}; a vtable's [-1] points at it."""
    d = np.frombuffer(pe.__data__, dtype=np.uint8)
    res = []
    for lo, hi in rdata:
        sec = next(s_ for s_ in pe.sections if s_.VirtualAddress == lo)
        raw = pe.__data__[sec.PointerToRawData:sec.PointerToRawData + min(sec.SizeOfRawData, hi - lo)]
        u = np.frombuffer(raw[:len(raw) // 4 * 4], dtype=np.uint32)
        idx = np.arange(len(u), dtype=np.int64) * 4 + lo
        cols = idx[:-6][(u[:-6] == 1) & (u[5:-1] == idx[:-6].astype(np.uint32))]
        colset = {int(c) + base for c in cols}
        q = np.frombuffer(raw[:len(raw) // 8 * 8], dtype=np.uint64)
        for i in np.nonzero(np.isin(q, np.array(sorted(colset), dtype=np.uint64)))[0]:
            vt = lo + int(i) * 8 + 8
            col = int(q[i]) - base
            off, td = struct.unpack('<II', img(col + 4, 4) + img(col + 12, 4))
            res.append((vt, 'rtti:%s:%d' % (td_name(td), off)))
    return res
_inslot = None
_vtname = {}
def emit_fnvt(name, a):
    """name -> a as the slot of a vtable (found by RTTI) whose function starts with a's first bytes."""
    global _inslot
    if _inslot is None:
        _inslot = {}
        for vn in sorted(rtti):  # (sorted: a set's order changes from run to run; the first vtable wins a slot)
            if vn.endswith('+0x10'):
                _vtname[tbl['addr'][vn]] = vn
        for vt, rn in all_vtables():
            _vtname.setdefault(vt, rn)
        for vt, vn in _vtname.items():
            for f in vt_slots(vt): _inslot.setdefault(f, vt)
    vt = _inslot.get(a)
    if vt is None: return False
    vn = _vtname[vt]
    if vn.startswith('rtti:') and sum(1 for x in _vtname.values() if x == vn) > 1: return False  # (not one vtable)
    slots = vt_slots(vt)
    for loose in (True, False):
        for L in (16, 32, 64, 128):
            pt = Pat(a, whole_end(a, L))
            if not any(g != a and pt.at(g - T0, loose) for g in slots):
                out.append('fnvt %s %s %s' % (E(name), E(vn), pt.text())); return True
    # look-alikes that differ only in what they call: its place among them, in slot order
    pt = Pat(a, whole_end(a, 64))
    for loose in (False, True):
        same = []
        for g in slots:
            if pt.at(g - T0, loose) and g not in same: same.append(g)
        if a in same and len(same) <= 4:
            out.append('fnvt %s %s %s %d %d' % (E(name), E(vn), pt.text(), same.index(a), len(same))); return True
    return False

# ---- the table
tbl = {'addr': {}, 'off': [], 'hook': {}, 'adj': [], 'hookreg': []}
for line in open(tblfile):
    w = line.split()
    if not w or w[0].startswith('#'): continue
    if w[0] == 'addr': tbl['addr'][' '.join(w[1:-1])] = int(w[-1], 16)
    elif w[0] == 'off':
        key = ' '.join(w[1:-1])
        if (key, int(w[-1], 16)) not in tbl['off']: tbl['off'].append((key, int(w[-1], 16)))
    elif w[0] == 'hook': tbl['hook'].setdefault(w[1], []).append((w[2], int(w[3], 16), int(w[4], 16)))
    elif w[0] == 'adj': tbl['adj'].append((w[1], int(w[2])))
    elif w[0] == 'hookreg': tbl['hookreg'].append(w[1:])
byaddr = {}
for n, a in tbl['addr'].items(): byaddr.setdefault(a, []).append(n)
_pset = set(pbeg)
leafs[:] = sorted({a for a in byaddr if in_text(a) and a not in _pset})

def E(n): return n.replace(' ', '~')  # names and keys have no spaces in the file
out = ['# platendoos signatures from rekordbox.exe %s (tools/win/makesigs.py)' % build, 'build ' + build]
miss = []

# ---- RTTI: a vtable's [-1] is its complete object locator {sig, offset, cdOffset, typeDescriptor rva, ...}; a
# type descriptor is {vtable ptr, spare, name}
def td_name(td): return img(td + 16, 256).split(b'\0')[0].decode('latin-1')
def col_of(vt):
    colva = struct.unpack('<Q', img(vt - 8, 8))[0]
    sig, off, cdoff, td = struct.unpack('<IIII', img(colva - base, 16))
    return off, td
rtti = set()
for n, a in tbl['addr'].items():
    if n.startswith('__ZTV'):
        vt = a + (0x10 if not n.endswith('+0x10') else 0)
        try:
            off, td = col_of(vt)
            nm = td_name(td)
            assert nm.startswith('.?A')
        except Exception:
            miss.append('vt ' + n); continue
        out.append('vt %s %d %s %d' % (E(n), a - vt, nm, off)); rtti.add(n)
    elif n.startswith('__ZTI'):
        try:
            nm = td_name(a); assert nm.startswith('.?A')
        except Exception:
            miss.append('ti ' + n); continue
        out.append('ti %s %s' % (E(n), nm)); rtti.add(n)

# ---- calls and rip-relative references into each address, for the ones not unique by their own bytes
arr = np.frombuffer(TB, dtype=np.uint8)
def callers(target):
    """call/jmp rel32 sites to target."""
    res = []
    for op in (0xe8, 0xe9):
        idx = np.nonzero(arr[:-5] == op)[0]
        rel = (arr[idx + 1].astype(np.int64) | (arr[idx + 2].astype(np.int64) << 8) | (arr[idx + 3].astype(np.int64) << 16) | (arr[idx + 4].astype(np.int64) << 24))
        rel = np.where(rel >= 1 << 31, rel - (1 << 32), rel)
        res += [int(i) + T0 for i in idx[(idx + T0 + 5 + rel) == target]]
    return res
i32 = None
def riprefs(target):
    """Instructions with a rip displacement to target: (instruction address, size, displacement offset)."""
    global i32
    if i32 is None:
        n = len(TB) - 8
        i32 = (arr[0:n].astype(np.int64) | (arr[1:n + 1].astype(np.int64) << 8) | (arr[2:n + 2].astype(np.int64) << 16) | (arr[3:n + 3].astype(np.int64) << 24))
        i32 = np.where(i32 >= 1 << 31, i32 - (1 << 32), i32)
    pos = np.arange(len(i32), dtype=np.int64) + T0
    res = []
    for k in (0, 1, 2, 4):  # bytes of immediate after the displacement
        for p in np.nonzero(pos + 4 + k + i32 == target)[0][:50]:
            p = int(p) + T0
            cs = chunk_start(p)
            if cs is None: continue
            for a, sz, m, ins, _ in insns(cs, min(fn_end(cs), p + 16)):
                if a <= p < a + sz and ins.disp_offset == p - a and a + sz == p + 4 + k:
                    res.append((a, sz, ins.disp_offset))
    return res

def emit_ref(name, a, scoped):
    """name -> a through a window around a call or reference to it (unique in .text, or scoped to a found function)."""
    cands = [(site, 5, 1) for site in (callers(a) if in_text(a) else [])] + riprefs(a)
    if not scoped:
        for site, sz, do in cands[:40]:
            pt = unique_around(site, sz)
            if pt:
                out.append('ref %s %s %d %d' % (E(name), pt.text(), site + do - pt.start, site + sz - pt.start)); return True
        return False
    # sites in found functions first
    have = set(found.values())
    cands.sort(key=lambda c: (chunk_start(c[0]) not in have, c[0]))
    for site, sz, do in cands[:500]:
        u = scoped_window(site, sz)
        if u:
            an, rng, pt = u
            out.append('refin %s %s %x %s %d %d' % (E(name), E(an), rng, pt.text(), site + do - pt.start, site + sz - pt.start)); return True
    return False

left = []
for n, a in sorted(tbl['addr'].items()):
    if n in rtti: continue
    u = unique_from(a, 512) if in_text(a) else None
    if u: out.append('fn %s %s' % (E(n), u.text())); found[n] = a
    else: left.append((n, a))
for scoped in (False, True, True):
    still = []
    for n, a in left:
        if emit_ref(n, a, scoped): found[n] = a
        else: still.append((n, a))
    left = still
    anchors = {k: v for k, v in anchors.items() if v}  # (a chunk that had no anchor may have one now)
# virtual methods: by their bytes among the slots of a vtable that has them
left = [(n, a) for n, a in left if not (emit_fnvt(n, a) and found.setdefault(n, a) is not None)]
miss += ['addr ' + n for n, a in left]

# ---- hooks
for name, sites in sorted(tbl['hook'].items()):
    out.append('hooksites %s %d' % (name, len(sites)))
    tnames = byaddr.get(sites[0][2], [])
    tname = next((t for t in tnames if t in found), '-')
    # a lea site takes a function's address (a callback): the function is named too (an @<rva> anchor when the table
    # has no name for it), so a new build's site must take the address of the same function
    if tname == '-' and sites[0][0] == 'lea':
        an = anchor_of(sites[0][2])
        if an and an[1] == sites[0][2]: tname = an[0]
    for kind, site, target in sites:
        size = 6 if kind == 'icall' else 5
        # fallback: the site's place among the calls of the target in its function (both found some other way)
        if kind in ('call', 'jmp'):
            ca, ta = anchor_of(site), (tname if tname != '-' else (anchor_of(target) or (None,))[0])
            if ca and ta and anchor_of(target) and anchor_of(target)[1] == target:
                cs = ca[1]
                op = 0xe9 if kind == 'jmp' else 0xe8
                at = [q for q in callers(target) if cs <= q < fn_end(cs) and TB[q - T0] == op]
                if site in at:
                    out.append('hookcall %s %s %s %x %s %d %d' % (name, kind, E(ca[0]), fn_end(cs) - cs, E(ta), at.index(site), len(at)))
            # the target's only calls in all of .text are the hook's (anchor *): its caller needn't be found
            if ta and anchor_of(target) and anchor_of(target)[1] == target:
                op = 0xe9 if kind == 'jmp' else 0xe8
                at = sorted(q for q in callers(target) if TB[q - T0] == op)
                if site in at and len(at) <= 4:
                    out.append('hookcall %s %s * 0 %s %d %d' % (name, kind, E(ta), at.index(site), len(at)))
        pt = unique_around(site, size)
        if pt:
            out.append('hook %s %s %s %d %s' % (name, kind, pt.text(), site - pt.start, E(tname))); continue
        u = scoped_window(site, size)
        if not u: miss.append('hook %s %x' % (name, site)); continue
        an, rng, pt = u
        out.append('hookin %s %s %s %x %s %d %s' % (name, kind, E(an), rng, pt.text(), site - pt.start, E(tname)))

# ---- offsets
wfun = pickle.load(open(os.path.join(work, 'win.pkl'), 'rb'))['funcs']
def chunks(fn_rva):
    va = fn_rva + base
    return [(c - base, c - base + f['size']) for c, f in wfun.items() if f['root'] == va] or [(fn_rva, fn_end(fn_rva))]
def wide_around(site, size, mask, span=48):
    """('' or 'in', Pat, anchor prefix): whole instructions from ~span before to ~span after the one at site, unique
    (loosely) in .text, or else within its found function."""
    cs = chunk_start(site)
    if cs is None: return None
    L = insns(cs, min(fn_end(cs), site + size + span))
    lo = next((x[0] for x in L if x[0] >= site - span), site)
    hi = L[-1][0] + L[-1][1] if L else site + size
    pt = Pat(lo, hi, mask)
    if pt.count(loose=True) == 1: return ('', pt, '')
    an = anchor_of(site)
    if an and pt.count_in(an[1], fn_end(an[1]), loose=True) == 1:
        return ('in', pt, '%s %x ' % (E(an[0]), fn_end(an[1]) - an[1]))
    return None

def disp_anchors(fn_rva, value, limit=3, like=None):
    """Windows around instructions in the function with that displacement (or 8/32-bit immediate), unique in .text or
    in a found function; like: a regex the instruction ("mnemonic operands") must match. Lines' tails:
    'off' <pattern> <dispPos> <dispSize>, or 'offin' <anchor> <range> <pattern> <dispPos> <dispSize>."""
    res = []
    for cs, ce in chunks(fn_rva):
        for a, sz, m, ins, _ in insns(cs, ce):
            if like and not re.search(like, ins.mnemonic + ' ' + ins.op_str): continue
            for off, size in ((ins.disp_offset, ins.disp_size), (ins.imm_offset, ins.imm_size)):
                if not size or size not in (1, 4) or off in m: continue
                raw = int.from_bytes(TB[a - T0 + off:a - T0 + off + size], 'little', signed=True)
                if raw != value: continue
                mask = range(a + off, a + off + size)
                # a wide window too: a new build may have look-alike reads nearby (three flag setters in a row)
                # that the smallest unique window can't tell apart, but its place among them can
                wide = wide_around(a, sz, mask)
                if wide:
                    k, pt, pre = wide
                    res.append((k, '%s%s %d %d' % (pre, pt.text(), a + off - pt.start, size)))
                pt = unique_around(a, sz, mask)
                if pt: res.append(('', '%s %d %d' % (pt.text(), a + off - pt.start, size)))
                else:
                    u = scoped_window(a, sz, mask)
                    if u:
                        an, rng, pt = u
                        res.append(('in', '%s %x %s %d %d' % (E(an), rng, pt.text(), a + off - pt.start, size)))
                    else:  # identical copies of this code: all of them must read the same field
                        for start, stop in windows(a, sz, 32, 32):
                            if stop - start < 8: continue
                            pt = Pat(start, stop, mask)
                            n = pt.count(9)
                            if 1 <= n <= 8:
                                res.append(('any', '%s %d %d' % (pt.text(), a + off - pt.start, size))); break
                if len(res) >= limit: break
                break
            if len(res) >= limit: break
        if len(res) >= limit: break
    for cs, ce in chunks(fn_rva):  # one more per read, outside the limit: a window that starts at the read and is
        for a, sz, m, ins, _ in insns(cs, ce):  # unique in the function with the registers ignored, which a new build
            if like and not re.search(like, ins.mnemonic + ' ' + ins.op_str): continue  # with other registers (and
            for off, size in ((ins.disp_offset, ins.disp_size), (ins.imm_offset, ins.imm_size)):  # code before the
                if not size or size not in (1, 4) or off in m: continue  # read) can still match
                if int.from_bytes(TB[a - T0 + off:a - T0 + off + size], 'little', signed=True) != value: continue
                u = scoped_after(a, sz, range(a + off, a + off + size))
                if u:
                    an, rng, pt = u
                    line = ('in', '%s %x %s %d %d' % (E(an), rng, pt.text(), a + off - pt.start, size))
                    if line not in res and len(res) < limit + 3: res.append(line)
                break
    return res
def seq_values(lo, hi, deltas, gap):
    """offseq: the values v for which v + deltas[0], v + deltas[1], ... are dwords in [lo, hi) in that order, each
    within gap bytes of the one before (retable does the same)."""
    vals = set()
    for q in range(lo - T0, hi - T0 - 3):
        v = struct.unpack_from('<i', TB, q)[0] - deltas[0]
        p = q
        for d in deltas[1:]:
            want = v + d
            if not -0x80000000 <= want < 0x80000000: p = -1; break
            p = TB.find(struct.pack("<i", want), p + 4, min(p + 7 + gap, hi - T0))
            if p < 0: break
        if p >= 0: vals.add(v)
        if len(vals) > 1: break
    return vals

def emit_offseq(key, value, fn, a, like=None):
    """A field read through a register whose struct moved (or whose register changed): the displacements of the
    reads through the same register around it, as differences from the field. offseq <key> <value> <fn> <range>
    <gap> <d0,d1,...>"""
    for cs, ce in chunks(a):
        L = insns(cs, ce)
        mem = []  # (index, disp address, disp, base)
        for i, (ia, sz, m, ins, _) in enumerate(L):
            if ins.disp_size != 4: continue
            for o in ins.operands:
                if o.type == capstone.x86.X86_OP_MEM and o.mem.base not in (0, capstone.x86.X86_REG_RIP) and o.mem.index == 0:
                    mem.append((i, ia + ins.disp_offset, ins.disp, o.mem.base)); break
        for i, da, d, b in mem:
            if d != value: continue
            if like and not re.search(like, L[i][3].mnemonic + ' ' + L[i][3].op_str): continue
            near = [x for x in mem if x[3] == b and abs(x[0] - i) <= 16]
            near.sort(key=lambda x: abs(x[0] - i))
            near = sorted(near[:8], key=lambda x: x[1])
            if len(near) < 4 or len({x[2] for x in near}) < 3: continue
            deltas = [x[2] - value for x in near]
            gap = max(near[j + 1][1] - near[j][1] for j in range(len(near) - 1)) + 32
            rng = fn_end(cs) - cs
            if seq_values(cs, cs + rng, deltas, gap) != {value}: continue
            return 'offseq %s %x %s %x %d %s' % (E(key), value, E(fn), rng, gap, ','.join(hexs(x) for x in deltas))
    return None
def field_base(a, value, like=None):
    """The base register of the first instruction in the function that reads the field (disp32), or None."""
    for cs, ce in chunks(a):
        for ia, sz, m, ins, _ in insns(cs, ce):
            if ins.disp_size != 4 or ins.disp != value: continue
            if like and not re.search(like, ins.mnemonic + ' ' + ins.op_str): continue
            for o in ins.operands:
                if o.type == capstone.x86.X86_OP_MEM and o.mem.base not in (0, capstone.x86.X86_REG_RIP) and o.mem.index == 0:
                    return o.mem.base
    return None
def hexs(v): return ('-%x' % -v) if v < 0 else '%x' % v

import json, glob
vnotes = {}  # (fn, value) -> Windows addresses named in the verification of that field
vfns = {}    # (fn, value) -> only the function(s) its "win_fn" names: where a pinned field (sigfields.txt) is read
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import verifydata
for f, entries in verifydata.out_files(repo, work):
    for r in entries:
        if r.get('kind') != 'offset' or r.get('value') is None: continue
        k = (r.get('fn'), int(str(r['value']), 0))
        for x in re.findall(r'0x14[0-9a-f]{7}', (r.get('win_fn') or '') + ' ' + (r.get('why') or '')):
            vnotes.setdefault(k, set()).add(int(x, 16) - base)
        for x in re.findall(r'0x14[0-9a-f]{7}', r.get('win_fn') or ''):
            vfns.setdefault(k, set()).add(int(x, 16) - base)
def anchors_in(rvas, value, like=None):
    res = []
    for a in sorted(rvas):
        cs = a if a in byaddr else chunk_start(a)
        if cs is None: continue
        res += disp_anchors(cs, value, 3 - len(res), like)
        if len(res) >= 3: break
    return res
# instructions some fields must be anchored on (tools/win/sigfields.txt: <key, spaces as ~> <regex>)
likes = {}
for line in verifydata.sigfields_lines(repo):
    if line.startswith('#') or not line.strip(): continue
    k, rx = line.rstrip('\n').split(' ', 1)
    likes[k.replace('~', ' ')] = rx.strip()
adjs = dict(tbl['adj'])
vtused = set()
for key, value in tbl['off']:
    fn, kind, nth = key.split('|')
    if kind.startswith('vt:'):
        done = False
        if fn in found or fn in rtti:
            if kind[3:] in found: out.append('offvt %s %x %s %s' % (E(key), value, E(fn), E(kind[3:]))); done = True; vtused.add(fn)
        vtn = fn + '+0x10'
        if vtn in rtti:  # the method in the old vtable's slot, by its first bytes
            vtused.add(vtn)
            m = struct.unpack('<Q', img(tbl['addr'][vtn] + value, 8))[0] - base
            if in_text(m):
                out.append('offvtfn %s %x %s %s' % (E(key), value, E(vtn), Pat(m, whole_end(m, 48)).text())); done = True
        if not done: miss.append('off ' + key)
        continue
    a = tbl['addr'].get(fn)
    an = disp_anchors(a, value, like=likes.get(key)) if a is not None else []
    if not an and key not in likes: an = anchors_in(vnotes.get((fn, value), ()), value)
    # a pinned field read in a function the table has no address for (ListViewSorter::compareElements): only in the
    # function its verification names as "win_fn", and only through the pinned instruction
    if not an and key in likes and a is None: an = anchors_in(vfns.get((fn, value), ()), value, likes[key])
    for k, rest in an: out.append('off%s %s %x %s' % (k, E(key), value, rest))
    if not an and a is not None and fn in adjs:  # a field of the whole object, read through the subobject `this`
        for k, rest in disp_anchors(a, value - adjs[fn]):
            out.append('offadj%s %s %x %s %s' % (k, E(key), value, E(fn), rest)); an = True
    if fn in found:
        out.append('offsame %s %x %s %s' % (E(key), value, E(fn), Pat(a, whole_end(a, 2048)).text()))
        sq = emit_offseq(key, value, fn, a, likes.get(key))
        if sq: out.append(sq)
    elif not an: miss.append('off ' + key)

# offrel: a field whose own lines may all miss keeps its distance to the nearest field of the same function read
# through the same register (the same struct, or the same vtable)
byfn = {}
for key, value in tbl['off']:
    fn, kind, nth = key.split('|')
    if kind.startswith('vt:') or fn not in found: continue
    b = field_base(tbl['addr'][fn], value, likes.get(key))
    if b is not None: byfn.setdefault((fn, b), []).append((key, value))
for (fn, b), fs in byfn.items():
    for key, value in fs:
        sib = [(abs(v - value), k, v) for k, v in fs if k != key and 0 < abs(v - value) <= 0x100]
        if sib:
            _, k, v = min(sib)
            out.append('offrel %s %x %s %x' % (E(key), value, E(k), v))

for n in sorted(vtused):
    if n in rtti: out.append('vtslots %s %d' % (E(n), len(vt_slots(tbl['addr'][n] + (0 if n.endswith('+0x10') else 0x10)))))
for n, d in tbl['adj']: out.append('adj %s %d' % (E(n), d))
for r in tbl['hookreg']: out.append('hookreg ' + ' '.join(r))

# ---- guards: constants in the mods' sources (tools/win/sigguards.txt: <fn symbol> <hex value> <hooks> <regex>)
gfile = os.path.join(repo, 'tools', 'win', 'sigguards.txt')
for line in open(gfile) if os.path.exists(gfile) else []:
    w = line.split('#')[0].split(None, 3)
    if len(w) != 4: continue
    fn, value, hooks, like = w[0], int(w[1], 16), w[2], w[3].strip()
    a = tbl['addr'].get(fn)
    an = disp_anchors(a, value, 1, like) if a is not None else []
    if not an: miss.append('guard %s %s' % (fn, w[1])); continue
    k, rest = an[0]
    out.append('guard%s %s %x %s' % (k, hooks, value, rest))

# ---- fallbacks: more ways to each function or data the mods use (and to the anchors of scoped lines), through
# calls and references from other found functions. A function whose own bytes changed, or matches several places
# loosely in a new build (all SingletonHolder<T>::get are the same but for the object size), is still found
# through a caller that's found. retable keeps the first line that resolves a name.
def anchor_name(cs):
    an = anchors.get(cs)
    if an: return an
    named = [n for n, a in found.items() if a == cs and not n.startswith('__ZT')]
    return named[0] if named else None
def emit_fallbacks(name, a, k=3):
    used = set(re.findall(r'^refin %s (\S+) ' % re.escape(E(name)), '\n'.join(out), re.M))
    have = set(found.values()) | {c for c, v in anchors.items() if v}
    cands = [(site, 5, 1) for site in (callers(a) if in_text(a) else [])] + riprefs(a)
    # callers in found functions first, then any (anchor_of gives such a caller a signature of its own: an @<rva> line)
    cands = [c for c in cands if chunk_start(c[0]) is not None and chunk_start(c[0]) != a]
    cands.sort(key=lambda c: (chunk_start(c[0]) not in have, c[0]))
    n = tries = 0
    for site, sz, do in cands[:300]:
        cs = chunk_start(site)
        if cs not in have:
            tries += 1
            if tries > 12: break
        an = anchor_name(cs)
        if an and (an == name or E(an) in used): continue
        u = scoped_window(site, sz)
        if not u or E(u[0]) in used: continue
        an, rng, pt = u
        used.add(E(an))
        out.append('refin %s %s %x %s %d %d' % (E(name), E(an), rng, pt.text(), site + do - pt.start, site + sz - pt.start))
        # and one of the code before the call only (the code after it may change on its own)
        b = scoped_window(site, sz, back=True)
        if b and b[2].text() != pt.text():
            out.append('refin %s %s %x %s %d %d' % (E(name), E(b[0]), b[1], b[2].text(), site + do - b[2].start, site + sz - b[2].start))
        n += 1
        if n >= k: break
    return n
fb_names = {n: a for n, a in found.items() if not n.startswith(('__ZTV', '__ZTI', '__ZTS')) and not n.startswith('@')}
for line in out:  # anchors of the scoped lines
    w = line.split()
    an = w[2] if w[0] == 'refin' else w[3] if w[0] in ('offin', 'hookin', 'guardin') else None
    if an and an.startswith('@'): fb_names[an] = int(an[1:], 16)
fb = sum(1 for n, a in sorted(fb_names.items()) if emit_fallbacks(n, a))
# virtual methods have no direct callers: also by their bytes among their vtable's slots
have_fnvt = set(re.findall(r'^fnvt (\S+) ', '\n'.join(out), re.M))
fb += sum(1 for n, a in sorted(fb_names.items()) if E(n) not in have_fnvt and in_text(a) and emit_fnvt(n, a))
print('%d names have fallback lines' % fb, file=sys.stderr)

out += ['# not covered: ' + m for m in miss]
print('\n'.join(out))
print('%s: %d lines, %d not covered' % (build, len(out), len(miss)), file=sys.stderr)
