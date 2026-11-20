#!/usr/bin/env python3
"""analyze.py <run dir> [reference.wav]: dropouts in a loopback recording of rekordbox (rbglitch rec).
Three detectors: (1) packets Windows marked silent while the stream ran, (2) runs of exact zeros,
(3) against the track's own audio: places where the recording stops matching it (residual) or jumps in time."""
import sys, wave, csv, os, numpy as np
args = [a for a in sys.argv[1:] if not a.startswith('-')]
d = args[0]; ref = args[1] if len(args) > 1 else os.path.expanduser('~/rbwin/glitch/demo1.wav')
quiet = '-q' in sys.argv
def rd(p):
    w = wave.open(p); return w.getframerate(), np.frombuffer(w.readframes(w.getnframes()), dtype=np.int16).astype(np.float64)
rate, x = rd(d + '/audio.wav')
rows = [r for r in csv.reader(open(d + '/audio.wav.pkt.csv')) if r and r[0][0].isdigit()]
fr = np.array([int(r[1]) for r in rows]); n = np.array([int(r[2]) for r in rows]); fl = np.array([int(r[3]) for r in rows]); q = np.array([float(r[5]) for r in rows]); t = np.array([float(r[0]) for r in rows])
t0 = np.median(t - q)
def wall(frame):
    i = max(0, np.searchsorted(fr, frame, side='right') - 1); return q[i] + t0 + (frame - fr[i]) * 1000.0 / rate
def clk(ms):
    ms %= 86400000; return '%02d:%02d:%06.3f' % (ms // 3600000, ms // 60000 % 60, ms % 60000 / 1000)
events = []   # (wall ms, kind, text)
# 1. silent packets (not the first: the stream's start), and capture-side discontinuities
for i in np.nonzero(fl)[0]:
    if i == 0: continue
    events.append((wall(fr[i]), 'silent-packet' if fl[i] & 2 else 'capture-discontinuity', '%d frames flags %d' % (n[i], fl[i])))
# 2. zero runs of 2 ms and more inside the music
nz = np.nonzero(x)[0]
if len(nz) == 0: print('recording is silent'); sys.exit(0)
a0, a1 = nz[0], nz[-1]
z = (x[a0:a1] == 0).astype(np.int8); e = np.diff(np.concatenate([[0], z, [0]])); s = np.nonzero(e == 1)[0] + a0; en = np.nonzero(e == -1)[0] + a0
for a, b in zip(s, en):
    if b - a >= rate * 2 // 1000: events.append((wall(a), 'zeros', '%.1f ms' % ((b - a) * 1000.0 / rate)))
# 3. against the reference
rr, r = rd(ref); assert rr == rate
def ncc(u, v):
    du = np.sqrt((u * u).sum() * (v * v).sum()); return (u * v).sum() / du if du > 0 else 0.0
def find(seg, lo, hi):   # best lag of seg within r[lo:hi]
    lo = max(0, lo); hi = min(len(r), hi)
    if hi - lo < len(seg): return None, 0
    w = r[lo:hi]; m = len(w) + len(seg); N = 1 << (m - 1).bit_length()
    c = np.fft.irfft(np.fft.rfft(w, N) * np.conj(np.fft.rfft(seg, N)), N)[:len(w) - len(seg) + 1]
    cs = np.concatenate([[0], np.cumsum(w * w)]); en = np.sqrt(np.maximum(cs[len(seg):] - cs[:-len(seg)], 1e-9) * (seg * seg).sum())
    k = int(np.argmax(c / en)); return lo + k, float((c / en)[k])
B = 2048
pos, c0 = find(x[a0 + rate:a0 + 3 * rate], 0, len(r))    # where the recording's second second is in the track
if c0 < 0.5: print('reference not found in the recording (best match %.2f)' % c0); pos = None
ncompared = 0; resid = []; lag = None
if pos is not None:
    lag = pos - (a0 + rate)              # r index = x index + lag
    i = a0 + rate
    # walk back to the start, then forward
    starts = list(range(a0 + rate, a0, -B))[1:] 
    i = a0 + (rate % B)
    while i + B <= a1 and i + lag + B <= len(r):
        seg = x[i:i + B]; refseg = r[i + lag:i + lag + B] if i + lag >= 0 else None
        if refseg is None or (seg * seg).mean() < 100 and (refseg * refseg).mean() < 100: i += B; continue
        c = ncc(seg, refseg)
        g = (seg * refseg).sum() / max((refseg * refseg).sum(), 1e-9)
        res = seg - g * refseg
        # worst 2 ms of this block against the block's own level
        w = rate * 2 // 1000; rm = np.sqrt(np.convolve(res * res, np.ones(w) / w, 'valid').max()); lvl = np.sqrt((refseg * refseg).mean()) * abs(g) + 1
        resid.append(rm / lvl); ncompared += 1
        if c < 0.9 or rm / lvl > 0.1:
            k = int(np.argmax(np.convolve(res * res, np.ones(w) / w, 'valid')))
            # did the track's position jump? the block after this one says (searched within 250 ms: the music repeats every beat)
            nxt = x[i + B:i + 2 * B]; jumped = False
            if len(nxt) == B and i + lag + 2 * B <= len(r) and ncc(nxt, r[i + B + lag:i + 2 * B + lag]) < 0.95:
                p2, c2 = find(nxt, i + B + lag - rate // 4, i + 2 * B + lag + rate // 4)
                if p2 is not None and c2 > 0.95 and p2 - (i + B) != lag:
                    events.append((wall(i + k), 'time-jump', '%+.1f ms' % ((p2 - (i + B) - lag) * 1000.0 / rate))); lag = p2 - (i + B); jumped = True
            if not jumped: events.append((wall(i + k), 'mismatch', 'match %.2f, worst 2 ms %.2fx the level' % (c, rm / lvl)))
        i += B
events.sort()
# merge events within 60 ms into one dropout
drops = []
for ev in events:
    if drops and ev[0] - drops[-1][-1][0] < 60: drops[-1].append(ev)
    else: drops.append([ev])
dur = (a1 - a0) / rate
print('%s: %.1f s of audio (%s - %s), %d packets of %d frames, compared %.1f s with the track, median residual %.3f' % (os.path.basename(d.rstrip('/')), dur, clk(wall(a0)), clk(wall(a1)), len(rows), int(np.median(n)), ncompared * B / rate, np.median(resid) if resid else -1))
if resid and not quiet: print('residual percentiles 50/99/99.9/max: %.4f %.4f %.4f %.4f' % tuple(np.percentile(resid, [50, 99, 99.9, 100])))
print('DROPOUTS %d' % len(drops))
for g in drops:
    print('  %s (%.0f): %s' % (clk(g[0][0]), g[0][0] % 86400000, '; '.join('%s %s' % (k, tx) for _, k, tx in g[:6]) + (' ...' if len(g) > 6 else '')))
