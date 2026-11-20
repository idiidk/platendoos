# Checks that Windows address tables and signatures were made for this edition (docs/editions.md): every hook they
# name must be a hook of the edition's hooks.def (tools/build/hooksdef.py). A table made for another edition names
# hooks this build doesn't have; it is refused rather than shipped.
#   python3 -I tools/win/checkinputs.py <repo> <file.tbl|file.sig>...
import sys, os, re
repo = sys.argv[1]
sys.path.insert(0, os.path.join(repo, 'tools', 'build'))
import hooksdef
hooks = set(re.findall(r'^HOOK\((\w+),', hooksdef.text(repo), re.M))
bad = {}
for f in sys.argv[2:]:
    for line in open(f, encoding='utf-8', errors='replace'):
        w = line.split()
        if len(w) > 1 and w[0] in ('hook', 'hookreg', 'hookin', 'hookcall', 'hooksites') and w[1] not in hooks:
            bad.setdefault(f, set()).add(w[1])
for f, names in bad.items():
    print('%s: made for another edition (hooks %s aren\'t in its hooks.def)' % (f, ', '.join(sorted(names))))
sys.exit(1 if bad else 0)
