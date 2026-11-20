# hooks.def as one edition's build reads it, for the Python tools: what the C++ preprocessor would leave of it.
# An `#include "<file>"` line (relative to the repository) is replaced by that file's lines; one under
# `#ifndef RBM_PUBLIC` only for the full edition. Other preprocessor lines are kept as they are.
#   import hooksdef; hooksdef.lines(repo) / hooksdef.text(repo)
# The edition (docs/editions.md) is RBM_EDITION's, full by default; a tree without private/ is always public.
import os, re

def edition(repo):
    if not os.path.isdir(os.path.join(repo, 'private')): return 'public'
    return os.environ.get('RBM_EDITION', 'full')

def lines(repo, ed=None, path='hooks.def'):
    ed = ed or edition(repo)
    out, live = [], [True]
    for line in open(os.path.join(repo, path), encoding='utf-8').read().split('\n'):
        s = line.strip()
        if s.startswith('#if'):
            live.append(live[-1] and not (s.split()[:2] == ['#ifndef', 'RBM_PUBLIC'] and ed == 'public'))
        elif s.startswith('#endif'):
            live.pop()
        elif not live[-1]:
            continue
        else:
            m = re.match(r'#include\s+"([^"]+)"', s)
            if m:
                out += lines(repo, ed, m.group(1))
                continue
        out.append(line)
    return out

def text(repo, ed=None):
    return '\n'.join(lines(repo, ed)) + '\n'
