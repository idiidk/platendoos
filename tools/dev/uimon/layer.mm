#import <AppKit/AppKit.h>
#import <QuartzCore/QuartzCore.h>
#include "rbmod.h"
using namespace rbm;
#ifndef COUNT
#define COUNT 0
#endif
#ifndef SYNC
#define SYNC -1
#endif
static void visit(CALayer* l, int depth) {
    if ([l isKindOfClass:[CAMetalLayer class]]) {
        CAMetalLayer* m = (CAMetalLayer*)l;
        logf("layer: CAMetalLayer depth %d drawables %d displaySync %d presentsWithTransaction %d size %.0fx%.0f scale %.1f opaque %d framebufferOnly %d", depth, (int)m.maximumDrawableCount, (int)m.displaySyncEnabled, (int)m.presentsWithTransaction, m.drawableSize.width, m.drawableSize.height, m.contentsScale, (int)m.opaque, (int)m.framebufferOnly);
        if (COUNT) m.maximumDrawableCount = COUNT;
        if (SYNC >= 0) m.displaySyncEnabled = SYNC;
        if (COUNT || SYNC >= 0) logf("layer: now drawables %d displaySync %d", (int)m.maximumDrawableCount, (int)m.displaySyncEnabled);
    }
    for (CALayer* s in l.sublayers) visit(s, depth + 1);
}
extern "C" void rbm_experiment() {
    plat::onMain([] {
        for (NSWindow* w in NSApp.windows) if (w.isVisible && w.contentView.layer) { logf("layer: window '%s' screen max fps %d", w.title.UTF8String, (int)w.screen.maximumFramesPerSecond); visit(w.contentView.layer, 0); }
    });
}
