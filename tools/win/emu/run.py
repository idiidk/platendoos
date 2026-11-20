#!/usr/bin/env python3
"""Runs the emulator tests (test_*.py here) against each rekordbox.exe build that has an address table.

    python3 tools/win/emu/run.py                      # every test, every rekordbox.exe found (see below)
    python3 tools/win/emu/run.py xfadelock -v         # one test, with the mod's log and unknown imports
    python3 tools/win/emu/run.py --exe path/to/rekordbox.exe [--exe ...] [--dll rbmods.dll] [--tables dir]

rekordbox.exe: --exe, else $RBEMU_EXE (paths separated by ':'), else every rekordbox.exe up to three folders deep
under $RBWIN (default ~/rbwin) whose build has a table in the tables folder (default: the built package's
build/win/platendoos/rbmods, i.e. what ships). One exe per build.

A test module defines run(t): t.r is an initialised rbmods.Rbmods (t.r.emu, t.r.exe, t.r.dll), t.check(cond, what)
records a result. Exit status 1 when anything failed.
"""
import argparse
import glob
import importlib
import os
import sys
import time
import traceback

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import rbmods  # noqa: E402


class T:
    def __init__(self, r, verbose):
        self.r, self.verbose = r, verbose
        self.passed, self.failed = 0, []

    def check(self, cond, what, detail=""):
        if cond:
            self.passed += 1
            if self.verbose:
                print(f"    ok   {what}")
        else:
            self.failed.append(what + (f": {detail}" if detail else ""))
            print(f"    FAIL {what}" + (f": {detail}" if detail else ""))
        return bool(cond)

    def eq(self, got, want, what):
        return self.check(got == want, what, f"got {got!r}, want {want!r}")

    def near(self, got, want, what, tol=1e-6):
        return self.check(got is not None and abs(got - want) <= tol, what, f"got {got!r}, want {want!r}")


def find_exes(tables):
    if os.environ.get("RBEMU_EXE"):
        return [p for p in os.environ["RBEMU_EXE"].split(":") if p]
    root = os.environ.get("RBWIN") or os.path.expanduser("~/rbwin")
    have = {os.path.basename(p)[len("rekordbox-"):-len(".tbl")] for p in glob.glob(os.path.join(tables, "rekordbox-*.tbl"))}
    out = {}
    for depth in ("", "*/", "*/*/", "*/*/*/"):
        for p in sorted(glob.glob(os.path.join(root, depth + "rekordbox.exe"))):
            try:
                b = rbmods.build_id(p)
            except Exception:
                continue
            if b in have and b not in out:
                out[b] = p
    return list(out.values())


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("tests", nargs="*", help="test names (test_<name>.py); default all")
    ap.add_argument("--exe", action="append", help="a rekordbox.exe (repeatable)")
    ap.add_argument("--dll", help="rbmods.dll (default build/win/platendoos/rbmods.dll; its .pdb next to it)")
    ap.add_argument("--tables", help="address tables folder (default: rbmods/ next to the DLL)")
    ap.add_argument("-v", "--verbose", action="store_true")
    a = ap.parse_args()
    here = os.path.dirname(os.path.abspath(__file__))
    names = a.tests or sorted(os.path.basename(p)[5:-3] for p in glob.glob(os.path.join(here, "test_*.py")))
    dll = a.dll or os.path.join(rbmods.DEFAULT_PKG, "rbmods.dll")
    tables = a.tables or os.path.join(os.path.dirname(dll), "rbmods")
    exes = a.exe or find_exes(tables)
    if not exes:
        print("no rekordbox.exe found: pass --exe or set RBWIN / RBEMU_EXE")
        return 2
    total_fail = 0
    for exe in exes:
        build = rbmods.build_id(exe)
        for name in names:
            mod = importlib.import_module("test_" + name)
            t0 = time.time()
            print(f"== {name} on rekordbox.exe {build}")
            try:
                r = rbmods.Rbmods(exe, dll, tables=tables, verbose=a.verbose)
                t = T(r, a.verbose)
                if r.init_failures:
                    for fn, why in r.init_failures:
                        t.check(False, f"static initialiser {fn}", why)
                mod.run(t)
                if a.verbose and r.emu.unknown_called:
                    print("    unknown imports called:", r.emu.unknown_called)
            except Exception as ex:
                t = locals().get("t") or T(None, a.verbose)
                t.check(False, "test raised", f"{type(ex).__name__}: {ex}")
                if a.verbose:
                    traceback.print_exc()
            verdict = "PASS" if not t.failed else "FAIL"
            print(f"   {verdict}: {t.passed} passed, {len(t.failed)} failed ({time.time() - t0:.1f} s)")
            total_fail += len(t.failed)
    return 1 if total_fail else 0


if __name__ == "__main__":
    sys.exit(main())
