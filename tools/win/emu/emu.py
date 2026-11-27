"""A small x86-64 Windows user-mode emulator on Unicorn, for testing Windows mod code on a Mac.

Maps PE images (rekordbox.exe, rbmods.dll) at their preferred bases, resolves their imports to stubs (Python
handlers for the Win32 calls the code under test needs, a recording "returns 0" stub for the rest), sets up a TEB
(gs), PEB, TLS and a stack, and lets Python call any function with the Windows x64 calling convention, read and
write memory, build fake objects and replace functions with Python callbacks.

    emu = Emu()
    exe = emu.load_image("rekordbox.exe")
    dll = emu.load_image("rbmods.dll", pdb="rbmods.pdb")
    emu.hook(exe.base + 0x1234, lambda e: 7)             # replace a function: returns 7
    r = emu.call(dll.sym("rbm::thisDelta"), some_ptr)     # rcx = some_ptr, result rax
    x = emu.call(f, 0, F32(0.25), ret="f32")              # a float argument in xmm1, float result

Python callbacks get the Emu; emu.arg(i) / emu.farg(i) read argument i at the call (as the callee sees it), and
the callback's return value goes to rax (int) or xmm0 (F32 / F64 / float). A callback may call back into emulated
code with emu.call (the outer state is saved and restored).
"""
import os
import re
import struct
import subprocess
import json
import hashlib
import shutil

import pefile
import capstone
from unicorn import Uc, UcError, UC_ARCH_X86, UC_MODE_64, UC_PROT_ALL, UC_HOOK_CODE, UC_HOOK_MEM_READ, \
    UC_HOOK_MEM_WRITE, UC_HOOK_MEM_UNMAPPED, UC_HOOK_MEM_FETCH_UNMAPPED, UC_HOOK_MEM_READ_UNMAPPED, \
    UC_HOOK_MEM_WRITE_UNMAPPED
from unicorn import x86_const as X


class EmuError(Exception):
    pass


class F32(float):
    """A float argument/return value (single precision, xmm register)."""


class F64(float):
    """A double argument/return value (xmm register). A plain Python float means the same."""


PAGE = 0x1000
STUB_BASE, STUB_SIZE = 0x7F000000, 0x100000      # import stubs and Python functions (int3 bytes)
SENTINEL = STUB_BASE + STUB_SIZE - 0x10            # return address of emu.call
THREAD_END = SENTINEL - 0xF8                       # where an emulated thread's run stops (it yielded or ended)
THREAD_STACK = 0x100000
STACK_BASE, STACK_SIZE = 0x10000000, 0x1000000     # 16 MB
TEB_BASE = 0x30000000                              # TEB, PEB, TLS slots, process parameters
HEAP_BASE, HEAP_CHUNK = 0x200000000, 0x4000000     # bump heap, mapped 64 MB at a time
HEAP_HANDLE = 0x5EA0000

REGS = [X.UC_X86_REG_RCX, X.UC_X86_REG_RDX, X.UC_X86_REG_R8, X.UC_X86_REG_R9]
XMMS = [X.UC_X86_REG_XMM0, X.UC_X86_REG_XMM1, X.UC_X86_REG_XMM2, X.UC_X86_REG_XMM3]


def align(v, a):
    return (v + a - 1) & ~(a - 1)


# ---- PDB symbols (llvm-pdbutil) ---------------------------------------------------------------------------------

def _pdbutil():
    p = os.environ.get("LLVM_PDBUTIL") or shutil.which("llvm-pdbutil")
    if p:
        return p
    for prefix in ("/opt/homebrew/opt/llvm/bin", "/usr/local/opt/llvm/bin"):
        if os.path.exists(prefix + "/llvm-pdbutil"):
            return prefix + "/llvm-pdbutil"
    raise EmuError("llvm-pdbutil not found (brew install llvm, or set LLVM_PDBUTIL)")


class Pdb:
    """Function and data symbols of a PDB: per module (object file) and global, as RVAs."""

    _rx_sym = re.compile(r"\| S_(?:[LG]PROC32|[LG]DATA32|PUB32|LTHREAD32|GTHREAD32) \[size = \d+\] `(.*)`$")
    _rx_addr = re.compile(r"addr = (\d+):(\d+)")

    def __init__(self, path, section_rvas, cache_dir=None):
        st = os.stat(path)
        key = hashlib.sha1(f"{os.path.abspath(path)}|{st.st_size}|{st.st_mtime_ns}".encode()).hexdigest()[:16]
        cache = os.path.join(cache_dir, f"pdb-{key}.json") if cache_dir else None
        if cache and os.path.exists(cache):
            with open(cache) as f:
                d = json.load(f)
        else:
            d = self._parse(path, section_rvas)
            if cache:
                os.makedirs(cache_dir, exist_ok=True)
                with open(cache, "w") as f:
                    json.dump(d, f)
        self.modules = d["modules"]   # module basename -> {name: rva}
        self.globals = d["globals"]   # name -> rva (the first seen)
        self.procs = sorted((rva, name) for name, rva in d["procs"].items())

    @classmethod
    def _parse(cls, path, section_rvas):
        out = subprocess.run([_pdbutil(), "dump", "-symbols", "-publics", path], capture_output=True, text=True,
                             check=True).stdout
        modules, globals_, procs = {}, {}, {}
        mod, pending = None, None
        for line in out.splitlines():
            if line.lstrip().startswith("Mod ") and "|" in line:
                m = re.search(r"`([^`]*)`", line)
                mod = os.path.basename(m.group(1).replace("\\", "/")) if m else None
                modules.setdefault(mod, {})
                continue
            if line.startswith("Public Symbols"):
                mod = None
            m = cls._rx_sym.search(line)
            if m:
                pending = (m.group(1), "PROC32" in line)
                continue
            if pending:
                a = cls._rx_addr.search(line)
                if a:
                    sec, off = int(a.group(1)), int(a.group(2))
                    if 1 <= sec <= len(section_rvas):
                        rva = section_rvas[sec - 1] + off
                        name, is_proc = pending
                        if mod:
                            modules[mod].setdefault(name, rva)
                        globals_.setdefault(name, rva)
                        if is_proc:
                            procs.setdefault(name if not mod else f"{name} [{mod}]", rva)
                    pending = None
        return {"modules": modules, "globals": globals_, "procs": procs}

    def module(self, hint):
        """The module whose object file name ends with hint (e.g. "xfadelock.cpp" -> mods_xfadelock.cpp.obj)."""
        hits = [m for m in self.modules if m and (m == hint or m.endswith("_" + hint + ".obj") or m == hint + ".obj")]
        if len(hits) != 1:
            raise EmuError(f"module {hint!r}: {len(hits)} matches {hits[:5]}")
        return hits[0]

    def rva(self, name, module=None):
        if module:
            syms = self.modules[self.module(module)]
            if name in syms:
                return syms[name]
            raise EmuError(f"no symbol {name!r} in module {module}")
        if name in self.globals:
            return self.globals[name]
        raise EmuError(f"no symbol {name!r}")


# ---- images --------------------------------------------------------------------------------------------------------

class Image:
    def __init__(self, emu, path, name):
        self.emu, self.path, self.name = emu, path, name
        self.pe = pefile.PE(path, fast_load=True)
        self.pe.parse_data_directories(directories=[
            pefile.DIRECTORY_ENTRY["IMAGE_DIRECTORY_ENTRY_IMPORT"],
            pefile.DIRECTORY_ENTRY["IMAGE_DIRECTORY_ENTRY_EXPORT"],
            pefile.DIRECTORY_ENTRY["IMAGE_DIRECTORY_ENTRY_TLS"]])
        oh = self.pe.OPTIONAL_HEADER
        self.base, self.size = oh.ImageBase, align(oh.SizeOfImage, PAGE)
        self.build = "%08X-%X" % (self.pe.FILE_HEADER.TimeDateStamp, oh.SizeOfImage)
        self.sections = {s.Name.rstrip(b"\0").decode(errors="replace"): (self.base + s.VirtualAddress,
                         self.base + s.VirtualAddress + max(s.Misc_VirtualSize, s.SizeOfRawData)) for s in self.pe.sections}
        self.section_rvas = [s.VirtualAddress for s in self.pe.sections]
        self.exports = {}
        if hasattr(self.pe, "DIRECTORY_ENTRY_EXPORT"):
            for e in self.pe.DIRECTORY_ENTRY_EXPORT.symbols:
                if e.name:
                    self.exports[e.name.decode()] = self.base + e.address
        self.pdb = None
        self.names = []  # sorted (addr, name): extra names for symbolize (e.g. from an address table)

    def contains(self, a):
        return self.base <= a < self.base + self.size

    def sym(self, name, module=None):
        """A symbol's address: export, or PDB symbol (optionally of one module, e.g. "xfadelock.cpp")."""
        if not module and name in self.exports:
            return self.exports[name]
        if not self.pdb:
            raise EmuError(f"{self.name}: no PDB for {name!r}")
        return self.base + self.pdb.rva(name, module)

    def section(self, name):
        return self.sections[name]


# ---- the emulator --------------------------------------------------------------------------------------------------

class Emu:
    def __init__(self, unknown="zero", verbose=False, cache_dir=None):
        """unknown: what an import without a handler does when called: "zero" (record it, return 0) or "trap"."""
        self.uc = Uc(UC_ARCH_X86, UC_MODE_64)
        self.unknown, self.verbose = unknown, verbose
        self.cache_dir = cache_dir
        self.images = []
        self.stubs = {}            # stub address -> (name, handler or None)
        self.hooks = {}            # address -> (name, handler): replaced functions
        self.stub_next = STUB_BASE
        self.api = dict(WINAPI)    # import name -> handler (overridable per Emu)
        self.calls = []            # (name, [rcx, rdx, r8, r9]) of every stub/hook call, for tests and post-mortems
        self.unknown_called = {}   # import name -> count
        self.heap_next = HEAP_BASE
        self.heap_end = HEAP_BASE
        self.sizes = {}
        self.last_error = 0
        self.tls = {}
        self.tls_next = 1
        self.handles = 0x1000
        self.windows = {}          # fake top-level/child windows: hwnd -> dict(title, rect, parent, visible)
        self.dpi = 96
        self.window_classes = {}   # class name -> window procedure (RegisterClassW)
        self._exc = None
        self._depth = 0
        self.threads = {}          # handle -> Thread (enable_threads)
        self.current = None        # the Thread running now (None: the main thread)
        self.events = {}           # handle -> {"manual": bool, "set": bool} (CreateEventW, with enable_threads)
        self.next_tid = 2
        self.uc.mem_map(STUB_BASE, STUB_SIZE, UC_PROT_ALL)
        self.uc.mem_write(STUB_BASE, b"\xCC" * STUB_SIZE)
        self.uc.mem_map(STACK_BASE, STACK_SIZE, UC_PROT_ALL)
        self.uc.mem_map(TEB_BASE, 0x100000, UC_PROT_ALL)
        self.uc.hook_add(UC_HOOK_CODE, self._on_stub, begin=STUB_BASE, end=STUB_BASE + STUB_SIZE - 1)
        self.uc.hook_add(UC_HOOK_MEM_UNMAPPED, self._on_unmapped)
        self._setup_teb()

    # -- process structures --

    def _setup_teb(self):
        teb, peb = TEB_BASE, TEB_BASE + 0x2000
        self.teb, self.peb = teb, peb
        self.tls_array = TEB_BASE + 0x4000       # ThreadLocalStoragePointer -> one slot per TLS image
        self.w64(teb + 0x08, STACK_BASE + STACK_SIZE)  # NT_TIB.StackBase
        self.w64(teb + 0x10, STACK_BASE)              # StackLimit
        self.w64(teb + 0x30, teb)                     # Self
        self.w64(teb + 0x40, 1234)                    # ClientId.UniqueProcess
        self.w64(teb + 0x48, 1)                       # UniqueThread
        self.w64(teb + 0x58, self.tls_array)
        self.w64(teb + 0x60, peb)
        self.w64(peb + 0x30, HEAP_HANDLE)             # ProcessHeap
        self.uc.msr_write(0xC0000101, teb)            # IA32_GS_BASE
        self.cmdline_w = TEB_BASE + 0x6000
        self.cmdline_a = TEB_BASE + 0x6400
        self.env_w = TEB_BASE + 0x6800
        self.mem_write(self.cmdline_w, "rekordbox.exe\0".encode("utf-16-le"))
        self.mem_write(self.cmdline_a, b"rekordbox.exe\0")
        self.mem_write(self.env_w, b"\0\0\0\0")

    def load_image(self, path, name=None, pdb=None):
        img = Image(self, path, name or os.path.basename(path))
        for o in self.images:
            if img.base < o.base + o.size and o.base < img.base + img.size:
                raise EmuError(f"{img.name} overlaps {o.name} (relocation is not implemented)")
        self.uc.mem_map(img.base, img.size, UC_PROT_ALL)
        self.uc.mem_write(img.base, img.pe.get_memory_mapped_image()[:img.size])
        if not self.images:
            self.w64(self.peb + 0x10, img.base)       # ImageBaseAddress: the first image is the process
        self.images.append(img)
        self._resolve_imports(img)
        self._setup_tls(img)
        if pdb:
            img.pdb = Pdb(pdb, img.section_rvas, self.cache_dir)
        return img

    def _resolve_imports(self, img):
        if not hasattr(img.pe, "DIRECTORY_ENTRY_IMPORT"):
            return
        for d in img.pe.DIRECTORY_ENTRY_IMPORT:
            dll = d.dll.decode()
            for i in d.imports:
                name = i.name.decode() if i.name else f"{dll}#{i.ordinal}"
                self.w64(i.address, self.stub(name, self.api.get(name)))

    def _setup_tls(self, img):
        if not hasattr(img.pe, "DIRECTORY_ENTRY_TLS"):
            return
        t = img.pe.DIRECTORY_ENTRY_TLS.struct
        n = sum(1 for o in self.images if o is not img and getattr(o, "tls_slot", None) is not None)
        data = self.mem_read(t.StartAddressOfRawData, t.EndAddressOfRawData - t.StartAddressOfRawData)
        block = self.alloc(len(data) + t.SizeOfZeroFill + 16)
        self.mem_write(block, data)
        img.tls_slot = n
        self.w32(t.AddressOfIndex, n)
        self.w64(self.tls_array + 8 * n, block)

    # -- stubs and hooks --

    def stub(self, name, handler=None):
        """A callable address that runs handler(emu) (None: the unknown-import behaviour) and returns."""
        a = self.stub_next
        self.stub_next += 16
        if self.stub_next >= SENTINEL:
            raise EmuError("out of stub space")
        self.stubs[a] = (name, handler)
        return a

    def py_func(self, fn, name="py"):
        """A function pointer for emulated code (a fake vtable slot, a callback) that runs fn(emu) in Python."""
        return self.stub(name, fn)

    def hook(self, addr, fn, name=None):
        """Replace the function at addr (in an image) by fn(emu): its result returns to the caller."""
        name = name or self.symbolize(addr)
        self.hooks[addr] = (name, fn)
        self.uc.hook_add(UC_HOOK_CODE, self._on_hook, begin=addr, end=addr)
        self.uc.ctl_flush_tb()   # (a function already translated would otherwise run unhooked)

    def set_api(self, name, handler):
        """Replace the handler of an import by name (for every image's stub of it)."""
        self.api[name] = handler
        for a, (n, _) in list(self.stubs.items()):
            if n == name:
                self.stubs[a] = (n, handler)

    def _args4(self):
        return [self.uc.reg_read(r) for r in REGS]

    def _run_py(self, name, handler):
        self.calls.append((name, self._args4()))
        if handler is None:
            self.unknown_called[name] = self.unknown_called.get(name, 0) + 1
            if self.verbose:
                print(f"    [emu] unknown import {name}{tuple(hex(a) for a in self._args4())} -> 0")
            if self.unknown == "trap":
                raise EmuError(f"call of unknown import {name}")
            return 0
        return handler(self)

    def _return(self, value):
        if isinstance(value, F32):
            self.uc.reg_write(X.UC_X86_REG_XMM0, struct.unpack("<I", struct.pack("<f", value))[0])
        elif isinstance(value, float):
            self.uc.reg_write(X.UC_X86_REG_XMM0, struct.unpack("<Q", struct.pack("<d", value))[0])
        elif value is not None:
            self.uc.reg_write(X.UC_X86_REG_RAX, int(value) & 0xFFFFFFFFFFFFFFFF)
        rsp = self.uc.reg_read(X.UC_X86_REG_RSP)
        self.uc.reg_write(X.UC_X86_REG_RSP, rsp + 8)
        self.uc.reg_write(X.UC_X86_REG_RIP, self.r64(rsp))

    def _dispatch(self, name, handler):
        try:
            v = self._run_py(name, handler)
            if v is _YIELD:     # an emulated thread waits: it continues after this call when it is resumed
                self._return(None)
                th = self.current
                th.ctx = self.uc.context_save()
                th.yielded = True
                self.uc.reg_write(X.UC_X86_REG_RIP, THREAD_END)
            elif v is _EXIT:    # ExitThread
                self.uc.reg_write(X.UC_X86_REG_RIP, THREAD_END)
            else:
                self._return(v)
        except BaseException as e:   # (unicorn can't carry Python exceptions through emulation: stop and re-raise)
            if self._exc is None:
                self._exc = e
            self.uc.emu_stop()

    def _on_stub(self, uc, addr, size, _):
        if SENTINEL - 0x100 <= addr <= SENTINEL:
            return
        s = self.stubs.get(addr)
        if s is None:
            self._exc = self._exc or EmuError(f"jump into stub space at {addr:#x} (no stub there)")
            uc.emu_stop()
            return
        self._dispatch(*s)

    def _on_hook(self, uc, addr, size, _):
        h = self.hooks.get(addr)
        if h:
            self._dispatch(*h)

    def _on_unmapped(self, uc, access, addr, size, value, _):
        kind = {UC_HOOK_MEM_READ_UNMAPPED: "read", UC_HOOK_MEM_WRITE_UNMAPPED: "write",
                UC_HOOK_MEM_FETCH_UNMAPPED: "fetch"}.get(access, f"access {access}")
        rip = uc.reg_read(X.UC_X86_REG_RIP)
        self._exc = self._exc or EmuError(f"unmapped {kind} of {addr:#x} at {self.where(rip)}")
        return False

    # -- calling --

    def arg(self, i):
        """Integer argument i (0-based) of the call being handled (at the callee's entry)."""
        if i < 4:
            return self.uc.reg_read(REGS[i])
        return self.r64(self.uc.reg_read(X.UC_X86_REG_RSP) + 8 + 8 * i)

    def farg(self, i):
        """Double argument i (in xmm i for i < 4)."""
        if i < 4:
            return struct.unpack("<d", struct.pack("<Q", self.uc.reg_read(XMMS[i]) & (2 ** 64 - 1)))[0]
        return self.f64(self.uc.reg_read(X.UC_X86_REG_RSP) + 8 + 8 * i)

    def farg32(self, i):
        """Float (single) argument i."""
        if i < 4:
            return struct.unpack("<f", struct.pack("<I", self.uc.reg_read(XMMS[i]) & 0xFFFFFFFF))[0]
        return self.f32(self.uc.reg_read(X.UC_X86_REG_RSP) + 8 + 8 * i)

    def call(self, addr, *args, ret="int", timeout=120):
        """Calls addr with args (int; F32/F64/float into xmm) under the Windows x64 convention; returns rax (ret="int"),
        xmm0 as "f32"/"f64", or None. A call from inside a Python hook saves and restores the outer state."""
        nested = self._depth > 0
        ctx = self.uc.context_save() if nested else None
        if nested:
            top = self.uc.reg_read(X.UC_X86_REG_RSP) - 0x800
        else:
            top = STACK_BASE + STACK_SIZE - 0x1000
        nstack = max(0, len(args) - 4)
        rsp = (top - 0x28 - 8 * nstack) & ~0xF
        rsp -= 8                                   # at entry rsp = 16n + 8: the return address
        sentinel = SENTINEL - 0x10 * self._depth
        self.w64(rsp, sentinel)
        for i, a in enumerate(args):
            if isinstance(a, float):
                bits = struct.unpack("<I", struct.pack("<f", a))[0] if isinstance(a, F32) else \
                    struct.unpack("<Q", struct.pack("<d", a))[0]
                if i < 4:
                    self.uc.reg_write(XMMS[i], bits)
                else:
                    self.w64(rsp + 8 + 8 * i, bits)
            else:
                v = int(a) & 0xFFFFFFFFFFFFFFFF
                if i < 4:
                    self.uc.reg_write(REGS[i], v)
                else:
                    self.w64(rsp + 8 + 8 * i, v)
        self.uc.reg_write(X.UC_X86_REG_RSP, rsp)
        self._depth += 1
        try:
            # (only the outermost call has a timeout: unicorn's timeout thread breaks a nested emu_start)
            self.uc.emu_start(addr, sentinel, timeout=0 if nested else int(timeout * 1e6))
        except UcError as e:
            rip = self.uc.reg_read(X.UC_X86_REG_RIP)
            exc, self._exc = self._exc, None
            if exc:
                raise exc
            raise EmuError(f"{e} at {self.where(rip)} (calling {self.symbolize(addr)})")
        finally:
            self._depth -= 1
        if self._exc:
            exc, self._exc = self._exc, None
            raise exc
        rip = self.uc.reg_read(X.UC_X86_REG_RIP)
        if rip != sentinel:
            raise EmuError(f"{self.symbolize(addr)} did not return (timeout {timeout}s?) at {self.where(rip)}")
        if ret == "int":
            r = self.uc.reg_read(X.UC_X86_REG_RAX)
        elif ret == "f64":
            r = struct.unpack("<d", struct.pack("<Q", self.uc.reg_read(X.UC_X86_REG_XMM0) & (2 ** 64 - 1)))[0]
        elif ret == "f32":
            r = struct.unpack("<f", struct.pack("<I", self.uc.reg_read(X.UC_X86_REG_XMM0) & 0xFFFFFFFF))[0]
        else:
            r = None
        if nested:
            self.uc.context_restore(ctx)
        return r

    # -- memory --

    def mem_read(self, a, n):
        return bytes(self.uc.mem_read(a, n))

    def mem_write(self, a, b):
        self.uc.mem_write(a, bytes(b))

    def r8(self, a): return self.mem_read(a, 1)[0]
    def r32(self, a): return struct.unpack("<I", self.mem_read(a, 4))[0]
    def r64(self, a): return struct.unpack("<Q", self.mem_read(a, 8))[0]
    def f32(self, a): return struct.unpack("<f", self.mem_read(a, 4))[0]
    def f64(self, a): return struct.unpack("<d", self.mem_read(a, 8))[0]
    def w8(self, a, v): self.mem_write(a, bytes([v & 0xFF]))
    def w32(self, a, v): self.mem_write(a, struct.pack("<I", v & 0xFFFFFFFF))
    def w64(self, a, v): self.mem_write(a, struct.pack("<Q", v & 0xFFFFFFFFFFFFFFFF))
    def wf32(self, a, v): self.mem_write(a, struct.pack("<f", v))
    def wf64(self, a, v): self.mem_write(a, struct.pack("<d", v))

    def cstr(self, a, n=4096):
        out = bytearray()
        while len(out) < n:
            chunk = self.mem_read(a + len(out), 64)
            z = chunk.find(b"\0")
            if z >= 0:
                out += chunk[:z]
                break
            out += chunk
        return out.decode("utf-8", errors="replace")

    def wstr(self, a, n=4096):
        out = bytearray()
        while len(out) < 2 * n:
            c = self.mem_read(a + len(out), 2)
            if c == b"\0\0":
                break
            out += c
        return out.decode("utf-16-le", errors="replace")

    def alloc(self, size, align_to=16, data=None):
        """Zeroed heap memory (never freed)."""
        a = align(self.heap_next, max(align_to, 16))
        end = a + max(size, 1)
        while end > self.heap_end:
            self.uc.mem_map(self.heap_end, HEAP_CHUNK, UC_PROT_ALL)
            self.heap_end += HEAP_CHUNK
        self.heap_next = end
        self.sizes[a] = size
        if data:
            self.mem_write(a, data)
        return a

    def alloc_cstr(self, s):
        return self.alloc(len(s.encode()) + 1, data=s.encode() + b"\0")

    def alloc_wstr(self, s):
        b = (s + "\0").encode("utf-16-le")
        return self.alloc(len(b), data=b)

    def vtable(self, slots, rtti=0):
        """A fake vtable: slots are addresses or Python callables (made into py_funcs). Returns its address point;
        rtti (a complete object locator address, or 0) goes in the word before it."""
        t = self.alloc(8 * (len(slots) + 1))
        self.w64(t, rtti)
        for i, s in enumerate(slots):
            self.w64(t + 8 + 8 * i, self.py_func(s, f"vtable slot {i}") if callable(s) else s)
        return t + 8

    class _Trace:
        def __init__(self, emu, lo, hi):
            self.emu, self.lo, self.hi = emu, lo, hi
            self.reads, self.writes = [], []   # (offset, size[, value]) relative to lo

        def __enter__(self):
            def rd(uc, access, addr, size, value, _):
                self.reads.append((addr - self.lo, size))

            def wr(uc, access, addr, size, value, _):
                self.writes.append((addr - self.lo, size, value))
            self.h = [self.emu.uc.hook_add(UC_HOOK_MEM_READ, rd, begin=self.lo, end=self.hi - 1),
                      self.emu.uc.hook_add(UC_HOOK_MEM_WRITE, wr, begin=self.lo, end=self.hi - 1)]
            self.emu.uc.ctl_flush_tb()   # (memory hooks are compiled into translated code: retranslate)
            return self

        def __exit__(self, *a):
            for h in self.h:
                self.emu.uc.hook_del(h)
            self.emu.uc.ctl_flush_tb()

        def read_offsets(self):
            return sorted({o for o, _ in self.reads})

        def write_offsets(self):
            return sorted({o for o, _, _ in self.writes})

    def send_message(self, hwnd, msg, wparam=0, lparam=0):
        """Calls the window procedure of a window made by CreateWindowExW (its class's, from RegisterClassW)."""
        proc = self.window_classes.get(self.windows[hwnd].get("class"))
        if not proc:
            raise EmuError(f"no window procedure for window {hwnd:#x}")
        return self.call(proc, hwnd, msg, wparam, lparam)

    def trace(self, addr, size):
        """with emu.trace(obj, 0x400) as t: ... -> t.reads / t.writes: the offsets the code touched."""
        return self._Trace(self, addr, addr + size)

    # -- threads (opt-in: enable_threads) --
    #
    # Cooperative: a thread runs only when the main thread lets it, on its own stack and TLS slots, until it waits
    # for something not yet signalled (WaitForSingleObject / WaitForMultipleObjects: it yields there) or ends. The
    # main thread's own waits run the threads until what they wait for is signalled; run_threads() runs them at a
    # point the test chooses. A wait's timeout never elapses by itself: run_threads(timeouts=True) lets blocked
    # threads' finite waits time out once.

    def enable_threads(self):
        for n, fn in _THREAD_API.items():
            self.set_api(n, fn)

    def _signalled(self, h):
        if h in self.events:
            return self.events[h]["set"]
        th = self.threads.get(h)
        return th is not None and th.state == "done"

    def _consume(self, h):
        ev = self.events.get(h)
        if ev and not ev["manual"]:
            ev["set"] = False

    def _wait_result(self, handles, wait_all):
        """WAIT_OBJECT_0 + i when the wait is satisfied now (consuming auto-reset events), else None."""
        if wait_all:
            if all(self._signalled(h) for h in handles):
                for h in handles:
                    self._consume(h)
                return 0
            return None
        for i, h in enumerate(handles):
            if self._signalled(h):
                self._consume(h)
                return i
        return None

    def _wait(self, handles, wait_all, timeout):
        r = self._wait_result(handles, wait_all)
        if r is not None:
            return r
        if timeout == 0:
            return 0x102                                     # WAIT_TIMEOUT
        if self.current is not None:                         # a thread: it waits (yields) here
            self.current.wait = (handles, wait_all, timeout)
            return _YIELD
        for _ in range(10000):                               # the main thread: the others run meanwhile
            if not self._step_threads():
                break
            r = self._wait_result(handles, wait_all)
            if r is not None:
                return r
        if timeout == 0xFFFFFFFF:
            raise EmuError(f"the main thread waits forever for {[hex(h) for h in handles]}: no thread will signal it")
        return 0x102

    def _run_thread(self, th, rax=None):
        """Runs th until it yields or ends (from the main thread, at any depth)."""
        outer = self.uc.context_save()
        prev, prev_tls = self.current, self.tls
        self.current, self.tls = th, th.tls
        self.w64(self.teb + 0x48, th.tid)
        self.w64(self.teb + 0x08, th.stack + THREAD_STACK)
        self.w64(self.teb + 0x10, th.stack)
        th.yielded = False
        if th.ctx is None:
            rsp = ((th.stack + THREAD_STACK - 0x100) & ~0xF) - 8
            self.w64(rsp, THREAD_END)
            self.uc.reg_write(X.UC_X86_REG_RSP, rsp)
            self.uc.reg_write(X.UC_X86_REG_RCX, th.param)
            begin = th.start
        else:
            self.uc.context_restore(th.ctx)
            if rax is not None:
                self.uc.reg_write(X.UC_X86_REG_RAX, rax)
            begin = self.uc.reg_read(X.UC_X86_REG_RIP)
        top = self._depth == 0
        self._depth += 1
        try:
            self.uc.emu_start(begin, THREAD_END, timeout=int(120e6) if top else 0)
        except UcError as e:
            rip = self.uc.reg_read(X.UC_X86_REG_RIP)
            exc, self._exc = self._exc, None
            th.state = "done"
            raise exc or EmuError(f"{e} at {self.where(rip)} (thread {th.tid})")
        finally:
            self._depth -= 1
            self.current, self.tls = prev, prev_tls
            self.w64(self.teb + 0x48, prev.tid if prev else 1)
            self.w64(self.teb + 0x08, STACK_BASE + STACK_SIZE)
            self.w64(self.teb + 0x10, STACK_BASE)
        if self._exc:
            exc, self._exc = self._exc, None
            th.state = "done"
            raise exc
        if self.uc.reg_read(X.UC_X86_REG_RIP) != THREAD_END:
            th.state = "done"
            raise EmuError(f"thread {th.tid} did not yield or end (timeout?) at {self.where(self.uc.reg_read(X.UC_X86_REG_RIP))}")
        if th.yielded:
            th.state = "blocked"
        else:
            th.state = "done"
            if th.exit_code is None:
                th.exit_code = self.uc.reg_read(X.UC_X86_REG_RAX) & 0xFFFFFFFF
        self.uc.context_restore(outer)

    def _step_threads(self, timeouts=False):
        """Gives every thread that can go on one turn; True if any ran."""
        ran = False
        for th in list(self.threads.values()):
            if th.state == "ready":
                th.state = "running"
                self._run_thread(th)
                ran = True
            elif th.state == "blocked":
                handles, wait_all, timeout = th.wait
                r = self._wait_result(handles, wait_all)
                if r is None and timeouts and timeout != 0xFFFFFFFF:
                    r = 0x102
                if r is not None:
                    th.state, th.wait = "running", None
                    self._run_thread(th, r)
                    ran = True
        return ran

    def run_threads(self, timeouts=False, limit=1000):
        """Runs the threads until none can go on (each blocked or ended). timeouts: blocked finite waits time out
        once each (as if their time had passed). Returns how many turns ran."""
        n = 0
        while n < limit and self._step_threads(timeouts):
            n += 1
            timeouts = False
        return n

    def set_event(self, h):
        self.events[h]["set"] = True

    # -- diagnostics --

    def image_of(self, a):
        for img in self.images:
            if img.contains(a):
                return img
        return None

    def symbolize(self, a):
        if a in self.stubs:
            return self.stubs[a][0]
        img = self.image_of(a)
        if not img:
            return f"{a:#x}"
        best = None
        if img.pdb:
            import bisect
            i = bisect.bisect_right(img.pdb.procs, (a - img.base, "\uffff")) - 1
            if i >= 0:
                best = (img.base + img.pdb.procs[i][0], img.pdb.procs[i][1])
        if img.names:
            import bisect
            i = bisect.bisect_right(img.names, (a, "\uffff")) - 1
            if i >= 0 and (not best or img.names[i][0] > best[0]):
                best = img.names[i]
        if best and a - best[0] < 0x10000:
            return f"{best[1]}+{a - best[0]:#x}" if a != best[0] else best[1]
        return f"{img.name}+{a - img.base:#x}"

    def backtrace(self, depth=0x800, limit=12):
        """Return addresses on the stack (heuristic: words pointing into an image's .text), symbolized."""
        rsp = self.uc.reg_read(X.UC_X86_REG_RSP)
        out = []
        for a in range(rsp, min(rsp + depth, STACK_BASE + STACK_SIZE), 8):
            v = self.r64(a)
            img = self.image_of(v)
            if img and ".text" in img.sections and img.sections[".text"][0] <= v < img.sections[".text"][1]:
                out.append(self.symbolize(v))
                if len(out) >= limit:
                    break
        return out

    def where(self, rip):
        s = f"{rip:#x} ({self.symbolize(rip)})"
        try:
            code = self.mem_read(rip, 16)
            md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
            for i in md.disasm(code, rip):
                s += f": {i.mnemonic} {i.op_str}"
                break
        except Exception:
            pass
        return s

    # -- printf (for captured logging functions) --

    def format_printf(self, fmt, first_arg):
        """Formats a printf call made with the Windows x64 varargs convention (fmt: a string)."""
        i = first_arg
        out = []
        pos = 0
        for m in re.finditer(r"%([-+ #0]*)(\*|\d+)?(?:\.(\*|\d+))?(hh|h|ll|l|z|j|t|I64|I32|L)?([diouxXeEfgGcspn%])", fmt):
            out.append(fmt[pos:m.start()])
            pos = m.end()
            flags, width, prec, length, conv = m.groups()
            if conv == "%":
                out.append("%")
                continue
            if width == "*":
                width = str(self.arg(i) & 0xFFFFFFFF)
                i += 1
            if prec == "*":
                prec = str(self.arg(i) & 0xFFFFFFFF)
                i += 1
            raw = self.arg(i)
            i += 1
            spec = "%" + (flags or "") + (width or "") + ("." + prec if prec is not None else "")
            if conv == "s":
                out.append((spec + "s") % (self.cstr(raw) if raw else "(null)"))
            elif conv == "c":
                out.append((spec + "c") % chr(raw & 0xFF))
            elif conv == "p":
                out.append(f"{raw:016X}")
            elif conv in "eEfgG":
                out.append((spec + conv) % struct.unpack("<d", struct.pack("<Q", raw))[0])
            else:
                bits = 64 if length in ("ll", "z", "j", "t", "I64") else 32
                v = raw & ((1 << bits) - 1)
                if conv in "di" and v >> (bits - 1):
                    v -= 1 << bits
                out.append((spec + ("d" if conv in "diu" else conv)) % v)
        out.append(fmt[pos:])
        return "".join(out)


# ---- Win32 API stand-ins --------------------------------------------------------------------------------------------

WINAPI = {}


def api(*names):
    def deco(fn):
        for n in names:
            WINAPI[n] = fn
        return fn
    return deco


def _const(v):
    return lambda e: v


for _n, _v in {
    "GetCurrentProcess": 0xFFFFFFFFFFFFFFFF, "GetCurrentThread": 0xFFFFFFFFFFFFFFFE, "GetCurrentProcessId": 1234,
    "GetCurrentThreadId": 1, "GetProcessHeap": HEAP_HANDLE, "HeapFree": 1, "VirtualFree": 1, "FlushInstructionCache": 1,
    "InitializeCriticalSectionAndSpinCount": 1, "InitializeCriticalSectionEx": 1, "TryEnterCriticalSection": 1,
    "TryAcquireSRWLockExclusive": 1, "TryAcquireSRWLockShared": 1, "SleepConditionVariableSRW": 1,
    "SleepConditionVariableCS": 1, "IsDebuggerPresent": 0, "IsProcessorFeaturePresent": 0, "GetACP": 65001,
    "GetOEMCP": 437, "IsValidCodePage": 1, "GetConsoleCP": 65001, "AreFileApisANSI": 1, "GetUserDefaultLCID": 0x409,
    "SetUnhandledExceptionFilter": 0, "GetTickCount": 100000, "GetTickCount64": 100000, "SwitchToThread": 1,
    "FreeEnvironmentStringsW": 1, "CloseHandle": 1, "GdiplusStartup": 0, "RegisterClassW": 0xC001,
    "LoadCursorW": 0x10003, "LoadCursorA": 0x10003, "InvalidateRect": 1,
    "SetWindowRgn": 1, "SendMessageW": 0, "IsIconic": 0, "SetCursor": 0,
}.items():
    WINAPI[_n] = _const(_v)
for _n in ("EnterCriticalSection", "LeaveCriticalSection", "DeleteCriticalSection", "InitializeCriticalSection",
           "AcquireSRWLockExclusive", "AcquireSRWLockShared", "ReleaseSRWLockExclusive", "ReleaseSRWLockShared",
           "InitializeSRWLock", "WakeAllConditionVariable", "WakeConditionVariable", "InitializeConditionVariable",
           "InitializeSListHead", "Sleep", "OutputDebugStringA", "OutputDebugStringW"):
    WINAPI[_n] = _const(0)


@api("RaiseException")
def _raise(e):
    """C++ exceptions (0xE06D7363) are reported with their type; nothing unwinds in the emulator."""
    code, n, params = e.arg(0) & 0xFFFFFFFF, e.arg(2), e.arg(3)
    what = f"exception {code:#x}"
    try:
        if code == 0xE06D7363 and n >= 4:
            info, base = e.r64(params + 16), e.r64(params + 24)
            cta = base + e.r32(info + 12)
            ct = base + e.r32(cta + 4)
            what = "C++ exception " + e.cstr(base + e.r32(ct + 4) + 16)
    except Exception:
        pass
    raise EmuError(f"{what} thrown; stack: " + " <- ".join(e.backtrace()))


@api("CreateThread", "_beginthreadex")
def _thread(e):
    """No threads in the emulator: a test that gets here reached code it should stand in for."""
    raise EmuError(f"CreateThread({e.symbolize(e.arg(2))}): threads aren't emulated; stack: " + " <- ".join(e.backtrace()))


@api("EncodePointer", "DecodePointer")
def _identity(e):
    return e.arg(0)


@api("GetLastError")
def _gle(e):
    return e.last_error


@api("SetLastError")
def _sle(e):
    e.last_error = e.arg(0) & 0xFFFFFFFF


@api("HeapAlloc")
def _halloc(e):
    return e.alloc(e.arg(2))


@api("HeapReAlloc")
def _hrealloc(e):
    old, size = e.arg(2), e.arg(3)
    new = e.alloc(size)
    n = min(size, e.sizes.get(old, 0))
    if old and n:
        e.mem_write(new, e.mem_read(old, n))
    return new


@api("HeapSize")
def _hsize(e):
    return e.sizes.get(e.arg(2), 0xFFFFFFFFFFFFFFFF)


@api("VirtualAlloc")
def _valloc(e):
    return e.alloc(align(e.arg(1), PAGE), PAGE)


@api("VirtualProtect")
def _vprotect(e):
    if e.arg(3):
        e.w32(e.arg(3), 0x40)
    return 1


@api("TlsAlloc", "FlsAlloc")
def _tlsalloc(e):
    i = e.tls_next
    e.tls_next += 1
    e.tls[i] = 0
    return i


@api("TlsGetValue", "FlsGetValue")
def _tlsget(e):
    return e.tls.get(e.arg(0) & 0xFFFFFFFF, 0)


@api("TlsSetValue", "FlsSetValue")
def _tlsset(e):
    e.tls[e.arg(0) & 0xFFFFFFFF] = e.arg(1)
    return 1


@api("TlsFree", "FlsFree")
def _tlsfree(e):
    return 1


@api("GetProcAddress")
def _gpa(e):
    name = e.cstr(e.arg(1)) if e.arg(1) > 0xFFFF else None
    if name in e.api:
        return e.stub(name, e.api[name])
    return 0


@api("LoadLibraryExW", "LoadLibraryW", "LoadLibraryA", "LoadLibraryExA")
def _loadlib(e):
    e.last_error = 126
    return 0


@api("GetModuleHandleW", "GetModuleHandleA")
def _gmh(e):
    p = e.arg(0)
    if not p:
        return e.images[0].base
    n = (e.wstr(p) if e.calls[-1][0].endswith("W") else e.cstr(p)).lower()
    for img in e.images:
        if img.name.lower() == n or img.name.lower().rsplit(".", 1)[0] == n:
            return img.base
    return 0


@api("GetModuleHandleExW")
def _gmhex(e):
    flags, p, out = e.arg(0), e.arg(1), e.arg(2)
    img = e.image_of(p) if flags & 4 else (e.images[0] if not p else None)
    if img is None and p and not flags & 4:
        n = e.wstr(p).lower()
        img = next((i for i in e.images if i.name.lower() == n), None)
    e.w64(out, img.base if img else 0)
    return 1 if img else 0


@api("GetModuleFileNameW", "GetModuleFileNameA")
def _gmfn(e):
    h, buf, n = e.arg(0), e.arg(1), e.arg(2) & 0xFFFFFFFF
    img = next((i for i in e.images if i.base == h), e.images[0] if not h else None)
    path = "C:\\rekordbox\\" + (img.name if img else "unknown.dll")
    wide = e.calls[-1][0].endswith("W")
    b = (path + "\0").encode("utf-16-le" if wide else "utf-8")
    e.mem_write(buf, b[: n * (2 if wide else 1)])
    return len(path)


@api("GetCommandLineW")
def _cmdw(e):
    return e.cmdline_w


@api("GetCommandLineA")
def _cmda(e):
    return e.cmdline_a


@api("GetEnvironmentStringsW")
def _envw(e):
    return e.env_w


@api("GetStartupInfoW")
def _gsi(e):
    e.mem_write(e.arg(0), b"\0" * 104)
    e.w32(e.arg(0), 104)


@api("GetStdHandle")
def _gstd(e):
    return 0xFFFFFFFFFFFFFFFF


@api("GetFileType")
def _gft(e):
    return 0


@api("GetCPInfo")
def _gcpi(e):
    e.mem_write(e.arg(1), struct.pack("<I", 4) + b"?\0" + b"\0" * 12)
    return 1


@api("QueryPerformanceCounter")
def _qpc(e):
    e.w64(e.arg(0), 1000000)
    return 1


@api("QueryPerformanceFrequency")
def _qpf(e):
    e.w64(e.arg(0), 10000000)
    return 1


@api("GetSystemTimeAsFileTime", "GetSystemTimePreciseAsFileTime")
def _gst(e):
    e.w64(e.arg(0), 133000000000000000)


@api("GetSystemInfo", "GetNativeSystemInfo")
def _gsysi(e):
    si = struct.pack("<HHIQQQIIIHH", 9, 0, PAGE, 0x10000, 0x7FFFFFFEFFFF, 1, 1, 8664, 0x10000, 6, 0)
    e.mem_write(e.arg(0), si.ljust(48, b"\0"))


@api("MultiByteToWideChar")
def _mbtowc(e):
    src, n, dst, dn = e.arg(2), e.arg(3) & 0xFFFFFFFF, e.arg(4), e.arg(5) & 0xFFFFFFFF
    raw = e.mem_read(src, n) if n != 0xFFFFFFFF else (e.cstr(src).encode() + b"\0")
    w = raw.decode("utf-8", errors="replace").encode("utf-16-le")
    if dst and dn:
        e.mem_write(dst, w[: 2 * dn])
    return len(w) // 2


@api("WideCharToMultiByte")
def _wctomb(e):
    src, n, dst, dn = e.arg(2), e.arg(3) & 0xFFFFFFFF, e.arg(4), e.arg(5) & 0xFFFFFFFF
    raw = e.mem_read(src, 2 * n) if n != 0xFFFFFFFF else (e.wstr(src).encode("utf-16-le") + b"\0\0")
    u = raw.decode("utf-16-le", errors="replace").encode()
    if dst and dn:
        e.mem_write(dst, u[:dn])
    return len(u)


@api("RtlPcToFileHeader")
def _pcfh(e):
    img = e.image_of(e.arg(0))
    e.w64(e.arg(1), img.base if img else 0)
    return img.base if img else 0


@api("InitOnceBeginInitialize")
def _iobegin(e):
    once, pending = e.arg(0), e.arg(2)
    done = e.r64(once) == 1
    e.w32(pending, 0 if done else 1)
    return 1


@api("InitOnceComplete")
def _iocomplete(e):
    e.w64(e.arg(0), 1)
    return 1


@api("InitOnceExecuteOnce")
def _ioexec(e):
    once, fn, param, ctx = e.arg(0), e.arg(1), e.arg(2), e.arg(3)
    if e.r64(once) != 1:
        e.w64(once, 1)
        return e.call(fn, once, param, ctx) & 0xFFFFFFFF
    return 1


# -- fake windows (user32): emu.windows[hwnd] = {"title", "rect" (l, t, r, b screen), "parent", "visible"} --

def _new_handle(e):
    e.handles += 4
    return e.handles


@api("EnumWindows")
def _enumwindows(e):
    cb, lp = e.arg(0), e.arg(1)
    for h, w in list(e.windows.items()):
        if not w.get("parent") and not e.call(cb, h, lp) & 0xFFFFFFFF:
            break
    return 1


@api("GetWindowThreadProcessId")
def _gwtpid(e):
    if e.arg(1):
        e.w32(e.arg(1), 1234)
    return 1


@api("IsWindowVisible")
def _iwv(e):
    return 1 if e.windows.get(e.arg(0), {}).get("visible") else 0


@api("IsWindow")
def _iw(e):
    return 1 if e.arg(0) in e.windows else 0


@api("GetWindowTextW")
def _gwt(e):
    t = e.windows.get(e.arg(0), {}).get("title", "")[: max(0, (e.arg(2) & 0xFFFFFFFF) - 1)]
    e.mem_write(e.arg(1), (t + "\0").encode("utf-16-le"))
    return len(t)


@api("GetWindowRect")
def _gwr(e):
    w = e.windows.get(e.arg(0), {})
    l, t, r, b = w.get("rect", (0, 0, 0, 0))
    ox, oy = _origin(e, w["parent"]) if w.get("parent") else (0, 0)   # (children: rect is in the parent's client)
    e.mem_write(e.arg(1), struct.pack("<iiii", l + ox, t + oy, r + ox, b + oy))
    return 1


@api("GetClientRect")
def _gcr(e):
    l, t, r, b = e.windows.get(e.arg(0), {}).get("rect", (0, 0, 0, 0))
    e.mem_write(e.arg(1), struct.pack("<iiii", 0, 0, r - l, b - t))
    return 1


def _origin(e, h):
    x = y = 0
    while h in e.windows:
        w = e.windows[h]
        x, y = x + w["rect"][0], y + w["rect"][1]
        h = w.get("parent")
        if h is None:
            break
    return x, y


@api("ScreenToClient")
def _s2c(e):
    x, y = struct.unpack("<ii", e.mem_read(e.arg(1), 8))
    ox, oy = _origin(e, e.arg(0))
    e.mem_write(e.arg(1), struct.pack("<ii", x - ox, y - oy))
    return 1


@api("ClientToScreen")
def _c2s(e):
    x, y = struct.unpack("<ii", e.mem_read(e.arg(1), 8))
    ox, oy = _origin(e, e.arg(0))
    e.mem_write(e.arg(1), struct.pack("<ii", x + ox, y + oy))
    return 1


@api("GetDpiForWindow", "GetDpiForSystem")
def _dpi(e):
    return e.dpi


@api("CreateWindowExW")
def _cwex(e):
    h = _new_handle(e)
    x, y, w, hh, parent = (e.arg(i) for i in (4, 5, 6, 7, 8))
    s32 = lambda v: v - (1 << 32) if v & 0x80000000 else v & 0xFFFFFFFF
    x, y, w, hh = (s32(v & 0xFFFFFFFF) for v in (x, y, w, hh))
    e.windows[h] = {"title": "", "rect": (x, y, x + w, y + hh), "parent": parent or None,
                    "visible": bool(e.arg(3) & 0x10000000), "class": e.wstr(e.arg(1)) if e.arg(1) > 0xFFFF else ""}
    return h


@api("SetWindowPos")
def _swp(e):
    h, x, y, w, hh = e.arg(0), e.arg(2), e.arg(3), e.arg(4), e.arg(5)
    s32 = lambda v: (v & 0xFFFFFFFF) - (1 << 32) if v & 0x80000000 else v & 0xFFFFFFFF
    x, y, w, hh = s32(x), s32(y), s32(w), s32(hh)
    if h in e.windows:
        e.windows[h]["rect"] = (x, y, x + w, y + hh)
    return 1


@api("RegisterClassW")
def _regclass(e):
    wc = e.arg(0)
    name = e.r64(wc + 64)
    e.window_classes[e.wstr(name) if name > 0xFFFF else f"#{name}"] = e.r64(wc + 8)
    return 0xC000 + len(e.window_classes)


@api("ShowWindow")
def _show(e):
    w = e.windows.get(e.arg(0))
    if w is None:
        return 0
    was = w["visible"]
    w["visible"] = (e.arg(1) & 0xFFFFFFFF) != 0
    return 1 if was else 0


@api("DestroyWindow")
def _destroy(e):
    return 1 if e.windows.pop(e.arg(0), None) is not None else 0


@api("CreateRoundRectRgn")
def _crr(e):
    return _new_handle(e)


# -- threads and events (Emu.enable_threads) --

class Thread:
    def __init__(self, emu, start, param, suspended):
        self.tid = emu.next_tid
        emu.next_tid += 1
        self.handle = _new_handle(emu)
        self.start, self.param = start, param
        self.stack = emu.alloc(THREAD_STACK, PAGE)
        self.tls = {}                  # its own TLS / FLS slot values
        self.ctx, self.wait, self.yielded = None, None, False
        self.state = "suspended" if suspended else "ready"
        self.exit_code = None


class _Marker:
    def __init__(self, name):
        self.name = name


_YIELD, _EXIT = _Marker("yield"), _Marker("exit")
_THREAD_API = {}


def _tapi(*names):
    def deco(fn):
        for n in names:
            _THREAD_API[n] = fn
        return fn
    return deco


@_tapi("CreateThread")
def _create_thread(e):
    th = Thread(e, e.arg(2), e.arg(3), bool(e.arg(4) & 4))   # (CREATE_SUSPENDED)
    e.threads[th.handle] = th
    if e.arg(5):
        e.w32(e.arg(5), th.tid)
    return th.handle


@_tapi("ResumeThread")
def _resume_thread(e):
    th = e.threads.get(e.arg(0))
    if not th:
        return 0xFFFFFFFF
    if th.state == "suspended":
        th.state = "ready"
        return 1
    return 0


@_tapi("ExitThread", "FreeLibraryAndExitThread")
def _exit_thread(e):
    if e.current is None:
        raise EmuError("ExitThread on the main thread")
    e.current.exit_code = e.arg(0 if e.calls[-1][0] == "ExitThread" else 1) & 0xFFFFFFFF
    return _EXIT


@_tapi("GetExitCodeThread")
def _exit_code(e):
    th = e.threads.get(e.arg(0))
    if not th:
        return 0
    e.w32(e.arg(1), th.exit_code if th.state == "done" else 259)   # (STILL_ACTIVE)
    return 1


@_tapi("GetCurrentThreadId")
def _tid(e):
    return e.current.tid if e.current else 1


@_tapi("GetThreadId")
def _thread_id(e):
    th = e.threads.get(e.arg(0))
    return th.tid if th else 0


@_tapi("CreateEventW", "CreateEventA")
def _create_event(e):
    h = _new_handle(e)
    e.events[h] = {"manual": bool(e.arg(1) & 0xFF), "set": bool(e.arg(2) & 0xFF)}
    return h


@_tapi("CreateEventExW", "CreateEventExA")
def _create_event_ex(e):
    h = _new_handle(e)
    e.events[h] = {"manual": bool(e.arg(2) & 1), "set": bool(e.arg(2) & 2)}
    return h


@_tapi("SetEvent")
def _set_event(e):
    ev = e.events.get(e.arg(0))
    if ev is None:
        return 0
    ev["set"] = True
    return 1


@_tapi("ResetEvent")
def _reset_event(e):
    ev = e.events.get(e.arg(0))
    if ev is None:
        return 0
    ev["set"] = False
    return 1


@_tapi("WaitForSingleObject", "WaitForSingleObjectEx")
def _wait_one(e):
    return e._wait([e.arg(0)], False, e.arg(1) & 0xFFFFFFFF)


@_tapi("WaitForMultipleObjects", "WaitForMultipleObjectsEx")
def _wait_many(e):
    n, arr = e.arg(0) & 0xFFFFFFFF, e.arg(1)
    return e._wait([e.r64(arr + 8 * i) for i in range(n)], bool(e.arg(2) & 0xFF), e.arg(3) & 0xFFFFFFFF)
