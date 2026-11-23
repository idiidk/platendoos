// The Mac patch, in C++ for the patcher app: each of rekordbox's slices (arm64, and x86_64 for Intel Macs) gets the
// load command for librbmods.dylib and its hooks.def call sites routed through it. A port of patch_binary.py and
// tools/dev/rbbin.py (same result, byte for byte; see those for the layout): call sites are found by name, a hook
// whose site can't be found is left out.
#pragma once
#include <stdint.h>
#include <string.h>
#include <algorithm>
#include <map>
#include <string>
#include <vector>

namespace macho {

struct HookDef {
    const char *name, *caller;
    int nth, count;
    const char* target;
    int kind;
};
static const HookDef kHooks[] = {
#define HOOK(n, caller, nth, count, target, kind) {#n, caller, nth, count, target, kind},
#include "../hooks.def"
#undef HOOK
};
static const int kHookCount = (int)(sizeof kHooks / sizeof *kHooks);
// hooks.def's HOOK_OR lines: other sites of a hook (by name), tried in order when its own isn't there.
static const HookDef kHookAlts[] = {
#define HOOK(n, caller, nth, count, target, kind)
#define HOOK_OR(n, caller, nth, count, target, kind) {#n, caller, nth, count, target, kind},
#include "../hooks.def"
#undef HOOK_OR
#undef HOOK
    {nullptr, nullptr, 0, 0, nullptr, 0},  // (never empty)
};
// hooks.def's HOOK_X86 lines: a hook's sites in the x86_64 slice where they differ (caller "": none there).
static const HookDef kHookX86[] = {
#define HOOK(n, caller, nth, count, target, kind)
#define HOOK_X86(n, caller, nth, count, target, kind) {#n, caller, nth, count, target, kind},
#include "../hooks.def"
#undef HOOK_X86
#undef HOOK
    {nullptr, nullptr, 0, 0, nullptr, 0},  // (never empty)
};

static const uint32_t kArm64 = 0x0100000C, kX86_64 = 0x01000007;  // cputypes
inline const char* archName(uint32_t cpu) { return cpu == kArm64 ? "arm64" : "x86_64"; }

inline uint32_t le32(const uint8_t* p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
inline uint64_t le64(const uint8_t* p) { return le32(p) | (uint64_t)le32(p + 4) << 32; }
inline uint32_t be32(const uint8_t* p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }
inline void put32(uint8_t* p, uint32_t v) { for (int i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (8 * i)); }
inline void put64(uint8_t* p, uint64_t v) { put32(p, (uint32_t)v), put32(p + 4, (uint32_t)(v >> 32)); }

inline int64_t sext(uint64_t v, int bits) { return (v & (1ull << (bits - 1))) ? (int64_t)v - (int64_t)(1ull << bits) : (int64_t)v; }
inline bool blTarget(uint64_t va, uint32_t w, uint64_t* t) {
    if ((w & 0xFC000000) != 0x94000000) return false;
    return *t = va + 4 * sext(w & 0x3FFFFFF, 26), true;
}
inline bool bTarget(uint64_t va, uint32_t w, uint64_t* t) {
    if ((w & 0xFC000000) != 0x14000000) return false;
    return *t = va + 4 * sext(w & 0x3FFFFFF, 26), true;
}
inline uint64_t adrTarget(uint64_t va, uint32_t w) { return va + sext(((w >> 5) & 0x7FFFF) << 2 | ((w >> 29) & 3), 21); }
inline uint32_t bl(uint64_t src, uint64_t dst) { return 0x94000000 | (uint32_t)(((int64_t)(dst - src) >> 2) & 0x3FFFFFF); }
inline uint32_t b(uint64_t src, uint64_t dst) { return 0x14000000 | (uint32_t)(((int64_t)(dst - src) >> 2) & 0x3FFFFFF); }
inline uint32_t adrp(uint64_t src, uint64_t dst, int rd) {
    int64_t imm = ((int64_t)(dst & ~0xFFFull) - (int64_t)(src & ~0xFFFull)) >> 12;
    return 0x90000000 | (uint32_t)((imm & 3) << 29) | (uint32_t)(((imm >> 2) & 0x7FFFF) << 5) | (uint32_t)rd;
}

struct Segment { uint64_t vmaddr, vmsize, fileoff, filesize; std::string name; };
struct Section { std::string seg, sect; uint64_t addr, size; uint32_t off; };

// One slice of rekordbox (arm64 or x86_64): symbols, sections, and the instructions hooks.def's sites are found by
// (x86_64: by a byte scan, as rbbin.py).
struct Binary {
    const std::vector<uint8_t>& d;
    uint32_t cpu;
    size_t base = 0;
    uint32_t ncmds = 0, sizeofcmds = 0;
    std::vector<Segment> segments;
    std::vector<Section> sections;
    std::map<std::string, uint64_t> symbols, stubs;
    std::vector<uint64_t> starts;  // function starts in __text (sorted)
    std::string uuid, error;

    explicit Binary(const std::vector<uint8_t>& data, uint32_t cputype = kArm64) : d(data), cpu(cputype) { parse(); }

    bool ok() const { return error.empty(); }
    const Section* section(const char* seg, const char* sect) const {
        for (const Section& s : sections)
            if (s.seg == seg && s.sect == sect) return &s;
        return nullptr;
    }
    uint64_t textStart() const { return section("__TEXT", "__text")->addr; }
    uint64_t textEnd() const { const Section* s = section("__TEXT", "__text"); return s->addr + s->size; }
    // The file offset of an address (SIZE_MAX if it isn't in the file).
    size_t fo(uint64_t va) const {
        for (const Segment& s : segments)
            if (s.vmaddr <= va && va < s.vmaddr + s.filesize) return base + s.fileoff + (va - s.vmaddr);
        return SIZE_MAX;
    }
    uint32_t u32(uint64_t va) const { size_t o = fo(va); return o + 4 <= d.size() ? le32(&d[o]) : 0; }
    // A symbol's address; "stub:_name" is the __stubs entry rekordbox calls an imported function through. 0: none.
    uint64_t sym(const std::string& name) const {
        if (name.rfind("stub:", 0) == 0) { auto it = stubs.find(name.substr(5)); return it == stubs.end() ? 0 : it->second; }
        auto it = symbols.find(name);
        return it == symbols.end() ? 0 : it->second;
    }
    uint64_t functionEnd(uint64_t start) const {
        auto it = std::upper_bound(starts.begin(), starts.end(), start);
        return it == starts.end() ? textEnd() : *it;
    }
    std::vector<uint64_t> calls(uint64_t fn, uint64_t target) const {
        if (cpu == kX86_64) return rel32(fn, target, 0xE8);
        std::vector<uint64_t> out;
        uint64_t t;
        for (uint64_t va = fn, end = functionEnd(fn); va < end; va += 4)
            if (blTarget(va, u32(va), &t) && t == target) out.push_back(va);
        return out;
    }
    std::vector<uint64_t> jumps(uint64_t fn, uint64_t target) const {
        if (cpu == kX86_64) return rel32(fn, target, 0xE9);
        std::vector<uint64_t> out;
        uint64_t t;
        for (uint64_t va = fn, end = functionEnd(fn); va < end; va += 4)
            if (bTarget(va, u32(va), &t) && t == target) out.push_back(va);
        return out;
    }
    std::vector<uint64_t> adrs(uint64_t fn, uint64_t target, int reg) const {
        if (cpu == kX86_64) return leas(fn, target);
        std::vector<uint64_t> out;
        for (uint64_t va = fn, end = functionEnd(fn); va < end; va += 4) {
            uint32_t w = u32(va);
            if ((w & 0x9F00001F) == (0x10000000u | (uint32_t)reg) && adrTarget(va, w) == target) out.push_back(va);
        }
        return out;
    }
    // x86_64: every <op> rel32 (E8 call, E9 jmp) inside fn that reaches target.
    std::vector<uint64_t> rel32(uint64_t fn, uint64_t target, uint8_t op) const {
        std::vector<uint64_t> out;
        size_t o = fo(fn);
        uint64_t n = functionEnd(fn) - fn;
        if (o == SIZE_MAX || o + n > d.size()) return out;
        for (uint64_t i = 0; i + 5 <= n; i++)
            if (d[o + i] == op && fn + i + 5 + (int64_t)(int32_t)le32(&d[o + i + 1]) == target) out.push_back(fn + i);
        return out;
    }
    // x86_64: every "lea reg, [rip + target]" (48 / 4C 8D, ModRM mod 00 rm 101) inside fn, at its REX byte.
    std::vector<uint64_t> leas(uint64_t fn, uint64_t target) const {
        std::vector<uint64_t> out;
        size_t o = fo(fn);
        uint64_t n = functionEnd(fn) - fn;
        if (o == SIZE_MAX || o + n > d.size()) return out;
        for (uint64_t i = 1; i + 6 <= n; i++)
            if (d[o + i] == 0x8D && (d[o + i - 1] == 0x48 || d[o + i - 1] == 0x4C) && (d[o + i + 1] & 0xC7) == 0x05 &&
                fn + i + 6 + (int64_t)(int32_t)le32(&d[o + i + 2]) == target)
                out.push_back(fn + i - 1);
        return out;
    }

private:
    std::string noCode() const {
        return cpu == kArm64 ? "rekordbox has no Apple Silicon (arm64) code" : "rekordbox has no Intel (x86_64) code";
    }
    std::string cstr(size_t at) const {
        size_t e = at;
        while (e < d.size() && d[e]) e++;
        return std::string((const char*)&d[at], e - at);
    }
    void parse() {
        if (d.size() < 32) return (void)(error = "not a Mach-O binary");
        if (le32(&d[0]) != 0xFEEDFACF) {
            if (be32(&d[0]) != 0xCAFEBABE) return (void)(error = "not a Mach-O binary");
            uint32_t n = be32(&d[4]);
            bool found = false;
            for (uint32_t i = 0; i < n && 8 + 20 * (i + 1) <= d.size(); i++)
                if (be32(&d[8 + 20 * i]) == cpu) base = be32(&d[8 + 20 * i + 8]), found = true;
            if (!found) return (void)(error = noCode());
        } else if (le32(&d[4]) != cpu) {
            return (void)(error = noCode());
        }
        ncmds = le32(&d[base + 16]), sizeofcmds = le32(&d[base + 20]);
        size_t off = base + 32;
        uint32_t symoff = 0, nsyms = 0, stroff = 0, indirectoff = 0;
        bool symtab = false, dysymtab = false;
        const Section* stubSect = nullptr;
        uint32_t stubFirst = 0, stubSize = 0;
        for (uint32_t c = 0; c < ncmds; c++) {
            uint32_t cmd = le32(&d[off]), size = le32(&d[off + 4]);
            if (cmd == 0x19) {  // LC_SEGMENT_64
                std::string seg((const char*)&d[off + 8], strnlen((const char*)&d[off + 8], 16));
                segments.push_back({le64(&d[off + 24]), le64(&d[off + 32]), le64(&d[off + 40]), le64(&d[off + 48]), seg});
                uint32_t nsects = le32(&d[off + 64]);
                for (uint32_t s = 0; s < nsects; s++) {
                    size_t so = off + 72 + 80 * s;
                    std::string sect((const char*)&d[so], strnlen((const char*)&d[so], 16));
                    sections.push_back({seg, sect, le64(&d[so + 32]), le64(&d[so + 40]), le32(&d[so + 48])});
                    if (seg == "__TEXT" && sect == "__stubs") stubFirst = le32(&d[so + 68]), stubSize = le32(&d[so + 72]);
                }
            } else if (cmd == 0x2) {  // LC_SYMTAB
                symoff = le32(&d[off + 8]), nsyms = le32(&d[off + 12]), stroff = le32(&d[off + 16]), symtab = true;
            } else if (cmd == 0xB) {  // LC_DYSYMTAB
                indirectoff = le32(&d[off + 56]), dysymtab = true;
            } else if (cmd == 0x1B) {  // LC_UUID
                static const char* hex = "0123456789ABCDEF";
                for (int i = 0; i < 16; i++) uuid += hex[d[off + 8 + i] >> 4], uuid += hex[d[off + 8 + i] & 15];
            }
            off += size;
        }
        if (!section("__TEXT", "__text")) return (void)(error = "no __text section");
        stubSect = section("__TEXT", "__stubs");
        uint64_t lo = textStart(), hi = textEnd();
        if (symtab) {
            for (uint32_t i = 0; i < nsyms; i++) {
                const uint8_t* e = &d[base + symoff + 16 * i];
                uint32_t strx = le32(e);
                uint8_t ntype = e[4];
                uint64_t value = le64(e + 8);
                if ((ntype & 0xE0) || (ntype & 0x0E) != 0x0E || !value) continue;
                symbols.emplace(cstr(base + stroff + strx), value);  // (the first wins, as setdefault)
                if (lo <= value && value < hi) starts.push_back(value);
            }
            std::sort(starts.begin(), starts.end());
            starts.erase(std::unique(starts.begin(), starts.end()), starts.end());
        }
        if (symtab && dysymtab && stubSect && stubSize) {
            for (uint64_t i = 0; i < stubSect->size / stubSize; i++) {
                uint32_t symi = le32(&d[base + indirectoff + 4 * (stubFirst + i)]);
                if (symi & 0xC0000000) continue;
                uint32_t strx = le32(&d[base + symoff + 16 * symi]);
                stubs.emplace(cstr(base + stroff + strx), stubSect->addr + stubSize * i);
            }
        }
    }
};

// One slice's outcome.
struct ArchResult {
    uint32_t cpu = 0;
    std::string uuid;
    int patched = 0, total = 0;         // total: the hooks that have a site on this architecture (hooks.def)
    std::vector<std::string> skipped;  // "<hook> (<why>)"
    std::vector<std::string> off;      // hooks never patched on this architecture (a HOOK_X86 line without a site)
    std::vector<bool> found;           // per hooks.def hook
};

struct Result {
    std::string error;                // the patch couldn't be made at all
    int patched = 0;                  // the arm64 slice's (as patched, skipped, found and uuid)
    std::vector<std::string> skipped;  // "<hook> (<why>)"
    std::vector<bool> found;           // per hooks.def hook
    std::string uuid;                  // the arm64 LC_UUID: identifies the rekordbox build
    std::vector<ArchResult> archs;     // every slice, arm64 first
    const ArchResult* arch(uint32_t cpu) const {
        for (const ArchResult& a : archs)
            if (a.cpu == cpu) return &a;
        return nullptr;
    }
};

// The architectures in the file (arm64 and x86_64 only), in its order.
inline std::vector<uint32_t> cpus(const std::vector<uint8_t>& d) {
    std::vector<uint32_t> out;
    if (d.size() >= 8 && le32(&d[0]) == 0xFEEDFACF) out.push_back(le32(&d[4]));
    else if (d.size() >= 8 && be32(&d[0]) == 0xCAFEBABE)
        for (uint32_t i = 0, n = be32(&d[4]); i < n && 8 + 20 * (i + 1) <= d.size(); i++)
            out.push_back(be32(&d[8 + 20 * i]));
    auto other = [](uint32_t c) { return c != kArm64 && c != kX86_64; };
    out.erase(std::remove_if(out.begin(), out.end(), other), out.end());
    return out;
}

// The sites to try for a hook, in order: arm64 its own then its HOOK_OR ones; x86_64 its HOOK_X86 ones if it has
// any, else the same.
inline std::vector<HookDef> hookSites(const HookDef& h, uint32_t cpu) {
    std::vector<HookDef> out;
    if (cpu == kX86_64)
        for (const HookDef& x : kHookX86)
            if (x.name && !strcmp(x.name, h.name)) out.push_back(x);
    if (!out.empty()) return out;
    out.push_back(h);
    for (const HookDef& a : kHookAlts)
        if (a.name && !strcmp(a.name, h.name)) out.push_back(a);
    return out;
}
inline bool offOn(const std::vector<HookDef>& sites) {
    for (const HookDef& s : sites)
        if (*s.caller) return false;
    return true;
}

// The call site of one hook (or why it can't be used); *orig: its target.
inline bool findSite(const Binary& b, const HookDef& h, uint64_t* site, uint64_t* orig, std::string* why) {
    if (!*h.caller) return *why = std::string("not on ") + archName(b.cpu) + " (hooks.def)", false;
    uint64_t fn = b.sym(h.caller);
    *orig = b.sym(h.target);
    if (!fn) return *why = std::string("no function ") + h.caller, false;
    if (!*orig) return *why = std::string("no target ") + h.target, false;
    std::vector<uint64_t> hits = h.kind == -1 ? b.calls(fn, *orig) : h.kind == -2 ? b.jumps(fn, *orig) : b.adrs(fn, *orig, h.kind);
    if ((int)hits.size() != h.count)
        return *why = std::to_string(hits.size()) + " calls to the target instead of " + std::to_string(h.count) + ": the function changed", false;
    *site = hits[h.nth];
    if (b.cpu == kArm64 && h.kind >= 0 && b.u32(*site + 4) != 0xD503201F) return *why = "no nop after the adr", false;
    return true;
}

// Where patch() puts its parts in one slice, or why they don't fit (error): the hook stubs and marker below __text,
// the load command after the others (the header padding between them must be empty), and the dispatch slot in __DATA.
struct Layout { uint64_t stubs = 0, marker = 0, slot = 0; size_t cmdsEnd = 0, cmdsize = 0; std::string error; };
static const int kStub = 16;
static const char kDylib[] = "@executable_path/librbmods.dylib";
// The marker's size, a multiple of 16 (patch_binary.py): x86_64's has the table of originals.
inline uint64_t markerSize(uint32_t cpu) {
    uint64_t n = (uint64_t)kHookCount;
    return ((cpu == kArm64 ? 24 + n : 24 + ((n + 3) & ~3ull) + 4 * n) + 15) & ~15ull;
}
inline Layout layout(const Binary& b, const std::vector<uint8_t>& d) {
    Layout l;
    uint64_t text = b.textStart();
    l.stubs = text - (uint64_t)kStub * kHookCount, l.marker = l.stubs - markerSize(b.cpu);
    size_t nameLen = sizeof kDylib;  // with the 0
    l.cmdsize = (24 + nameLen + 7) & ~7ull;
    l.cmdsEnd = b.base + 32 + b.sizeofcmds;
    if (l.cmdsEnd + l.cmdsize > b.fo(l.marker)) return l.error = "no room for the hooks in the header padding", l;
    for (size_t i = l.cmdsEnd; i < b.fo(text); i++)
        if (d[i]) return l.error = "this rekordbox is already patched (or isn't an original): use an unmodified rekordbox", l;

    // The dispatch slot: 8 zero bytes of file-backed padding between two of __DATA's sections, else the unused tail
    // of __DATA after its last section (zero-filled memory).
    std::vector<const Section*> data;
    for (const Section& s : b.sections)
        if (s.seg == "__DATA" && s.off) data.push_back(&s);
    std::sort(data.begin(), data.end(), [](const Section* x, const Section* y) { return x->addr < y->addr; });
    for (size_t i = 0; i + 1 < data.size() && !l.slot; i++) {
        uint64_t s = (data[i]->addr + data[i]->size + 15) & ~15ull;
        if (s + 8 <= data[i + 1]->addr) {
            size_t o = b.fo(s);
            if (o != SIZE_MAX && o + 8 <= d.size() && !memcmp(&d[o], "\0\0\0\0\0\0\0\0", 8)) l.slot = s;
        }
    }
    for (const Segment& seg : b.segments) {
        if (l.slot || seg.name != "__DATA") continue;
        uint64_t end = 0;
        for (const Section& s : b.sections)
            if (s.seg == "__DATA") end = std::max(end, s.addr + s.size);
        if (end && ((end + 15) & ~15ull) + 8 <= seg.vmaddr + seg.vmsize) l.slot = (end + 15) & ~15ull;
    }
    if (!l.slot) l.error = "no padding for the dispatch slot in __DATA";
    return l;
}

// A hook's call site in one slice: the first of its sites that is there (*kind: that site's kind). why: of the first.
inline bool findHook(const Binary& b, const std::vector<HookDef>& sites, uint64_t* site, uint64_t* orig, int* kind,
                     std::string* why) {
    for (const HookDef& s : sites) {
        std::string w;
        if (findSite(b, s, site, orig, &w)) return *kind = s.kind, true;
        if (why->empty()) *why = w;
    }
    return false;
}

// One slice: finds the sites and, if d is given, patches it (sites are found in the original, b's data: two hooks
// can share a function).
inline std::string patchSlice(const Binary& b, std::vector<uint8_t>* d, ArchResult* a) {
    a->cpu = b.cpu, a->uuid = b.uuid;
    Layout l = layout(b, b.d);
    if (!l.error.empty()) return l.error;
    uint64_t stubs = l.stubs, marker = l.marker, slot = l.slot, textBase = 0;
    for (const Segment& s : b.segments)
        if (s.name == "__TEXT") textBase = s.vmaddr;
    size_t cmdsEnd = l.cmdsEnd, cmdsize = l.cmdsize, nameLen = sizeof kDylib;
    bool arm = b.cpu == kArm64;
    auto put = [&](uint64_t va, uint32_t w) { put32(&(*d)[b.fo(va)], w); };

    std::vector<uint8_t> patched(kHookCount);
    std::vector<uint32_t> origs(kHookCount);
    for (int i = 0; i < kHookCount; i++) {
        const HookDef& h = kHooks[i];
        uint64_t stub = stubs + (uint64_t)kStub * i;
        if (d && arm) {
            put(stub, 0xD2800011u | ((uint32_t)i << 5));                                             // movz x17, #i
            put(stub + 4, adrp(stub + 4, slot, 16));
            put(stub + 8, 0xF9400000u | (uint32_t)(((slot & 0xFFF) / 8) << 10) | (16 << 5) | 16);  // ldr x16, [x16, #off]
            put(stub + 12, 0xD61F0200u);                                                             // br x16
        } else if (d) {
            uint8_t* p = &(*d)[b.fo(stub)];                                                          // mov r11d, i
            p[0] = 0x41, p[1] = 0xBB, put32(p + 2, (uint32_t)i);
            p[6] = 0xFF, p[7] = 0x25, put32(p + 8, (uint32_t)(int32_t)(slot - (stub + 12)));        // jmp [rip + slot]
            memset(p + 12, 0xCC, 4);                                                                 // int3
        }
        std::vector<HookDef> sites = hookSites(h, b.cpu);
        a->found.push_back(false);
        if (offOn(sites)) {
            a->off.push_back(h.name);
            continue;
        }
        a->total++;
        uint64_t site, orig;
        int kind;
        std::string why;
        if (!findHook(b, sites, &site, &orig, &kind, &why)) {
            a->skipped.push_back(std::string(h.name) + " (" + why + ")");
            continue;
        }
        a->found.back() = true;
        a->patched++;
        patched[i] = 1;
        origs[i] = (uint32_t)(orig - textBase);
        if (!d) continue;
        if (arm) {
            if (kind == -1) put(site, bl(site, stub));
            else if (kind == -2) put(site, macho::b(site, stub));
            else {
                put(site, adrp(site, stub, kind));
                put(site + 4, 0x91000000u | (uint32_t)((stub & 0xFFF) << 10) | (uint32_t)(kind << 5) | (uint32_t)kind);  // add
            }
        } else if (kind == -1 || kind == -2) {
            put(site + 1, (uint32_t)(int32_t)(stub - (site + 5)));  // call / jmp rel32
        } else {
            put(site + 3, (uint32_t)(int32_t)(stub - (site + 7)));  // lea reg, [rip + rel32]
        }
    }
    if (!d) return "";
    // The marker: "RBMHOOKS", slot (8), hook count (4), 0 (arm64) / 1 (x86_64) (4), one byte per hook; x86_64 then
    // pads to a multiple of 4 and has each hook's original target (offset from __TEXT, 0: not patched).
    size_t m = b.fo(marker);
    memcpy(&(*d)[m], "RBMHOOKS", 8);
    put64(&(*d)[m + 8], slot);
    put32(&(*d)[m + 16], (uint32_t)kHookCount);
    put32(&(*d)[m + 20], arm ? 0 : 1);
    memcpy(&(*d)[m + 24], patched.data(), patched.size());
    if (!arm)
        for (int i = 0; i < kHookCount; i++) put32(&(*d)[m + 24 + ((kHookCount + 3) & ~3) + 4 * i], origs[i]);

    // LC_LOAD_DYLIB, appended.
    put32(&(*d)[cmdsEnd], 0xC), put32(&(*d)[cmdsEnd + 4], (uint32_t)cmdsize), put32(&(*d)[cmdsEnd + 8], 24);
    put32(&(*d)[cmdsEnd + 12], 2), put32(&(*d)[cmdsEnd + 16], 0x10000), put32(&(*d)[cmdsEnd + 20], 0x10000);
    memcpy(&(*d)[cmdsEnd + 24], kDylib, nameLen);
    put32(&(*d)[b.base + 16], b.ncmds + 1);
    put32(&(*d)[b.base + 20], b.sizeofcmds + (uint32_t)cmdsize);
    return "";
}

// Every slice (arm64 first in the result); patches *d in place when it is given (an unmodified rekordbox).
inline Result run(const std::vector<uint8_t>& orig, std::vector<uint8_t>* d) {
    Result r;
    std::vector<uint32_t> archs = cpus(orig);
    if (std::find(archs.begin(), archs.end(), kArm64) == archs.end()) {
        Binary b(orig);  // (its error says why)
        return r.error = b.ok() ? "rekordbox has no Apple Silicon (arm64) code" : b.error, r;
    }
    std::stable_partition(archs.begin(), archs.end(), [](uint32_t c) { return c == kArm64; });
    for (uint32_t cpu : archs) {
        Binary b(orig, cpu);
        ArchResult a;
        std::string err = b.ok() ? patchSlice(b, d, &a) : b.error;
        if (!err.empty()) return r.error = cpu == kArm64 ? err : err + " (in its Intel code)", r;
        r.archs.push_back(a);
    }
    const ArchResult& arm = r.archs[0];
    r.patched = arm.patched, r.skipped = arm.skipped, r.found = arm.found, r.uuid = arm.uuid;
    return r;
}

// Only checks (for the patcher's "x of y call sites found"): how many hooks would be patched, and (error) whether
// the patch fits at all, so the patcher knows before it replaces anything.
inline Result check(const std::vector<uint8_t>& data) { return run(data, nullptr); }

// Patches d in place (an unmodified rekordbox).
inline Result patch(std::vector<uint8_t>& d) {
    const std::vector<uint8_t> orig = d;  // sites are found in the original (two hooks can share a function)
    return run(orig, &d);
}

}  // namespace macho
