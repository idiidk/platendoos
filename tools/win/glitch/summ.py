#!/usr/bin/env python3
"""summ.py <run dir>...: one line per run: dropouts, glitch-log events, worst UI stall, CPU, times from the load key."""
import sys, os, re, subprocess, csv
def one(d):
    name = os.path.basename(d.rstrip('/'))
    log = open(d + '/run.log').read()
    m = re.search(r'^(\d+) \S+ key shift\+right$', log, re.M); T0 = float(m.group(1)) if m else 0
    out = subprocess.run([sys.executable, os.path.dirname(os.path.abspath(__file__)) + '/analyze.py', d, '-q'], capture_output=True, text=True).stdout
    nd = re.search(r'DROPOUTS (\d+)', out); nd = int(nd.group(1)) if nd else -1
    secs = re.search(r'compared ([\d.]+) s', out); secs = float(secs.group(1)) if secs else 0
    dl = [(float(x) - T0) / 1000 for x in re.findall(r'^\s+\S+ \((\d+)\)', out, re.M)]
    gl = [l for l in open(d + '/glitchlog.txt', errors='ignore').read().splitlines() if l.strip()] if os.path.exists(d + '/glitchlog.txt') else []
    ui = [(float(r[1]), float(r[2])) for r in csv.reader(open(d + '/ui.csv')) if r and r[0] == 'U']
    uiw = max([u[1] for u in ui if u[0] >= T0 - 500] or [0]); uib = max([u[1] for u in ui if u[0] < T0 - 500] or [0])
    S = []; rb = {}
    for r in csv.reader(open(d + '/threads.csv', errors='ignore')):
        if not r: continue
        if r[0] == 'S': S.append((float(r[1]), float(r[2])))
        if r[0] == 'T': b = int((float(r[1]) - T0) // 1000); rb[b] = rb.get(b, 0) + float(r[3])
    def avg(a, b): v = [s[1] for s in S if T0 + a * 1000 <= s[0] < T0 + b * 1000]; return sum(v) / len(v) if v else 0
    def mx(a, b):
        v = [avg(k, k + 1) for k in range(a, b)]; return max(v) if v else 0
    def series(nm):
        p = d + '/' + nm + '.csv'
        return [[float(v) for v in r] for r in csv.reader(open(p)) if r and r[0][0].isdigit()] if os.path.exists(p) else []
    st = {nm: series(nm) for nm in ('title', 'over', 'lane')}
    # the load shows: the first change of the deck's title or waveform lane after the key
    ch = [r[0] for nm in ('title', 'lane') for r in st[nm] if r[0] > T0 + 50]
    tl = min(ch) if ch else None
    def strip(nm, col, thr):
        if tl is None: return None
        cur = None
        for r in st[nm]:
            if r[0] <= tl + 400: cur = r
        if cur and cur[0] > T0 and cur[col] >= thr: return (tl - T0) / 1000      # there with the load itself
        for r in st[nm]:
            if r[0] > tl + 400 and r[col] >= thr: return (r[0] - T0) / 1000
        return None
    tload = None if tl is None else (tl - T0) / 1000
    f = lambda v: '-' if v is None else '%.1f' % v
    mods = open(d + '/mods.log', errors='ignore').read() if os.path.exists(d + '/mods.log') else ''
    def lg(pat):
        m = re.search(r'^(\d+) \S+ ' + pat, mods, re.M); return (float(m.group(1)) - T0) / 1000 if m else None
    trk = re.search(r'deck 2: loading (\S+)', mods); trk = trk.group(1)[-22:] if trk else '?'
    lost = 'TOKEN-LOST ' if os.path.exists(d + '/spotify.txt') and 'Play token lost' in open(d + '/spotify.txt', errors='ignore').read() else ''
    print('%-14s drop=%d %s glitchlog=%d ui=%3.0f (before %3.0f) cpu avg0-10=%2.0f%% max1s=%3.0f%% avg10-30=%2.0f%% rb(core-s 0-10)=%.1f | shown %s over %s lane %s grid %s | log: load %s wave %s rbgrid %s grid %s stems %s %s%s' % (
        name, nd, ('[' + ' '.join('%+.1f' % x for x in dl) + ']') if dl else '', len(gl), uiw, uib, avg(0, 10), mx(0, 30), avg(10, 30), sum(v for k, v in rb.items() if 0 <= k < 10) / 1000,
        f(tload), f(strip('over', 1, 3)), f(strip('lane', 1, 3)), f(strip('lane', 2, 3)),
        f(lg('deck 2: loading')), f(lg('quickdeck: deck 2 shows with the waveforms')), f(lg("quickdeck: deck 2 shows with rekordbox's grid")), f(lg('quickdeck: deck 2 shows with the beat grid')), f(lg(r'stemcache: \d+ chunks from disk, \d+ separated')), lost, trk))
for d in sys.argv[1:]: one(d)
