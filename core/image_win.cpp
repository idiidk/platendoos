// rekordbox's image on Windows. rekordbox.exe is stripped, so its addresses, fields and hook sites come from a
// table made for each Windows build by tools/win (which matches the Windows build against the mac build of the
// same version, whose symbols name everything): rbmods/rekordbox-<build>.tbl next to rbmods.dll. The mods keep
// naming things by their mac (Itanium) symbols; the table maps those names to this build.
//
// Table lines (addresses are RVAs, numbers hex):
//   build <TimeDateStamp>-<SizeOfImage>
//   addr <name>[+plus] <rva>            an rbm::Addr by name (mangled symbol, or a finder's description)
//   off <fn>|<kind>|<nth> <value>       an rbm::Offset by its mac recipe
//   hook <name> <call|jmp|icall|lea> <site rva> <target rva>   a hooks.def call site (icall: call [rip+x], 6 bytes;
//                                       lea: lea reg,[rip+target], the function's address taken for a callback table);
//                                       repeated for a call the Windows build makes in more places (inlined callers)
//   orig <name> <rva>                   a hook's original target when the site doesn't call the mac target
//   hookreg <name> <reg> <arg>          the hook needs a register of the caller (rbx, rsi, rdi, rbp, r12-r15) as
//                                       argument <arg> (0-3) of rbm_<name> (the mac trampolines' x19/x20)
// A hook, address or field the table doesn't have is off, and so is the mod that needs it (checkBuild()).
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include "rbmod.h"
#include <string.h>
#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

// Every global of this file (the table, g_build...) is constructed in the "lib" initialiser segment, before the
// mods' statics, and in file order, so g_imageInit (last in the file) finds them built. Only the ImageInit
// object in lib while the table stayed in the normal segment crashed: loadTable wrote into unbuilt maps.
#pragma warning(disable : 4075)
#pragma init_seg(lib)

namespace rbm::plat { void startMainThread(); }
namespace rbm { void modOffForHooks(const char* mod); }
extern "C" void* rbm_hook_addr(int id);   // hooks_win.cpp: rbm_hook_<name> by HookId
extern "C" void* rbm_hook_regfn(int id);  // rbm_<name>, for hooks with a hookreg line

namespace rbm {

void start();

namespace {

struct Table {
    std::string build;
    std::unordered_map<std::string, uintptr_t> addr;  // name -> rva
    std::unordered_map<std::string, size_t> off;
    struct Hook { int kind; uintptr_t site, target; std::vector<uintptr_t> more; };  // kind 0 call, 1 jmp, 2 icall, 3 lea
    std::unordered_map<std::string, Hook> hooks;
    std::unordered_map<std::string, uintptr_t> orig;
    struct Reg { int reg, arg; };
    std::unordered_map<std::string, Reg> regs;
    std::unordered_map<std::string, intptr_t> adj;  // name -> this delta
    bool loaded = false;
};

uintptr_t g_base;       // where rekordbox.exe is loaded
uintptr_t g_preferred;  // its preferred ImageBase: "unslid" addresses are preferred + rva, as on mac
std::string g_build;
Table g_table;
std::vector<uint8_t> g_installed(HK_COUNT);
struct Saved { uint8_t bytes[6]; int len; };
std::vector<std::vector<std::pair<uintptr_t, Saved>>> g_saved(HK_COUNT);  // per hook: site, its bytes
const char* const kHookNames[] = {
#define HOOK(name, c_, n_, k_, t_, kind_) #name,
#include "../hooks.def"
#undef HOOK
};

std::string buildId() {
    auto dos = (const IMAGE_DOS_HEADER*)g_base;
    auto nt = (const IMAGE_NT_HEADERS64*)(g_base + dos->e_lfanew);
    char b[40];
    snprintf(b, sizeof b, "%08lX-%lX", (unsigned long)nt->FileHeader.TimeDateStamp, (unsigned long)nt->OptionalHeader.SizeOfImage);
    return b;
}

void loadTable() {
    std::string path = plat::resourcesDir() + "/rekordbox-" + g_build + ".tbl", text;
    if (!plat::readFile(path, &text)) return;
    size_t pos = 0;
    while (pos < text.size()) {
        size_t e = text.find('\n', pos);
        if (e == std::string::npos) e = text.size();
        std::string line = text.substr(pos, e - pos);
        pos = e + 1;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty() || line[0] == '#') continue;
        std::vector<std::string> f;
        for (size_t a = 0, b; a <= line.size(); a = b + 1) {
            b = line.find(' ', a);
            if (b == std::string::npos) b = line.size();
            f.push_back(line.substr(a, b - a));
        }
        auto hex = [](const std::string& s) { return (uintptr_t)strtoull(s.c_str(), nullptr, 16); };
        if (f[0] == "build" && f.size() >= 2) g_table.build = f[1];
        else if (f[0] == "addr" && f.size() >= 3)  // a finder's name may hold spaces: the value is last
            g_table.addr[line.substr(5, line.rfind(' ') - 5)] = hex(f.back());
        else if (f[0] == "off" && f.size() >= 3)  // the key (fn|kind|nth) may hold spaces: the value is last
            g_table.off[line.substr(4, line.rfind(' ') - 4)] = hex(f.back());
        else if (f[0] == "adj" && f.size() >= 3)
            g_table.adj[line.substr(4, line.rfind(' ') - 4)] = (intptr_t)strtoll(f.back().c_str(), nullptr, 0);
        else if (f[0] == "orig" && f.size() >= 3) g_table.orig[f[1]] = hex(f[2]);
        else if (f[0] == "hookreg" && f.size() >= 4) {
            static const char* names[16] = {"rax", "rcx", "rdx", "rbx", "rsp", "rbp", "rsi", "rdi",
                                            "r8", "r9", "r10", "r11", "r12", "r13", "r14", "r15"};
            for (int r = 0; r < 16; r++)
                if (f[2] == names[r]) g_table.regs[f[1]] = {r, atoi(f[3].c_str())};
        } else if (f[0] == "hook" && f.size() >= 5) {
            Table::Hook h{f[2] == "call" ? 0 : f[2] == "jmp" ? 1 : f[2] == "lea" ? 3 : 2, hex(f[3]), hex(f[4]), {}};
            auto it = g_table.hooks.find(f[1]);
            if (it == g_table.hooks.end()) g_table.hooks[f[1]] = h;
            else if (it->second.kind == h.kind && it->second.target == h.target) it->second.more.push_back(h.site);
        }
    }
    g_table.loaded = g_table.build == g_build;
}

// Jump thunks within +-2 GB of rekordbox's code, so a rel32 call can reach the hooks: jmp [rip+0]; dq target.
uint8_t* thunkPage() {
    static uint8_t* page = [] {
        SYSTEM_INFO si;
        GetSystemInfo(&si);
        auto dos = (const IMAGE_DOS_HEADER*)g_base;
        auto nt = (const IMAGE_NT_HEADERS64*)(g_base + dos->e_lfanew);
        uintptr_t end = g_base + nt->OptionalHeader.SizeOfImage;
        for (uintptr_t a = (end + 0xFFFF) & ~(uintptr_t)0xFFFF; a < end + 0x70000000; a += 0x10000)
            if (void* p = VirtualAlloc((void*)a, 0x10000, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE)) return (uint8_t*)p;
        for (uintptr_t a = (g_base - 0x10000) & ~(uintptr_t)0xFFFF; a > g_base - 0x70000000; a -= 0x10000)
            if (void* p = VirtualAlloc((void*)a, 0x10000, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE)) return (uint8_t*)p;
        return (uint8_t*)nullptr;
    }();
    return page;
}

// reg >= 0: first "mov <argument arg's register>, <reg>" (the caller's value as an extra argument).
uint8_t* makeThunk(void* target, int reg = -1, int arg = 0) {
    static size_t used;
    uint8_t* page = thunkPage();
    if (!page || used + 32 > 0x10000) return nullptr;
    uint8_t* t = page + used;
    uint8_t* p = t;
    if (reg >= 0) {
        static const int argRegs[4] = {1, 2, 8, 9};  // rcx, rdx, r8, r9
        int dst = argRegs[arg & 3];
        *p++ = (uint8_t)(0x48 | (reg >= 8 ? 4 : 0) | (dst >= 8 ? 1 : 0));  // REX.W (+R src, +B dst)
        *p++ = 0x89;                                                       // mov r/m64, r64
        *p++ = (uint8_t)(0xC0 | (reg & 7) << 3 | (dst & 7));
    }
    *p++ = 0xFF, *p++ = 0x25;  // jmp [rip+0]
    memset(p, 0, 4);
    memcpy(p + 4, &target, 8);
    used += 32;
    return t;
}

bool rel32(uintptr_t from, uintptr_t to, int32_t* out) {
    intptr_t d = (intptr_t)to - (intptr_t)from;
    if (d < INT32_MIN || d > INT32_MAX) return false;
    *out = (int32_t)d;
    return true;
}

// Routes one hooks.def call site to rbm_hook_<name>, after checking the bytes are the call the table expects.
bool siteIsCall(const Table::Hook& h, uint8_t* site) {
    if (h.kind == 2) return site[0] == 0xFF && site[1] == 0x15;  // call [rip+disp32] through the import table
    if (h.kind == 3) {  // REX.W lea reg,[rip+disp32]
        int32_t d;
        memcpy(&d, site + 3, 4);
        return (site[0] & 0xFB) == 0x48 && site[1] == 0x8D && (site[2] & 0xC7) == 0x05 && (uintptr_t)(site + 7 + d) == g_base + h.target;
    }
    if (site[0] != (h.kind == 0 ? 0xE8 : 0xE9)) return false;
    int32_t d;
    memcpy(&d, site + 1, 4);
    return (uintptr_t)(site + 5 + d) == g_base + h.target;
}

void unpatch(int id) {
    for (auto& [site, saved] : g_saved[id]) plat::patchMemory((void*)site, saved.bytes, saved.len);
    g_saved[id].clear();
}

bool installHook(int id, const char* name) {
    auto it = g_table.hooks.find(name);
    if (it == g_table.hooks.end()) return false;
    const Table::Hook& h = it->second;
    std::vector<uintptr_t> sites{h.site};
    sites.insert(sites.end(), h.more.begin(), h.more.end());
    for (uintptr_t s : sites)
        if (!siteIsCall(h, (uint8_t*)(g_base + s))) return false;
    auto reg = g_table.regs.find(name);
    void* fn = reg != g_table.regs.end() ? rbm_hook_regfn(id) : rbm_hook_addr(id);
    if (!fn) return false;  // its mod isn't built for Windows
    uint8_t* thunk = reg != g_table.regs.end() ? makeThunk(fn, reg->second.reg, reg->second.arg) : makeThunk(fn);
    if (!thunk) return false;
    int len = h.kind == 2 ? 6 : 5;
    for (uintptr_t s : sites) {
        uint8_t* site = (uint8_t*)(g_base + s);
        int32_t d;
        if (h.kind == 3) {  // only the displacement changes: the register gets the thunk's address
            if (!rel32((uintptr_t)site + 7, (uintptr_t)thunk, &d)) return unpatch(id), false;
            Saved saved;
            memcpy(saved.bytes, site + 3, 4);
            saved.len = 4;
            if (!plat::patchMemory(site + 3, &d, 4)) return unpatch(id), false;
            g_saved[id].push_back({(uintptr_t)site + 3, saved});
            continue;
        }
        if (!rel32((uintptr_t)site + 5, (uintptr_t)thunk, &d)) return unpatch(id), false;
        uint8_t code[6] = {(uint8_t)(h.kind == 1 ? 0xE9 : 0xE8), 0, 0, 0, 0, 0x90};
        memcpy(code + 1, &d, 4);
        Saved saved;
        memcpy(saved.bytes, site, len);
        saved.len = len;
        if (!plat::patchMemory(site, code, len)) return unpatch(id), false;
        g_saved[id].push_back({(uintptr_t)site, saved});
    }
    return true;
}

// A mod whose hooks aren't all in place would run half-hooked (a lock taken but never released, a column
// half-added): its other hooks are taken out again and the mod is off. The mod of each hook comes from
// tools/win/build.sh (the source file defining rbm_hook_<name>; core files by their own name).
extern "C" const char* const rbm_hook_mod[];

void dropHalfHookedMods() {
    std::map<std::string, std::string> off;  // mod -> a hook it lacks
    for (int i = 0; i < HK_COUNT; i++)
        if (!g_installed[i] && rbm_hook_mod[i][0]) off.emplace(rbm_hook_mod[i], kHookNames[i]);
    for (int i = 0; i < HK_COUNT; i++) {
        if (!g_installed[i] || !off.count(rbm_hook_mod[i])) continue;
        unpatch(i);
        g_installed[i] = 0;
    }
    for (auto& [mod, hook] : off) {
        modOffForHooks(mod.c_str());
        logf("rbmods: %s is off in this rekordbox build: hook %s has no site", mod.c_str(), hook.c_str());
    }
}

// rekordbox's entry point is patched to come here first: we're on its main thread, before its own code and
// outside the loader lock, so the main-thread window can be made; then on to the real entry point.
uint8_t g_entrySaved[14];
uintptr_t g_entry;

int entryHook() {
    plat::patchMemory((void*)g_entry, g_entrySaved, sizeof g_entrySaved);
    plat::startMainThread();
    start();
    return ((int (*)())g_entry)();
}

void hookEntry() {
    auto dos = (const IMAGE_DOS_HEADER*)g_base;
    auto nt = (const IMAGE_NT_HEADERS64*)(g_base + dos->e_lfanew);
    g_entry = g_base + nt->OptionalHeader.AddressOfEntryPoint;
    memcpy(g_entrySaved, (void*)g_entry, sizeof g_entrySaved);
    uint8_t code[14] = {0xFF, 0x25, 0, 0, 0, 0};  // jmp [rip+0]; dq entryHook
    void* to = (void*)&entryHook;
    memcpy(code + 6, &to, 8);
    plat::patchMemory((void*)g_entry, code, sizeof code);
}

}  // namespace

// A mod switched off later, for an address or field this build lacks (symbols.cpp checkBuild), mustn't leave
// its hooks in: they'd run in code the table may have placed wrongly. all: every mod is off. Main thread,
// before rekordbox's own code runs. Returns how many hooks were taken out.
int unpatchOffMods(bool all, const std::set<std::string>& mods) {
    int n = 0;
    for (int i = 0; i < HK_COUNT; i++) {
        if (!g_installed[i] || !(all || mods.count(rbm_hook_mod[i]))) continue;
        unpatch(i);
        g_installed[i] = 0;
        n++;
    }
    return n;
}

bool imageResolve(const Addr& a, uintptr_t* out) {
    std::string key = a.name;
    if (a.plus) {
        char b[24];
        snprintf(b, sizeof b, "+0x%llx", (unsigned long long)a.plus);
        key += b;
    }
    auto it = g_table.addr.find(key);
    *out = it == g_table.addr.end() ? 0 : g_preferred + it->second;
    return true;  // never the mac finders
}

bool imageResolve(const Offset& o, size_t* out, bool* found) {
    auto it = g_table.off.find(std::string(o.fn) + "|" + o.kind + "|" + std::to_string(o.nth));
    *found = it != g_table.off.end();
    *out = *found ? it->second : o.expected;
    return true;
}

void imageDone() {
    g_table.addr.clear();
    g_table.off.clear();
}

intptr_t thisDelta(const Addr& a) {
    auto it = g_table.adj.find(a.name);
    return it == g_table.adj.end() ? 0 : it->second;
}

uintptr_t symbol(const char* name) {
    auto it = g_table.addr.find(name);
    return it == g_table.addr.end() ? 0 : g_preferred + it->second;
}
// The arm64 code readers have no x64 counterpart: finders resolve through the table by their name instead.
uint32_t insn(uintptr_t) { return 0; }
uintptr_t callSite(const char*, const char*, int) { return 0; }
uintptr_t adrpAddBefore(uintptr_t, int) { return 0; }
uintptr_t adrpAdd(const char*, int) { return 0; }
uintptr_t singleton(const char*, const char*) { return 0; }
bool immediate(const char*, const char*, int, size_t*) { return false; }

extern "C" void* __RTDynamicCast(void* obj, long vfDelta, void* srcType, void* dstType, int isReference);
void* dynCast(void* obj, const void* srcType, const void* dstType) {
    // MSVC's runtime compares type descriptors by name, so our CRT's copy works on rekordbox's objects
    return obj ? __RTDynamicCast(obj, 0, (void*)srcType, (void*)dstType, 0) : nullptr;
}

std::string rekordboxBuild() { return g_build; }
// The rekordbox build platendoos was tested on (7.2.19); other builds with a table work too, but untested.
// (The field recipes' expected values are mac's: checkBuild compares them on macOS only.)
bool testedBuild() { return g_build == "6A97B2E4-6573000"; }
// rekordbox 7.2.20: every Windows mod was run on it with its shipped table (2026-11-19, docs/windows.md).
bool checkedBuild() { return testedBuild() || g_build == "6AFC1795-66AA000"; }
bool hookPatched(HookId h) { return h < HK_COUNT && g_installed[h]; }

// Hook originals that differ from the mac target (kHookOrig is built from hooks.def's targets).
// An icall site's original: the function its import slot holds (juce's CriticalSection::enter/exit are
// inlined on Windows into calls of EnterCriticalSection/LeaveCriticalSection on the same object).
uintptr_t importTarget(uintptr_t site) {
    int32_t d;
    memcpy(&d, (void*)(site + 2), 4);
    return *(uintptr_t*)(site + 6 + d) - g_slide;  // kHookOrig values are unslid, like the table's
}

void fixHookOrigs() {
    int i = 0;
#define HOOK(name, c_, n_, k_, t_, kind_) \
    if (auto it = g_table.orig.find(#name); it != g_table.orig.end()) kHookOrig[i].value = g_preferred + it->second; \
    else if (auto h = g_table.hooks.find(#name); h != g_table.hooks.end() && h->second.kind != 2) kHookOrig[i].value = g_preferred + h->second.target; \
    else if (h != g_table.hooks.end()) kHookOrig[i].value = importTarget(g_base + h->second.site); \
    i++;
#include "../hooks.def"
#undef HOOK
}

// Before any static initialiser of the mods resolves an Addr: called from the DLL's first initialiser.
void imageInit() {
    g_base = (uintptr_t)GetModuleHandleW(nullptr);
    auto dos = (const IMAGE_DOS_HEADER*)g_base;
    auto nt = (const IMAGE_NT_HEADERS64*)(g_base + dos->e_lfanew);
    g_preferred = nt->OptionalHeader.ImageBase;
    g_slide = (intptr_t)(g_base - g_preferred);
    g_build = buildId();
    loadTable();
}

}  // namespace rbm

// Runs before the mods' static initialisers (init_seg(lib), top of this file), so the Addrs and Offsets they
// declare as statics resolve through a loaded table.
static struct ImageInit { ImageInit() { rbm::imageInit(); } } g_imageInit;

namespace rbm::plat {  // core/plat_win.cpp: the mods' threads run below normal priority
void workerPriorityInit();
void workerThreadAttach();
}

BOOL WINAPI DllMain(HINSTANCE, DWORD reason, LPVOID) {
    using namespace rbm;
    if (reason == DLL_THREAD_ATTACH) plat::workerThreadAttach();
    if (reason != DLL_PROCESS_ATTACH) return TRUE;
    // Statics are constructed by now (the CRT runs them before DllMain).
    plat::workerPriorityInit();
    if (!g_table.loaded)
        logf("rbmods: no table for rekordbox build %s (rbmods/rekordbox-%s.tbl): every mod is off", g_build.c_str(), g_build.c_str());
    fixHookOrigs();
    int i = 0, ok = 0;
#define HOOK(name, caller, nth, count, target, kind) ok += (g_installed[i] = installHook(i, #name)); i++;
#include "../hooks.def"
#undef HOOK
    dropHalfHookedMods();
    ok = 0;
    for (int h = 0; h < HK_COUNT; h++) ok += g_installed[h];
    logf("rbmods: %d of %d hooks in place", ok, (int)HK_COUNT);
    hookEntry();
    return TRUE;
}
