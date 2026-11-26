"""The UI benchmark behind mods/relayout.mm, quickframe.mm and framerate.cpp's numbers. Needs `uimon.sh start`.
    python3 bench.py <tag>          Preferences open/close, the layout drop-down, repaint-to-frame latency
    python3 bench.py <tag> layout   also: window resize (40 steps), three panel toggles, the 2/4 deck layout switch
The click positions are for the 2-deck horizontal PERFORMANCE layout on a 1512 pt wide window (panel buttons at the
top left): check them with `uimon.sh snap` after a rekordbox update or on another display. Every toggle is done an even
number of times, the layout switch ends on 2 decks. Sums of the main thread's stalls in ms; the first of a row is
often the cold one."""
import meas, lat, time, sys, re, json

def stalls(label, cmds, dur, after=0.0):
    t0, s, _ = meas.run(label, cmds, dur)
    return round(sum(ms for at, ms in s if at >= after)), t0

def busy(t0, dur):
    seen, total = set(), 0.0
    for ln in open(meas.D + '/stalls.txt'):
        m = re.match(r'(\d+\.\d+) BUSY ([\d.]+) ms', ln)
        if m and t0 <= float(m.group(1)) <= t0 + dur and round(float(m.group(1)), 2) not in seen:
            seen.add(round(float(m.group(1)), 2)); total += float(m.group(2))
    return round(total)

tag = sys.argv[1]
res = {'prefs_open': [], 'prefs_close': [], 'popup': []}
for i in range(6):
    res['prefs_open'].append(stalls(f'{tag}_prefs_open{i}', ['menu Apple/Preferences'], 2.5)[0])
    res['prefs_close'].append(stalls(f'{tag}_prefs_close{i}', ['closewin Preferences'], 2.5)[0])
print(tag, 'Preferences open', res['prefs_open'], 'close', res['prefs_close'])
for i in range(6):
    t0, s, _ = meas.run(f'{tag}_popup{i}', ['click 150 75', 'sleep 1.2', 'key 53 0'], 3.0)
    res['popup'].append(round(sum(ms for at, ms in s if at < 1.1)))
print(tag, 'layout drop-down opens', res['popup'])
o = lat.latency(14)
a = sorted(x[0] for x in o); b = sorted(x[1] for x in o if x[1] == x[1])
if a: print(tag, 'repaint -> frame start ms: min %.1f med %.1f max %.1f' % (a[0], a[len(a) // 2], a[-1]))
if b: print(tag, 'repaint -> presented ms: min %.1f med %.1f max %.1f (n=%d; with displaySync off: handed over, not on the glass)' % (b[0], b[len(b) // 2], b[-1], len(b)))
if 'layout' in sys.argv[2:]:
    res['resize'] = []
    for i in range(3):
        _, t0 = stalls(f'{tag}_resize{i}', ['resize 20 -4 -3 80'], 5.0)
        res['resize'].append(busy(t0, 5))
    print(tag, 'resize 40 steps, main thread busy ms:', res['resize'])
    for name, x in (('fx', 205), ('sampler', 279), ('mixer', 316)):
        res[name] = [stalls(f'{tag}_{name}{i}', [f'click {x} 75'], 2.0)[0] for i in range(6)]
        print(tag, name, 'panel toggle:', res[name])
    res['layout'] = []
    for i in range(3):
        res['layout'].append(stalls(f'{tag}_to4H{i}', ['click 150 75', 'sleep 1.0', 'click 200 187'], 3.8, 0.9)[0])
        res['layout'].append(stalls(f'{tag}_to2H{i}', ['click 150 75', 'sleep 1.0', 'click 200 99'], 3.8, 0.9)[0])
    print(tag, 'layout switch (4 decks, 2 decks, ...):', res['layout'])
json.dump(res, open(f'{meas.D}/bench_{tag}.json', 'w'))
