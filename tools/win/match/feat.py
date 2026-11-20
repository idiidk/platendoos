# Extracts per-function features from rekordbox's mac x86_64 slice (with symbols) and the Windows x64 exe
# (stripped; functions from .pdata), for cross-binary matching.
#   python3 -I feat.py mac <macho_x86_64> out.pkl
#   python3 -I feat.py win <rekordbox.exe> out.pkl
import sys, re, pickle, struct, subprocess, bisect
import capstone

RIP = re.compile(r'\[rip ([+-]) (0x[0-9a-f]+|\d+)\]')
IMM = re.compile(r'(?:^|, )(-?0x[0-9a-f]+|-?\d+)$')
md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)


def cstr(buf, off, maxlen=400):
    if off < 0 or off >= len(buf): return None
    end = buf.find(b'\0', off, off + maxlen)
    if end < 0 or end - off < 3:
        # utf-16?
        return wstr(buf, off, maxlen)
    s = buf[off:end]
    try: t = s.decode('utf-8')
    except UnicodeDecodeError: return None
    if sum(c.isprintable() or c in '\n\t\r' for c in t) < len(t): return wstr(buf, off, maxlen)
    if len(t) == 1 and (off + 2 <= len(buf)) and buf[off+1] == 0: return wstr(buf, off, maxlen)
    return t


def wstr(buf, off, maxlen):
    out = []
    for k in range(off, min(off + 2 * maxlen, len(buf) - 1), 2):
        c = buf[k] | buf[k+1] << 8
        if c == 0: break
        out.append(chr(c))
    else: return None
    t = ''.join(out)
    if len(t) < 3 or not all(c.isprintable() or c in '\n\t\r' for c in t): return None
    return 'W:' + t


class Bin:
    def __init__(self): self.secs = []  # (va, size, fileoff, name)
    def fileoff(self, va):
        for s in self.secs:
            if s[0] <= va < s[0] + s[1]: return s[2] + va - s[0], s[3]
        return None, None


def disasm(data, foff, va, size, b, out, funcs_sorted, imports):
    calls, strs, refs, consts = [], [], [], []
    n = 0
    end = va + size
    for (a, sz, mn, op) in md.disasm_lite(data[foff:foff + size], va):
        n += 1
        if mn == 'call' or mn == 'jmp':
            if op.startswith('0x'):
                t = int(op, 16)
                if mn == 'call' or not (va <= t < end):
                    calls.append(imports.get(t, t) if mn == 'call' else ('tail', imports.get(t, t)))
                continue
            m = RIP.search(op)
            if m:
                t = a + sz + (int(m.group(2), 0) if m.group(1) == '+' else -int(m.group(2), 0))
                if t in imports: calls.append(imports[t] if mn == 'call' else ('tail', imports[t]))
            continue
        m = RIP.search(op)
        if m:
            t = a + sz + (int(m.group(2), 0) if m.group(1) == '+' else -int(m.group(2), 0))
            fo, sec = b.fileoff(t)
            if fo is not None:
                s = cstr(data, fo) if sec in b.strsecs else None
                if s is not None: strs.append(s)
                else: refs.append(t)
            continue
        m = IMM.search(op)
        if m and mn not in ('jmp', 'call') and not mn.startswith('j'):
            v = int(m.group(1), 0)
            if v & 0xffffffffffffffff >= 0x100 and abs(v) < (1 << 40): consts.append(v & 0xffffffffffffffff)
    return dict(calls=calls, strs=strs, refs=refs, consts=consts, n=n, size=size)


def mac(path, outp):
    data = open(path, 'rb').read()
    b = Bin()
    # sections
    ncmds, = struct.unpack_from('<I', data, 16)
    off = 32
    text = None
    for _ in range(ncmds):
        cmd, sz = struct.unpack_from('<II', data, off)
        if cmd == 0x19:
            nsect, = struct.unpack_from('<I', data, off + 64)
            for k in range(nsect):
                so = off + 72 + 80 * k
                sect = data[so:so+16].rstrip(b'\0').decode(); seg = data[so+16:so+32].rstrip(b'\0').decode()
                addr, size, fo = struct.unpack_from('<QQI', data, so + 32)
                b.secs.append((addr, size, fo, seg + ',' + sect))
                if sect == '__text': text = (addr, size, fo)
        off += sz
    b.strsecs = {'__TEXT,__cstring', '__TEXT,__const', '__DATA_CONST,__const', '__TEXT,__ustring'}
    # symbols
    names = {}
    out = subprocess.run(['nm', '-n', path], capture_output=True, text=True).stdout
    for line in out.splitlines():
        p = line.split(' ', 2)
        if len(p) == 3 and p[0] and p[1] in 'tT':
            a = int(p[0], 16)
            if text[0] <= a < text[0] + text[1]: names.setdefault(a, []).append(p[2])
    datanames = {}
    for line in out.splitlines():
        p = line.split(' ', 2)
        if len(p) == 3 and p[0] and p[1] in 'sSdD': datanames.setdefault(int(p[0], 16), []).append(p[2])
    # stubs
    imports = {}
    st = subprocess.run(['otool', '-Iv', path], capture_output=True, text=True).stdout
    insec = False
    for line in st.splitlines():
        if line.startswith('Indirect symbols for'): insec = '__stubs' in line; continue
        p = line.split()
        if insec and len(p) >= 3 and p[0].startswith('0x'): imports[int(p[0], 16)] = 'imp:' + p[2]
    starts = sorted(names)
    funcs = {}
    for i, a in enumerate(starts):
        e = starts[i + 1] if i + 1 < len(starts) else text[0] + text[1]
        f = disasm(data, text[2] + a - text[0], a, e - a, b, out, starts, imports)
        f['names'] = names[a]
        funcs[a] = f
    pickle.dump(dict(funcs=funcs, datanames=datanames, secs=b.secs), open(outp, 'wb'))
    print(len(funcs), 'functions')


def win(path, outp):
    import pefile
    pe = pefile.PE(path, fast_load=True)
    pe.parse_data_directories(directories=[pefile.DIRECTORY_ENTRY['IMAGE_DIRECTORY_ENTRY_IMPORT'], pefile.DIRECTORY_ENTRY['IMAGE_DIRECTORY_ENTRY_EXCEPTION']])
    data = pe.__data__
    base = pe.OPTIONAL_HEADER.ImageBase
    b = Bin()
    for s in pe.sections:
        b.secs.append((base + s.VirtualAddress, max(s.Misc_VirtualSize, 0), s.PointerToRawData, s.Name.rstrip(b'\0').decode()))
    b.strsecs = {'.rdata', '.data'}
    imports = {}
    for d in pe.DIRECTORY_ENTRY_IMPORT:
        for imp in d.imports:
            imports[imp.address] = 'imp:' + (imp.name.decode() if imp.name else '%s#%d' % (d.dll.decode(), imp.ordinal))
    # .pdata: primary functions (unwind info without CHAININFO) and their chained parts
    funcs = {}
    chunks = []
    for e in pe.DIRECTORY_ENTRY_EXCEPTION:
        s = e.struct
        chunks.append((s.BeginAddress, s.EndAddress, s.UnwindData))
    text = [s for s in pe.sections if s.Name.startswith(b'.text')][0]
    def u8(rva): return data[pe.get_offset_from_rva(rva)]
    parent = {}
    for (bg, en, uw) in chunks:
        flags = u8(uw) >> 3
        if flags & 4:  # chained: RUNTIME_FUNCTION after the unwind codes
            cnt = u8(uw + 2)
            co = uw + 4 + 2 * ((cnt + 1) & ~1)
            pb, = struct.unpack_from('<I', data, pe.get_offset_from_rva(co))
            parent[bg] = pb
    for (bg, en, uw) in chunks:
        foff = pe.get_offset_from_rva(bg)
        f = disasm(data, foff, base + bg, en - bg, b, None, None, imports)
        root = bg
        while root in parent: root = parent[root]
        f['root'] = base + root
        funcs[base + bg] = f
    # leaf functions have no .pdata entry: direct call targets that aren't known functions, disassembled to
    # their first ret / jmp (a leaf is small and straight)
    tx = [s for s in b.secs if s[3] == '.text'][0]
    known = set(funcs)
    starts = sorted(known)
    import bisect
    targets = set()
    for f in list(funcs.values()):
        for c in f['calls']:
            t = c[1] if isinstance(c, tuple) else c
            if isinstance(t, int) and t not in known and tx[0] <= t < tx[0] + tx[1]: targets.add(t)
    nleaf = 0
    for t in sorted(targets):
        i = bisect.bisect_right(starts, t) - 1
        if i >= 0 and t < funcs[starts[i]].get('end', starts[i] + funcs[starts[i]]['size']): continue  # inside a function
        size = 0
        fo = tx[2] + t - tx[0]
        for (a, sz, mn, op) in md.disasm_lite(data[fo:fo + 0x400], t):
            size = a + sz - t
            if mn in ('ret', 'jmp', 'int3'): break
        if size == 0: continue
        f = disasm(data, fo, t, size, b, None, None, imports)
        f['root'] = t; f['leaf'] = True
        funcs[t] = f; nleaf += 1
    print('leaf functions', nleaf)
    pickle.dump(dict(funcs=funcs, secs=b.secs, base=base), open(outp, 'wb'))
    print(len(funcs), 'chunks', sum(1 for f in funcs.values() if f['root'] == 0 or True))


if __name__ == '__main__':
    {'mac': mac, 'win': win}[sys.argv[1]](sys.argv[2], sys.argv[3])
