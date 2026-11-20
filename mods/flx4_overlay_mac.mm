// flx4's toasts and mixer marks as AppKit views over rekordbox's window (see flx4_overlay.h).
#import <AppKit/AppKit.h>
#include <vector>
#include "flx4_overlay.h"
#include "../core/rbmod.h"

namespace {

NSWindow* mainWindow() {
    NSWindow* best = nil;
    for (NSWindow* w in NSApp.windows) {
        if (!w.visible || ![w.title isEqualToString:@"rekordbox"]) continue;
        if (!best || w.frame.size.width * w.frame.size.height > best.frame.size.width * best.frame.size.height) best = w;
    }
    return best;
}

// A juce screen rectangle in the window content's coordinates.
NSRect onWindowRect(NSView* host, flx4ui::Rect r) {
    double top = NSMaxY(NSScreen.screens[0].frame);  // juce's screen coordinates start at the main screen's top left
    NSRect s = NSMakeRect(r.x, top - r.y - r.h, r.w, r.h);
    return [host convertRect:[host.window convertRectFromScreen:s] fromView:nil];
}

}  // namespace

@interface RBMFlx4Toast : NSView
@property(copy) NSString* title;
@property(copy) NSString* detail;
@end

@implementation RBMFlx4Toast
- (void)drawRect:(NSRect)dirty {
    [[NSColor colorWithWhite:0.08 alpha:0.92] setFill];
    [[NSBezierPath bezierPathWithRoundedRect:self.bounds xRadius:8 yRadius:8] fill];
    [[NSColor colorWithSRGBRed:0.16 green:0.47 blue:0.92 alpha:1] setStroke];
    NSBezierPath* edge = [NSBezierPath bezierPathWithRoundedRect:NSInsetRect(self.bounds, 1, 1) xRadius:7 yRadius:7];
    edge.lineWidth = 2;
    [edge stroke];
    NSMutableParagraphStyle* center = [NSMutableParagraphStyle new];
    center.alignment = NSTextAlignmentCenter;
    NSDictionary* big = @{NSFontAttributeName: [NSFont boldSystemFontOfSize:24], NSForegroundColorAttributeName: NSColor.whiteColor,
                          NSParagraphStyleAttributeName: center, NSKernAttributeName: @1.0};
    NSDictionary* small = @{NSFontAttributeName: [NSFont systemFontOfSize:12], NSForegroundColorAttributeName: [NSColor colorWithWhite:0.75 alpha:1],
                            NSParagraphStyleAttributeName: center};
    NSRect b = self.bounds;
    [self.title drawInRect:NSMakeRect(8, b.size.height - 44, b.size.width - 16, 32) withAttributes:big];
    [self.detail drawInRect:NSMakeRect(8, 10, b.size.width - 16, b.size.height - 58) withAttributes:small];
}
@end

static RBMFlx4Toast* g_toast[2];
static flx4ui::Rect g_decks[4];  // where the decks are (setMixerMarks), for the toasts
static int g_toastShown[2];  // counts showings, so a toast shown again isn't faded out by the one before

namespace flx4ui {

bool windowAvailable() { return mainWindow().contentView != nil; }

void toast(int side, const std::string& titleText, const std::string& detailText, int deck) {
    NSView* host = mainWindow().contentView;
    if (!host) return;
    NSString* title = [NSString stringWithUTF8String:titleText.c_str()];
    NSString* detail = [NSString stringWithUTF8String:detailText.c_str()];
    RBMFlx4Toast* t = g_toast[side];
    if (!t) t = g_toast[side] = [[RBMFlx4Toast alloc] initWithFrame:NSZeroRect];
    t.title = title;
    t.detail = detail;
    NSSize size = NSMakeSize(300, !detail.length ? 56 : [detail containsString:@"\n"] ? 100 : 84);
    // Over the side's half of the decks: between the window's top and rekordbox's browser (a view of its own).
    NSRect hb = host.bounds;
    double decksEnd = hb.size.height * 0.6;  // from the top
    for (NSView* v in host.subviews)
        if (v != t && !v.hidden && v.frame.size.width > hb.size.width * 0.8)
            decksEnd = host.isFlipped ? NSMinY(v.frame) : hb.size.height - NSMaxY(v.frame);
    double x = hb.size.width * (side ? 0.75 : 0.25) - size.width / 2;
    double y = decksEnd * 0.6 - size.height / 2;
    if (!host.isFlipped) y = hb.size.height - y - size.height;
    if (deck >= 0 && deck < 4 && g_decks[deck].w > 0) {  // over that deck
        NSRect d = onWindowRect(host, g_decks[deck]);
        x = NSMidX(d) - size.width / 2, y = NSMidY(d) - size.height / 2;
    }
    t.frame = NSMakeRect(round(x), round(y), size.width, size.height);
    [t removeFromSuperview];  // (re)added last: on top of rekordbox's own views, e.g. its browser
    [host addSubview:t positioned:NSWindowAbove relativeTo:nil];
    t.alphaValue = 1;
    t.needsDisplay = YES;
    int shown = ++g_toastShown[side];
    rbm::plat::onMainAfter(1.4, [side, shown, t] {
        if (g_toastShown[side] != shown) return;
        [NSAnimationContext runAnimationGroup:^(NSAnimationContext* c) {
            c.duration = 0.35;
            t.animator.alphaValue = 0;
        }];
    });
}

}  // namespace flx4ui

static flx4ui::MixerMarks g_marks;  // as drawn (main thread)

@interface RBMFlx4Mixer : NSView
@end

@implementation RBMFlx4Mixer
- (NSView*)hitTest:(NSPoint)p {
    return nil;  // only shows; the mouse goes to rekordbox
}
- (NSRect)rect:(flx4ui::Rect)r {
    return [self convertRect:onWindowRect(self.superview, r) fromView:self.superview];
}
- (void)drawRect:(NSRect)dirty {
    using namespace flx4ui;
    NSColor* blue = [NSColor colorWithSRGBRed:0.16 green:0.47 blue:0.92 alpha:1];
    NSColor* orange = [NSColor colorWithSRGBRed:1 green:0.55 blue:0.1 alpha:1];
    for (int d = 0; d < 4; d++) {  // CFX knobs, as rekordbox draws its mixer's knobs
        NSRect r = [self rect:g_marks.cfxKnob[d]];
        int val = g_marks.cfxValue[d];
        if (r.size.width <= 0 || val < 0) continue;
        NSPoint c0 = NSMakePoint(NSMidX(r), NSMidY(r));
        double rad = std::min(r.size.width, r.size.height) / 2 - 2.5;
        // angles in AppKit's degrees (counter-clockwise from 3 o'clock): 7 o'clock is 225, 5 o'clock -45, 12 o'clock 90
        auto angle = [&](double v) { double a = 225 - 270 * v / 127.0; return self.isFlipped ? -a : a; };
        NSBezierPath* track = [NSBezierPath bezierPath];
        [track appendBezierPathWithArcWithCenter:c0 radius:rad startAngle:angle(0) endAngle:angle(127) clockwise:!self.isFlipped];
        track.lineWidth = 2;
        [[NSColor colorWithWhite:0.23 alpha:1] setStroke];
        [track stroke];
        if (abs(val - 64) > 1) {  // from the centre: the effect's amount
            NSBezierPath* arc = [NSBezierPath bezierPath];
            [arc appendBezierPathWithArcWithCenter:c0 radius:rad startAngle:angle(64) endAngle:angle(val) clockwise:(val > 64) != self.isFlipped];
            arc.lineWidth = 2;
            [[NSColor colorWithSRGBRed:0.2 green:0.45 blue:0.95 alpha:1] setStroke];
            [arc stroke];
        }
        double a = angle(val) * M_PI / 180;
        NSBezierPath* pointer = [NSBezierPath bezierPath];
        [pointer moveToPoint:NSMakePoint(c0.x + cos(a) * rad * 0.55, c0.y + sin(a) * rad * 0.55)];
        [pointer lineToPoint:NSMakePoint(c0.x + cos(a) * (rad - 0.5), c0.y + sin(a) * (rad - 0.5))];
        pointer.lineWidth = 1.8;
        pointer.lineCapStyle = NSLineCapStyleRound;
        [NSColor.whiteColor setStroke];
        [pointer stroke];
        NSRect lr = [self rect:g_marks.cfxLabel[d]];
        NSMutableParagraphStyle* center = [NSMutableParagraphStyle new];
        center.alignment = NSTextAlignmentCenter;
        [@"CFX" drawInRect:NSMakeRect(NSMinX(lr), NSMinY(lr) - 1.5, lr.size.width, lr.size.height + 3)
            withAttributes:@{NSFontAttributeName: [NSFont boldSystemFontOfSize:8.5], NSForegroundColorAttributeName: NSColor.whiteColor,
                             NSParagraphStyleAttributeName: center}];
    }
    for (int side = 0; side < 2; side++) {
        NSRect col = [self rect:g_marks.column[side]];
        if (col.size.width > 0) {
            [blue setStroke];
            NSBezierPath* p = [NSBezierPath bezierPathWithRoundedRect:NSInsetRect(col, -3, -3) xRadius:5 yRadius:5];
            p.lineWidth = 1.5;
            [p stroke];
        }
        // where the FLX4's knobs and fader are, for the controls still to pick up
        for (int c = 0; c < kControls; c++) {
            int phys = g_marks.phys[side][c];
            if (phys < 0) continue;
            NSRect r = [self rect:g_marks.control[side][c]];
            [orange set];
            if (c == kFader) {
                double y = NSMinY(r) + 4 + (r.size.height - 8) * phys / 127.0;
                if (self.isFlipped) y = NSMaxY(r) - (y - NSMinY(r));
                NSRectFill(NSMakeRect(NSMinX(r) - 3, y - 1, r.size.width + 6, 2));
            } else {  // a rotary knob: 7 to 5 o'clock
                double a = (-135 + 270 * phys / 127.0) * M_PI / 180, rad = std::min(r.size.width, r.size.height) / 2;
                double dx = sin(a), dy = self.isFlipped ? -cos(a) : cos(a);
                NSPoint c0 = NSMakePoint(NSMidX(r), NSMidY(r));
                NSBezierPath* p = [NSBezierPath bezierPath];
                [p moveToPoint:NSMakePoint(c0.x + dx * rad * 0.3, c0.y + dy * rad * 0.3)];
                [p lineToPoint:NSMakePoint(c0.x + dx * rad, c0.y + dy * rad)];
                p.lineWidth = 2;
                p.lineCapStyle = NSLineCapStyleRound;
                [p stroke];
                [[NSBezierPath bezierPathWithOvalInRect:NSMakeRect(c0.x + dx * rad - 2.5, c0.y + dy * rad - 2.5, 5, 5)] fill];
            }
        }
    }
}
@end

static RBMFlx4Mixer* g_overlay;
static std::vector<int> g_drawn;  // what the overlay shows, to redraw only when it changes

namespace flx4ui {

void setMixerMarks(const MixerMarks& m) {
    for (int d = 0; d < 4; d++) g_decks[d] = m.deck[d];
    NSView* host = mainWindow().contentView;
    std::vector<int> drawn{host && m.shown};
    NSRect area = NSZeroRect;
    auto rect = [&](Rect r) { return onWindowRect(host, r); };
    for (int side = 0; side < 2 && host && m.shown; side++) {
        NSRect col = rect(m.column[side]);
        drawn.insert(drawn.end(), {side, (int)col.origin.x, (int)col.origin.y, (int)col.size.width, (int)col.size.height});
        area = NSUnionRect(area, col);
        for (int c = 0; c < kControls; c++) {
            drawn.push_back(m.phys[side][c]);
            NSRect cr = rect(m.control[side][c]);
            if (cr.size.width > 0) area = NSUnionRect(area, cr);
        }
    }
    for (int d = 0; d < 4 && host && m.shown; d++) {
        drawn.push_back(m.cfxValue[d]);
        NSRect knob = rect(m.cfxKnob[d]);
        if (knob.size.width > 0) area = NSUnionRect(area, NSUnionRect(knob, rect(m.cfxLabel[d])));
    }
    if (!host || !m.shown || NSIsEmptyRect(area)) {
        [g_overlay removeFromSuperview];
        g_drawn.clear();
        return;
    }
    g_marks = m;
    if (!g_overlay) g_overlay = [[RBMFlx4Mixer alloc] initWithFrame:NSZeroRect];
    NSRect frame = NSInsetRect(area, -12, -12);
    NSView* top = host.subviews.lastObject;  // above rekordbox's own views (e.g. its browser), below a toast
    if (g_overlay.superview != host || (top != g_overlay && top != g_toast[0] && top != g_toast[1])) {
        [g_overlay removeFromSuperview];
        [host addSubview:g_overlay positioned:NSWindowAbove relativeTo:nil];
    }
    if (!NSEqualRects(g_overlay.frame, frame)) g_overlay.frame = frame, g_drawn.clear();
    if (drawn != g_drawn) {
        g_drawn = drawn;
        g_overlay.needsDisplay = YES;
    }
}

}  // namespace flx4ui
