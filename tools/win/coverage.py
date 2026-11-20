# What each mod still lacks in a Windows table: its Addrs (by name), Offsets (by mac recipe) and hooks.
#   python3 -I tools/win/coverage.py tools/win/tables/rekordbox-<build>.tbl [-v]
import sys, os, re, glob
repo = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
tbl = open(sys.argv[1]).read().splitlines()
verbose = '-v' in sys.argv
addr = {l[5:l.rfind(' ')] for l in tbl if l.startswith('addr ')}
off = {l[4:l.rfind(' ')] for l in tbl if l.startswith('off ')}
hook = {l.split()[1] for l in tbl if l.startswith('hook ')}
import subprocess
hookmod = re.findall(r'"(\w*)",  // (\w+)', subprocess.run([sys.executable, '-I', os.path.join(repo, 'tools/win/hookmods.py'), repo], capture_output=True, text=True).stdout)
def modname(f):
    m = os.path.basename(f).split('.')[0]
    return m[:-6] if m.endswith('_panel') else m
edition = os.environ.get('RBM_EDITION', 'full') if os.path.isdir(os.path.join(repo, 'private')) else 'public'
dirs = ('core', 'mods', 'engines') + (('private/mods',) if edition == 'full' else ())  # (the edition's sources, docs/editions.md)
for f in sorted(f for d in dirs for f in glob.glob(os.path.join(repo, d, '*.cpp'))):
    if f.endswith('_win.cpp'): continue
    src = open(f).read()
    consts = dict(re.findall(r'const\s+char\s*\*\s*(?:const\s+)?(k\w+)\s*=\s*"([^"]+)"', src))
    a = set()
    for m in re.finditer(r'\bAddr\s+\w+\s*\{\s*("((?:[^"\\]|\\.)+)"|(k\w+))\s*(,\s*0x([0-9a-f]+))?', src):
        n = (m.group(2) or '').replace('\\"', '"') or consts.get(m.group(3), m.group(3))
        a.add(n + ('+0x%x' % int(m.group(5), 16) if m.group(5) else ''))
    o = set()
    for m in re.finditer(r'\bOffset\s+\w+\s*\{\s*(0x[0-9a-f]+|\d+)\s*,\s*("([^"]+)"|(k\w+))\s*,\s*"([^"]+)"\s*,\s*(\d+)\s*(?:,\s*"[^"]*"\s*)?\}', src):
        o.add('%s|%s|%s' % (m.group(3) or consts.get(m.group(4), m.group(4)), m.group(5), m.group(6)))
    h = {hk for mod, hk in hookmod if mod == modname(f)} if not f.endswith('_panel.cpp') else set()
    ma, mo, mh = a - addr, o - off, h - hook
    print('%-26s addrs %3d/%-3d fields %2d/%-2d hooks %2d/%-2d' % (os.path.relpath(f, repo), len(a) - len(ma), len(a), len(o) - len(mo), len(o), len(h) - len(mh), len(h)))
    if verbose:
        for x in sorted(ma): print('    addr', x)
        for x in sorted(mo): print('    off ', x)
        for x in sorted(mh): print('    hook', x)
