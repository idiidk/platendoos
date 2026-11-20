# Packs PNGs (one per size, square) into a Windows .ico (PNG entries, as Windows Vista and later read them).
#   python3 -I tools/win/make_ico.py out.ico a.png b.png ...
import sys, struct
pngs = [open(p, 'rb').read() for p in sys.argv[2:]]
def size(png): return struct.unpack('>I', png[16:20])[0]
pngs.sort(key=size)
out = struct.pack('<HHH', 0, 1, len(pngs))
off = 6 + 16 * len(pngs)
for p in pngs:
    s = size(p)
    out += struct.pack('<BBBBHHII', s % 256, s % 256, 0, 0, 1, 32, len(p), off)
    off += len(p)
open(sys.argv[1], 'wb').write(out + b''.join(pngs))
