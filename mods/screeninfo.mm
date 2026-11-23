// mac-only: caches NSScreen deviceDescription (AppKit); no Windows equivalent, leave it out of a Windows build.
// screeninfo: JUCE repaints on every display refresh and each time asks the window's NSScreen for its
// deviceDescription to read NSScreenNumber; AppKit builds a new dictionary for that every time (~1% of the main
// thread while idle). This hands out one dictionary per screen, rebuilt when the displays change.
//   config: screeninfo=0 to disable
#import <AppKit/AppKit.h>
#include "../core/rbmod.h"
#include <dlfcn.h>
#include <mach-o/getsect.h>
#include <mach/mach.h>
#include <objc/runtime.h>

using namespace rbm;

static volatile bool g_on = true;
static NSMapTable<NSScreen*, NSDictionary*>* g_cache;

extern "C" NSDictionary* rbm_hook_screen_desc(NSScreen* screen) {
    auto orig = RBM_ORIG(screen_desc, NSDictionary* (*)(NSScreen*, SEL));
    if (!g_on || !screen || ![NSThread isMainThread]) return orig(screen, @selector(deviceDescription));
    if (!g_cache) {
        g_cache = [NSMapTable weakToStrongObjectsMapTable];
        [[NSNotificationCenter defaultCenter] addObserverForName:NSApplicationDidChangeScreenParametersNotification
                                                          object:nil queue:nil usingBlock:^(NSNotification*) { [g_cache removeAllObjects]; }];
    }
    NSDictionary* d = [g_cache objectForKey:screen];
    if (!d) {
        // A native copy: AppKit's is a Swift-bridged dictionary, whose lookups were still ~0.4% of the main thread.
        NSDictionary* fresh = orig(screen, @selector(deviceDescription));
        d = fresh ? [[NSDictionary alloc] initWithDictionary:fresh] : nil;
        if (d) [g_cache setObject:d forKey:screen];
    }
    return d;
}

#if defined(__x86_64__)
// Intel Macs: JUCE's x86_64 code loads objc_msgSend into a register once and calls it five times, so the call has no
// site of its own to patch (hooks.def). Instead rekordbox's one selector reference for deviceDescription (in its
// __DATA, writable) is pointed at a selector of ours that NSScreen gets. All five places in rekordbox that ask for a
// deviceDescription (JUCE's display list and display links, rekordbox's OpenGL display link, the repaint) then come
// here; only the repaint's call gets the cached dictionary, as on arm64, the others AppKit's own. Code in this dylib
// has selector references of its own, so [screen deviceDescription] below still reaches AppKit.
static const Addr A_repaint{"__ZN4juce19NSViewComponentPeer14AsyncRepainter17handleAsyncUpdateEv"};

static NSDictionary* screenDesc(NSScreen* self, SEL) {
    void* from = __builtin_return_address(0);  // objc_msgSend jumps here: the caller's return address
    if (!g_on || ![NSThread isMainThread]) return [self deviceDescription];
    // Which function a return address is in, asked of dyld once per call site (there are five).
    static void* sites[8];
    static bool repaint[8];
    static int nSites;
    int i = 0;
    while (i < nSites && sites[i] != from) i++;
    if (i == nSites) {
        if (nSites == 8) return [self deviceDescription];
        Dl_info di;
        repaint[i] = dladdr(from, &di) && di.dli_saddr == at<void*>(A_repaint);
        sites[i] = from, nSites++;
    }
    if (!repaint[i]) return [self deviceDescription];
    if (!g_cache) {
        g_cache = [NSMapTable weakToStrongObjectsMapTable];
        [[NSNotificationCenter defaultCenter] addObserverForName:NSApplicationDidChangeScreenParametersNotification
                                                          object:nil queue:nil usingBlock:^(NSNotification*) { [g_cache removeAllObjects]; }];
    }
    NSDictionary* d = [g_cache objectForKey:self];
    if (!d) {
        NSDictionary* fresh = [self deviceDescription];
        d = fresh ? [[NSDictionary alloc] initWithDictionary:fresh] : nil;
        if (d) [g_cache setObject:d forKey:self];
    }
    return d;
}

static void init() {
    SEL ours = sel_registerName("rbm_deviceDescription");
    Dl_info di;
    if (!dladdr(at<void*>(A_repaint), &di) || !di.dli_fbase) return logf("screeninfo: rekordbox's image not found");
    const mach_header_64* h = (const mach_header_64*)di.dli_fbase;
    unsigned long size = 0;
    SEL* refs = (SEL*)getsectiondata(h, "__DATA", "__objc_selrefs", &size);
    if (!refs) refs = (SEL*)getsectiondata(h, "__DATA_CONST", "__objc_selrefs", &size);
    SEL* ref = nullptr;
    for (unsigned long i = 0; refs && i < size / sizeof(SEL); i++)
        if (refs[i] == @selector(deviceDescription)) ref = &refs[i];
    if (!ref) return logf("screeninfo: rekordbox's selector reference for deviceDescription not found: off");
    // Only where it can be written as it is (patchSlot would leave the page read-only, and __DATA is written).
    vm_address_t a = (vm_address_t)ref;
    vm_size_t len = 0;
    vm_region_basic_info_data_64_t info;
    mach_msg_type_number_t count = VM_REGION_BASIC_INFO_COUNT_64;
    mach_port_t object;
    if (vm_region_64(mach_task_self(), &a, &len, VM_REGION_BASIC_INFO_64, (vm_region_info_t)&info, &count, &object) != KERN_SUCCESS ||
        !(info.protection & VM_PROT_WRITE))
        return logf("screeninfo: rekordbox's selector references are read-only: off");
    if (!class_addMethod([NSScreen class], ours, (IMP)screenDesc, "@@:")) return logf("screeninfo: NSScreen already has %s: off", sel_getName(ours));
    __atomic_store_n(ref, ours, __ATOMIC_RELEASE);
    logf("screeninfo: x86_64: rekordbox's deviceDescription calls come through %s", sel_getName(ours));
}

RBM_MOD({.name = "screeninfo", .init = init, .enabled = &g_on});
#else
RBM_MOD({.name = "screeninfo", .enabled = &g_on});
#endif
