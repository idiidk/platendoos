// The Mac patch, in C++ for the patcher app: rekordbox's arm64 slice gets the load command for librbmods.dylib and
// its hooks.def call sites routed through it. A port of patch_binary.py and tools/dev/rbbin.py (same result, byte
// for byte; see those for the layout): call sites are found by name, a hook whose site can't be found is left out.
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

struct Segment { uint64_t vmaddr, vmsize, fileoff, filesize; };
struct Section { std::string seg, sect; uint64_t addr, size; uint32_t off; };

// rekordbox's arm64 slice: symbols, sections, and the instructions hooks.def's sites are found by.
struct Binary {
    const std::vector<uint8_t>& d;
    size_t base = 0;
    uint32_t ncmds = 0, sizeofcmds = 0;
    std::vector<Segment> segments;
    std::vector<Section> sections;
    std::map<std::string, uint64_t> symbols, stubs;
    std::vector<uint64_t> starts;  // function starts in __text (sorted)
    std::string uuid, error;

    explicit Binary(const std::vector<uint8_t>& data) : d(data) { parse(); }

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
        std::vector<uint64_t> out;
        uint64_t t;
        for (uint64_t va = fn, end = functionEnd(fn); va < end; va += 4)
            if (blTarget(va, u32(va), &t) && t == target) out.push_back(va);
        return out;
    }
    std::vector<uint64_t> jumps(uint64_t fn, uint64_t target) const {
        std::vector<uint64_t> out;
        uint64_t t;
        for (uint64_t va = fn, end = functionEnd(fn); va < end; va += 4)
            if (bTarget(va, u32(va), &t) && t == target) out.push_back(va);
        return out;
    }
    std::vector<uint64_t> adrs(uint64_t fn, uint64_t target, int reg) const {
        std::vector<uint64_t> out;
        for (uint64_t va = fn, end = functionEnd(fn); va < end; va += 4) {
            uint32_t w = u32(va);
            if ((w & 0x9F00001F) == (0x10000000u | (uint32_t)reg) && adrTarget(va, w) == target) out.push_back(va);
        }
        return out;
    }

private:
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
                if (be32(&d[8 + 20 * i]) == 0x0100000C) base = be32(&d[8 + 20 * i + 8]), found = true;
            if (!found) return (void)(error = "rekordbox has no Apple Silicon (arm64) code");
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
                segments.push_back({le64(&d[off + 24]), le64(&d[off + 32]), le64(&d[off + 40]), le64(&d[off + 48])});
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

struct Result {
    std::string error;                // the patch couldn't be made at all
    int patched = 0;
    std::vector<std::string> skipped;  // "<hook> (<why>)"
    std::vector<bool> found;           // per hooks.def hook (check() only)
    std::string uuid;
};

// The call site of one hook (or why it can't be used).
inline bool findSite(const Binary& b, const HookDef& h, uint64_t* site, std::string* why) {
    uint64_t fn = b.sym(h.caller), orig = b.sym(h.target);
    if (!fn) return *why = std::string("no function ") + h.caller, false;
    if (!orig) return *why = std::string("no target ") + h.target, false;
    std::vector<uint64_t> hits = h.kind == -1 ? b.calls(fn, orig) : h.kind == -2 ? b.jumps(fn, orig) : b.adrs(fn, orig, h.kind);
    if ((int)hits.size() != h.count)
        return *why = std::to_string(hits.size()) + " calls to the target instead of " + std::to_string(h.count) + ": the function changed", false;
    *site = hits[h.nth];
    if (h.kind >= 0 && b.u32(*site + 4) != 0xD503201F) return *why = "no nop after the adr", false;
    return true;
}

// Where patch() puts its parts, or why they don't fit (error): the hook stubs and marker below __text, the load
// command after the others (the header padding between them must be empty), and the dispatch slot in __DATA.
struct Layout { uint64_t stubs = 0, marker = 0, slot = 0; size_t cmdsEnd = 0, cmdsize = 0; std::string error; };
static const int kStub = 16;
static const char kDylib[] = "@executable_path/librbmods.dylib";
inline Layout layout(const Binary& b, const std::vector<uint8_t>& d) {
    Layout l;
    uint64_t text = b.textStart();
    uint64_t markerSize = (24 + kHookCount + 15) & ~15ull;
    l.stubs = text - (uint64_t)kStub * kHookCount, l.marker = l.stubs - markerSize;
    size_t nameLen = sizeof kDylib;  // with the 0
    l.cmdsize = (24 + nameLen + 7) & ~7ull;
    l.cmdsEnd = b.base + 32 + b.sizeofcmds;
    if (l.cmdsEnd + l.cmdsize > b.fo(l.marker)) return l.error = "no room for the hooks in the header padding", l;
    for (size_t i = l.cmdsEnd; i < b.fo(text); i++)
        if (d[i]) return l.error = "this rekordbox is already patched (or isn't an original): use an unmodified rekordbox", l;

    // The dispatch slot: 8 zero bytes of file-backed padding between two of __DATA's sections.
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
    if (!l.slot) l.error = "no padding for the dispatch slot in __DATA";
    return l;
}

// A hook's call site: its own, else the first of its HOOK_OR sites that is there (*kind: that site's kind). why: of its own site.
inline bool findHook(const Binary& b, const HookDef& h, uint64_t* site, int* kind, std::string* why) {
    *kind = h.kind;
    if (findSite(b, h, site, why)) return true;
    for (const HookDef& a : kHookAlts) {
        std::string w;
        if (a.name && !strcmp(a.name, h.name) && findSite(b, a, site, &w)) return *kind = a.kind, true;
    }
    return false;
}

// Only checks (for the patcher's "x of y call sites found"): how many hooks would be patched, and (error) whether
// the patch fits at all, so the patcher knows before it replaces anything.
inline Result check(const std::vector<uint8_t>& data) {
    Result r;
    Binary b(data);
    if (!b.ok()) return r.error = b.error, r;
    r.uuid = b.uuid;
    if (Layout l = layout(b, data); !l.error.empty()) return r.error = l.error, r;
    for (const HookDef& h : kHooks) {
        uint64_t site;
        int kind;
        std::string why;
        r.found.push_back(findHook(b, h, &site, &kind, &why));
        if (r.found.back()) r.patched++;
        else r.skipped.push_back(std::string(h.name) + " (" + why + ")");
    }
    return r;
}

// Patches d in place (an unmodified rekordbox).
inline Result patch(std::vector<uint8_t>& d) {
    Result r;
    const std::vector<uint8_t> orig = d;  // sites are found in the original (two hooks can share a function)
    Binary b(orig);
    if (!b.ok()) return r.error = b.error, r;
    r.uuid = b.uuid;
    auto put = [&](uint64_t va, uint32_t w) { put32(&d[b.fo(va)], w); };

    Layout l = layout(b, orig);
    if (!l.error.empty()) return r.error = l.error, r;
    uint64_t stubs = l.stubs, marker = l.marker, slot = l.slot;
    size_t cmdsEnd = l.cmdsEnd, cmdsize = l.cmdsize, nameLen = sizeof kDylib;

    std::vector<uint8_t> patched(kHookCount);
    for (int i = 0; i < kHookCount; i++) {
        const HookDef& h = kHooks[i];
        uint64_t stub = stubs + (uint64_t)kStub * i;
        put(stub, 0xD2800011u | ((uint32_t)i << 5));                                             // movz x17, #i
        put(stub + 4, adrp(stub + 4, slot, 16));
        put(stub + 8, 0xF9400000u | (uint32_t)(((slot & 0xFFF) / 8) << 10) | (16 << 5) | 16);  // ldr x16, [x16, #off]
        put(stub + 12, 0xD61F0200u);                                                             // br x16
        uint64_t site;
        int kind;
        std::string why;
        if (!findHook(b, h, &site, &kind, &why)) {
            r.skipped.push_back(std::string(h.name) + " (" + why + ")");
            continue;
        }
        if (kind == -1) put(site, bl(site, stub));
        else if (kind == -2) put(site, macho::b(site, stub));
        else {
            put(site, adrp(site, stub, kind));
            put(site + 4, 0x91000000u | (uint32_t)((stub & 0xFFF) << 10) | (uint32_t)(kind << 5) | (uint32_t)kind);  // add
        }
        patched[i] = 1;
        r.patched++;
    }
    // The marker: "RBMHOOKS", slot (8), hook count (4), 0 (4), one byte per hook.
    size_t m = b.fo(marker);
    memcpy(&d[m], "RBMHOOKS", 8);
    put64(&d[m + 8], slot);
    put32(&d[m + 16], (uint32_t)kHookCount);
    put32(&d[m + 20], 0);
    memcpy(&d[m + 24], patched.data(), patched.size());

    // LC_LOAD_DYLIB, appended.
    put32(&d[cmdsEnd], 0xC), put32(&d[cmdsEnd + 4], (uint32_t)cmdsize), put32(&d[cmdsEnd + 8], 24);
    put32(&d[cmdsEnd + 12], 2), put32(&d[cmdsEnd + 16], 0x10000), put32(&d[cmdsEnd + 20], 0x10000);
    memcpy(&d[cmdsEnd + 24], kDylib, nameLen);
    put32(&d[b.base + 16], b.ncmds + 1);
    put32(&d[b.base + 20], b.sizeofcmds + (uint32_t)cmdsize);
    return r;
}

}  // namespace macho
