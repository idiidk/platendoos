# The Windows tables' verification data for one edition (docs/editions.md): what maketable.py and makesigs.py read.
#   - verify/*.out.json from tools/win/verify (kept with the code) and the work dir (~/rbwin: a newer batch that isn't
#     in the repository yet; a file there replaces the repository's of the same name);
#   - overrides.json's "drop" (the work dir's, else the repository's);
#   - tools/win/sigfields.txt.
# The full edition adds the same files from private/tools/win/: a verify file there with the name of a public one
# holds that file's full-edition entries, each with "at" = its index in the merged list (entries already in it are
# skipped); one without a public namesake is a file of its own; the drop entries and sigfields lines come after.
# The edition is RBM_EDITION's, full by default; a tree without private/ is always public.
import os, glob, json

def edition(repo):
    if not os.path.isdir(os.path.join(repo, 'private')): return 'public'
    return os.environ.get('RBM_EDITION', 'full')

def _extra(repo, ed, *path):
    return os.path.join(repo, 'private', 'tools', 'win', *path) if (ed or edition(repo)) == 'full' else None

def out_files(repo, work, ed=None):
    """[(path, entries)] of the verify/*.out.json files, in the order maketable reads them (sorted by path)."""
    vfiles = {}
    for d in (os.path.join(repo, 'tools', 'win', 'verify'), os.path.join(work, 'verify')):
        for f in glob.glob(os.path.join(d, '*.out.json')): vfiles[os.path.basename(f)] = f
    extra = _extra(repo, ed, 'verify')
    more = {os.path.basename(f): f for f in glob.glob(os.path.join(extra, '*.out.json'))} if extra else {}
    for b, f in more.items(): vfiles.setdefault(b, f)
    res = []
    for f in sorted(vfiles.values()):
        entries = json.load(open(f))
        b = os.path.basename(f)
        if b in more and more[b] != f:
            for r in sorted(json.load(open(more[b])), key=lambda r: r.get('at', 1 << 30)):
                r = dict(r)
                at = r.pop('at', len(entries))
                if r not in entries: entries.insert(at, r)
        else:
            entries = [{k: v for k, v in r.items() if k != 'at'} for r in entries]
        res.append((f, entries))
    return res

def drops(repo, work, ed=None):
    ovr = os.path.join(work, 'verify', 'overrides.json')
    if not os.path.exists(ovr): ovr = os.path.join(repo, 'tools', 'win', 'verify', 'overrides.json')
    drop = json.load(open(ovr)).get('drop', {}) if os.path.exists(ovr) else {}
    extra = _extra(repo, ed, 'verify', 'overrides.json')
    if extra and os.path.exists(extra):
        for k, v in json.load(open(extra)).get('drop', {}).items(): drop.setdefault(k, v)
    return drop

def sigfields_lines(repo, ed=None):
    out = []
    for f in (os.path.join(repo, 'tools', 'win', 'sigfields.txt'), _extra(repo, ed, 'sigfields.txt')):
        if f and os.path.exists(f): out += open(f).read().splitlines(True)
    return out
