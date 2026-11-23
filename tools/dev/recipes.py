"""Finds and checks the recipes core/image_mac.mm reads struct offsets with (rbm::Offset): "the immediate of the
nth instruction of a kind inside a function".

    python3 tools/dev/recipes.py find <binary> <symbol regex> <offset>   # instructions using offset, with their recipe
    python3 tools/dev/recipes.py check <binary> [--x86] [--root <dir>]  # every recipe in the sources against its expected value

Kinds (as written in the sources): "ldr", "str" (any size; rbm decodes size and scale), "add" (add immediate),
"tb" (tbz/tbnz: the bit number), "movz" (a single movz), "mov" (movz + movk: a 32-bit constant built in one
register), each optionally with a register (the base, or the bit-tested / written one): "ldr x0" = loads based
on x0. "vt:<method symbol>" in vtable fn gives the method's slot offset from the address point (+0x10).

check reads every Offset declaration in core/, mods/, engines/ and private/mods/ (--root: another checkout), and runs
it in the arm64 slice: FAIL if it finds nothing, or another value in the tested build (7.2.19; in a later one that is
a field that "moved", which the mods follow). --x86 also confirms each one's x86_64 value (the arm64 value plus the declaration's X86{}
difference, docs/intel-mac.md) in the x86_64 slice of the same file: a vtable slot there, or the value used by the
x86_64 code of the same function (a displacement or immediate; between the same named calls as the arm64 instruction
when it is small enough to match by coincidence). A recipe it can't confirm "needs X86{}": its field is likely laid
out differently there (look at the x86_64 code: tools/dev/rbbin.py, or capstone). --x86 needs capstone (pip install
capstone). Exits 1 if any recipe fails.
"""
import os
import re
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from rbbin import CPU_X86_64, TEXT_BASE, Binary  # noqa: E402

HERE = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
TESTED_BUILD = "D827BD186918357A9F57C506CF428700"  # rekordbox 7.2.19 arm64: the expected values (core/image_mac.mm)


def decode(w):
    """(kind, base register, immediate) of the instructions recipes use, else None."""
    if w & 0x3B000000 == 0x39000000:                          # load/store register (also FP), unsigned immediate
        size, opc = w >> 30, (w >> 22) & 3
        if w & 0x04000000 and opc >= 2:                       # 128-bit FP
            return ("str" if opc == 2 else "ldr", (w >> 5) & 31, ((w >> 10) & 0xFFF) << 4)
        return ("str" if opc == 0 else "ldr", (w >> 5) & 31, ((w >> 10) & 0xFFF) << size)
    if w & 0x7F800000 == 0x11000000:                          # add (immediate)
        return ("add", (w >> 5) & 31, ((w >> 10) & 0xFFF) << (12 if w & 0x400000 else 0))
    if w & 0x7E000000 == 0x36000000:                          # tbz / tbnz
        return ("tb", w & 31, (w >> 31) << 5 | (w >> 19) & 31)
    if w & 0x7F800000 == 0x52800000:                          # movz
        return ("movz", w & 31, ((w >> 5) & 0xFFFF) << 16 * ((w >> 21) & 3))
    return None


def matches(kind, k, rn):
    want = kind.split()
    return k == want[0] and (len(want) == 1 or rn == int(want[1][1:]))


def run_at(b, fn, kind, nth):
    """The recipe's value and the address of the instruction it read (the vtable slot for vt:), or (None, None)."""
    if kind.startswith("vt:"):
        method = b.sym(kind[3:])
        if method is None:
            return None, None
        for i in range(1024):
            v = struct.unpack_from("<Q", b.data, b.fo(fn + 0x10 + 8 * i))[0]
            if (v & 0xFFFFFFFFF) + TEXT_BASE == method:   # chained-fixup rebase: target in the low 36 bits
                if nth == 0:
                    return 8 * i, fn + 0x10 + 8 * i
                nth -= 1
        return None, None
    if kind == "mov":
        val, n = None, -1
        for va, w in b.insns(fn):
            if w & 0x7F800000 == 0x52800000:      # movz w
                rd, val, n = w & 31, ((w >> 5) & 0xFFFF) << 16 * ((w >> 21) & 3), n + 1
            elif w & 0x7F800000 == 0x72800000 and val is not None and (w & 31) == rd:  # movk w, same register
                val |= ((w >> 5) & 0xFFFF) << 16 * ((w >> 21) & 3)
                if n == nth:
                    return val, va
        return None, None
    n = 0
    for va, w in b.insns(fn):
        d = decode(w)
        if d and matches(kind, d[0], d[1]):
            if n == nth:
                return d[2], va
            n += 1
    return None, None


def run(b, fn, kind, nth):
    """The recipe's value, or None."""
    return run_at(b, fn, kind, nth)[0]


def find(b, pattern, offset):
    for name, fn in sorted(b.symbols.items()):
        if not re.search(pattern, name) or not (b.text_range()[0] <= fn < b.text_range()[1]):
            continue
        counts = {}
        for va, w in b.insns(fn):
            d = decode(w)
            if not d:
                continue
            for kind in (d[0], "%s x%d" % (d[0], d[1])):
                if d[2] == offset:
                    print('%s  {"%s", "%s", %d}  (%d insns)' % (hex(va), name, kind, counts.get(kind, 0),
                                                              (b.function_end(fn) - fn) // 4))
                counts[kind] = counts.get(kind, 0) + 1


# ---- the declarations ------------------------------------------------------------------------------------------

STR = r'"[^"\n]*"'
# Offset NAME{expected, fn, kind, nth[, X86{value}][, "file"]}: fn a string or a name (#define, const char* ...);
# kind strings and names one after another ("vt:" MACRO); nth n or intptr_t(n).
RECIPE_RE = re.compile(r'\bOffset\s+(\w+)\s*\{\s*(0x[0-9a-fA-F]+|\d+)\s*,\s*(%s|\w+)\s*,\s*((?:%s|\w+)(?:\s+(?:%s|\w+))*)\s*,'
                       r'\s*(?:intptr_t\(\s*)?(\d+)\s*\)?\s*(?:,\s*X86\s*\{\s*(0x[0-9a-fA-F]+|\d+)\s*\})?' % (STR, STR, STR))
DEFINE_RE = re.compile(r'^\s*#define\s+(\w+)\s+(%s)\s*$' % STR, re.M)
CONST_RE = re.compile(r'(?:const\s+char\s*\*\s*(?:const\s+)?|constexpr\s+char\s+|const\s+char\s+)(\w+)(?:\[\])?\s*=\s*(%s)\s*;' % STR)


def recipes(root):
    """Every Offset declared in the sources under root: dicts of where, name, expected, fn, kind, nth, x86 (None: the
    same value in both slices)."""
    out = []
    for d in ("core", "mods", "engines", "private/mods"):
        if not os.path.isdir(os.path.join(root, d)):
            continue
        for f in sorted(os.listdir(os.path.join(root, d))):
            path = os.path.join(root, d, f)
            if not f.endswith((".cpp", ".mm", ".h")) or f == "rbmod.h":
                continue
            src = open(path, errors="replace").read()
            names = {n: v[1:-1] for n, v in CONST_RE.findall(src)}
            names.update({n: v[1:-1] for n, v in DEFINE_RE.findall(src)})

            def text(tokens):
                return "".join(t[1:-1] if t.startswith('"') else names.get(t, "?" + t) for t in re.findall(STR + r'|\w+', tokens))
            for m in RECIPE_RE.finditer(src):
                out.append(dict(where="%s/%s:%d" % (d, f, src.count("\n", 0, m.start()) + 1), name=m.group(1),
                                expected=int(m.group(2), 0), fn=text(m.group(3)), kind=text(m.group(4)), nth=int(m.group(5)),
                                x86=int(m.group(6), 0) if m.group(6) else None))
    return out


# ---- x86_64 ----------------------------------------------------------------------------------------------------

class X86Check:
    """Confirms recipe values in the x86_64 slice (capstone)."""

    def __init__(self, arm, data):
        try:
            import capstone
            from capstone import x86
        except ImportError:
            raise SystemExit("--x86 needs capstone: pip install capstone")
        self.X = x86
        self.arm, self.xb = arm, Binary(data, CPU_X86_64)
        self.md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
        self.md.detail = True
        self.stack = {x86.X86_REG_RSP, x86.X86_REG_RBP, x86.X86_REG_ESP, x86.X86_REG_EBP}
        self.names = {}
        for b in (arm, self.xb):
            names = {}
            for n, a in b.symbols.items():
                names.setdefault(a, n)
            for n, a in b.stubs.items():
                names.setdefault(a, "stub:" + n)
            self.names[id(b)] = names
        self.cache = {}

    def dis(self, fn):
        if fn not in self.cache:
            o = self.xb.fo(fn)
            self.cache[fn] = list(self.md.disasm(self.xb.data[o:o + self.xb.function_end(fn) - fn], fn))
        return self.cache[fn]

    def arm_calls(self, fn):
        names, out = self.names[id(self.arm)], []
        for va, w in self.arm.insns(fn):
            if w & 0xFC000000 == 0x94000000:
                t = va + 4 * ((((w & 0x3FFFFFF) ^ 0x2000000) - 0x2000000))
                out.append((va, names.get(t, "?")))
        return out

    def x86_calls(self, ins):
        names = self.names[id(self.xb)]
        return [(i.address, names.get(i.operands[0].imm, "?") if i.operands[0].type == self.X.X86_OP_IMM else "?")
                for i in ins if i.mnemonic == "call"]

    @staticmethod
    def keyed(calls):
        seen, out = {}, []
        for va, n in calls:
            out.append((va, (n, seen.get(n, 0))))
            seen[n] = seen.get(n, 0) + 1
        return out

    def window(self, fn, xfn, va, ins):
        """The x86_64 addresses [lo, hi] between the same named calls as the arm64 instruction at va (one call of slack
        on each side); None for an end that has no such call."""
        ac, xc = self.keyed(self.arm_calls(fn)), self.keyed(self.x86_calls(ins))
        xpos = {key: a for a, key in xc if key[0] != "?"}
        before, after = [c for c in ac if c[0] < va], [c for c in ac if c[0] > va]

        def first(cands):
            for _, key in cands:
                if key in xpos:
                    return xpos[key]
            return None
        return (first(reversed(before[:-1])) if len(before) > 1 else None, first(after[1:]) if len(after) > 1 else None)

    def uses(self, k, want, ins):
        """The x86_64 instructions that use want the way a recipe of kind k reads it."""
        X, out = self.X, []
        for idx, i in enumerate(ins):
            branch = i.mnemonic == "call" or i.mnemonic.startswith("j")  # only through memory: a vtable slot
            for op in i.operands:
                if branch and op.type != X.X86_OP_MEM:
                    continue
                if op.type == X.X86_OP_MEM:
                    m = op.mem
                    if m.base and m.base not in self.stack and m.base != X.X86_REG_RIP and m.disp == want and k != "tb":
                        out.append(i)
                    if k in ("movz", "mov") and m.index and m.scale == want:  # a size the code scales by
                        out.append(i)
                elif op.type == X.X86_OP_IMM:
                    v = op.imm & 0xFFFFFFFFFFFFFFFF
                    if k == "tb":
                        if (v == 1 << want or v == 1 << (want % 8) and i.operands[0].type == X.X86_OP_MEM and i.operands[0].size == 1
                                or i.mnemonic.startswith("bt") and v == want):
                            out.append(i)
                    elif v == want and not (i.operands[0].type == X.X86_OP_REG and i.operands[0].reg in self.stack):
                        out.append(i)
                    elif k in ("movz", "mov") and i.mnemonic in ("shl", "sal") and want == 1 << v:
                        out.append(i)
            if k in ("movz", "mov") and i.mnemonic == "lea":  # x * 12 as lea t, [x + x*2] then [.. + t*4]
                m = i.operands[1].mem
                if m.base and m.base == m.index and m.scale in (2, 4, 8) and not m.disp:
                    t = i.operands[0].reg
                    for j in ins[idx + 1:idx + 8]:
                        if any(o.type == X.X86_OP_MEM and o.mem.index == t and (m.scale + 1) * o.mem.scale == want for o in j.operands):
                            out.append(i)
                            break
        return out

    def virtual_call(self, va):
        """The arm64 instruction at va loads a register that a blr / br a few instructions later jumps through."""
        w = self.arm.u32(va)
        d = decode(w)
        if not d or d[0] != "ldr":
            return False
        rt = w & 31
        return any(self.arm.u32(va + 4 * j) & 0xFFDFFFFF == 0xD61F0000 | rt << 5 for j in range(1, 6))  # blr / br rt

    def same_fields(self, fn, ins):
        """Every field offset the function's arm64 code loads or stores (>= 0x10, not on the stack; at least two) is a
        displacement in its x86_64 code too: no member it touches is laid out differently."""
        arm = set()
        for va, w in self.arm.insns(fn):
            d = decode(w)
            if d and d[0] in ("ldr", "str") and d[1] not in (29, 31) and d[2] >= 0x10:
                arm.add(d[2])
        x86 = {op.mem.disp for i in ins for op in i.operands if op.type == self.X.X86_OP_MEM and op.mem.base
               and op.mem.base not in self.stack and op.mem.base != self.X.X86_REG_RIP and op.mem.disp >= 0x10}
        return len(arm) >= 2 and arm <= x86

    def check(self, r, value, va):
        """(confirmed, note) for the recipe r whose arm64 value is value (read at va)."""
        want = value + (r["x86"] - r["expected"] if r["x86"] is not None else 0)
        k = r["kind"].split()[0].split(":")[0]
        if k == "vt":
            fn, method = self.xb.sym(r["fn"]), self.xb.sym(r["kind"][3:])
            if fn is None or method is None:
                return False, "no x86_64 symbol for the vtable or the method", want
            nth = r["nth"]
            for i in range(1024):
                if struct.unpack_from("<Q", self.xb.data, self.xb.fo(fn + 0x10 + 8 * i))[0] == method:
                    if nth == 0:
                        return 8 * i == want, "vtable slot %#x" % (8 * i), want
                    nth -= 1
            return False, "method not in the x86_64 vtable", want
        fn, xfn = self.arm.sym(r["fn"]), self.xb.sym(r["fn"])
        if xfn is None:
            return False, "no x86_64 symbol %s" % r["fn"], want
        ins = self.dis(xfn)
        lo, hi = self.window(fn, xfn, va, ins)
        near = [i for i in self.uses(k, want, ins) if (lo is None or i.address >= lo) and (hi is None or i.address <= hi)]
        if near:
            return True, "%s %s" % (near[0].mnemonic, near[0].op_str), want
        anywhere = self.uses(k, want, ins)
        if anywhere and want >= 0x100:  # big enough not to match by coincidence
            return True, "%s %s (elsewhere in the function)" % (anywhere[0].mnemonic, anywhere[0].op_str), want
        calls = [i for i in anywhere if i.mnemonic == "call" or i.mnemonic.startswith("j")]
        if calls and self.virtual_call(va):  # a vtable slot: the same in both slices (the code around may move)
            return True, "%s %s (a virtual call, elsewhere in the function)" % (calls[0].mnemonic, calls[0].op_str), want
        if anywhere and self.same_fields(fn, ins):
            return True, "%s %s (elsewhere in the function, which uses the same field offsets in both slices)" % (
                anywhere[0].mnemonic, anywhere[0].op_str), want
        hints = sorted({op.mem.disp for i in ins if (lo is None or i.address >= lo) and (hi is None or i.address <= hi)
                        for op in i.operands if op.type == self.X.X86_OP_MEM and op.mem.base not in self.stack
                        and op.mem.base != self.X.X86_REG_RIP and want < op.mem.disp <= want + 0x40})
        return False, "%#x not used near the same calls%s" % (
            want, "; displacements just above it there: " + ", ".join(map(hex, hints)) if hints else ""), want


def check(path, x86=False, root=HERE):
    data = open(path, "rb").read()
    b = Binary(data)
    xc = X86Check(b, data) if x86 else None
    tested = b.uuid == TESTED_BUILD
    bad, unconfirmed, total = 0, [], 0
    for r in recipes(root):
        if r["kind"].startswith("win:"):  # a Windows table field (#ifdef _WIN32): tools/win checks those
            print("win   %s %s %#x (%s)" % (r["where"], r["name"], r["expected"], r["kind"]))
            continue
        total += 1
        fn = b.sym(r["fn"])
        got, va = run_at(b, fn, r["kind"], r["nth"]) if fn is not None else (None, None)
        ok = got == r["expected"]
        state = "ok" if ok else "FAIL" if got is None or tested else "moved"
        bad += state == "FAIL"
        line = "%-5s %s %s %#x = %s%s" % (state, r["where"], r["name"], r["expected"],
                                          hex(got) if got is not None else None, "" if fn is not None else " (no symbol %s)" % r["fn"])
        if xc and got is not None:
            confirmed, note, want = xc.check(r, got, va)
            line += "   x86_64 %#x: %s" % (want, note if confirmed else "NEEDS X86{} - " + note)
            if not confirmed:
                unconfirmed.append(r)
        print(line)
    print("%d recipes, %d fail in the arm64 slice%s" % (total, bad, ", %d not confirmed in the x86_64 slice" % len(unconfirmed) if x86 else ""))
    if not tested:
        print("(not the tested build: \"moved\" is a field at another offset than 7.2.19's, which the mods follow)")
    for r in unconfirmed:
        print("  needs X86{}: %s %s {%#x, %s, \"%s\", %d}" % (r["where"], r["name"], r["expected"], r["fn"][:60], r["kind"], r["nth"]))
    sys.exit(1 if bad or unconfirmed else 0)


if __name__ == "__main__":
    if sys.argv[1] == "find":
        find(Binary(open(sys.argv[2], "rb").read()), sys.argv[3], int(sys.argv[4], 0))
    else:
        args = sys.argv[3:]
        root = args[args.index("--root") + 1] if "--root" in args else HERE
        check(sys.argv[2], "--x86" in args, root)
