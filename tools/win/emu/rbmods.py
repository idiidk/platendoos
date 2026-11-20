"""rbmods.dll in the emulator: rekordbox.exe and the DLL mapped, the DLL's C runtime and static initialisers run (so
core/image_win.cpp reads the real address table and every rbm::Addr / rbm::Offset resolves through it, as in
rekordbox), with the log captured. DllMain itself (hook installation, the mods' threads) is not run: a test calls
the mod's own functions (its init, its replacements) directly.

Stand-ins at the platform layer: rbm::logf (formatted in Python, into .log) and rbm::plat::readFile (reads the
host file: rbmods/rekordbox-<build>.tbl from the table directory).
"""
import os
import struct

from emu import Emu, EmuError
import msvc

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..", ".."))
DEFAULT_PKG = os.path.join(ROOT, "build", "win", "platendoos")


def build_id(exe):
    import pefile
    pe = pefile.PE(exe, fast_load=True)
    return "%08X-%X" % (pe.FILE_HEADER.TimeDateStamp, pe.OPTIONAL_HEADER.SizeOfImage)


class Rbmods:
    """rekordbox.exe + rbmods.dll, initialised. r.emu, r.exe, r.dll; r.log (lines); r.addr(...), r.offset(...)."""

    def __init__(self, exe_path, dll_path=None, pdb_path=None, tables=None, verbose=False, unknown="zero"):
        dll_path = dll_path or os.path.join(DEFAULT_PKG, "rbmods.dll")
        pdb_path = pdb_path or os.path.splitext(dll_path)[0] + ".pdb"
        self.tables = tables or os.path.join(os.path.dirname(dll_path), "rbmods")
        self.verbose = verbose
        self.log = []
        self.files_read = []
        self.emu = e = Emu(unknown=unknown, verbose=verbose, cache_dir=os.path.join(ROOT, "build", "win", "emu-cache"))
        self.exe = e.load_image(exe_path, "rekordbox.exe")
        self.dll = e.load_image(dll_path, "rbmods.dll", pdb=pdb_path)
        self._load_table_names()
        e.hook(self.dll.sym("rbm::logf"), self._logf, "rbm::logf")
        e.hook(self.dll.sym("rbm::plat::readFile"), self._read_file, "rbm::plat::readFile")
        self._crt_init()

    # -- names for post-mortems: the table's addresses name rekordbox's code --
    def _load_table_names(self):
        p = os.path.join(self.tables, f"rekordbox-{self.exe.build}.tbl")
        self.table_path = p if os.path.exists(p) else None
        names = []
        if self.table_path:
            for line in open(p, encoding="utf-8", errors="replace"):
                f = line.split()
                if len(f) >= 3 and f[0] == "addr" and "+" not in f[1]:
                    names.append((self.exe.base + int(f[-1], 16), line[5:line.rfind(" ")]))
        self.exe.names = sorted(names)

    # -- platform stand-ins --
    def _logf(self, e):
        fmt = e.cstr(e.arg(0))
        line = e.format_printf(fmt, 1)
        self.log.append(line)
        if self.verbose:
            print("    [log]", line)
        return None

    def _read_file(self, e):
        path = msvc.read_string(e, e.arg(0))
        name = path.replace("\\", "/").rsplit("/", 1)[-1]
        host = os.path.join(self.tables, name)
        self.files_read.append(path)
        if not name.endswith(".tbl") or not os.path.exists(host):
            return 0
        with open(host, "rb") as f:
            msvc.write_string(e, e.arg(1), f.read())
        return 1

    # -- the DLL's C runtime and static initialisers (as dllmain_crt_process_attach, without DllMain) --
    def _crt_init(self):
        e, d = self.emu, self.dll
        if not e.call(d.sym("__scrt_initialize_crt"), 1) & 0xFF:
            raise EmuError("__scrt_initialize_crt failed")
        if not e.call(d.sym("__scrt_dllmain_before_initialize_c")) & 0xFF:
            raise EmuError("__scrt_dllmain_before_initialize_c failed")
        if e.call(d.sym("_initterm_e"), d.sym("__xi_a"), d.sym("__xi_z")) & 0xFFFFFFFF:
            raise EmuError("_initterm_e (C initialisers) failed")
        self.init_failures = []
        a, z = d.sym("__xc_a"), d.sym("__xc_z")
        for p in range(a, z, 8):   # one at a time: a failing initialiser is named, the rest still run
            fn = e.r64(p)
            if not fn:
                continue
            try:
                e.call(fn)
            except EmuError as ex:
                self.init_failures.append((e.symbolize(fn), str(ex)))
                if self.verbose:
                    print(f"    [init] {e.symbolize(fn)} failed: {ex}")

    # -- reading the mods' statics --
    def sym(self, name, module=None):
        return self.dll.sym(name, module)

    def addr(self, name, module):
        """The resolved value of an rbm::Addr static (Addr.value: unslid address, 0 if the table lacks it)."""
        return self.emu.r64(self.sym(name, module) + 32)

    def offset(self, name, module):
        """(value, ok) of an rbm::Offset static."""
        a = self.sym(name, module)
        return self.emu.r64(a + 40), bool(self.emu.r8(a + 48))

    def table_value(self, kind, key):
        """A line's value straight from the table file (for checks independent of the DLL's parser)."""
        for line in open(self.table_path, encoding="utf-8", errors="replace"):
            if line.startswith(kind + " ") and line[len(kind) + 1:line.rfind(" ")] == key:
                v = line.rsplit(" ", 1)[1].strip()
                return int(v, 0) if kind == "adj" else int(v, 16)
        return None
