"""Checks that every hook's x86_64 site is the same call as its arm64 one, in the two slices of one rekordbox.

    python3 tools/dev/hookorder.py <rekordbox binary> [--repo <tree>] [-v]

A hooks.def site is "the nth of count calls to target inside caller", and the compiler lays the same calls out in
another order in the two slices: the nth call in x86_64 code can be another call than arm64's nth, and nothing
notices (docs/intel-mac.md; spotify_rev_item1/2 and bfx_rollWrap were). This takes each hook's site in both slices
the way patch_binary.py finds it (HOOK / HOOK_OR, HOOK_X86 on x86_64), and compares the calls around it: every
direct call (bl / b, call / jmp) out of the caller, by the name of what it calls. Of the caller's x86_64 calls to the
target, the one whose neighbours look most like the arm64 site's should be the one the x86_64 line picks; a hook
where another one looks more alike is reported as WRONG, with the nth that would match. Ties (calls with the same
neighbours) aren't reported: there the order can't be told from the calls, and doesn't matter for them either.

--repo reads hooks.def (and the hook files it includes) from another tree, e.g. a copy to try a changed line; RBM_EDITION picks
the edition as for patch_binary.py. -v lists every hook. The exit status is 1 when a hook is WRONG or missing.
No disassembler: calls are found by the same byte scan the patchers use, kept to calls of named functions.
"""
import bisect
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, HERE)
sys.path.insert(0, REPO)
from rbbin import CPU_ARM64, CPU_X86_64, Binary, b_target, bl_target  # noqa: E402
import patch_binary  # noqa: E402

NEAR, FAR = 6, 24  # calls on each side compared first, then (to break a tie) further out
SAME = {"stub:___bzero": "stub:_bzero"}  # imports that have another name in one slice


class Calls:
    """One slice's direct calls out of a function, by callee name, in address order."""

    def __init__(self, b):
        self.b = b
        self.names = {}
        for name, va in sorted(b.symbols.items()):
            self.names.setdefault(va, name)  # (an address with several names: the first by name, in both slices)
        for name, va in b.stubs.items():
            self.names[va] = SAME.get("stub:" + name, "stub:" + name)
        self.cache = {}

    def of(self, fn):
        """[(address, callee name)] for every bl / b (call / jmp rel32) from fn to a named function outside it."""
        if fn in self.cache:
            return self.cache[fn]
        b, end, out = self.b, self.b.function_end(fn), []
        if b.cpu == CPU_ARM64:
            for va, w in b.insns(fn):
                t = bl_target(va, w)
                t = b_target(va, w) if t is None else t
                if t is not None and not fn <= t < end and t in self.names:
                    out.append((va, self.names[t]))
        else:
            code = b.code(fn)
            for i in range(len(code) - 4):
                if code[i] in (0xE8, 0xE9):
                    t = fn + i + 5 + struct.unpack_from("<i", code, i + 1)[0]
                    if not fn <= t < end and t in self.names:
                        out.append((fn + i, self.names[t]))
        self.cache[fn] = out
        return out


def lcs(a, b):
    """The length of the longest common subsequence of two short lists."""
    row = [0] * (len(b) + 1)
    for x in a:
        prev = 0
        for j, y in enumerate(b):
            prev, row[j + 1] = row[j + 1], prev + 1 if x == y else max(row[j + 1], row[j])
    return row[-1]


def around(calls, site, k):
    """The callee names of the k calls before the site and of the k calls after it (nearest first)."""
    addrs = [va for va, _ in calls]
    i = bisect.bisect_left(addrs, site)
    j = i + 1 if i < len(addrs) and addrs[i] == site else i  # (the site itself, when it is a call, is neither)
    return [n for _, n in calls[max(0, i - k):i]][::-1], [n for _, n in calls[j:j + k]]


def likeness(ca, sa, cx, sx):
    """How alike the neighbours of the arm64 site sa and the x86_64 site sx are: the common calls among the NEAR on
    each side, then among the FAR, then how close they are in the function."""
    near, far = [lcs(*z) for z in zip(around(ca, sa, NEAR), around(cx, sx, NEAR))], \
        [lcs(*z) for z in zip(around(ca, sa, FAR), around(cx, sx, FAR))]
    pa = bisect.bisect_left([va for va, _ in ca], sa) / max(1, len(ca))
    px = bisect.bisect_left([va for va, _ in cx], sx) / max(1, len(cx))
    return 10000 * sum(near) + 100 * sum(far) + 99 * (1 - abs(pa - px))


def assign(score):
    """The one-to-one pairing of rows and columns with the highest total score (Hungarian method; the matrix may
    be rectangular). row -> column, or None for rows left over."""
    rows, cols = len(score), len(score[0])
    n = max(rows, cols)
    top = max(max(r) for r in score)
    cost = [[top - score[i][j] if i < rows and j < cols else top for j in range(n)] for i in range(n)]
    u, v, p, way = [0.0] * (n + 1), [0.0] * (n + 1), [0] * (n + 1), [0] * (n + 1)
    for i in range(1, n + 1):
        p[0], j0 = i, 0
        minv, used = [float("inf")] * (n + 1), [False] * (n + 1)
        while True:
            used[j0] = True
            i0, delta, j1 = p[j0], float("inf"), 0
            for j in range(1, n + 1):
                if not used[j]:
                    c = cost[i0 - 1][j - 1] - u[i0] - v[j]
                    if c < minv[j]:
                        minv[j], way[j] = c, j0
                    if minv[j] < delta:
                        delta, j1 = minv[j], j
            for j in range(n + 1):
                if used[j]:
                    u[p[j]] += delta
                    v[j] -= delta
                else:
                    minv[j] -= delta
            j0 = j1
            if p[j0] == 0:
                break
        while j0:
            j1 = way[j0]
            p[j0] = p[j1]
            j0 = j1
    out = [None] * rows
    for j in range(1, n + 1):
        if p[j] - 1 < rows and j - 1 < cols:
            out[p[j] - 1] = j - 1
    return out


def site_of(b, sites):
    """(site, kind, the line's caller, nth, count, target) of the first of a hook's sites that is there, or None."""
    for s in sites:
        if s[0]:
            found, _ = patch_binary.find_site(b, *s)
            if found:
                return found[0], s[4], s
    return None


def candidates(b, line):
    """Every site in the function that fits the line but for its nth: the count calls (or adr / lea) to its target."""
    caller, _, _, target, kind = line
    fn, t = b.sym(caller), b.sym(target)
    return b.calls(fn, t) if kind == -1 else b.jumps(fn, t) if kind == -2 else b.adrs(fn, t, kind)


def main(argv):
    path = argv[1]
    verbose = "-v" in argv
    if "--repo" in argv:
        patch_binary.REPO = os.path.abspath(argv[argv.index("--repo") + 1])
    hooks = patch_binary.load_hooks()
    alts = patch_binary.load_alternatives()
    x86 = patch_binary.load_sites("HOOK_X86")
    data = open(path, "rb").read()
    arm, intel = Binary(data, CPU_ARM64), Binary(data, CPU_X86_64)
    calls = {CPU_ARM64: Calls(arm), CPU_X86_64: Calls(intel)}
    bad = ok = tied = off = 0
    for hook in hooks:
        name = hook[0]
        xs = patch_binary.hook_sites(hook, alts, x86)
        if all(not s[0] for s in xs):
            off += 1
            if verbose:
                print("%-22s not on x86_64 (hooks.def)" % name)
            continue
        a, x = site_of(arm, patch_binary.hook_sites(hook, alts, {})), site_of(intel, xs)
        if not a or not x:
            bad += 1
            print("%-22s MISSING: no %s site (patch_binary.py says why)" % (name, "arm64" if not a else "x86_64"))
            continue
        ca, cx = calls[CPU_ARM64].of(arm.sym(a[2][0])), calls[CPU_X86_64].of(intel.sym(x[2][0]))
        # Pair every arm64 call to the target with the x86_64 call most like it, one to one: the hook's arm64 call
        # should get the x86_64 one its line picks.
        ra, rx = candidates(arm, a[2]), candidates(intel, x[2])
        score = [[likeness(ca, sa, cx, sx) for sx in rx] for sa in ra]
        pair = assign(score)
        i, mine = ra.index(a[0]), rx.index(x[0])
        line = "%-22s arm64 %#x nth %d/%d, x86_64 %#x nth %d/%d" % (name, a[0], a[2][1], a[2][2], x[0], x[2][1], x[2][2])
        if pair[i] == mine:
            ok += 1
            if verbose:
                print(line + ": same call")
            continue
        # Another pairing as good as the best one with this hook's x86_64 call: calls that can't be told apart.
        other = pair.index(mine) if mine in pair else None
        swap = score[i][mine] + (score[other][pair[i]] if other is not None and pair[i] is not None else 0)
        best = (score[i][pair[i]] if pair[i] is not None else 0) + (score[other][mine] if other is not None else 0)
        if swap >= best:
            tied += 1
            if verbose:
                print(line + ": same call or one with the same neighbours (nth %s)" % pair[i])
            continue
        bad += 1
        print(line + ": WRONG, arm64's call is x86_64's %s" % ("nth %d" % pair[i] if pair[i] is not None else "none"))
        if verbose:
            (ba, aa), (bx, ax) = around(ca, a[0], 3), around(cx, x[0], 3)
            print("    arm64  before %s after %s\n    x86_64 before %s after %s" % (ba, aa, bx, ax))
    print("%d hooks: %d the same call in both slices, %d among calls with the same neighbours, %d not on x86_64, "
          "%d WRONG or missing (rekordbox arm64 %s, x86_64 %s)" % (len(hooks), ok, tied, off, bad, arm.uuid, intel.uuid))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
