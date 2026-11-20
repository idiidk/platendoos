"""Scores rekordbox's and the CNN's keys against tools/keytest/data/keytest.csv (GiantSteps Key expert annotations),
with the MIREX weighted score used in the papers (same 1, fifth above 0.5, relative 0.3, parallel 0.2).
    python3 tools/keytest/keyscore.py [betterkey.csv]"""
import csv, os, re, sys
here = os.path.dirname(os.path.abspath(__file__))
truth_rows = list(csv.DictReader(open(os.path.join(here, 'data', 'keytest.csv'))))
res_path = sys.argv[1] if len(sys.argv) > 1 else os.path.expanduser('~/Library/Logs/rekordbox-betterkey.csv')

PC = {'C': 0, 'D': 2, 'E': 4, 'F': 5, 'G': 7, 'A': 9, 'B': 11}
def parse(k):
    m = re.match(r'^\s*([A-Ga-g])([#b]?)\s*(m|min|minor)?', k)
    if not m: return None
    pc = (PC[m.group(1).upper()] + {'#': 1, 'b': -1, '': 0}[m.group(2)]) % 12
    return pc, bool(m.group(3))

def score(ref, est):  # mir_eval.key.weighted_score
    if ref is None or est is None: return 0, 'n/a'
    (r, rm), (e, em) = ref, est
    if (r, rm) == (e, em): return 1.0, 'correct'
    if rm == em and (e - r) % 12 == 7: return 0.5, 'fifth'
    if rm != em and ((not rm and (r - e) % 12 == 3) or (rm and (e - r) % 12 == 3)): return 0.3, 'relative'
    if rm != em and r == e: return 0.2, 'parallel'
    return 0, 'other'

truth = {}
for t in truth_rows:
    m = re.search(r'track/([A-Za-z0-9]+)', t['spotify_url'])
    if m: truth['spotify:track:' + m.group(1)] = t
# Spotify may relink a pasted track to another track id: match by tools/keytest/data/keytest-aliases.csv
# (played id -> test id), else by title (+ artist), which the analyzer records in newer CSV lines.
aliases = {}
alias_path = os.path.join(here, 'data', 'keytest-aliases.csv')
if os.path.exists(alias_path):
    for a in csv.DictReader(open(alias_path)): aliases['spotify:track:' + a['played_id']] = 'spotify:track:' + a['test_id']
norm = lambda s: re.sub(r'[^a-z0-9]', '', s.lower())
by_title = {norm(t['title']): uri for uri, t in truth.items()}
latest = {}
rd = csv.reader(open(res_path))
next(rd)
for f in rd:
    r = {'rekordbox': f[1], 'cnn': f[2], 'path': f[6]}
    uri = aliases.get(r['path'], r['path'])
    if uri not in truth and len(f) > 7 and f[7]:
        t = norm(f[7])
        uri = by_title.get(t) or next((u for k, u in by_title.items() if k and (k in t or t in k)), uri)
    if uri in truth: latest[uri] = r
print(f"{len(latest)} of {len(truth)} test tracks analysed\n")
tot = {'rekordbox': [0, {}], 'cnn': [0, {}]}
for uri, r in latest.items():
    t = truth[uri]
    ref = parse(t['key_short'])
    line = f"{t['artist'][:22]:22s} {t['title'][:26]:26s} truth {t['key_short']:4s}"
    for who in ('rekordbox', 'cnn'):
        s, cat = score(ref, parse(r[who]))
        tot[who][0] += s
        tot[who][1][cat] = tot[who][1].get(cat, 0) + 1
        line += f"  {who} {r[who]:4s} {cat:8s}"
    print(line)
n = max(1, len(latest))
print()
for who, (s, cats) in tot.items():
    print(f"{who:9s} weighted {100 * s / n:5.1f}   correct {100 * cats.get('correct', 0) / n:5.1f}%   " +
          ", ".join(f"{k} {v}" for k, v in sorted(cats.items())))
