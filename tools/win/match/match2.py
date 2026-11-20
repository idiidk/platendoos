# Matches rekordbox's mac x86_64 functions (named) to the Windows exe's functions (stripped).
#   python3 -I match2.py mac.pkl win.pkl vt.pkl out.pkl
# Signals, iterated to a fixed point:
#   keys       strings, imports, large constants, references to known data (vtables, then globals learned
#              from matched pairs); a key unique to one function on each side pairs them
#   vtables    slots of classes paired by RTTI name, DP-aligned by similarity
#   calls      callee lists of matched pairs, aligned at known anchors, gaps DP-aligned by similarity
#   data       rip-relative data refs of matched pairs aligned the same way -> data pairs -> more keys
import sys, pickle, collections, math

mac = pickle.load(open(sys.argv[1], 'rb'))
win = pickle.load(open(sys.argv[2], 'rb'))
vt = pickle.load(open(sys.argv[3], 'rb'))
outp = sys.argv[4]
import os, re
HOLDOUT = os.environ.get('HOLDOUT') == '1'
FNRE = re.compile(r'^[A-Za-z_][A-Za-z0-9_]*(::[A-Za-z_~][A-Za-z0-9_]*)+$')

M = mac['funcs']
W = {}
for a, f in sorted(win['funcs'].items()):
    r = f['root']
    if r not in W: W[r] = dict(calls=[], strs=[], refs=[], consts=[], n=0, size=0, chunks=[])
    g = W[r]
    for k in ('calls', 'strs', 'refs', 'consts'): g[k] += f[k]
    g['n'] += f['n']; g['size'] += f['size']; g['chunks'].append(a)
chunk_root = {a: f['root'] for a, f in win['funcs'].items()}

def prep(F, valid, fix=lambda t: t):
    for a, f in F.items():
        cs = []
        for c in f['calls']:
            t = c[1] if isinstance(c, tuple) else c
            if not isinstance(t, str): t = fix(t)
            if isinstance(t, str) or t in valid: cs.append(t)
        f['cl'] = cs
        f['ss'] = set(s for s in f['strs'] if len(s) >= 4 and not (HOLDOUT and FNRE.match(s)))
        f['cs'] = set(c for c in f['consts'] if 0x10000 <= c < (1 << 40) and c not in (0xffffffff, 0xffff, 0x7fffffff))
        f['im'] = set(t for t in cs if isinstance(t, str))
        f['sc'] = set(c for c in f['consts'] if 0x20 <= c < 0x100000)
prep(M, M)
prep(W, W, lambda t: chunk_root.get(t, t))

def callers(F):
    r = collections.defaultdict(set)
    for a, f in F.items():
        for t in f['cl']:
            if not isinstance(t, str): r[t].add(a)
    return r
Mcallers, Wcallers = callers(M), callers(W)
# only features that exist in both binaries can be compared
def common(key):
    a = set().union(*(f[key] for f in M.values())); b = set().union(*(f[key] for f in W.values()))
    both = a & b
    for F in (M, W):
        for f in F.values(): f[key] = f[key] & both
for k in ('ss', 'im', 'cs', 'sc'): common(k)

match, rmatch, how = {}, {}, {}
banned = set()
dmatch, rdmatch = {}, {}   # data: mac addr -> win addr

def accept(m, w, why):
    if m in match or w in rmatch or (m, w) in banned: return False
    match[m] = w; rmatch[w] = m; how[m] = why
    return True

# vtable data seeds: mac __ZTV+16 <-> win vtable
macsym = {}
for a, ns in mac['datanames'].items():
    for n in ns: macsym[n] = a
for msym, waddr in vt['vtables'].items():
    a = macsym.get(msym)
    if a: dmatch[a + 16] = waddr; rdmatch[waddr] = a + 16

def jac(a, b):
    return len(a & b) / len(a | b) if (a or b) else None

for a, td in vt.get('typeinfo', {}).items():
    if a not in dmatch and td not in rdmatch: dmatch[a] = td; rdmatch[td] = a

def sim(m, w):
    fm, fw = M[m], W[w]
    parts = []
    a, b = fm['n'], fw['n']
    parts.append((1.0, min(a, b) / max(a, b, 1)))
    j = jac(fm['ss'], fw['ss'])
    if j is not None:
        parts.append((2.0, j))
    j = jac(fm['im'], fw['im'])
    if j is not None:
        parts.append((1.0, j))
    j = jac(fm['sc'], fw['sc'])
    if j is not None: parts.append((1.5, j))
    ca, cb = len(fm['cl']), len(fw['cl'])
    if ca or cb: parts.append((0.5, min(ca, cb) / max(ca, cb)))
    km = set(match[t] for t in fm['cl'] if not isinstance(t, str) and t in match)
    kw = set(t for t in fw['cl'] if not isinstance(t, str) and t in rmatch)
    if km or kw:
        j = jac(km, kw)
        parts.append((2.0, j))
    return sum(w * v for w, v in parts) / sum(w for w, _ in parts)

def dp_align(xs, ys, score, thresh=0.35, maxcells=250000):
    """Needleman-Wunsch with gap 0; pairs with score >= thresh."""
    if not xs or not ys or len(xs) * len(ys) > maxcells: return []
    n, m = len(xs), len(ys)
    S = [[0.0] * (m + 1) for _ in range(n + 1)]
    sc = {}
    for i in range(n - 1, -1, -1):
        for j in range(m - 1, -1, -1):
            s = score(xs[i], ys[j]); s = s if s >= thresh else -1
            sc[i, j] = s
            S[i][j] = max(S[i+1][j], S[i][j+1], S[i+1][j+1] + s)
    out = []; i = j = 0
    while i < n and j < m:
        if S[i][j] == S[i+1][j+1] + sc[i, j] and sc[i, j] > 0: out.append((xs[i], ys[j], sc[i, j])); i += 1; j += 1
        elif S[i][j] == S[i+1][j]: i += 1
        else: j += 1
    return out

def dsim(dm, dw):
    return 1.0

# ---------- keys ----------
def keys_m(m):
    f = M[m]
    k = set(('s', s) for s in f['ss'])
    k |= set(('c', c) for c in f['cs'])
    k |= set(('d', dmatch[r]) for r in f['refs'] if r in dmatch)
    k |= set(('f', match[t]) for t in f['cl'] if not isinstance(t, str) and t in match)
    k |= set(('i', t) for t in f['im'])
    return k
def keys_w(w):
    f = W[w]
    k = set(('s', s) for s in f['ss'])
    k |= set(('c', c) for c in f['cs'])
    k |= set(('d', r) for r in f['refs'] if r in rdmatch)
    k |= set(('f', t) for t in f['cl'] if not isinstance(t, str) and t in rmatch)
    k |= set(('i', t) for t in f['im'])
    return k
WEIGHT = {'s': 3, 'c': 1.5, 'd': 3, 'f': 1, 'i': 0.5}

def key_round():
    im, iw = collections.defaultdict(list), collections.defaultdict(list)
    for m in M:
        if m not in match:
            for k in keys_m(m): im[k].append(m)
    for w in W:
        if w not in rmatch:
            for k in keys_w(w): iw[k].append(w)
    votes = collections.defaultdict(float)
    for k, ms in im.items():
        ws = iw.get(k)
        if ws and len(ms) == 1 and len(ws) == 1 and k[0] in ('s', 'd', 'c'):
            votes[(ms[0], ws[0])] += WEIGHT[k[0]]
    # also: rare keys (<=3 each side) with exact same full key set overlap
    return votes

def best_pairs(votes, minscore, simmin, tag='p'):
    bm, bw = {}, {}
    for (m, w), v in votes.items():
        if m in match or w in rmatch: continue
        s = sim(m, w)
        if s < simmin: continue
        sc = v + s
        if sc > bm.get(m, (0, None))[0]: bm[m] = (sc, w)
        if sc > bw.get(w, (0, None))[0]: bw[w] = (sc, m)
    out = 0
    for m, (sc, w) in bm.items():
        if bw[w][1] == m and sc >= minscore and accept(m, w, tag): out += 1
    return out

def vt_round():
    votes = collections.defaultdict(float)
    for nm, (ms, ws) in vtpairs.items():
        xs = [x for x in ms if x not in match]
        ys = [y for y in ws if y not in rmatch]
        # anchored: segments between already matched slots
        for a, b, s in dp_align(ms, ws, lambda x, y: (2.0 if match.get(x) == y else (0 if x in match or y in rmatch else sim(x, y))), 0.3):
            if a not in match and b not in rmatch: votes[(a, b)] += 1 + s
    return votes

def anchored_lists(xm, xw, keym, keyw):
    """aligns two ordered lists at equal anchor keys (LCS), returns gap pairs of unknown items."""
    am = [(i, keym(t)) for i, t in enumerate(xm)]; am = [(i, k) for i, k in am if k is not None]
    aw = [(j, keyw(t)) for j, t in enumerate(xw)]; aw = [(j, k) for j, k in aw if k is not None]
    km = [k for _, k in am]; kw = [k for _, k in aw]
    pts = [(-1, -1)]
    if km and kw and len(km) * len(kw) <= 250000:
        L = [[0] * (len(kw) + 1) for _ in range(len(km) + 1)]
        for i in range(len(km) - 1, -1, -1):
            for j in range(len(kw) - 1, -1, -1):
                L[i][j] = L[i+1][j+1] + 1 if km[i] == kw[j] else max(L[i+1][j], L[i][j+1])
        i = j = 0
        while i < len(km) and j < len(kw):
            if km[i] == kw[j]: pts.append((am[i][0], aw[j][0])); i += 1; j += 1
            elif L[i+1][j] >= L[i][j+1]: i += 1
            else: j += 1
    pts.append((len(xm), len(xw)))
    gaps = []
    for (a0, b0), (a1, b1) in zip(pts, pts[1:]):
        gaps.append((xm[a0+1:a1], xw[b0+1:b1]))
    return gaps

def call_round():
    votes = collections.defaultdict(float)
    km = lambda t: t if isinstance(t, str) else (('M', match[t]) if t in match else None)
    kw = lambda t: t if isinstance(t, str) else (('M', t) if t in rmatch else None)
    for m, w in list(match.items()):
        for gm, gw in anchored_lists(M[m]['cl'], W[w]['cl'], km, kw):
            gm = list(dict.fromkeys(t for t in gm if not isinstance(t, str) and t not in match))
            gw = list(dict.fromkeys(t for t in gw if not isinstance(t, str) and t not in rmatch))
            if not gm or not gw: continue
            if len(gm) == len(gw):
                for a, b in zip(gm, gw):
                    s = sim(a, b)
                    if s >= 0.3: votes[(a, b)] += 1
            else:
                for a, b, s in dp_align(gm, gw, sim, 0.5): votes[(a, b)] += 0.6
        um = [c for c in Mcallers.get(m, ()) if c not in match]
        uw = [c for c in Wcallers.get(w, ()) if c not in rmatch]
        if len(um) == 1 and len(uw) == 1: votes[(um[0], uw[0])] += 0.7
    return votes

def data_round():
    """learn data pairs from aligned refs of matched functions."""
    dv = collections.defaultdict(float)
    km = lambda r: ('D', dmatch[r]) if r in dmatch else None
    kw = lambda r: ('D', r) if r in rdmatch else None
    for m, w in match.items():
        for gm, gw in anchored_lists(M[m]['refs'], W[w]['refs'], km, kw):
            gm = list(dict.fromkeys(r for r in gm if r not in dmatch))
            gw = list(dict.fromkeys(r for r in gw if r not in rdmatch))
            if gm and len(gm) == len(gw):
                for a, b in zip(gm, gw): dv[(a, b)] += 1
    bm, bw = {}, {}
    for (a, b), v in dv.items():
        if v > bm.get(a, (0, None))[0]: bm[a] = (v, b)
        if v > bw.get(b, (0, None))[0]: bw[b] = (v, a)
    n = 0
    for a, (v, b) in bm.items():
        if bw[b][1] == a and v >= 2 and a not in dmatch and b not in rdmatch:
            dmatch[a] = b; rdmatch[b] = a; n += 1
    return n

# pins: verified pairs (accepted first) and pairs found wrong (never made); verify/pins.json
# [{"mac": symbol, "win": "0x..."} | {"mac": symbol, "notwin": "0x..."}]
banned = set()
pinf = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'verify', 'pins.json')
if os.path.exists(pinf):
    import json
    byname0 = {n: a for a, f in M.items() for n in f['names']}
    np_ = 0
    for p in json.load(open(pinf)):
        m = byname0.get(p['mac'])
        if m is None: continue
        if p.get('win'):
            w = int(p['win'], 16); w = chunk_root.get(w, w)
            if w in W: np_ += accept(m, w, 'pin')
        if p.get('notwin'): banned.add((m, int(p['notwin'], 16)))
    print('pins', np_, 'banned', len(banned))

# Windows-only __FUNCTION__ names
if not HOLDOUT:
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__))); import fnnames
    byq, wn = fnnames.load(sys.argv[1], sys.argv[2])
    na = 0
    for w, q in wn.items():
        cands = [m for m in byq[q] if m in M]
        if len(cands) == 1 and sim(cands[0], w) >= 0.35: na += accept(cands[0], w, 'name')
        elif len(cands) > 1:
            sc = sorted(((sim(m, w), m) for m in cands), reverse=True)
            if sc[0][0] >= 0.5 and sc[0][0] - sc[1][0] >= 0.15: na += accept(sc[0][1], w, 'name')
    print('name anchors', na)

# vtable slot lists
vtpairs = {}
macvt = vt['macvt']
for msym, waddr in vt['vtables'].items():
    ms = [s for s in macvt[msym][1] if s in M]
    # collapse D1/D0
    col = []; i = 0
    while i < len(ms):
        n0 = M[ms[i]]['names'][0]
        if i + 1 < len(ms) and n0.endswith('D1Ev') and M[ms[i+1]]['names'][0].endswith('D0Ev'): col.append(ms[i+1]); i += 2
        else: col.append(ms[i]); i += 1
    for nm, lst in vt['winvt'].items():
        pass
    vtpairs[msym] = (col, None)
# win slots by vtable address
wslots = {}
for nm, lst in vt['winvt'].items():
    for addr, ofs, slots in lst: wslots[addr] = [chunk_root.get(s, s) for s in slots]
for msym in list(vtpairs):
    ws = wslots.get(vt['vtables'][msym])
    if ws is None: del vtpairs[msym]; continue
    vtpairs[msym] = (vtpairs[msym][0], [s for s in ws if s in W])
# slots used by many classes (folded / pure) are useless
cnt = collections.Counter(s for _, ws in vtpairs.values() for s in set(ws))
for k, (ms, ws) in vtpairs.items(): vtpairs[k] = (ms, [s for s in ws if cnt[s] <= 2])

for it in range(30):
    n0 = len(match)
    a = best_pairs(key_round(), 3.25, 0.25, 'key')
    b = best_pairs(vt_round(), 1.3, 0.3, 'vt')
    c = best_pairs(call_round(), 1.4, 0.4, 'call')
    d = data_round()
    print('iter', it, 'keys', a, 'vt', b, 'calls', c, 'data', d, 'total', len(match), 'data total', len(dmatch), flush=True)
    if len(match) == n0 and d == 0: break

pickle.dump(dict(match=match, how=how, dmatch=dmatch), open(outp, 'wb'))

if os.environ.get('RESOLVE'):
    # candidates for needed-but-unmatched mac functions, from their matched neighbours
    need = [l.strip() for l in open(os.environ['RESOLVE']) if l.strip()]
    byname = {n: a for a, f in M.items() for n in f['names']}
    res = {}
    for n in need:
        x = byname.get(n)
        if x is None: continue
        if x in match:
            res[n] = dict(win=match[x], how=how[x], sim=round(sim(x, match[x]), 2), cands=[])
            continue
        cand = collections.Counter()
        for c in Mcallers.get(x, ()):          # callers matched -> their callees
            if c in match:
                for t in W[match[c]]['cl']:
                    if not isinstance(t, str) and t not in rmatch: cand[t] += 1
        for t in M[x]['cl']:                    # callees matched -> their callers
            if not isinstance(t, str) and t in match:
                for c in Wcallers.get(match[t], ()):
                    if c not in rmatch: cand[c] += 1
        sc = sorted(((cand[w] * 0.3 + sim(x, w), w, cand[w]) for w in cand), reverse=True)[:6]
        res[n] = dict(win=None, how=None, cands=[(w, round(s, 2), v, round(sim(x, w), 2)) for s, w, v in sc],
                      ncallers=len(Mcallers.get(x, ())), mcallers_matched=sum(1 for c in Mcallers.get(x, ()) if c in match), insns=M[x]['n'])
    pickle.dump(res, open(outp + '.resolve', 'wb'))

if os.environ.get('CALIB'):
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__))); import fnnames
    byq, wn = fnnames.load(sys.argv[1], sys.argv[2])
    for name, votes in (('call', call_round()), ('key', key_round()), ('vt', vt_round())):
        tab = collections.Counter()
        for (m, w), v in votes.items():
            if w in wn and m not in match and w not in rmatch:
                s = sim(m, w)
                ok = m in byq[wn[w]]
                tab[(min(int(v), 4), round(s, 1), ok)] += 1
        print(name)
        keys = sorted(set((a, b) for a, b, _ in tab))
        for a, b in keys:
            t, f = tab[(a, b, True)], tab[(a, b, False)]
            print('  votes>=%d sim~%.1f  ok %4d  bad %4d' % (a, b, t, f))
