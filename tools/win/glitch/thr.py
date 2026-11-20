#!/usr/bin/env python3
"""thr.py <run dir> <from s> <to s>: CPU per thread in a window after the load key (ms of CPU, priority, start module)"""
import sys, csv, re, collections
d, a, b = sys.argv[1], float(sys.argv[2]), float(sys.argv[3])
T0 = float(re.search(r'^(\d+) \S+ key shift\+right$', open(d + '/run.log').read(), re.M).group(1))
cpu = collections.Counter(); pr = {}; info = {}; other = collections.Counter(); tot = []
for r in csv.reader(open(d + '/threads.csv', errors='ignore')):
    if not r: continue
    if r[0] == 'T' and T0 + a * 1000 <= float(r[1]) < T0 + b * 1000: cpu[r[2]] += float(r[3]); pr.setdefault(r[2], set()).add(r[4])
    if r[0] == 'P' and T0 + a * 1000 <= float(r[1]) < T0 + b * 1000: other[r[2]] += float(r[3])
    if r[0] == 'S' and T0 + a * 1000 <= float(r[1]) < T0 + b * 1000: tot.append(float(r[2]))
    if r[0] == 'N': info[r[1]] = r
print('%s +%.1f..%.1f s: all cores %.0f%% avg; rekordbox %.0f ms CPU (%.1f cores)' % (d, a, b, sum(tot) / max(len(tot), 1), sum(cpu.values()), sum(cpu.values()) / (b - a) / 1000))
bymod = collections.Counter()
for t, c in cpu.items():
    m = info.get(t, [''] * 9)[7].split('+')[0]; bymod[m + ' base ' + info.get(t, [''] * 9)[4]] += c
print('  by start module:', ', '.join('%s %.0f' % kv for kv in bymod.most_common(10)))
for t, c in cpu.most_common(int(sys.argv[4]) if len(sys.argv) > 4 else 10):
    i = info.get(t, [''] * 9); print('  tid %6s %6.0f ms  prio %-8s base %-3s %s %s' % (t, c, '/'.join(sorted(pr[t])), i[4], i[7], i[8]))
print('  other processes:', ', '.join('%s %.0f' % kv for kv in other.most_common(6)))
