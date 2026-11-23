// The registry of rekordbox addresses and fields the mods use (rbm::Addr, rbm::Offset, rbmod.h): resolved at
// load by the platform's image reader (image_mac.mm: symbol table and arm64 code; image_win.cpp: the per-build
// table made by tools/win), and checkBuild() then switches off the mods whose addresses or fields weren't found.
#include "rbmod.h"
#include <string.h>
#include <map>
#include <set>

namespace rbm {

void imageDone();
#ifdef _WIN32
int unpatchOffMods(bool all, const std::set<std::string>& mods);  // image_win.cpp
#endif
bool imageResolve(const Addr& a, uintptr_t* out);    // platform override (Windows table); false: use name/finder
bool imageResolve(const Offset& o, size_t* out, bool* found);

namespace {

// ---- registry -------------------------------------------------------------------------------

std::vector<const Addr*>& addrs() { static std::vector<const Addr*> v; return v; }
std::vector<const Offset*>& offsets() { static std::vector<const Offset*> v; return v; }
std::set<std::string> g_off;    // mods switched off for this build
bool g_coreOff = false;

void resolve(const Addr& a) {
    uintptr_t v;
    if (imageResolve(a, &v)) { a.value = v; return; }
    v = a.find ? a.find() : symbol(a.name);
    a.value = v ? v + a.plus : 0;
}

void resolve(const Offset& o) {
    size_t v;
    bool found;
    if (imageResolve(o, &v, &found)) {
        o.value = found ? v : o.expected;
        o.ok = found;
        return;
    }
    // In the x86_64 slice the instruction recipes give the arm64 value (image_mac.mm reads them from the arm64 half of
    // the file): plus the X86{} difference.
    if (immediate(o.fn, o.kind, o.nth, &v)) {
        o.value = v + (RBM_MAC_X86 ? o.x86Delta : 0);
        o.ok = true;
    } else {
        o.value = o.expectedHere();
        o.ok = false;
    }
}

// The mod a source file belongs to: mods/analyzer_panel.mm -> "analyzer", core/... -> "core".
std::string modOf(const char* file) {
    std::string f = file;
    if (f.find("/core/") != std::string::npos) return "core";
    if (f.find("hooks.def") != std::string::npos) return "";
    f = f.substr(f.rfind('/') + 1);
    f = f.substr(0, f.find('.'));
    if (f.size() > 6 && !f.compare(f.size() - 6, 6, "_panel")) f.resize(f.size() - 6);
    return f;
}

}  // namespace

void* patchSlot(void** slot, void* fn) {
    void* old = *slot;
    return plat::patchMemory(slot, &fn, sizeof fn) ? old : nullptr;
}

Addr::Addr(const char* name, intptr_t plus, const char* file) : name(name), plus(plus), find(nullptr), file(file) {
    addrs().push_back(this);
    resolve(*this);
}
Addr::Addr(const char* what, uintptr_t (*find)(), const char* file) : name(what), plus(0), find(find), file(file) {
    addrs().push_back(this);
    resolve(*this);
}
Offset::Offset(size_t expected, const char* fn, const char* kind, int nth, const char* file) : Offset(expected, fn, kind, nth, X86(), file) {}
Offset::Offset(size_t expected, const char* fn, const char* kind, int nth, X86 x86, const char* file)
    : expected(expected), fn(fn), kind(kind), nth(nth), x86Delta(x86.set ? (intptr_t)x86.value - (intptr_t)expected : 0), file(file), value(expected) {
    offsets().push_back(this);
    resolve(*this);
}

// Each hook's original target (RBM_ORIG): the target of its hooks.def line. In the x86_64 slice that is its first
// HOOK_X86 line's where it has one ("" for a hook never patched there: no address); image_mac.mm then takes the
// target of the site the patcher actually patched from the marker's table of originals (imageResolve).
#if RBM_MAC_X86
namespace {
struct X86Target { const char* name; const char* target; };
constexpr X86Target kX86Targets[] = {
#define HOOK(name, caller, nth, count, target, kind)
#define HOOK_X86(name, caller, nth, count, target, kind) {#name, (caller)[0] ? target : ""},
#include "../hooks.def"
#undef HOOK_X86
#undef HOOK
    {"", ""},
};
constexpr bool sameName(const char* a, const char* b) {
    while (*a && *a == *b) a++, b++;
    return *a == *b;
}
constexpr const char* hookTarget(const char* name, const char* target) {
    for (const X86Target& t : kX86Targets)
        if (sameName(t.name, name)) return t.target;
    return target;
}
}  // namespace
#define RBM_HOOK_TARGET(name, target) hookTarget(#name, target)
#else
#define RBM_HOOK_TARGET(name, target) target
#endif
const Addr kHookOrig[HK_COUNT] = {
#define HOOK(name, caller, nth, count, target, kind) {RBM_HOOK_TARGET(name, target), intptr_t(0), "hooks.def"},
#include "../hooks.def"
#undef HOOK
};
#undef RBM_HOOK_TARGET

// Windows: a mod some of whose hooks have no site in this build (image_win.cpp).
void modOffForHooks(const char* mod) { g_off.insert(mod); }

bool versionOff(const char* mod) { return g_coreOff || g_off.count(mod); }

// Which mods can run in this build; logs what was missing. Main thread, once every Addr and Offset exists.
void checkBuild() {
    std::map<std::string, std::vector<std::string>> missing;  // mod -> what
    for (const Addr* a : addrs())
        if (!a->value) missing[modOf(a->file)].push_back(a->name);
#ifdef _WIN32
    bool tested = false;  // the recipes' expected values are mac's: on Windows a field the table lacks is missing
#else
    bool tested = testedBuild();
#endif
    int selfTest = 0;
    for (const Offset* o : offsets()) {
        if (o->ok && o->value == o->expectedHere()) continue;
        if (tested) {  // the expected value is right here: a recipe that disagrees is out of date
            selfTest++;
            logf("rbmods: field recipe %s / %s #%d gives %s, expected %#zx (%s)", o->fn, o->kind, o->nth,
                 o->ok ? "another value" : "nothing", o->expectedHere(), o->file);
            o->value = o->expectedHere();
        } else if (!o->ok) {
            missing[modOf(o->file)].push_back(std::string("field in ") + o->fn);
        } else {
#ifndef _WIN32  // Windows: every field differs from mac's expected value; the table is the source
            logf("rbmods: field from %s moved: %#zx -> %#zx", o->fn, o->expectedHere(), o->value);
#endif
        }
    }
    for (auto& [mod, what] : missing) {
        if (mod.empty()) continue;
        if (mod == "core") g_coreOff = true;
        else g_off.insert(mod);
        std::string list;
        for (size_t i = 0; i < what.size() && i < 3; i++) list += (i ? ", " : "") + what[i];
        logf("rbmods: %s is off in this rekordbox build: %zu not found (%s%s)", mod == "core" ? "every mod" : mod.c_str(),
             what.size(), list.c_str(), what.size() > 3 ? ", ..." : "");
    }
#ifdef _WIN32
    if (int n = unpatchOffMods(g_coreOff, g_off)) logf("rbmods: %d hooks of mods that are off taken out again", n);
#endif
    std::string left;
#ifdef _WIN32  // the unpatched hooks belong to mods already logged as off (image_win.cpp) or mac-only: count them
    int unpatched = 0;
    for (int h = 0; h < HK_COUNT; h++) unpatched += !hookPatched((HookId)h);
    if (unpatched) left = std::to_string(unpatched) + " (mods off or mac-only)";
#else
    for (int h = 0; h < HK_COUNT; h++)
        if (!hookPatched((HookId)h)) left += (left.empty() ? "" : ", ") + std::to_string(h);
#endif
    logf("rbmods: rekordbox build %s%s: %zu addresses, %zu fields%s%s", rekordboxBuild().c_str(), testedBuild() ? " (tested)" : checkedBuild() ? " (checked)" : " (untested)",
         addrs().size(), offsets().size(), selfTest ? ", field recipes out of date (see above)" : "",
         left.empty() ? "" : (", hooks not patched: " + left).c_str());
    imageDone();
}

}  // namespace rbm
