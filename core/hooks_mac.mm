// Hook dispatch: every stub in the patched binary loads its hook index (line order in hooks.def) into x17 (arm64;
// r11 in the x86_64 slice, docs/intel-mac.md) and jumps to rbm_dispatch, which jumps on to rbm_hook_<name>.
// Kept apart from the hook definitions because the declarations below don't carry real signatures.
#include "rbmod.h"

extern "C" __attribute__((visibility("hidden"))) void* rbm_hook_table[::rbm::HK_COUNT];
void* rbm_hook_table[::rbm::HK_COUNT];
#if defined(__aarch64__)
asm(R"(
    .text
    .p2align 2
_rbm_dispatch:
    adrp x16, _rbm_hook_table@PAGE
    add  x16, x16, _rbm_hook_table@PAGEOFF
    ldr  x16, [x16, x17, lsl #3]
    br   x16
)");
#elif defined(__x86_64__)
// r11 holds the index and r10 is the scratch register: the only two touched. Neither carries an argument (rdi rsi
// rdx rcx r8 r9, xmm0-7, al for variadic calls, rdi/rax for a struct return) nor is callee-saved, and rsp is as the
// call site left it. No function of rekordbox reads r11 before writing it.
asm(R"(
    .text
    .p2align 4
_rbm_dispatch:
    leaq  _rbm_hook_table(%rip), %r10
    jmpq  *(%r10,%r11,8)
)");
#endif
extern "C" void rbm_dispatch();

#define HOOK(name, caller, nth, count, target, kind) extern "C" void rbm_hook_##name();
#include "../hooks.def"
#undef HOOK

// A hook whose mod isn't built in (a build without that mod's file): a weak default definition for it, so the dylib
// still links. It is entered through rbm_dispatch (x17 / r11 = the hook index, untouched) and continues straight into
// the original target, as if the site had never been patched.
static const uintptr_t* const rbm_hook_origs[::rbm::HK_COUNT] = {
#define HOOK(name, caller, nth, count, target, kind) &::rbm::kHookOrig[::rbm::HK_##name].value,
#include "../hooks.def"
#undef HOOK
};
extern "C" __attribute__((used)) const uintptr_t* const* const rbm_hook_origs_ptr = rbm_hook_origs;
#if defined(__aarch64__)
asm(R"(
    .text
    .p2align 2
_rbm_hook_default:
    adrp x16, _rbm_hook_origs_ptr@PAGE
    ldr  x16, [x16, _rbm_hook_origs_ptr@PAGEOFF]
    ldr  x16, [x16, x17, lsl #3]
    ldr  x16, [x16]
    adrp x17, __ZN3rbm7g_slideE@PAGE
    ldr  x17, [x17, __ZN3rbm7g_slideE@PAGEOFF]
    add  x16, x16, x17
    br   x16
)");
#define HOOK(name, caller, nth, count, target, kind) \
    asm(".text\n.p2align 2\n.globl _rbm_hook_" #name "\n.weak_definition _rbm_hook_" #name "\n_rbm_hook_" #name ":\n b _rbm_hook_default\n");
#include "../hooks.def"
#undef HOOK
#elif defined(__x86_64__)
asm(R"(
    .text
    .p2align 4
_rbm_hook_default:
    movq  _rbm_hook_origs_ptr(%rip), %r10
    movq  (%r10,%r11,8), %r10
    movq  (%r10), %r10
    addq  __ZN3rbm7g_slideE(%rip), %r10
    jmpq  *%r10
)");
#define HOOK(name, caller, nth, count, target, kind) \
    asm(".text\n.globl _rbm_hook_" #name "\n.weak_definition _rbm_hook_" #name "\n_rbm_hook_" #name ":\n jmp _rbm_hook_default\n");
#include "../hooks.def"
#undef HOOK
#endif

// For an app patched from another hooks.def (image_mac.mm's runStock): stub x17 / r11 jumps to targets[index], its
// call site's original target, with no hook in between.
extern "C" __attribute__((visibility("hidden"))) const uintptr_t* rbm_stock_targets;
const uintptr_t* rbm_stock_targets;
#if defined(__aarch64__)
asm(R"(
    .text
    .p2align 2
_rbm_dispatch_stock:
    adrp x16, _rbm_stock_targets@PAGE
    ldr  x16, [x16, _rbm_stock_targets@PAGEOFF]
    ldr  x16, [x16, x17, lsl #3]
    br   x16
)");
#elif defined(__x86_64__)
asm(R"(
    .text
    .p2align 4
_rbm_dispatch_stock:
    movq  _rbm_stock_targets(%rip), %r10
    jmpq  *(%r10,%r11,8)
)");
#endif
extern "C" void rbm_dispatch_stock();
extern "C" void* rbm_stock_dispatch(const uintptr_t* targets) {
    rbm_stock_targets = targets;
    return (void*)&rbm_dispatch_stock;
}

// Fills the table and returns the address for the binary's dispatch slot.
extern "C" void* rbm_fill_hook_table() {
    int i = 0;
#define HOOK(name, caller, nth, count, target, kind) rbm_hook_table[i++] = (void*)&rbm_hook_##name;
#include "../hooks.def"
#undef HOOK
    return (void*)&rbm_dispatch;
}
