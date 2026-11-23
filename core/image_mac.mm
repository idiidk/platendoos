// rekordbox's image on macOS: its symbol table and code (see symbols.cpp for the registry).
// Finds rekordbox's addresses and field offsets at load, from its symbol table (rekordbox isn't stripped) and
// a few instructions of its own code, so the mods follow an update that moves things. rbm::Addr and rbm::Offset
// (rbmod.h) resolve themselves when they're constructed; checkBuild() then switches off the mods whose
// addresses or fields weren't found and logs it. tools/dev/rbbin.py and tools/dev/recipes.py read the binary file the
// same way (keep the decoders in step).
// In rekordbox's x86_64 slice (an Intel Mac, docs/intel-mac.md) the symbols are read the same way; the field recipes
// name arm64 instructions, so they are read from the arm64 half of the same file on disk (armImage()).
// The reader works on the running image and on a slice of a file alike (Image): RBM_IMAGE_TEST builds it for an
// offline test, with a file standing in for the running rekordbox.
#include "rbmod.h"
#include <CoreFoundation/CoreFoundation.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <dirent.h>
#include <libkern/OSByteOrder.h>
#include <mach-o/dyld.h>
#include <mach-o/fat.h>
#include <mach-o/loader.h>
#include <mach-o/nlist.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <algorithm>
#include <map>
#include <set>
#include <string_view>
#include <unordered_map>

namespace rbm {

#if defined(__x86_64__)
// The x86_64 slices have LC_UUIDs of their own. The expected values (plus their X86{} differences) are 7.2.19's, and
// tools/dev/recipes.py check --x86 confirmed every recipe in both builds.
static const char* kTestedBuild = "08B8BB799BBF3D63B6D69B1C5A362F0B";   // LC_UUID of rekordbox 7.2.19 x86_64
static const char* kCheckedBuild = "F5BE9C7D32853E11A3EBF4B4FD401904";  // rekordbox 7.2.20 x86_64
#else
static const char* kTestedBuild = "D827BD186918357A9F57C506CF428700";  // LC_UUID of rekordbox 7.2.19 arm64
static const uintptr_t kTestedSlot = 0x1053be860;                     // its dispatch slot (patchers before the marker)
// rekordbox 7.2.20 arm64: every mod was run on it (2026-11-19, docs/rekordbox-updates.md).
static const char* kCheckedBuild = "4AF5D59D5EC937DCBD893FBC84D970AD";
#endif

namespace {

// A Mach-O image: the running rekordbox (mapped by dyld: an address plus the slide), or one slice of a file on disk
// (mapped read-only: an address through its segment's file offset). Addresses are unslid.
struct Image {
    const mach_header_64* header = nullptr;
    intptr_t slide = 0;
    const uint8_t* file = nullptr;  // read from a file: the slice's first byte (null: the running image)
    size_t fileSize = 0;            // the slice's size
    std::string path;               // the file (the running one: its executable)
    struct Segment { uint64_t vmaddr, vmsize, fileoff, filesize; };
    std::vector<Segment> segments;
    uintptr_t textVm = 0;              // the __TEXT segment (it starts with the header)
    uintptr_t textLo = 0, textHi = 0;  // __TEXT,__text
    uintptr_t stubsLo = 0;             // __TEXT,__stubs: stubsCount entries of stubSize bytes, for the indirect
    uint32_t stubsCount = 0, stubSize = 0, stubsFirst = 0;  // symbols from stubsFirst on
    const uint32_t* indirect = nullptr;
    uint32_t nindirect = 0;
    bool chained = false;  // LC_DYLD_CHAINED_FIXUPS: a pointer in the file is a chained fixup, not the address
    std::string uuid;
    std::unordered_map<std::string_view, uintptr_t> byName;  // freed by imageDone()
    std::vector<uintptr_t> starts;     // function starts in __text, sorted
    const nlist_64* syms = nullptr;
    uint32_t nsyms = 0;
    const char* strs = nullptr;

    // Where n bytes at a can be read; null if not all of them are in the file.
    const uint8_t* at(uintptr_t a, size_t n) const {
        if (!file) return (const uint8_t*)(a + slide);
        for (const Segment& s : segments)
            if (a >= s.vmaddr && a - s.vmaddr <= s.filesize && n <= s.filesize - (a - s.vmaddr) &&
                s.fileoff + (a - s.vmaddr) + n <= fileSize)
                return file + s.fileoff + (a - s.vmaddr);
        return nullptr;
    }
    uint32_t u32(uintptr_t a) const {
        if (!file) return *(const uint32_t*)(a + slide);
        uint32_t v = 0;
        if (const uint8_t* p = at(a, 4)) memcpy(&v, p, 4);
        return v;
    }
    // The pointer stored at a, unslid.
    uintptr_t pointer(uintptr_t a) const {
        if (!file) return *(const uintptr_t*)(a + slide) - slide;
        uint64_t v = 0;
        if (const uint8_t* p = at(a, 8)) memcpy(&v, p, 8);
        return chained ? (v & 0xFFFFFFFFFull) + textVm : v;  // a chained rebase holds the target's offset (low 36 bits)
    }
    bool inSegment(uintptr_t a) const {
        for (const Segment& s : segments)
            if (s.vmsize && (s.fileoff || s.filesize) && a >= s.vmaddr && a - s.vmaddr < s.vmsize) return true;  // not __PAGEZERO
        return false;
    }
};

// Reads the load commands, symbol table and stubs of m.header (header, slide or file already set).
void parse(Image& m) {
    const mach_header_64* h = m.header;
    if (h->magic != MH_MAGIC_64 || (m.file && sizeof *h + (uint64_t)h->sizeofcmds > m.fileSize)) return;
    const segment_command_64* linkedit = nullptr;
    const symtab_command* symtab = nullptr;
    const dysymtab_command* dysymtab = nullptr;
    const char* lcp = (const char*)(h + 1);
    const char* lcEnd = lcp + h->sizeofcmds;
    for (uint32_t i = 0; i < h->ncmds && lcp + sizeof(load_command) <= lcEnd; i++) {
        const load_command* lc = (const load_command*)lcp;
        if (lc->cmdsize < sizeof(load_command) || lcp + lc->cmdsize > lcEnd) break;
        if (lc->cmd == LC_SEGMENT_64 && lc->cmdsize >= sizeof(segment_command_64)) {
            const segment_command_64* seg = (const segment_command_64*)lc;
            m.segments.push_back({seg->vmaddr, seg->vmsize, seg->fileoff, seg->filesize});
            if (!strcmp(seg->segname, SEG_LINKEDIT)) linkedit = seg;
            if (!strcmp(seg->segname, SEG_TEXT)) m.textVm = seg->vmaddr;
            const section_64* sect = (const section_64*)(seg + 1);
            for (uint32_t s = 0; s < seg->nsects && (const char*)(sect + s + 1) <= lcp + lc->cmdsize; s++) {
                if (strncmp(sect[s].segname, "__TEXT", 16)) continue;
                if (!strncmp(sect[s].sectname, "__text", 16)) m.textLo = sect[s].addr, m.textHi = sect[s].addr + sect[s].size;
                if (!strncmp(sect[s].sectname, "__stubs", 16) && (sect[s].flags & SECTION_TYPE) == S_SYMBOL_STUBS && sect[s].reserved2)
                    m.stubsLo = sect[s].addr, m.stubSize = sect[s].reserved2, m.stubsCount = (uint32_t)(sect[s].size / sect[s].reserved2),
                    m.stubsFirst = sect[s].reserved1;
            }
        } else if (lc->cmd == LC_SYMTAB) {
            symtab = (const symtab_command*)lc;
        } else if (lc->cmd == LC_DYSYMTAB) {
            dysymtab = (const dysymtab_command*)lc;
        } else if (lc->cmd == LC_DYLD_CHAINED_FIXUPS) {
            m.chained = true;
        } else if (lc->cmd == LC_UUID && lc->cmdsize >= sizeof(uuid_command)) {
            char buf[33];
            const uint8_t* u = ((const uuid_command*)lc)->uuid;
            for (int k = 0; k < 16; k++) snprintf(buf + 2 * k, 3, "%02X", u[k]);
            m.uuid = buf;
        }
        lcp += lc->cmdsize;
    }
    if (!linkedit || !symtab) return;
    // __LINKEDIT's file offsets: in a file, from the slice's start; in the running image, through the segment.
    auto linkeditAt = [&](uint64_t off, uint64_t len) -> const char* {
        if (m.file) return off <= m.fileSize && len <= m.fileSize - off ? (const char*)m.file + off : nullptr;
        return (const char*)(linkedit->vmaddr + m.slide - linkedit->fileoff + off);
    };
    m.syms = (const nlist_64*)linkeditAt(symtab->symoff, (uint64_t)symtab->nsyms * sizeof(nlist_64));
    m.strs = linkeditAt(symtab->stroff, symtab->strsize);
    if (!m.syms || !m.strs) {
        m.syms = nullptr, m.strs = nullptr;
        return;
    }
    m.nsyms = symtab->nsyms;
    if (dysymtab && (m.indirect = (const uint32_t*)linkeditAt(dysymtab->indirectsymoff, 4ull * dysymtab->nindirectsyms)))
        m.nindirect = dysymtab->nindirectsyms;
    m.byName.reserve(m.nsyms);
    for (uint32_t i = 0; i < m.nsyms; i++) {
        const nlist_64& n = m.syms[i];
        if ((n.n_type & N_STAB) || (n.n_type & N_TYPE) != N_SECT || !n.n_value || n.n_un.n_strx >= symtab->strsize) continue;
        m.byName.emplace(m.strs + n.n_un.n_strx, n.n_value);
        if (n.n_value >= m.textLo && n.n_value < m.textHi) m.starts.push_back(n.n_value);
    }
    std::sort(m.starts.begin(), m.starts.end());
    m.starts.erase(std::unique(m.starts.begin(), m.starts.end()), m.starts.end());
}

// The slice for cpu of the file at path, mapped read-only (kept for the app's lifetime); false if there is none.
[[maybe_unused]] bool loadFile(const char* path, cpu_type_t cpu, Image& m) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return false;
    struct stat st;
    void* map = fstat(fd, &st) == 0 && st.st_size >= (off_t)sizeof(mach_header_64)
        ? mmap(nullptr, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, fd, 0) : MAP_FAILED;
    close(fd);
    if (map == MAP_FAILED) return false;
    const uint8_t* base = (const uint8_t*)map;
    size_t size = (size_t)st.st_size;
    uint64_t off = 0, len = 0;
    if (OSReadBigInt32(base, 0) == FAT_MAGIC) {
        uint32_t n = OSReadBigInt32(base, 4);
        for (uint32_t i = 0; i < n && 8 + 20 * (uint64_t)(i + 1) <= size; i++)
            if ((cpu_type_t)OSReadBigInt32(base, 8 + 20 * i) == cpu)
                off = OSReadBigInt32(base, 8 + 20 * i + 8), len = OSReadBigInt32(base, 8 + 20 * i + 12);
    } else if (((const mach_header_64*)base)->magic == MH_MAGIC_64 && ((const mach_header_64*)base)->cputype == cpu) {
        len = size;
    }
    if (!len || off > size || len > size - off || len < sizeof(mach_header_64)) {
        munmap(map, size);
        return false;
    }
    m.file = base + off;
    m.fileSize = len;
    m.header = (const mach_header_64*)m.file;
    m.path = path;
    parse(m);
    return true;
}

Image& image() {
    static Image* img = [] {
        Image* m = new Image;
#ifdef RBM_IMAGE_TEST
        // The offline test: this architecture's slice of $RBM_IMAGE_TEST_FILE stands in for the running rekordbox.
        if (const char* path = getenv("RBM_IMAGE_TEST_FILE"))
#if defined(__x86_64__)
            loadFile(path, CPU_TYPE_X86_64, *m);
#else
            loadFile(path, CPU_TYPE_ARM64, *m);
#endif
        return m;
#endif
        for (uint32_t i = 0; i < _dyld_image_count(); i++) {
            const mach_header* h = _dyld_get_image_header(i);
            if (h->filetype == MH_EXECUTE) {
                m->header = (const mach_header_64*)h;
                m->slide = _dyld_get_image_vmaddr_slide(i);
                if (const char* path = _dyld_get_image_name(i)) m->path = path;
                break;
            }
        }
        if (m->header) parse(*m);
        return m;
    }();
    return *img;
}

#if defined(__x86_64__)
// The arm64 half of the running executable's file, for the field recipes (they name arm64 instructions; the
// patched file's arm64 half still has every one). Null if the file has none or it can't be read: the recipes then
// find nothing and their mods are off.
const Image* armImage() {
    static const Image* arm = []() -> const Image* {
        Image* a = new Image;
        const std::string& path = image().path;
        if (!path.empty() && loadFile(path.c_str(), CPU_TYPE_ARM64, *a) && a->syms && a->textLo) {
            logf("rbmods: x86_64: field offsets read from the arm64 half of %s (plus their X86{} differences)", path.c_str());
            return a;
        }
        logf("rbmods: x86_64: %s has no readable arm64 half: no field offsets, the mods that need them are off",
             path.empty() ? "the executable" : path.c_str());
        delete a;
        return nullptr;
    }();
    return arm;
}
#endif

uintptr_t symbolIn(const Image& m, const char* name) {
    if (!name || !*name) return 0;
    if (!m.byName.empty()) {
        auto it = m.byName.find(name);
        return it == m.byName.end() ? 0 : it->second;
    }
    for (uint32_t i = 0; i < m.nsyms; i++) {  // after imageDone() freed the index
        const nlist_64& n = m.syms[i];
        if (!(n.n_type & N_STAB) && (n.n_type & N_TYPE) == N_SECT && n.n_value && !strcmp(m.strs + n.n_un.n_strx, name)) return n.n_value;
    }
    return 0;
}

// The __stubs entry rekordbox calls the imported function name ("_x") through; 0 if none.
[[maybe_unused]] uintptr_t stubIn(const Image& m, const char* name) {
    for (uint32_t i = 0; i < m.stubsCount && m.stubsFirst + i < m.nindirect; i++) {
        uint32_t s = m.indirect[m.stubsFirst + i];
        if (s & (INDIRECT_SYMBOL_LOCAL | INDIRECT_SYMBOL_ABS) || s >= m.nsyms) continue;
        if (!strcmp(m.strs + m.syms[s].n_un.n_strx, name)) return m.stubsLo + (uintptr_t)m.stubSize * i;
    }
    return 0;
}

uintptr_t functionEnd(const Image& m, uintptr_t fn) {
    auto it = std::upper_bound(m.starts.begin(), m.starts.end(), fn);
    return it == m.starts.end() ? m.textHi : *it;
}

int64_t sext(uint64_t v, int bits) { return (int64_t)(v << (64 - bits)) >> (64 - bits); }

[[maybe_unused]] uintptr_t blTarget(uintptr_t a, uint32_t w) {
    return (w & 0xFC000000) == 0x94000000 ? a + 4 * sext(w & 0x3FFFFFF, 26) : 0;
}
[[maybe_unused]] bool isAdrp(uint32_t w) { return (w & 0x9F000000) == 0x90000000; }
[[maybe_unused]] uintptr_t adrpPage(uintptr_t a, uint32_t w) {
    return (a & ~(uintptr_t)0xFFF) + (sext(((w >> 5) & 0x7FFFF) << 2 | ((w >> 29) & 3), 21) << 12);
}

// The instructions recipes read (tools/dev/recipes.py decode()).
struct Decoded { const char* kind; int rn; size_t imm; };
bool decode(uint32_t w, Decoded* d) {
    if ((w & 0x3B000000) == 0x39000000) {  // load/store register (also FP), unsigned immediate
        unsigned size = w >> 30, opc = (w >> 22) & 3;
        size_t imm12 = (w >> 10) & 0xFFF;
        if ((w & 0x04000000) && opc >= 2) *d = {opc == 2 ? "str" : "ldr", (int)(w >> 5) & 31, imm12 << 4};
        else *d = {opc == 0 ? "str" : "ldr", (int)(w >> 5) & 31, imm12 << size};
        return true;
    }
    if ((w & 0x7F800000) == 0x11000000) {  // add (immediate)
        *d = {"add", (int)(w >> 5) & 31, (size_t)((w >> 10) & 0xFFF) << ((w & 0x400000) ? 12 : 0)};
        return true;
    }
    if ((w & 0x7E000000) == 0x36000000) {  // tbz / tbnz
        *d = {"tb", (int)(w & 31), (size_t)((w >> 31) << 5 | ((w >> 19) & 31))};
        return true;
    }
    if ((w & 0x7F800000) == 0x52800000) {  // movz
        *d = {"movz", (int)(w & 31), (size_t)((w >> 5) & 0xFFFF) << (16 * ((w >> 21) & 3))};
        return true;
    }
    return false;
}

// "ldr" matches every such load, "ldr x19" those based on x19.
bool kindMatches(const char* kind, const Decoded& d) {
    size_t n = strlen(d.kind);
    if (strncmp(kind, d.kind, n) || (kind[n] && kind[n] != ' ')) return false;
    return !kind[n] || atoi(kind + n + 2) == d.rn;
}

// An Offset recipe in image m (fn: the function's or vtable's address there): see rbm::immediate.
bool immediateIn(const Image& m, uintptr_t fn, const char* kind, int nth, size_t* out) {
    if (!fn || !kind) return false;
    if (!strncmp(kind, "vt:", 3)) {  // a method's slot in the vtable `name`, from its address point (+0x10)
        uintptr_t method = symbolIn(m, kind + 3);
        if (!method) return false;
        for (int i = 0; i < 1024; i++)
            if (m.pointer(fn + 0x10 + 8 * i) == method && nth-- == 0) return *out = 8 * i, true;
        return false;
    }
    if (!strcmp(kind, "mov")) {  // a 32-bit constant: movz, then movk into the same register
        int n = -1, rd = -1;
        size_t val = 0;
        for (uintptr_t a = fn, end = functionEnd(m, fn); a < end; a += 4) {
            uint32_t w = m.u32(a);
            if ((w & 0x7F800000) == 0x52800000) {
                rd = w & 31, val = (size_t)((w >> 5) & 0xFFFF) << (16 * ((w >> 21) & 3)), n++;
            } else if ((w & 0x7F800000) == 0x72800000 && rd == (int)(w & 31)) {
                val |= (size_t)((w >> 5) & 0xFFFF) << (16 * ((w >> 21) & 3));
                if (n == nth) return *out = val, true;
            }
        }
        return false;
    }
    for (uintptr_t a = fn, end = functionEnd(m, fn); a < end; a += 4) {
        Decoded d;
        if (decode(m.u32(a), &d) && kindMatches(kind, d) && nth-- == 0) return *out = d.imm, true;
    }
    return false;
}

// The holder of a juce::SingletonHolder<T>::get(), by its own symbol: the class's names from get()'s, e.g.
// __ZN4juce15SingletonHolderIN2db10DatabaseIFENS_15CriticalSectionELb0EE3getEv -> __ZN2db10DatabaseIF15singletonHolderE
// ("" if get isn't such a name).
[[maybe_unused]] std::string singletonSymbol(const char* get) {
    static const char kPrefix[] = "__ZN4juce15SingletonHolderI";
    if (!get || strncmp(get, kPrefix, sizeof kPrefix - 1)) return "";
    const char* p = get + sizeof kPrefix - 1;
    bool nested = *p == 'N';
    p += nested;
    std::string names;
    while (*p >= '1' && *p <= '9') {
        char* e;
        long n = strtol(p, &e, 10);
        if ((long)strnlen(e, n) < n) return "";
        names.append(p, e + n - p);
        p = e + n;
        if (!nested) break;
    }
    if (names.empty() || (nested && *p++ != 'E') || strncmp(p, "NS_", 3)) return "";  // the mutex type follows
    return "__ZN" + names + "15singletonHolderE";
}

// The patcher's marker in front of its stubs: "RBMHOOKS", slot, hook count, format, one byte per hook (1 = patched);
// format 1 (the x86_64 patcher) then has, 4-aligned, per hook its original target as a 32-bit offset from __TEXT's
// address (0: not patched).
struct HookMarker {
    bool found = false;
    uintptr_t at = 0;  // unslid
    uint64_t slot = 0;
    uint32_t count = 0, format = 0;
    std::vector<uint8_t> patched;
    std::vector<uintptr_t> origs;  // format 1: unslid, 0 = not patched
};

const HookMarker& marker() {
    static HookMarker* mk = [] {
        HookMarker* k = new HookMarker;
        const Image& m = image();
        if (!m.header) return k;
        for (uintptr_t a = m.textVm + sizeof(mach_header_64) + m.header->sizeofcmds; a + 24 <= m.textLo; a += 8) {
            const uint8_t* p = m.at(a, 24);
            if (!p) break;
            if (memcmp(p, "RBMHOOKS", 8)) continue;
            memcpy(&k->slot, p + 8, 8);
            memcpy(&k->count, p + 16, 4);
            memcpy(&k->format, p + 20, 4);
            uint32_t n = std::min<uint32_t>(k->count, 4096);
            if (const uint8_t* b = m.at(a + 24, n)) k->patched.assign(b, b + n);
            uintptr_t table = a + ((24 + (uintptr_t)k->count + 3) & ~(uintptr_t)3);
            if (k->format == 1 && k->count <= 4096)
                if (const uint8_t* t = m.at(table, 4 * (size_t)k->count)) {
                    k->origs.resize(k->count);
                    for (uint32_t i = 0; i < k->count; i++) {
                        uint32_t off;
                        memcpy(&off, t + 4 * i, 4);
                        k->origs[i] = off ? m.textVm + off : 0;
                    }
                }
            k->found = true;
            k->at = a;
            break;
        }
        return k;
    }();
    return *mk;
}

std::vector<uint8_t> g_patched;  // per hook, from the patcher's marker
uint32_t g_markerCount;          // the marker's hook count, when it isn't HK_COUNT (0: it matches)
uintptr_t g_markerAt;            // where the marker is (unslid)

#if defined(__x86_64__)
// Every patched stub's original target (slid), from the marker's table of originals; false without one.
bool stubTargets(std::vector<uintptr_t>* out) {
    const HookMarker& k = marker();
    if (!k.found || k.origs.size() != k.count) return false;
    std::vector<uintptr_t> t(k.count);
    size_t n = 0;
    for (uint32_t i = 0; i < k.count; i++)
        if (k.origs[i]) t[i] = k.origs[i] + image().slide, n++;
    logf("rbmods: %zu patched call sites sent to their original targets", n);
    *out = std::move(t);
    return true;
}
#else
// The unpatched rekordbox the app was made from: same LC_UUID and no marker at `marker` (an offset in the arm64
// slice); the slice's offset in the file. -1: none.
int openOriginal(const std::string& uuid, uint64_t marker, uint64_t* slice) {
    std::vector<std::string> paths{"/Applications/rekordbox.app/Contents/MacOS/rekordbox"};  // as the patcher looks
    if (DIR* d = opendir("/Applications")) {  // "/Applications/rekordbox 7/rekordbox.app"
        while (dirent* e = readdir(d))
            if (!strncasecmp(e->d_name, "rekordbox", 9) && !strstr(e->d_name, ".app"))
                paths.push_back(std::string("/Applications/") + e->d_name + "/rekordbox.app/Contents/MacOS/rekordbox");
        closedir(d);
    }
    for (const std::string& path : paths) {
        int fd = open(path.c_str(), O_RDONLY);
        if (fd < 0) continue;
        uint64_t off = 0;
        uint32_t magic = 0;
        if (pread(fd, &magic, 4, 0) == 4 && OSSwapBigToHostInt32(magic) == FAT_MAGIC) {
            fat_header fh;
            fat_arch fa[8];
            uint32_t n = 0;
            if (pread(fd, &fh, sizeof fh, 0) == sizeof fh) n = std::min<uint32_t>(OSSwapBigToHostInt32(fh.nfat_arch), 8);
            if (pread(fd, fa, n * sizeof(fat_arch), sizeof fh) != (ssize_t)(n * sizeof(fat_arch))) n = 0;
            for (uint32_t i = 0; i < n; i++)
                if ((cpu_type_t)OSSwapBigToHostInt32(fa[i].cputype) == CPU_TYPE_ARM64) off = OSSwapBigToHostInt32(fa[i].offset);
        }
        mach_header_64 h{};
        std::vector<char> cmds;
        if (pread(fd, &h, sizeof h, off) == sizeof h && h.magic == MH_MAGIC_64 && h.sizeofcmds < (1u << 20)) {
            cmds.resize(h.sizeofcmds);
            if (pread(fd, cmds.data(), cmds.size(), off + sizeof h) != (ssize_t)cmds.size()) cmds.clear();
        }
        for (size_t p = 0; p + sizeof(load_command) <= cmds.size();) {
            const load_command* lc = (const load_command*)(cmds.data() + p);
            if (lc->cmdsize < sizeof(load_command) || p + lc->cmdsize > cmds.size()) break;
            if (lc->cmd == LC_UUID && lc->cmdsize >= sizeof(uuid_command)) {
                char buf[33];
                for (int k = 0; k < 16; k++) snprintf(buf + 2 * k, 3, "%02X", ((const uuid_command*)lc)->uuid[k]);
                char tag[8] = {};
                if (uuid == buf && pread(fd, tag, 8, off + marker) == 8 && memcmp(tag, "RBMHOOKS", 8)) {
                    logf("rbmods: original rekordbox for the hook stubs: %s", path.c_str());
                    *slice = off;
                    return fd;
                }
                break;
            }
            p += lc->cmdsize;
        }
        close(fd);
    }
    return -1;
}

// Every patched stub's original target (slid), from the original's instructions at the stub's call sites; false
// if one can't be read. Stubs no site uses stay 0.
bool stubTargets(std::vector<uintptr_t>* out) {
    const Image& m = image();
    uint32_t count = g_markerCount;
    uintptr_t stubs = m.textLo - 16 * (uintptr_t)count;
    if (g_markerAt != stubs - ((24 + count + 15) & ~15u)) return false;  // not the layout patch_binary.py makes
    auto stubIndex = [&](uintptr_t t) { return t >= stubs && t < m.textLo && !((t - stubs) & 15) ? (int)((t - stubs) / 16) : -1; };
    // The sites: every b / bl to a stub, every adrp + add of a stub's address (patch_binary.py's three kinds).
    struct Site { uintptr_t a; int kind; int reg; int id; };  // kind 0 b, 1 bl, 2 adrp + add
    std::vector<Site> sites;
    for (uintptr_t a = m.textLo; a + 4 <= m.textHi; a += 4) {
        uint32_t w = insn(a);
        if ((w & 0x7C000000) == 0x14000000) {
            if (int id = stubIndex(a + 4 * sext(w & 0x3FFFFFF, 26)); id >= 0) sites.push_back({a, (int)(w >> 31), 0, id});
        } else if (isAdrp(w) && a + 8 <= m.textHi) {
            uint32_t next = insn(a + 4);
            Decoded d;
            int rd = w & 31;
            if (decode(next, &d) && !strcmp(d.kind, "add") && d.rn == rd && (int)(next & 31) == rd)
                if (int id = stubIndex(adrpPage(a, w) + d.imm); id >= 0) sites.push_back({a, 2, rd, id});
        }
    }
    // __TEXT's file offset and address: the original has the same layout (same UUID).
    uint64_t textVm = 0, textFile = 0;
    const load_command* lc = (const load_command*)(m.header + 1);
    for (uint32_t i = 0; i < m.header->ncmds; i++, lc = (const load_command*)((const char*)lc + lc->cmdsize))
        if (lc->cmd == LC_SEGMENT_64 && !strcmp(((const segment_command_64*)lc)->segname, SEG_TEXT))
            textVm = ((const segment_command_64*)lc)->vmaddr, textFile = ((const segment_command_64*)lc)->fileoff;
    if (!textVm) return false;
    uint64_t slice = 0;
    int fd = openOriginal(m.uuid, textFile + (g_markerAt - textVm), &slice);
    if (fd < 0) return false;
    std::vector<uintptr_t> t(count);
    bool ok = true;
    for (const Site& s : sites) {
        uint32_t w[2];
        if (pread(fd, w, sizeof w, slice + textFile + (s.a - textVm)) != sizeof w) { ok = false; break; }
        uintptr_t target = 0;
        if (s.kind < 2 && (w[0] & 0xFC000000) == (s.kind ? 0x94000000u : 0x14000000u))
            target = s.a + 4 * sext(w[0] & 0x3FFFFFF, 26);
        else if (s.kind == 2 && (w[0] & 0x9F00001F) == (0x10000000u | s.reg) && w[1] == 0xD503201F)  // adr reg; nop
            target = s.a + sext(((w[0] >> 5) & 0x7FFFF) << 2 | ((w[0] >> 29) & 3), 21);
        uintptr_t slid = target + m.slide;
        if (!target || stubIndex(target) >= 0 || (t[s.id] && t[s.id] != slid)) { ok = false; break; }
        t[s.id] = slid;
    }
    close(fd);
    if (ok) logf("rbmods: %zu patched call sites sent to their original targets", sites.size());
    if (ok) *out = std::move(t);
    return ok;
}
#endif

}  // namespace

extern "C" void* __dynamic_cast(const void* sub, const void* srcType, const void* dstType, ptrdiff_t hint);
void* dynCast(void* obj, const void* srcType, const void* dstType) { return __dynamic_cast(obj, srcType, dstType, -1); }

// macOS reads everything from the binary itself. In the x86_64 slice a hook's original is the target of the site the
// patcher patched, from the marker's table (the first HOOK_X86 line's target where it has none).
bool imageResolve(const Addr& a, uintptr_t* out) {
#if defined(__x86_64__)
    uintptr_t i = ((uintptr_t)&a - (uintptr_t)kHookOrig) / sizeof(Addr);
    if ((uintptr_t)&a >= (uintptr_t)kHookOrig && i < HK_COUNT) {
        const HookMarker& k = marker();
        if (k.count == HK_COUNT && k.origs.size() == HK_COUNT && k.origs[i]) return *out = k.origs[i], true;
    }
#else
    (void)a, (void)out;
#endif
    return false;
}
bool imageResolve(const Offset&, size_t*, bool*) { return false; }

uintptr_t symbol(const char* name) {
    if (!name) return 0;
    const Image& m = image();
    if (!strncmp(name, "stub:_", 6)) {  // an imported function: where the stub's binding points
        void* p = dlsym(RTLD_DEFAULT, name + 6);
        return p ? (uintptr_t)p - m.slide : 0;
    }
    return symbolIn(m, name);
}

uint32_t insn(uintptr_t a) { return image().u32(a); }

uintptr_t callSite(const char* caller, const char* target, int nth) {
    const Image& m = image();
#if defined(__x86_64__)
    // "call target" (E8 rel32): a byte scan finds the same calls a disassembler does (docs/intel-mac.md). The nth is
    // this slice's own order of the calls, which can differ from arm64's.
    uintptr_t fn = symbol(caller), t = target && !strncmp(target, "stub:", 5) ? stubIn(m, target + 5) : symbol(target);
    if (!fn || !t) return 0;
    uintptr_t end = functionEnd(m, fn);
    const uint8_t* p = end > fn + 5 ? m.at(fn, end - fn) : nullptr;
    if (!p) return 0;
    for (uintptr_t i = 0; fn + i + 5 <= end; i++) {
        if (p[i] != 0xE8) continue;
        int32_t rel;
        memcpy(&rel, p + i + 1, 4);
        if (fn + i + 5 + rel == t && nth-- == 0) return fn + i;
    }
    return 0;
#else
    uintptr_t fn = symbol(caller), t = symbol(target);
    if (!fn || !t) return 0;
    for (uintptr_t a = fn, end = functionEnd(m, fn); a < end; a += 4)
        if (blTarget(a, insn(a)) == t && nth-- == 0) return a;
    return 0;
#endif
}

uintptr_t adrpAddBefore(uintptr_t site, int reg) {
#if defined(__x86_64__)
    (void)site, (void)reg;
    return 0;  // no arm64 code here: leaBefore
#else
    if (!site) return 0;
    size_t add = 0;
    bool haveAdd = false;
    for (int i = 1; i <= 8; i++) {
        uintptr_t a = site - 4 * i;
        uint32_t w = insn(a);
        Decoded d;
        if (!haveAdd && decode(w, &d) && !strcmp(d.kind, "add") && d.rn == reg && (int)(w & 31) == reg)
            add = d.imm, haveAdd = true;
        else if (isAdrp(w) && (int)(w & 31) == reg)
            return haveAdd ? adrpPage(a, w) + add : 0;
    }
    return 0;
#endif
}

uintptr_t adrpAdd(const char* name, int nth) {
#if defined(__x86_64__)
    (void)name, (void)nth;
    return 0;  // no arm64 code here
#else
    uintptr_t fn = symbol(name);
    if (!fn) return 0;
    for (uintptr_t a = fn, end = functionEnd(image(), fn); a + 4 < end; a += 4) {
        uint32_t w = insn(a), next = insn(a + 4);
        Decoded d;
        if (isAdrp(w) && decode(next, &d) && !strcmp(d.kind, "add") && d.rn == (int)(w & 31) && nth-- == 0)
            return adrpPage(a, w) + d.imm;
    }
    return 0;
#endif
}

uintptr_t leaBefore(uintptr_t site, int reg) {
#if defined(__x86_64__)
    // lea reg, [rip + disp32]: REX.W (REX.R: reg 8-15), 8D, ModRM mod 00 r/m 101. Read backwards by bytes, so the
    // target must also lie in the image (not in __text) for a match to count.
    const Image& m = image();
    if (!site || reg < 0 || reg > 15) return 0;
    for (uintptr_t back = 7; back <= 48 && back <= site; back++) {
        uintptr_t a = site - back;
        const uint8_t* p = m.at(a, 7);
        if (!p) return 0;
        if ((p[0] & 0xF8) != 0x48 || p[1] != 0x8D || (p[2] & 0xC7) != 0x05) continue;
        if ((((p[0] & 4) << 1) | ((p[2] >> 3) & 7)) != reg) continue;
        int32_t disp;
        memcpy(&disp, p + 3, 4);
        uintptr_t t = a + 7 + disp;
        if (m.inSegment(t) && (t < m.textLo || t >= m.textHi)) return t;
    }
    return 0;
#else
    (void)site, (void)reg;
    return 0;
#endif
}

uintptr_t singleton(const char* caller, const char* get) {
#if defined(__x86_64__)
    (void)caller;
    std::string holder = singletonSymbol(get);
    return holder.empty() ? 0 : symbol(holder.c_str());
#else
    return adrpAddBefore(callSite(caller, get, 0), 0);
#endif
}

bool immediate(const char* name, const char* kind, int nth, size_t* out) {
    if (!name || !kind) return false;
#if defined(__x86_64__)
    if (strncmp(kind, "vt:", 3)) {  // an instruction recipe: arm64 code, from the arm64 half of the file
        const Image* arm = armImage();
        return arm && immediateIn(*arm, symbolIn(*arm, name), kind, nth, out);
    }
#endif
    return immediateIn(image(), symbol(name), kind, nth, out);
}

std::string rekordboxBuild() { return image().uuid; }
bool testedBuild() { return image().uuid == kTestedBuild; }
// On arm64: 7.2.20, every mod was run on it. On x86_64: 7.2.20's x86_64 slice, every recipe and hook site checked.
bool checkedBuild() { return testedBuild() || image().uuid == kCheckedBuild; }

// The patcher's marker (HookMarker): the dispatch slot, which hooks it patched.
void** dispatchSlot() {
    const Image& m = image();
    if (!m.header) return nullptr;
    const HookMarker& k = marker();
    if (k.found) {
        g_patched = k.patched;
        if (k.count != HK_COUNT) g_markerCount = k.count, g_markerAt = k.at;  // rbm_init runs it stock
        return (void**)(k.slot + m.slide);
    }
#if defined(__x86_64__)
    return nullptr;
#else
    if (!testedBuild()) return nullptr;
    g_patched.assign(HK_COUNT, 1);
    return (void**)(kTestedSlot + m.slide);
#endif
}

intptr_t thisDelta(const Addr&) { return 0; }

bool hookPatched(HookId h) { return h < (int)g_patched.size() && g_patched[h]; }


// After checkBuild(): the name indexes aren't needed any more.
void imageDone() {
    Image& m = image();
    m.byName.clear();
    m.byName.rehash(0);
#if defined(__x86_64__)
    if (Image* arm = (Image*)armImage()) {
        arm->byName.clear();
        arm->byName.rehash(0);
    }
#endif
}

bool imageHasCode() { return image().header != nullptr; }

void start();

}  // namespace rbm

#ifndef RBM_IMAGE_TEST
extern "C" void* rbm_fill_hook_table();
extern "C" void* rbm_stock_dispatch(const uintptr_t* targets);

// The app was patched from another hooks.def than this dylib was built with (install.sh after a hooks.def change,
// or a newer dylib on an older app): stub i isn't hook i any more, and every stub jumps through the slot, so the
// slot can't stay empty either. Each stub goes straight to its call site's original target instead and no mod
// starts: rekordbox runs as stock. Without the original targets (arm64: the original rekordbox to read them from;
// x86_64: the marker's table) it can't run at all.
static void runStock(void** slot) {
    using namespace rbm;
    logf("rbmods: the app was patched with %u hooks, this build of the mods has %d (install.sh after a hooks.def change?): "
         "platendoos is off for this run; rebuild the app with build.sh or reinstall it with the patcher",
         g_markerCount, (int)HK_COUNT);
    std::vector<uintptr_t> targets;
    if (!stubTargets(&targets)) {
#if defined(__x86_64__)
        logf("rbmods: the patcher's marker has no table of the original call targets: it can't run stock, quitting");
#else
        logf("rbmods: the unpatched rekordbox this app was made from isn't in /Applications: it can't run stock, quitting");
#endif
        CFOptionFlags answer;
        CFUserNotificationDisplayAlert(0, kCFUserNotificationStopAlertLevel, nullptr, nullptr, nullptr, CFSTR("platendoos can't start"),
            CFSTR("This app and its mods come from different platendoos builds, and the rekordbox it was made from "
                  "wasn't found in /Applications (it may have been updated since), so it can't start.\n\n"
                  "Reinstall platendoos (build.sh, or the platendoos patcher). rekordbox itself works as usual."),
            CFSTR("Quit"), nullptr, nullptr, &answer);
        _exit(1);
    }
    *slot = rbm_stock_dispatch((new std::vector<uintptr_t>(std::move(targets)))->data());  // kept for the app's lifetime
    plat::onMainAfter(3, [] {  // once the app is up; shown by macOS, so it doesn't block rekordbox
        CFUserNotificationDisplayNotice(0, kCFUserNotificationCautionAlertLevel, nullptr, nullptr, nullptr,
            CFSTR("platendoos is off for this run"),
            CFSTR("This app and its mods come from different platendoos builds, so no mod was started. rekordbox works "
                  "as usual.\n\nReinstall platendoos (build.sh, or the platendoos patcher) to fix this."),
            CFSTR("OK"));
    });
}

// The dylib's entry point: rekordbox's slide, the patched binary's hook slot, then core's start().
__attribute__((constructor)) static void rbm_init() {
    using namespace rbm;
    for (uint32_t i = 0; i < _dyld_image_count(); i++) {
        const struct mach_header* h = _dyld_get_image_header(i);
        if (h->filetype == MH_EXECUTE) { g_slide = _dyld_get_image_vmaddr_slide(i); break; }
    }
    void** slot = dispatchSlot();
    if (slot && g_markerCount) return runStock(slot);
    if (slot) *slot = rbm_fill_hook_table();
    else logf("rbmods: this rekordbox wasn't patched by this version of patch_binary.py (no hook marker); hooks are off");
    start();
}
#endif
