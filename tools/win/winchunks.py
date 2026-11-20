# The function chunks of the Windows analysis (win.pkl, from tools/win/match) as text, for the C++ makesigs
# (tools/win/makesigs.cpp reads <workdir>/win_chunks.txt; the Python makesigs reads win.pkl itself).
#   python3 -I tools/win/winchunks.py [workdir]        (default ~/rbwin; writes <workdir>/win_chunks.txt)
import os, pickle, sys

work = sys.argv[1] if len(sys.argv) > 1 else os.path.expanduser('~/rbwin')
funcs = pickle.load(open(os.path.join(work, 'win.pkl'), 'rb'))['funcs']
with open(os.path.join(work, 'win_chunks.txt'), 'w') as f:
    f.write("# va size root (hex), from win.pkl funcs: makesigs's chunks()\n")
    for va, fn in funcs.items():  # in the pickle's order: chunks() lists a function's chunks in it
        f.write('%x %x %x\n' % (va, fn['size'], fn['root']))
print('%d chunks' % len(funcs), file=sys.stderr)
