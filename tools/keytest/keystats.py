"""Summarises ~/Library/Logs/rekordbox-betterkey.csv: how often rekordbox and the CNN agree.
    python3 tools/keytest/keystats.py [csv]"""
import collections, csv, os, sys
rows = list(csv.DictReader(open(sys.argv[1] if len(sys.argv) > 1 else os.path.expanduser('~/Library/Logs/rekordbox-betterkey.csv'))))
print(f"{len(rows)} tracks")
for rel, n in collections.Counter(r['relation'] for r in rows).most_common():
    print(f"  {rel:9s} {n:4d}  {100 * n / max(1, len(rows)):5.1f}%")
for lo in (0.5, 0.7, 0.9):
    sel = [r for r in rows if float(r['cnn_prob']) >= lo]
    same = sum(r['relation'] == 'same' for r in sel)
    print(f"  CNN prob >= {lo}: {len(sel)} tracks, {100 * same / max(1, len(sel)):.0f}% agree")
