// xfadelock's button on macOS: an AppKit view on rekordbox's window (mods/xfadelock.cpp puts it next to the
// crossfader and says what a click does).
#import <AppKit/AppKit.h>
#include "xfadelock_view.h"

using namespace xfadelockview;

namespace {

int (*g_onClick)();

NSWindow* mainWindow() {
    NSWindow* best = nil;
    for (NSWindow* w in NSApp.windows) {
        if (!w.visible || ![w.title isEqualToString:@"rekordbox"]) continue;
        if (!best || w.frame.size.width * w.frame.size.height > best.frame.size.width * best.frame.size.height) best = w;
    }
    return best;
}

// A juce screen rectangle in the window content's coordinates (juce's start at the main screen's top left).
NSRect onWindowRect(NSView* host, xfadelockview::Rect r) {
    double top = NSMaxY(NSScreen.screens[0].frame);
    NSRect s = NSMakeRect(r.x, top - r.y - r.h, r.w, r.h);
    return [host convertRect:[host.window convertRectFromScreen:s] fromView:nil];
}

}  // namespace

@interface RBMXfadeLock : NSView
@property int state;
@end

@implementation RBMXfadeLock
- (BOOL)isFlipped {
    return YES;
}
- (BOOL)acceptsFirstMouse:(NSEvent*)event {
    return YES;  // one click locks, also while another window has the focus
}
- (void)mouseDown:(NSEvent*)event {
    if (!g_onClick) return;
    self.state = g_onClick();
    self.needsDisplay = YES;
}
// Lit in the blue of rekordbox's lit assign buttons when locked; else dark grey like the unlit ones, with an open
// lock (orange while the controller's crossfader has yet to pick up).
- (void)drawRect:(NSRect)dirty {
    NSRect b = self.bounds;
    bool on = self.state == kLocked;
    [(on ? [NSColor colorWithSRGBRed:0.16 green:0.47 blue:0.92 alpha:1] : [NSColor colorWithWhite:0.17 alpha:1]) setFill];
    [[NSBezierPath bezierPathWithRoundedRect:b xRadius:2.5 yRadius:2.5] fill];
    NSColor* glyph = on ? NSColor.whiteColor
                   : self.state == kPickup ? [NSColor colorWithSRGBRed:1 green:0.55 blue:0.1 alpha:1]
                                           : [NSColor colorWithWhite:0.72 alpha:1];
    [glyph set];
    double cx = NSMidX(b), top = NSMidY(b) - 4.6;  // a 7 x 9.5 pt lock, centred
    [[NSBezierPath bezierPathWithRoundedRect:NSMakeRect(cx - 3.5, top + 4.5, 7, 5) xRadius:1 yRadius:1] fill];
    double lift = on ? 0 : 1.3;  // open: the shackle up, its right leg out of the body
    NSBezierPath* shackle = [NSBezierPath bezierPath];
    [shackle moveToPoint:NSMakePoint(cx - 2, top + 4.6)];
    [shackle lineToPoint:NSMakePoint(cx - 2, top + 2.4 - lift)];
    [shackle curveToPoint:NSMakePoint(cx + 2, top + 2.4 - lift) controlPoint1:NSMakePoint(cx - 2, top - 0.2 - lift)
             controlPoint2:NSMakePoint(cx + 2, top - 0.2 - lift)];
    [shackle lineToPoint:NSMakePoint(cx + 2, on ? top + 4.6 : top + 3.2 - lift)];
    shackle.lineWidth = 1.3;
    shackle.lineCapStyle = NSLineCapStyleRound;
    [shackle stroke];
}
@end

static RBMXfadeLock* g_button;

namespace xfadelockview {

bool hostReady() { return mainWindow().contentView != nil; }

void setOnClick(int (*fn)()) { g_onClick = fn; }

void update(const Rect* r, int st, const char* tip) {
    NSView* host = mainWindow().contentView;
    if (!r || !host) {
        g_button.hidden = YES;
        return;
    }
    if (!g_button) g_button = [[RBMXfadeLock alloc] initWithFrame:NSZeroRect];
    // On rekordbox's views: added again if JUCE replaced its content view or put a view of its own over it
    // (the other mods' views, RBM*, may stay above it: flx4's overlay lets the mouse through).
    bool covered = g_button.superview != host;
    NSArray<NSView*>* subs = host.subviews;
    for (NSUInteger i = covered ? subs.count : [subs indexOfObject:g_button] + 1; i < subs.count; i++)
        if (![NSStringFromClass([subs[i] class]) hasPrefix:@"RBM"]) covered = true;
    if (covered) {
        [g_button removeFromSuperview];
        [host addSubview:g_button positioned:NSWindowAbove relativeTo:nil];
    }
    NSRect frame = onWindowRect(host, *r);
    if (!NSEqualRects(g_button.frame, frame)) g_button.frame = frame;
    g_button.hidden = NO;
    if (g_button.state != st) {
        g_button.state = st;
        g_button.needsDisplay = YES;
    }
    NSString* t = tip ? @(tip) : @"";
    if (![g_button.toolTip isEqualToString:t]) g_button.toolTip = t;
}

}  // namespace xfadelockview
