// mac-only: caches NSScreen deviceDescription (AppKit); no Windows equivalent, leave it out of a Windows build.
// screeninfo: JUCE repaints on every display refresh and each time asks the window's NSScreen for its
// deviceDescription to read NSScreenNumber; AppKit builds a new dictionary for that every time (~1% of the main
// thread while idle). This hands out one dictionary per screen, rebuilt when the displays change.
//   config: screeninfo=0 to disable
#import <AppKit/AppKit.h>
#include "../core/rbmod.h"

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

RBM_MOD({.name = "screeninfo", .enabled = &g_on});
