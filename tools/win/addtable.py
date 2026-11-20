# A Windows address table for a rekordbox build that has no full verification of its own: what patcher/retable.h
# finds in it from the signatures of the verified build, plus the lines checked by hand for what that build's code
# changed (tools/win/verify/rekordbox-<build>.add: table lines, with comments saying how each was checked).
#   build/retable <rekordbox.exe> tools/win/sigs/rekordbox-<verified build>.sig \
#     | python3 -I tools/win/addtable.py - tools/win/verify/rekordbox-<build>.add > tools/win/tables/rekordbox-<build>.tbl
# An added line may only fill a gap: a name, field or hook the scanner found itself must agree with it (then the
# added line is no longer needed), anything else stops with an error. Run it again whenever the signatures change
# (a new Addr, Offset or hook in the mods): the shipped table is used as it is, the patcher doesn't scan a build
# that has one.
import sys, re

def key(line):
    """(kind, what the line gives a value for), or None for a comment / the build line."""
    w = line.split(' ')
    if w[0] in ('addr', 'off', 'adj'): return (w[0], line[len(w[0]) + 1:line.rfind(' ')])
    if w[0] in ('hook', 'hookreg', 'orig'): return (w[0], w[1])
    return None

src = sys.stdin.read() if sys.argv[1] == '-' else open(sys.argv[1]).read()
base = src.splitlines()
add = [l.rstrip() for l in open(sys.argv[2]).read().splitlines()]
build = next(l.split()[1] for l in base if l.startswith('build '))
abuild = next((l.split()[1] for l in add if l.startswith('build ')), None)
if abuild != build: sys.exit('addtable: the additions are for build %s, the table is for %s' % (abuild, build))

have = {}
for l in base:
    k = key(l)
    if k: have.setdefault(k, []).append(l)
new = {}
for l in add:
    k = key(l)
    if k: new.setdefault(k, []).append(l)
extra, same = [], []
for k, lines in new.items():
    if k not in have: extra += lines
    elif sorted(have[k]) == sorted(lines): same.append(k)
    else: sys.exit('addtable: %s %s is in the scanned table with another value:\n  %s\n  %s' % (k[0], k[1], '\n  '.join(have[k]), '\n  '.join(lines)))
for k in same: sys.stderr.write('addtable: the scanner finds %s %s itself now: the added line can go\n' % k)

given = {'%s %s' % k for k in new}
out = ['build ' + build]
frm = next((m.group(1) for l in base for m in [re.search(r'from signatures of (\S+)', l)] if m), '?')
out.append('# platendoos address table for rekordbox.exe %s: found by patcher/retable.h from the signatures of %s,' % (build, frm))
out.append('# plus %d lines checked by hand (tools/win/verify/rekordbox-%s.add, merged by tools/win/addtable.py)' % (len(extra), build))
order = {'addr': 0, 'off': 1, 'hook': 2, 'hookreg': 3, 'adj': 3, 'orig': 3}
body = [l for l in base if key(l)]
# the added lines go after the last line of their kind
for kind in ('addr', 'off', 'hook', 'hookreg', 'adj', 'orig'):
    mine = [l for l in extra if key(l)[0] == kind]
    if not mine: continue
    at = max((i for i, l in enumerate(body) if order[key(l)[0]] <= order[kind]), default=-1) + 1
    body[at:at] = mine
out += body
for l in base:
    if l.startswith('# ') and ' parts found' in l: out.append(l + ' by the scanner')
    # what the scanner missed and the added lines don't give (a hook's note may carry a remark in brackets)
    m = re.match(r'# not found: (addr|off|hook) (.*?)( \(.*\))?$', l)
    if m and '%s %s' % (m.group(1), m.group(2)) not in given: out.append(l)
sys.stdout.write('\n'.join(out) + '\n')
sys.stderr.write('addtable: %s: %d lines added to the %d scanned\n' % (build, len(extra), len(body) - len(extra)))
