#!/usr/bin/env python3
"""Reads a Windows minidump (rekordbox's Crashes/*.dmp): the exception, the crashing thread's registers and the
return addresses on its stack, as module+offset. rbmods.dll offsets can be looked up with llvm-symbolizer
(tools/win/minidump.py <dmp> [--pdb build/win/platendoos/rbmods.pdb] does that itself).

    python3 -I tools/win/minidump.py crash.dmp [--all-threads] [--pdb rbmods.pdb]
"""
import os, shutil, struct, subprocess, sys

def u32(b, o): return struct.unpack_from('<I', b, o)[0]
def u64(b, o): return struct.unpack_from('<Q', b, o)[0]

class Dump:
    def __init__(self, path):
        self.b = open(path, 'rb').read()
        b = self.b
        assert b[:4] == b'MDMP', 'not a minidump'
        n, rva = u32(b, 8), u32(b, 12)
        self.streams = {}
        for i in range(n):
            t, size, r = struct.unpack_from('<III', b, rva + 12 * i)
            self.streams[t] = (r, size)
        self.modules = self._modules()
        self.mem = self._memory()

    def _str(self, rva):
        n = u32(self.b, rva)
        return self.b[rva + 4: rva + 4 + n].decode('utf-16-le', 'replace')

    def _modules(self):
        if 4 not in self.streams: return []
        r, _ = self.streams[4]
        n = u32(self.b, r)
        mods = []
        for i in range(n):
            o = r + 4 + 108 * i
            base, size = u64(self.b, o), u32(self.b, o + 8)
            name = self._str(u32(self.b, o + 20))
            mods.append((base, size, os.path.basename(name.replace('\\', '/'))))
        return sorted(mods)

    def _memory(self):
        ranges = []
        if 9 in self.streams:  # Memory64List
            r, _ = self.streams[9]
            n, base_rva = u64(self.b, r), u64(self.b, r + 8)
            off = base_rva
            for i in range(n):
                start, size = u64(self.b, r + 16 + 16 * i), u64(self.b, r + 24 + 16 * i)
                ranges.append((start, size, off)); off += size
        if 5 in self.streams:  # MemoryList
            r, _ = self.streams[5]
            n = u32(self.b, r)
            for i in range(n):
                o = r + 4 + 16 * i
                start, size, rva = u64(self.b, o), u32(self.b, o + 8), u32(self.b, o + 12)
                ranges.append((start, size, rva))
        return ranges

    def read(self, addr, n):
        for start, size, off in self.mem:
            if start <= addr and addr + n <= start + size:
                return self.b[off + addr - start: off + addr - start + n]
        return None

    def where(self, a):
        for base, size, name in self.modules:
            if base <= a < base + size:
                return name, a - base
        return None, None

    def threads(self):
        r, _ = self.streams[3]
        n = u32(self.b, r)
        out = []
        for i in range(n):
            o = r + 4 + 48 * i
            tid = u32(self.b, o)
            st_start, st_size, st_rva = u64(self.b, o + 24), u32(self.b, o + 32), u32(self.b, o + 36)
            ctx_rva = u32(self.b, o + 44)
            out.append((tid, st_start, st_size, st_rva, ctx_rva))
        return out

    def exception(self):
        if 6 not in self.streams: return None
        r, _ = self.streams[6]
        tid = u32(self.b, r)
        code, flags = u32(self.b, r + 8), u32(self.b, r + 12)
        addr = u64(self.b, r + 24)
        nparams = u32(self.b, r + 32)
        params = [u64(self.b, r + 40 + 8 * i) for i in range(min(nparams, 15))]
        ctx_rva = u32(self.b, r + 8 + 152 + 4)
        return tid, code, addr, params, ctx_rva

REGS = ['rax', 'rcx', 'rdx', 'rbx', 'rsp', 'rbp', 'rsi', 'rdi', 'r8', 'r9', 'r10', 'r11', 'r12', 'r13', 'r14', 'r15']

def context(d, rva):
    regs = {name: u64(d.b, rva + 0x78 + 8 * i) for i, name in enumerate(REGS)}
    regs['rip'] = u64(d.b, rva + 0xf8)
    return regs

def symbolize(pdb, dll, offs):
    tool = shutil.which('llvm-symbolizer') or '/opt/homebrew/opt/llvm/bin/llvm-symbolizer'
    if not pdb or not os.path.exists(tool): return {}
    args = [tool, '--obj=' + dll, '--pdb=' + pdb, '--functions=linkage', '--demangle'] + [hex(0x180000000 + o) for o in offs]
    out = subprocess.run(args, capture_output=True, text=True).stdout.strip().split('\n\n')
    return {o: ' / '.join(x.strip().split('\n')[::2][:3]) for o, x in zip(offs, out)}

def fmt(d, a, names):
    m, off = d.where(a)
    if not m: return hex(a)
    s = f'{m}+{off:#x}'
    if (m, off) in names: s += '  ' + names[(m, off)]
    return s

def stack_frames(d, rsp, st_start, st_size, limit=40):
    frames = []
    for a in range(max(rsp, st_start), st_start + st_size, 8):
        v = d.read(a, 8)
        if v is None: break
        v = u64(v, 0)
        m, off = d.where(v)
        if m and m.lower() in ('rekordbox.exe', 'rbmods.dll') or (m and m.lower().endswith('.dll') and False):
            frames.append((a, v))
            if len(frames) >= limit: break
    return frames

def main():
    args = sys.argv[1:]
    pdb = None
    if '--pdb' in args:
        i = args.index('--pdb'); pdb = args[i + 1]; del args[i:i + 2]
    allt = '--all-threads' in args
    args = [a for a in args if not a.startswith('--')]
    d = Dump(args[0])
    dll = os.path.join(os.path.dirname(pdb), 'rbmods.dll') if pdb else None
    ex = d.exception()
    tid, code, addr, params, ctx_rva = ex
    regs = context(d, ctx_rva)
    # collect rbmods offsets for symbolizing
    th = {t[0]: t for t in d.threads()}
    targets = [tid] + ([t for t in th if t != tid] if allt else [])
    offs = set()
    stacks = {}
    for t in targets:
        _, st_start, st_size, _, crva = th[t]
        r = regs if t == tid else context(d, crva)
        fr = stack_frames(d, r['rsp'], st_start, st_size)
        stacks[t] = (r, fr)
        for a in [r['rip']] + [v for _, v in fr]:
            m, off = d.where(a)
            if m == 'rbmods.dll': offs.add(off)
    syms = symbolize(pdb, dll, sorted(offs)) if dll else {}
    names = {('rbmods.dll', o): s for o, s in syms.items()}
    print(f'exception {code:#x} at {fmt(d, addr, names)} thread {tid}' + (f' params {[hex(p) for p in params]}' if params else ''))
    for t in targets:
        r, fr = stacks[t]
        print(f'\n== thread {t}' + (' (crashed)' if t == tid else ''))
        print('rip', fmt(d, r['rip'], names))
        if t == tid:
            print(' '.join(f'{k}={v:#x}' for k, v in r.items() if k != 'rip'))
        for a, v in fr:
            print(f'  [{a:#x}] {fmt(d, v, names)}')

if __name__ == '__main__':
    main()
