# Writes verification batches (verify/v<first>.json ...) for what a table still lacks: field offsets and
# functions the mods need (from tools/win/coverage.py), skipping items already being verified in pending batches.
#   RBWIN=~/rbwin python3 -I tools/win/match/missing.py <table> <first N> <batches> [pending vN.json ...]
import json, re, subprocess, pickle, glob, os, sys
work = os.environ.get('RBWIN', os.path.expanduser('~/rbwin'))
repo = os.path.dirname(os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))
table, first, nb = sys.argv[1], int(sys.argv[2]), int(sys.argv[3])
pending = set()
for p in sys.argv[4:]:
    for i in json.load(open(p)): pending.add(i.get('name') or i.get('sym'))
cov = subprocess.run([sys.executable, '-I', os.path.join(repo, 'tools/win/coverage.py'), table, '-v'], capture_output=True, text=True).stdout
tbl = {l[5:l.rfind(' ')]: int(l.split()[-1], 16) + 0x140000000 for l in open(table) if l.startswith('addr ')}
m = pickle.load(open(os.path.join(work, 'm.pkl'), 'rb')); mm = m.get('match', m)
mac = pickle.load(open(os.path.join(work, 'mac.pkl'), 'rb'))
byname = {n: a for a, f in mac['funcs'].items() for n in f['names']}
done = {}
for f in glob.glob(os.path.join(work, 'verify', '*.out.json')):
    for r in json.load(open(f)): done[r.get('item')] = r.get('verdict') or ('value' if r.get('value') else None)
srcoffs = json.load(open(os.path.join(work, 'offsets.json')))
items = []
for line in cov.splitlines():
    line = line.strip()
    if line.startswith('off '):
        fn, kind, nth = line[4:].strip().rsplit('|', 2)
        if kind.startswith('vt:'): continue
        o = next(o for o in srcoffs if o['fn'] == fn and o['kind'] == kind and str(o['nth']) == nth)
        if o['name'] in pending: continue
        src = open(glob.glob(os.path.join(repo, '*', o['file']))[0]).read()
        ln = next((l.strip() for l in src.splitlines() if re.search(r'\b%s\b' % o['name'], l) and 'Offset' in l), '')
        items.append(dict(kind='offset', name=o['name'], file=o['file'], fn=fn, recipe=kind, nth=int(nth), mac_value=hex(o['expected']),
                          win_fn=hex(tbl[fn]) if fn in tbl else None, line=ln, prior=done.get(o['name'])))
    elif line.startswith('addr '):
        n = line[5:].strip()
        if n.startswith('__ZTV') or not n.startswith('__Z') or n in pending: continue
        a = byname.get(n); w = mm.get(a) if a else None
        items.append(dict(kind='sym', sym=n, win=hex(w) if isinstance(w, int) else None, how='missing', prior=done.get(n), cands=[]))
seen, out = set(), []
for i in items:
    k = json.dumps(i, sort_keys=True)
    if k not in seen: seen.add(k); out.append(i)
for b in range(nb): json.dump(out[b::nb], open(os.path.join(work, 'verify', 'v%d.json' % (first + b)), 'w'), indent=1)
print(len(out), 'items in', nb, 'batches from v%d' % first)
