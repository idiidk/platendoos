# Removes the full edition's private parts from a file of the public tree (docs/editions.md):
#  - a block from a line containing "<private>" to the next line containing "</private>", both included
#    (// <private> in C++/JS/CSS, <!-- <private> --> in HTML/Markdown, # <private> in scripts);
#  - a line ending in "<!-- private -->" (Markdown: one bullet or row);
# and shows the public edition's own lines: a line that is only "<!-- public: TEXT -->" (hidden in the full
# edition's Markdown) becomes TEXT.
#   python3 -I tools/build/strip_private.py <file> [<file>...]    rewrites the files in place
#   python3 -I tools/build/strip_private.py -                      stdin to stdout
# Fails on a block that isn't closed.
import sys, re

OPEN, CLOSE, LINE = re.compile(r'<private>'), re.compile(r'</private>'), re.compile(r'<!--\s*private\s*-->\s*$')
PUBLIC = re.compile(r'^(\s*)<!--\s*public:\s?(.*?)\s*-->\s*$')

def strip(text, name):
    out, inside, start = [], False, 0
    for n, line in enumerate(text.split('\n'), 1):
        if not inside and OPEN.search(line) and not CLOSE.search(line):
            inside, start = True, n
            continue
        if inside:
            if CLOSE.search(line): inside = False
            continue
        if LINE.search(line): continue
        m = PUBLIC.match(line)
        out.append(m.group(1) + m.group(2) if m else line)
    if inside: sys.exit('%s:%d: <private> block not closed' % (name, start))
    return '\n'.join(out)

if sys.argv[1:] == ['-']:
    sys.stdout.write(strip(sys.stdin.read(), 'stdin'))
else:
    for f in sys.argv[1:]:
        t = open(f, encoding='utf-8').read()
        s = strip(t, f)
        if s != t: open(f, 'w', encoding='utf-8').write(s)
