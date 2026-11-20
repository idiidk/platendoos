// Windows: rbm_hook_<name> by hook id, for image_win.cpp's call-site thunks. A hook whose mod isn't built for
// Windows (mac-only mods) falls back to rbm_hook_missing at link time; its site is then left alone.
#include "rbmod.h"

extern "C" void rbm_hook_missing() {}

// The comparison must happen at run time: rbm_hook_x and rbm_hook_missing are different functions to the
// compiler, so "&rbm_hook_x == &rbm_hook_missing" folds to false although the linker makes them one address.
// That patched every mac-only mod's hook sites to this empty function (badge's status_text returned garbage and
// crashed rekordbox's status bar during analysis).
static bool isMissing(void* p) {
    void* volatile missing = (void*)&rbm_hook_missing;
    void* volatile q = p;
    return q == missing;
}

#define HOOK(name, caller, nth, count, target, kind)                     \
    extern "C" void rbm_hook_##name();                                    \
    __pragma(comment(linker, "/alternatename:rbm_hook_" #name "=rbm_hook_missing"))
#include "../hooks.def"
#undef HOOK

// Hooks that need a register of rekordbox's caller (mac: an arm64 trampoline rbm_hook_<name> moving it into an
// extra argument of rbm_<name>): on Windows image_win.cpp makes that trampoline from the table's hookreg line.
#define HOOK(name, caller, nth, count, target, kind)                     \
    extern "C" void rbm_##name();                                         \
    __pragma(comment(linker, "/alternatename:rbm_" #name "=rbm_hook_missing"))
#include "../hooks.def"
#undef HOOK

extern "C" void* rbm_hook_regfn(int id) {
    static void* const table[] = {
#define HOOK(name, caller, nth, count, target, kind) (void*)&rbm_##name,
#include "../hooks.def"
#undef HOOK
    };
    void* p = table[id];
    return isMissing(p) ? nullptr : p;
}

extern "C" void* rbm_hook_addr(int id) {
    static void* const table[] = {
#define HOOK(name, caller, nth, count, target, kind) (void*)&rbm_hook_##name,
#include "../hooks.def"
#undef HOOK
    };
    void* p = table[id];
    return isMissing(p) ? nullptr : p;
}
