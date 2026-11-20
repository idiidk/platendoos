# Pairs mac (Itanium) and Windows (MSVC RTTI) vtables by class name, and their slots.
#   python3 -I vtables.py mac.pkl <macho_x86_64> <rekordbox.exe> seeds.pkl
# seeds.pkl: dict(seeds=[(mac_fn, win_fn, 'vt')], vtables={mac_vtable_sym: win_vtable_addr}, winvt={name: [(addr, offset, slots)]})
import sys, pickle, struct, subprocess, collections, re
import pefile

mac = pickle.load(open(sys.argv[1], 'rb'))
macho, exe, outp = sys.argv[2], sys.argv[3], sys.argv[4]
M = mac['funcs']
names = {}
for a, f in M.items():
    for n in f['names']: names[n] = a

# ---- mac vtables
d = open(macho, 'rb').read()
segs = []
o = subprocess.run(['otool', '-l', macho], capture_output=True, text=True).stdout.split('\n')
cur = {}
for l in o:
    p = l.split()
    if len(p) == 2 and p[0] in ('vmaddr', 'vmsize', 'fileoff'): cur[p[0]] = int(p[1], 0)
    if len(p) == 2 and p[0] == 'filesize': segs.append(dict(cur))
def mfo(a):
    for s in segs:
        if s['vmaddr'] <= a < s['vmaddr'] + s['vmsize']: return s['fileoff'] + a - s['vmaddr']
vtsyms = sorted((a, n) for a, ns in mac['datanames'].items() for n in ns if n.startswith('__ZTV'))
allsyms = sorted(mac['datanames'])
import bisect
macvt = {}
for a, n in vtsyms:
    i = bisect.bisect_right(allsyms, a)
    end = allsyms[i] if i < len(allsyms) else a + 0x10000
    fo = mfo(a)
    slots = []
    k = a + 16
    while k + 8 <= end:
        v, = struct.unpack_from('<Q', d, fo + k - a)
        if v not in M and not (0x100005c40 <= v < 0x1038c4814): break
        slots.append(v); k += 8
    macvt[n] = (a, slots)
dem = subprocess.run(['c++filt'], input='\n'.join(n[1:] for n in macvt), capture_output=True, text=True).stdout.split('\n')
mac_byclass = {}
for n, dn in zip(macvt, dem):
    if dn.startswith('vtable for '): mac_byclass[dn[len('vtable for '):]] = n

# ---- win RTTI
pe = pefile.PE(exe, fast_load=True)
base = pe.OPTIONAL_HEADER.ImageBase
data = pe.__data__
rd = [s for s in pe.sections if s.Name.startswith(b'.rdata')][0]
tx = [s for s in pe.sections if s.Name.startswith(b'.text')][0]
tlo, thi = base + tx.VirtualAddress, base + tx.VirtualAddress + tx.Misc_VirtualSize
rbuf = rd.get_data()
rva0 = rd.VirtualAddress
cols = {}
tds = {}
for off in range(0, len(rbuf) - 24, 4):
    sig, ofs, cdo, td, ch, slf = struct.unpack_from('<IIIIII', rbuf, off)
    if sig == 1 and slf == rva0 + off:
        tdo = pe.get_offset_from_rva(td)
        nm = data[tdo + 16: data.find(b'\0', tdo + 16)].decode('latin1')
        cols[base + rva0 + off] = (nm, ofs)
        tds[nm] = base + td
print('COLs', len(cols))
def msvc_name(nm):
    if not nm.startswith('.?AV') and not nm.startswith('.?AU'): return None
    s = nm[4:]
    if not s.endswith('@@') or '?' in s or '$' in s: return None
    parts = s[:-2].split('@')
    return '::'.join(reversed(parts))
winvt = collections.defaultdict(list)
for off in range(0, len(rbuf) - 8, 8):
    v, = struct.unpack_from('<Q', rbuf, off)
    if v in cols:
        nm, ofs = cols[v]
        slots = []
        k = off + 8
        while k + 8 <= len(rbuf):
            p, = struct.unpack_from('<Q', rbuf, k)
            if not (tlo <= p < thi): break
            if k != off + 8 and struct.unpack_from('<Q', rbuf, k)[0] in cols: break
            slots.append(p); k += 8
        winvt[nm].append((base + rva0 + off + 8, ofs, slots))
print('win vtables', sum(len(v) for v in winvt.values()))

# ---- pair
seeds = []
vtmap = {}
pairs_per_win = collections.defaultdict(set)
cnt = collections.Counter()
for nm, vts in winvt.items():
    cls = msvc_name(nm)
    if not cls or cls not in mac_byclass: cnt['noclass'] += 1; continue
    prim = [v for v in vts if v[1] == 0]
    if len(prim) != 1: cnt['ambiguous'] += 1; continue
    waddr, _, wslots = prim[0]
    msym = mac_byclass[cls]
    vtmap[msym] = waddr
    ms = macvt[msym][1]
    # collapse D1/D0
    col = []
    i = 0
    while i < len(ms):
        n0 = M[ms[i]]['names'][0]
        if i + 1 < len(ms) and re.search(r'D1Ev$', n0) and re.search(r'D0Ev$', M[ms[i+1]]['names'][0]):
            col.append(ms[i+1]); i += 2
        else:
            col.append(ms[i]); i += 1
    if len(col) != len(wslots): cnt['lenmismatch'] += 1;
    n = min(len(col), len(wslots)) if len(col) == len(wslots) else 0
    cnt['paired'] += 1 if n else 0
    for k in range(n):
        seeds.append((col[k], wslots[k], 'vt'))
        pairs_per_win[wslots[k]].add(col[k])
print(cnt)
# drop windows functions claimed by different mac functions (identical-code folding, pure virtuals)
good = [(m, w, why) for m, w, why in seeds if len(pairs_per_win[w]) == 1]
good = list({(m, w): (m, w, why) for m, w, why in good}.values())
print('slot seeds', len(seeds), 'unique', len(good))
# typeinfo <-> TypeDescriptor
tisyms = [(a, n) for a, ns in mac['datanames'].items() for n in ns if n.startswith('__ZTI')]
tdem = subprocess.run(['c++filt'], input='\n'.join(n[1:] for _, n in tisyms), capture_output=True, text=True).stdout.split('\n')
byq = {}
for (a, n), dn in zip(tisyms, tdem):
    if dn.startswith('typeinfo for '): byq[dn[len('typeinfo for '):]] = a
typeinfo = {}
for nm, td in tds.items():
    c = msvc_name(nm)
    if c and c in byq: typeinfo[byq[c]] = td
print('typeinfo pairs', len(typeinfo))
pickle.dump(dict(typeinfo=typeinfo, seeds=good, vtables=vtmap, winvt=dict(winvt), macvt=macvt), open(outp, 'wb'))
