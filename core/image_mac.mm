// rekordbox's image on macOS: its symbol table and arm64 code (see symbols.cpp for the registry).
// Finds rekordbox's addresses and field offsets at load, from its symbol table (rekordbox isn't stripped) and
// a few instructions of its own code, so the mods follow an update that moves things. rbm::Addr and rbm::Offset
// (rbmod.h) resolve themselves when they're constructed; checkBuild() then switches off the mods whose
// addresses or fields weren't found and logs it. tools/dev/rbbin.py and tools/dev/recipes.py read the binary file the
// same way (keep the decoders in step).
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
#include <unistd.h>
#include <algorithm>
#include <map>
#include <set>
#include <string_view>
#include <unordered_map>

namespace rbm {

static const char* kTestedBuild = "D827BD186918357A9F57C506CF428700";  // LC_UUID of rekordbox 7.2.19 arm64
static const uintptr_t kTestedSlot = 0x1053be860;                     // its dispatch slot (patchers before the marker)

namespace {

struct Image {
    const mach_header_64* header = nullptr;
    intptr_t slide = 0;
    uintptr_t textLo = 0, textHi = 0;  // __TEXT,__text, unslid
    std::string uuid;
    std::unordered_map<std::string_view, uintptr_t> byName;  // unslid; freed by checkBuild()
    std::vector<uintptr_t> starts;     // function starts in __text, sorted
    const nlist_64* syms = nullptr;
    uint32_t nsyms = 0;
    const char* strs = nullptr;
};

Image& image() {
    static Image* img = [] {
        Image* m = new Image;
        for (uint32_t i = 0; i < _dyld_image_count(); i++) {
            const mach_header* h = _dyld_get_image_header(i);
            if (h->filetype == MH_EXECUTE) {
                m->header = (const mach_header_64*)h;
                m->slide = _dyld_get_image_vmaddr_slide(i);
                break;
            }
        }
        if (!m->header) return m;
        const segment_command_64* linkedit = nullptr;
        const symtab_command* symtab = nullptr;
        const load_command* lc = (const load_command*)(m->header + 1);
        for (uint32_t i = 0; i < m->header->ncmds; i++, lc = (const load_command*)((const char*)lc + lc->cmdsize)) {
            if (lc->cmd == LC_SEGMENT_64) {
                const segment_command_64* seg = (const segment_command_64*)lc;
                if (!strcmp(seg->segname, SEG_LINKEDIT)) linkedit = seg;
                const section_64* sect = (const section_64*)(seg + 1);
                for (uint32_t s = 0; s < seg->nsects; s++)
                    if (!strcmp(sect[s].segname, "__TEXT") && !strcmp(sect[s].sectname, "__text"))
                        m->textLo = sect[s].addr, m->textHi = sect[s].addr + sect[s].size;
            } else if (lc->cmd == LC_SYMTAB) {
                symtab = (const symtab_command*)lc;
            } else if (lc->cmd == LC_UUID) {
                char buf[33];
                const uint8_t* u = ((const uuid_command*)lc)->uuid;
                for (int k = 0; k < 16; k++) snprintf(buf + 2 * k, 3, "%02X", u[k]);
                m->uuid = buf;
            }
        }
        if (!linkedit || !symtab) return m;
        const char* base = (const char*)(linkedit->vmaddr + m->slide - linkedit->fileoff);
        m->syms = (const nlist_64*)(base + symtab->symoff);
        m->nsyms = symtab->nsyms;
        m->strs = base + symtab->stroff;
        m->byName.reserve(m->nsyms);
        for (uint32_t i = 0; i < m->nsyms; i++) {
            const nlist_64& n = m->syms[i];
            if ((n.n_type & N_STAB) || (n.n_type & N_TYPE) != N_SECT || !n.n_value) continue;
            m->byName.emplace(m->strs + n.n_un.n_strx, n.n_value);
            if (n.n_value >= m->textLo && n.n_value < m->textHi) m->starts.push_back(n.n_value);
        }
        std::sort(m->starts.begin(), m->starts.end());
        m->starts.erase(std::unique(m->starts.begin(), m->starts.end()), m->starts.end());
        return m;
    }();
    return *img;
}

uintptr_t functionEnd(uintptr_t fn) {
    const Image& m = image();
    auto it = std::upper_bound(m.starts.begin(), m.starts.end(), fn);
    return it == m.starts.end() ? m.textHi : *it;
}

int64_t sext(uint64_t v, int bits) { return (int64_t)(v << (64 - bits)) >> (64 - bits); }

uintptr_t blTarget(uintptr_t a, uint32_t w) {
    return (w & 0xFC000000) == 0x94000000 ? a + 4 * sext(w & 0x3FFFFFF, 26) : 0;
}
bool isAdrp(uint32_t w) { return (w & 0x9F000000) == 0x90000000; }
uintptr_t adrpPage(uintptr_t a, uint32_t w) {
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

std::vector<uint8_t> g_patched;  // per hook, from the patcher's marker
uint32_t g_markerCount;          // the marker's hook count, when it isn't HK_COUNT (0: it matches)
uintptr_t g_markerAt;            // where the marker is (unslid)

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

}  // namespace

extern "C" void* __dynamic_cast(const void* sub, const void* srcType, const void* dstType, ptrdiff_t hint);
void* dynCast(void* obj, const void* srcType, const void* dstType) { return __dynamic_cast(obj, srcType, dstType, -1); }

// macOS reads everything from the binary itself.
bool imageResolve(const Addr&, uintptr_t*) { return false; }
bool imageResolve(const Offset&, size_t*, bool*) { return false; }

uintptr_t symbol(const char* name) {
    if (!name) return 0;
    const Image& m = image();
    if (!strncmp(name, "stub:_", 6)) {  // an imported function: where the stub's binding points
        void* p = dlsym(RTLD_DEFAULT, name + 6);
        return p ? (uintptr_t)p - m.slide : 0;
    }
    if (!m.byName.empty()) {
        auto it = m.byName.find(name);
        return it == m.byName.end() ? 0 : it->second;
    }
    for (uint32_t i = 0; i < m.nsyms; i++) {  // after checkBuild() freed the index
        const nlist_64& n = m.syms[i];
        if (!(n.n_type & N_STAB) && (n.n_type & N_TYPE) == N_SECT && !strcmp(m.strs + n.n_un.n_strx, name)) return n.n_value;
    }
    return 0;
}

uint32_t insn(uintptr_t a) { return *(const uint32_t*)(a + image().slide); }

uintptr_t callSite(const char* caller, const char* target, int nth) {
    uintptr_t fn = symbol(caller), t = symbol(target);
    if (!fn || !t) return 0;
    for (uintptr_t a = fn, end = functionEnd(fn); a < end; a += 4)
        if (blTarget(a, insn(a)) == t && nth-- == 0) return a;
    return 0;
}

uintptr_t adrpAddBefore(uintptr_t site, int reg) {
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
}

uintptr_t adrpAdd(const char* name, int nth) {
    uintptr_t fn = symbol(name);
    if (!fn) return 0;
    for (uintptr_t a = fn, end = functionEnd(fn); a + 4 < end; a += 4) {
        uint32_t w = insn(a), next = insn(a + 4);
        Decoded d;
        if (isAdrp(w) && decode(next, &d) && !strcmp(d.kind, "add") && d.rn == (int)(w & 31) && nth-- == 0)
            return adrpPage(a, w) + d.imm;
    }
    return 0;
}

uintptr_t singleton(const char* caller, const char* get) { return adrpAddBefore(callSite(caller, get, 0), 0); }

bool immediate(const char* name, const char* kind, int nth, size_t* out) {
    uintptr_t fn = symbol(name);
    if (!fn || !kind) return false;
    if (!strncmp(kind, "vt:", 3)) {  // a method's slot in the vtable `name`, from its address point (+0x10)
        uintptr_t method = symbol(kind + 3);
        if (!method) return false;
        const uintptr_t* slots = (const uintptr_t*)(fn + 0x10 + image().slide);
        for (int i = 0; i < 1024; i++)
            if (slots[i] == method + image().slide && nth-- == 0) return *out = 8 * i, true;
        return false;
    }
    if (!strcmp(kind, "mov")) {  // a 32-bit constant: movz, then movk into the same register
        int n = -1, rd = -1;
        size_t val = 0;
        for (uintptr_t a = fn, end = functionEnd(fn); a < end; a += 4) {
            uint32_t w = insn(a);
            if ((w & 0x7F800000) == 0x52800000) {
                rd = w & 31, val = (size_t)((w >> 5) & 0xFFFF) << (16 * ((w >> 21) & 3)), n++;
            } else if ((w & 0x7F800000) == 0x72800000 && rd == (int)(w & 31)) {
                val |= (size_t)((w >> 5) & 0xFFFF) << (16 * ((w >> 21) & 3));
                if (n == nth) return *out = val, true;
            }
        }
        return false;
    }
    for (uintptr_t a = fn, end = functionEnd(fn); a < end; a += 4) {
        Decoded d;
        if (decode(insn(a), &d) && kindMatches(kind, d) && nth-- == 0) return *out = d.imm, true;
    }
    return false;
}

std::string rekordboxBuild() { return image().uuid; }
bool testedBuild() { return image().uuid == kTestedBuild; }
// rekordbox 7.2.20 arm64: every mod was run on it (2026-11-19, docs/rekordbox-updates.md).
bool checkedBuild() { return testedBuild() || image().uuid == "4AF5D59D5EC937DCBD893FBC84D970AD"; }

// The patcher's marker in front of its stubs: "RBMHOOKS", slot, hook count, 0, one byte per hook.
void** dispatchSlot() {
    const Image& m = image();
    if (!m.header) return nullptr;
    const char* p = (const char*)(m.header + 1) + m.header->sizeofcmds;
    const char* end = (const char*)(m.textLo + m.slide);
    for (; p + 24 <= end; p += 8) {
        if (memcmp(p, "RBMHOOKS", 8)) continue;
        uint64_t slot;
        uint32_t count;
        memcpy(&slot, p + 8, 8);
        memcpy(&count, p + 16, 4);
        g_patched.assign((const uint8_t*)p + 24, (const uint8_t*)p + 24 + std::min<uint32_t>(count, 4096));
        if (count != HK_COUNT) g_markerCount = count, g_markerAt = (uintptr_t)p - m.slide;  // rbm_init runs it stock
        return (void**)(slot + m.slide);
    }
    if (!testedBuild()) return nullptr;
    g_patched.assign(HK_COUNT, 1);
    return (void**)(kTestedSlot + m.slide);
}

intptr_t thisDelta(const Addr&) { return 0; }

bool hookPatched(HookId h) { return h < (int)g_patched.size() && g_patched[h]; }


// After checkBuild(): the name index isn't needed any more.
void imageDone() {
    Image& m = image();
    m.byName.clear();
    m.byName.rehash(0);
}

bool imageHasCode() { return image().header != nullptr; }

void start();

}  // namespace rbm

extern "C" void* rbm_fill_hook_table();
extern "C" void* rbm_stock_dispatch(const uintptr_t* targets);

// The app was patched from another hooks.def than this dylib was built with (install.sh after a hooks.def change,
// or a newer dylib on an older app): stub i isn't hook i any more, and every stub jumps through the slot, so the
// slot can't stay empty either. Each stub goes straight to its call site's original target instead and no mod
// starts: rekordbox runs as stock. Without the original rekordbox to read those targets from, it can't run at all.
static void runStock(void** slot) {
    using namespace rbm;
    logf("rbmods: the app was patched with %u hooks, this build of the mods has %d (install.sh after a hooks.def change?): "
         "platendoos is off for this run; rebuild the app with build.sh or reinstall it with the patcher",
         g_markerCount, (int)HK_COUNT);
    std::vector<uintptr_t> targets;
    if (!stubTargets(&targets)) {
        logf("rbmods: the unpatched rekordbox this app was made from isn't in /Applications: it can't run stock, quitting");
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

namespace rbm {

}  // namespace rbm
