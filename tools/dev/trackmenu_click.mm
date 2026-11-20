// Development: opens the real track menu without a mouse. Control-clicks a row of the browser's track list from inside the app
// (a synthesized NSEvent; a synthesized right click arrives as a left one) and closes the menu 1.5 s later. juce only takes a click on a window that is
// the topmost one at that point, so rekordbox's window is put on top for it and back afterwards. With tidymenu_log=1 the log
// then lists the menu. Run: tools/dev/hotload.sh tools/dev/trackmenu_click.mm
#import <AppKit/AppKit.h>
#include "rbmod.h"
using namespace rbm;
static const Addr A_bounds{"__ZNK4juce9Component15getScreenBoundsEv"};
static const Addr A_dismiss{"__ZN4juce9PopupMenu21dismissAllActiveMenusEv"};
struct R { int x, y, w, h; };
static void after(double s, void (^b)()) { dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(s * NSEC_PER_SEC)), dispatch_get_main_queue(), b); }
extern "C" void rbm_experiment() {
    void* want = browserListViewer();
    void* comp = nullptr;
    forEachListViewer([&](void* c, void* lv) { if (lv == want) comp = c; });
    if (!comp) return logf("exp: no browser list");
    R r = at<R (*)(const void*)>(A_bounds)(comp);
    CGFloat mainH = NSScreen.screens[0].frame.size.height;
    NSPoint screen = NSMakePoint(r.x + 220, mainH - (r.y + 70));
    NSWindow* win = nil;
    for (NSWindow* w in NSApp.orderedWindows)
        if (w.isVisible && NSPointInRect(screen, w.frame)) { win = w; break; }
    if (!win) return logf("exp: no window at the point");
    NSPoint p = [win convertPointFromScreen:screen];
    NSInteger num = win.windowNumber;
    NSInteger top = [NSWindow windowNumberAtPoint:screen belowWindowWithWindowNumber:0];
    [win orderFrontRegardless];  // (macOS doesn't let an app activate itself any more; on top is enough for juce)
    auto ev = ^NSEvent*(NSEventType t) {
        return [NSEvent mouseEventWithType:t location:p modifierFlags:NSEventModifierFlagControl timestamp:NSProcessInfo.processInfo.systemUptime
                              windowNumber:num context:nil eventNumber:0 clickCount:1 pressure:1];
    };
    after(0.8, ^{
        after(0.15, ^{ [win sendEvent:ev(NSEventTypeLeftMouseUp)]; });
        after(1.5, ^{
            bool any = at<bool (*)()>(A_dismiss)();
            logf("exp: dismissed the menus (%d)", any);
            if (top != num) [win orderWindow:NSWindowBelow relativeTo:top];
        });
        logf("exp: ctrl-click (button %d; topmost there: %d, selected %d)", (int)ev(NSEventTypeLeftMouseDown).buttonNumber, (int)([NSWindow windowNumberAtPoint:screen belowWindowWithWindowNumber:0] == num), (int)selectedTracks(want).size());
        [win sendEvent:ev(NSEventTypeLeftMouseDown)];
        logf("exp: right mouse down returned (selected %d)", (int)selectedTracks(want).size());
    });
}
