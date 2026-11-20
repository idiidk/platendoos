# Makes the Windows address table for one rekordbox build (see core/image_win.cpp for the format) from
# the matching work in a work dir (default ~/rbwin):
#   - verify/*.out.json   verified functions and hook sites (verdict ok/pick/wrong-with-fix; high or medium confidence)
#   - verify/offsets*.out.json  verified Windows field offsets
#   - vt.pkl              RTTI pairing: vtables and typeinfos by class name (exact, no verification needed)
#   - vtable slots for "vt:" recipes, computed from the Windows vtable and the verified method address
# Only verified or name-exact entries go in: a wrong address crashes rekordbox, a missing one switches a mod off.
#   python3 -I tools/win/maketable.py <rekordbox.exe> [workdir] > tools/win/tables/rekordbox-<build>.tbl
# The table is the edition's (RBM_EDITION, docs/editions.md): its verification data (tools/win/verifydata.py), its
# hooks (tools/build/hooksdef.py) and the vtable-slot recipes of its sources.
import sys, os, json, glob, pickle, re, struct
import pefile
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), 'build'))
import verifydata, hooksdef

exe = sys.argv[1]
work = sys.argv[2] if len(sys.argv) > 2 else os.path.expanduser('~/rbwin')
repo = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

pe = pefile.PE(exe, fast_load=True)
base = pe.OPTIONAL_HEADER.ImageBase
build = '%08X-%X' % (pe.FILE_HEADER.TimeDateStamp, pe.OPTIONAL_HEADER.SizeOfImage)
rva = lambda a: a - base

out = ['# platendoos address table for rekordbox.exe %s (made by tools/win/maketable.py)' % build, 'build ' + build]
addr = {}      # key -> rva
notes = []

# ---- verified functions and hooks (verify/overrides.json "drop": items, or item@site, found wrong afterwards, with why;
# an offset line by "off:<fn>|<kind>|<nth>", see offdrop)
edition = verifydata.edition(repo)
drop = verifydata.drops(repo, work, edition)
syms, hooks, vofs, hooknotes, hookregs = {}, {}, [], [], {}
# verification results: tools/win/verify in the repo (kept with the code), plus any newer ones in the work dir
for f, entries in verifydata.out_files(repo, work, edition):
    if os.path.basename(f).startswith('offsets'): continue
    for r in entries:
        if r.get('confidence') not in ('high', 'medium'): continue
        item, v = r.get('item'), r.get('verdict')
        # a drop key is an item, or item@site / item@win address: only that verdict (a later one may be right)
        if item in drop or any('%s@%s' % (item, a) in drop for a in (r.get('sites') or []) + [r.get('win')]): continue
        if r.get('kind') == 'hookreg':
            if r.get('reg') and r.get('arg') is not None: hookregs[item] = (r['reg'], int(r['arg']))
            continue
        if r.get('kind') == 'offset':
            if r.get('value') is not None and r.get('fn') and v in (None, 'ok', 'pick', 'wrong'):
                vofs.append(r)
            continue
        if r.get('kind') == 'hook' or r.get('sites') is not None:  # a hook
            if v != 'ok': hooknotes.append('hook %s: %s' % (item, v))
            if v == 'ok' and r['sites']: hooks[item] = r
        elif v in ('ok', 'pick', 'wrong') and r.get('win'):
            syms[item] = int(r['win'], 16)
for n, a in syms.items(): addr[n] = rva(a)

# ---- vtables and typeinfos by class name
vt = pickle.load(open(os.path.join(work, 'vt.pkl'), 'rb'))
mac = pickle.load(open(os.path.join(work, 'mac.pkl'), 'rb'))
tiname = {a: n for a, ns in mac['datanames'].items() for n in ns if n.startswith('__ZTI')}
# a vtable's mac symbol is 0x10 before its address point (offset-to-top, typeinfo); MSVC's vptr points at
# the vtable itself, so the symbol maps to 0x10 before it and the address point (+0x10) onto it
for msym, waddr in vt['vtables'].items():
    addr[msym + '+0x10'] = rva(waddr)
    addr[msym] = rva(waddr) - 0x10
for ti, td in vt['typeinfo'].items():
    if ti in tiname: addr[tiname[ti]] = rva(td)

# ---- SingletonHolder Addrs. The mods only pass the holder to SingletonHolder<T>::get(holder); MSVC's get()
# takes no argument (its holder is a global it uses itself), so on Windows the holder is that global: the
# first rip-relative data reference in the verified get().
import capstone, glob as _g
md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
def first_data_ref(fn):
    o = pe.get_offset_from_rva(rva(fn))
    for (a, sz, mn, op) in md.disasm_lite(pe.__data__[o:o + 0x200], fn):
        m = re.search(r'\[rip ([+-]) (0x[0-9a-f]+)\]', op)
        if m: return a + sz + int(m.group(2), 16) * (1 if m.group(1) == '+' else -1)
        if mn in ('ret', 'jmp'): break
    return None
for f in _g.glob(os.path.join(repo, 'core', '*.cpp')) + _g.glob(os.path.join(repo, 'mods', '*.cpp')):
    src = open(f).read()
    for what in re.findall(r'Addr \w+\{"(SingletonHolder<(\w+)>)"', src):
        key, cls = what
        if key in addr: continue
        gets = [n for n in syms if 'SingletonHolder' in n and n.endswith('3getEv') and re.search(r'\d%s' % cls + r'ENS_', n)]
        if len(gets) != 1:
            notes.append('holder %s: get() not verified' % key); continue
        ref = first_data_ref(syms[gets[0]])
        if ref: addr[key] = rva(ref)
        else: notes.append('holder %s: no data reference in get()' % key)

# ---- this adjustments (rbm::thisDelta): a function that only sits in vtables at a non-zero subobject offset
# takes that subobject as `this` on Windows; mac passes the whole object (or object + N for a __ZThn<N>_ thunk)
where = {}
for nm, lst in vt['winvt'].items():
    for va, off, sl in lst:
        for f in sl: where.setdefault(f, set()).add(off)
adj_lines = []
for n, a in syms.items():
    offs = where.get(a)
    if not offs or 0 in offs: continue
    if len(offs) > 1: notes.append('adj %s: several subobject offsets %s' % (n, sorted(offs))); continue
    m = re.match(r'__ZThn(\d+)_', n)
    d = offs.pop() - (int(m.group(1)) if m else 0)
    if d: adj_lines.append('adj %s %d' % (n, d))

# ---- hooks.def: sites from verification
hookdef = {}
for line in hooksdef.lines(repo, edition):
    m = re.match(r'HOOK\((\w+), "([^"]+)", (\d+), (\d+), "([^"]+)", (-?\d+)\)', line)
    if m: hookdef[m.group(1)] = dict(caller=m.group(2), target=m.group(5), kind=int(m.group(6)))
data = pe.__data__
def site_kind(a):
    o = pe.get_offset_from_rva(rva(a))
    b0, b1 = data[o], data[o + 1]
    if b0 == 0xE8: return 'call', a + 5 + struct.unpack_from('<i', data, o + 1)[0]
    if b0 == 0xE9: return 'jmp', a + 5 + struct.unpack_from('<i', data, o + 1)[0]
    if b0 == 0xFF and b1 == 0x15:
        slot = a + 6 + struct.unpack_from('<i', data, o + 2)[0]
        return 'icall', struct.unpack_from('<Q', data, pe.get_offset_from_rva(rva(slot)))[0]
    if (b0 & 0xFB) == 0x48 and b1 == 0x8D and (data[o + 2] & 0xC7) == 0x05:  # lea reg,[rip+target]: its address is taken
        return 'lea', a + 7 + struct.unpack_from('<i', data, o + 3)[0]
    return None, None
hook_lines = []
for name, r in hooks.items():
    if name not in hookdef or not r.get('sites'):
        notes.append('hook %s: no sites, left out' % name)
        continue
    # several sites: the same call in each copy of an inlined caller (all must call the same target)
    found = [(int(x, 16),) + tuple(site_kind(int(x, 16))) for x in r['sites']]
    bad = [site for site, kind, target in found if not kind or (kind, target) != found[0][1:]]
    if bad:
        notes.append('hook %s: %x is not a call/jmp (of the first site\'s target), left out' % (name, bad[0])); continue
    hook_lines += ['hook %s %s %x %x' % (name, kind, rva(site), rva(target)) for site, kind, target in found]

# ---- offsets: verified Windows values, and vtable slots computed from the Windows vtables
off_lines = []
# a drop key "off:<fn>|<kind>|<nth>" drops that one offset line (the item of an offset entry is often just its
# function's name, which would drop the function's other fields too)
def offdrop(r): return 'off:%s|%s|%d' % (r['fn'], r.get('kind_recipe', r['kind']), r['nth']) in drop
for f in sorted(glob.glob(os.path.join(work, 'verify', 'offsets*.out.json'))):
    for r in json.load(open(f)):
        if r.get('confidence') in ('high', 'medium') and r.get('value') is not None and not offdrop(r):
            off_lines.append('off %s|%s|%d %x' % (r['fn'], r['kind'], r['nth'], int(str(r['value']), 0)))
for r in vofs:
    if offdrop(r): continue
    off_lines.append('off %s|%s|%d %x' % (r['fn'], r['kind_recipe'], r['nth'], int(str(r['value']), 0)))
srcoffs = json.load(open(os.path.join(work, 'offsets.json')))
# (only the recipes of this edition's sources: offsets.json is made from all of them, tools/win/match/offsets_src.py)
srcdirs = ('core', 'mods', 'engines') + (('private/mods',) if edition == 'full' else ())
srcnames = {os.path.basename(f) for d in srcdirs for f in glob.glob(os.path.join(repo, d, '*'))}
srcoffs = [o for o in srcoffs if o.get('file') in srcnames]
wslots = {}
for nm, lst in vt['winvt'].items():
    for a, ofs, slots in lst: wslots[a] = slots
macaddr = {}
for a, f in mac['funcs'].items() if isinstance(mac['funcs'], dict) else mac['funcs']:
    for n in f.get('names', []): macaddr[n] = a
seedmap = {}
for m, w, _ in vt['seeds']: seedmap.setdefault(m, set()).add(w)
dtor = re.compile(r'D[012]Ev$')
macfuncs = mac['funcs'] if isinstance(mac['funcs'], dict) else dict(mac['funcs'])
macbin = open(os.path.join(work, 'mac', 'rekordbox_x86'), 'rb').read()
tiaddrs = {a for a, ns in mac['datanames'].items() if any(n.startswith('__ZTI') for n in ns)}
def macword(a):
    for va, sz, off, nm in mac['secs']:
        if va <= a < va + sz: return struct.unpack_from('<Q', macbin, off + a - va)[0]
    return None
for o in srcoffs:
    if not o['kind'].startswith('vt:'): continue
    va = vt['vtables'].get(o['fn']); meth = o['kind'][3:]
    if va is None or va not in wslots or o['fn'] not in vt['macvt']:
        notes.append('vt recipe %s / %s: vtable not paired' % (o['fn'], o['kind'])); continue
    win = wslots[va]
    if meth in syms:  # verified method: its slot in the Windows vtable
        hits = [i for i, x in enumerate(win) if x == syms[meth]]
        if len(hits) == 1 and o['nth'] == 0:  # (a stub shared by several slots: by position below)
            off_lines.append('off %s|%s|%d %x' % (o['fn'], o['kind'], o['nth'], 8 * hits[o['nth']])); continue
    # else by position: Itanium's two destructor slots are one (the deleting one) in MSVC's vtable, and MSVC
    # puts a group of overloads in reverse order. (vt.pkl's mac slots stop at the first null one: read them here.)
    vaddr = next((a for a, ns in mac['datanames'].items() if o['fn'] in ns), None)
    ma = macaddr.get(meth)
    mslots = [macword(vaddr + 16 + 8 * k) for k in range(o['expected'] // 8 + 1)] if vaddr else []
    mhits = [i for i, x in enumerate(mslots) if x == ma]
    if ma is None or len(mhits) <= o['nth']:
        notes.append('vt recipe %s / %s: method not in the mac vtable' % (o['fn'], o['kind'])); continue
    i = mhits[o['nth']]
    # words between +0x10 and the address point (virtual-base offsets put the typeinfo later): the address
    # point is just past the first typeinfo pointer
    skip = next((k for k in range(0, 8) if macword(vaddr + 8 * k) in tiaddrs), 1) - 1
    if 8 * i != o['expected']:
        notes.append('vt recipe %s / %s: mac slot %x != expected %x' % (o['fn'], o['kind'], 8 * i, o['expected'])); continue
    nm = lambda a: (macfuncs.get(a, {}).get('names') or [''])[0]
    ndtor = sum(1 for x in mslots[skip:i] if dtor.search(nm(x)))
    base = lambda n: re.sub(r'E.*$', '', n.replace('__ZNK', '__ZN'))  # __ZN<class><name> without the parameters
    g0 = i
    while g0 > 0 and base(nm(mslots[g0 - 1])) == base(meth): g0 -= 1
    g1 = i
    while g1 + 1 < len(mslots) and base(nm(mslots[g1 + 1])) == base(meth): g1 += 1
    while g1 + 1 < 4096 and base(nm(macword(vaddr + 16 + 8 * (g1 + 1)))) == base(meth): g1 += 1
    wi = (g0 + (g1 - i)) - skip - ndtor // 2
    if wi >= len(win):
        notes.append('vt recipe %s / %s: Windows vtable too short' % (o['fn'], o['kind'])); continue
    if ma in seedmap and win[wi] not in seedmap[ma]:
        notes.append('vt recipe %s / %s: Windows slot %d disagrees with the pairing' % (o['fn'], o['kind'], wi)); continue
    if g1 > g0 and ma not in seedmap:
        notes.append('vt recipe %s / %s: overloads, and no pairing to confirm slot %d' % (o['fn'], o['kind'], wi)); continue
    off_lines.append('off %s|%s|%d %x' % (o['fn'], o['kind'], o['nth'], 8 * wi))

out += ['addr %s %x' % (k, v) for k, v in sorted(addr.items())]
reg_lines = ['hookreg %s %s %d' % (n, r, a) for n, (r, a) in hookregs.items() if any(l.split()[1] == n for l in hook_lines)]
out += sorted(off_lines) + sorted(hook_lines) + sorted(reg_lines) + sorted(adj_lines)
out += ['# ' + n for n in notes + hooknotes] + ['# dropped %s: %s' % kv for kv in drop.items()]
print('\n'.join(out))
print('%s: %d addresses, %d fields, %d hooks; %d notes' % (build, len(addr), len(off_lines), len(hook_lines), len(notes)), file=sys.stderr)
