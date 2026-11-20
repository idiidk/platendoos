# Offset recipes from the mods' sources: [(file, name, expected, fn_symbol, kind, nth)]
import os
import re, glob, json, sys
repo = os.path.dirname(os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))
out = []
# (every edition's sources: maketable.py takes the recipes of the edition it makes a table for)
for f in sorted(glob.glob(repo + '/mods/*.mm') + glob.glob(repo + '/mods/*.cpp') + glob.glob(repo + '/core/*.cpp') + glob.glob(repo + '/core/*.mm') + glob.glob(repo + '/mods/*.h')
                + glob.glob(repo + '/private/mods/*.mm') + glob.glob(repo + '/private/mods/*.cpp') + glob.glob(repo + '/private/mods/*.h')):
    src = open(f).read()
    consts = dict(re.findall(r'(?:static\s+)?(?:constexpr\s+)?const\s+char\s*\*\s*(?:const\s+)?(k\w+)\s*=\s*"([^"]+)"', src))
    # (core's shared parts name their pseudo-mod after nth: {..., intptr_t(0), "analysis"})
    for m in re.finditer(r'Offset\s+(\w+)\s*\{\s*(0x[0-9a-fA-F]+|\d+)\s*,\s*("[^"]+"|k\w+)\s*,\s*"([^"]+)"\s*,\s*(?:intptr_t\()?(\d+)\)?\s*(?:,\s*"\w+"\s*)?\}', src, re.S):
        name, exp, fn, kind, nth = m.groups()
        fn = fn.strip('"') if fn.startswith('"') else consts.get(fn, fn)
        out.append(dict(file=f.split('/')[-1], name=name, expected=int(exp, 0), fn=fn, kind=kind, nth=int(nth)))
json.dump(out, open(sys.argv[1], 'w'), indent=0)
print(len(out), 'unresolved fn consts:', [o['name'] for o in out if not o['fn'].startswith('_')])
