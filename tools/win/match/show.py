# Side-by-side: a mac function (by symbol or address) and a Windows function (address), with calls and data
# refs annotated by names known from the match.
#   python3 -I show.py <mac symbol|0xaddr> [0xwinaddr] [--max N]
import os
import sys, pickle, re, subprocess, struct, functools
import capstone

R = os.environ.get('RBWIN', os.path.expanduser('~/rbwin')) + '/'
mac = pickle.load(open(R + 'mac.pkl', 'rb'))
win = pickle.load(open(R + 'win.pkl', 'rb'))
mt = pickle.load(open(R + (sys.argv[sys.argv.index('--match') + 1] if '--match' in sys.argv else 'm.pkl'), 'rb'))
M = mac['funcs']
match = mt['match']; rmatch = {w: m for m, w in match.items()}
dmatch = mt.get('dmatch', {}); rdmatch = {w: m for m, w in dmatch.items()}
mx = int(sys.argv[sys.argv.index('--max') + 1]) if '--max' in sys.argv else 400
mx = mx or 10**9
md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)

@functools.lru_cache(None)
def dem(n):
    return subprocess.run(['c++filt'], input=n[1:] if n.startswith('__Z') else n, capture_output=True, text=True).stdout.strip()

datan = {}
for a, ns in mac['datanames'].items(): datan[a] = ns[0]
def mname(a):
    if a in M: return dem(M[a]['names'][0])
    if a in datan: return dem(datan[a])
    # vtable +16
    if a - 16 in datan: return dem(datan[a - 16]) + '+16'
    return None

def disp(data, foff, va, size, label):
    out = []
    for (a, sz, mn, op) in md.disasm_lite(data[foff:foff + size], va):
        note = ''
        t = None
        if op.startswith('0x') and mn in ('call', 'jmp'): t = int(op, 16)
        m = re.search(r'\[rip ([+-]) (0x[0-9a-f]+)\]', op)
        if m: t = a + sz + (int(m.group(2), 16) if m.group(1) == '+' else -int(m.group(2), 16))
        if t is not None: note = label(t) or ''
        out.append('%x  %-6s %-40s %s' % (a, mn, op[:40], note[:110]))
    return out

def macdis(a):
    f = M[a]
    d = open(R + 'mac/rekordbox_x86', 'rb').read()
    # file offset = va - 0x100000000 for __TEXT
    return disp(d, a - 0x100000000, a, f['size'], lambda t: (mname(t) or '') + ('   [=win %x]' % match[t] if t in match else ''))

import pefile
pe = pefile.PE(R + 'x/rekordbox.exe', fast_load=True)
base = pe.OPTIONAL_HEADER.ImageBase
imp = {}
pe.parse_data_directories(directories=[pefile.DIRECTORY_ENTRY['IMAGE_DIRECTORY_ENTRY_IMPORT']])
for dd in pe.DIRECTORY_ENTRY_IMPORT:
    for i in dd.imports: imp[i.address] = (i.name or b'?').decode()
def wlabel(t):
    if t in imp: return 'imp:' + imp[t]
    r = win['funcs'].get(t, {}).get('root', t)
    if r in rmatch: return '= ' + (mname(rmatch[r]) or hex(rmatch[r]))
    if t in rdmatch: return '= ' + (mname(rdmatch[t]) or hex(rdmatch[t]))
    try:
        o = pe.get_offset_from_rva(t - base)
        s = pe.__data__[o:o + 80].split(b'\0')[0]
        if len(s) >= 4 and all(32 <= c < 127 for c in s): return '"%s"' % s.decode()
    except Exception: pass
    return None
def windis(w):
    out = []
    for c in sorted(a for a, f in win['funcs'].items() if f['root'] == w):
        f = win['funcs'][c]
        out.append('--- chunk %x' % c)
        out += disp(pe.__data__, pe.get_offset_from_rva(c - base), c, f['size'], wlabel)
    return out

x = sys.argv[1]
if x.startswith('0x'): ma = int(x, 16)
else: ma = next(a for a, f in M.items() if x in f['names'])
print('=== MAC %x %s  (%d insns)' % (ma, dem(M[ma]['names'][0]), M[ma]['n']))
print('\n'.join(macdis(ma)[:mx]))
wa = int(sys.argv[2], 16) if len(sys.argv) > 2 and sys.argv[2].startswith('0x') else match.get(ma)
if wa:
    print('=== WIN %x %s' % (wa, '(matched)' if match.get(ma) == wa else ''))
    print('\n'.join(windis(wa)[:mx]))
