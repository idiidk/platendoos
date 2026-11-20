// badge's chip on macOS: an AppKit view laid over the main window's content (see badge_view.h).
#import <AppKit/AppKit.h>
#import <QuartzCore/QuartzCore.h>
#include "badge_view.h"
#include "badge_chip.h"

static void (*g_onClick)();
static bool g_pending;

@interface RBMBadge : NSView
@end

@implementation RBMBadge
- (void)drawRect:(NSRect)dirty {
    drawChip(self.bounds);
}
- (void)mouseDown:(NSEvent*)event {
    if (g_onClick) g_onClick();
}
@end

static RBMBadge* g_badge;
static CALayer* g_pulse;  // the tile in orange over the blue one, fading in and out while a restart is pending

// Pending: the icon's tile slowly pulses orange (Core Animation: runs in the window server, no redraws).
static void setPulse(bool on) {
    if (!on) {
        [g_pulse removeFromSuperlayer];
        g_pulse = nil;
        return;
    }
    if (g_pulse) return;
    g_badge.wantsLayer = YES;
    CGFloat h = g_badge.bounds.size.height;
    if (h <= 0) return;
    NSImage* img = [NSImage imageWithSize:NSMakeSize(h, h) flipped:NO drawingHandler:^BOOL(NSRect r) {
        drawChipIcon(r, false, [NSColor colorWithSRGBRed:1 green:0.58 blue:0.1 alpha:1]);
        return YES;
    }];
    g_pulse = [CALayer layer];
    g_pulse.frame = CGRectMake(0, 0, h, h);
    g_pulse.contents = img;
    g_pulse.contentsScale = g_badge.window.backingScaleFactor ?: 2;
    g_pulse.opacity = 0;
    // Blue, a quick fade to orange, orange, a quick fade back (every 2.4 s): a slow crossfade would sit in the muddy
    // grey halfway between the two. Same curve as badge_view_win.cpp's pulse().
    CAKeyframeAnimation* a = [CAKeyframeAnimation animationWithKeyPath:@"opacity"];
    a.values = @[ @0.0, @0.0, @1.0, @1.0, @0.0 ];
    a.keyTimes = @[ @0.0, @0.38, @0.5, @0.88, @1.0 ];
    CAMediaTimingFunction *lin = [CAMediaTimingFunction functionWithName:kCAMediaTimingFunctionLinear],
                          *ease = [CAMediaTimingFunction functionWithName:kCAMediaTimingFunctionEaseInEaseOut];
    a.timingFunctions = @[ lin, ease, lin, ease ];
    a.duration = 2.4;
    a.repeatCount = HUGE_VALF;
    [g_pulse addAnimation:a forKey:@"pulse"];
    [g_badge.layer addSublayer:g_pulse];
}

// rekordbox's main window: its largest titled window.
static NSWindow* mainWindow() {
    NSWindow* best = nil;
    for (NSWindow* w in NSApp.windows) {
        if (!w.visible || ![w.title isEqualToString:@"rekordbox"]) continue;
        if (!best || w.frame.size.width * w.frame.size.height > best.frame.size.width * best.frame.size.height) best = w;
    }
    return best;
}

namespace badgeview {

void setOnClick(void (*fn)()) { g_onClick = fn; }

// Keeps the badge on the main window (JUCE can replace its content view, e.g. on a layout change), or off it.
// (The position is fixed: right of the logo at the bottom left; `bar` isn't needed here.)
void update(bool on, bool hidden, const Bar*, const std::string& tip, bool pending) {
    if (!on) {
        [g_badge removeFromSuperview];
        return;
    }
    NSWindow* w = mainWindow();
    NSView* host = w.contentView;
    if (!g_badge) g_badge = [[RBMBadge alloc] initWithFrame:NSZeroRect];
    g_badge.hidden = hidden;
    NSString* tipText = [NSString stringWithUTF8String:tip.c_str()];
    if (![g_badge.toolTip isEqualToString:tipText]) [g_badge setToolTip:tipText];
    g_pending = pending;
    if (!host || g_badge.superview == host) {
        setPulse(g_pending && g_badge.superview);
        return;
    }
    [g_badge removeFromSuperview];
    // Right of the "rekordbox" logo at the bottom left (it ends ~112 pt in), on its centre line (13 pt up).
    NSRect frame = NSMakeRect(120, 5, 90, 16);
    if (host.isFlipped) frame.origin.y = host.bounds.size.height - frame.origin.y - frame.size.height;
    g_badge.frame = frame;
    g_badge.autoresizingMask = NSViewMaxXMargin | (host.isFlipped ? NSViewMinYMargin : NSViewMaxYMargin);
    [host addSubview:g_badge positioned:NSWindowAbove relativeTo:nil];
    setPulse(false);  // (re)made on the new host
    setPulse(g_pending);
}

}  // namespace badgeview
