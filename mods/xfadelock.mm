// xfadelock: a small lock button right of rekordbox's on-screen crossfader (the bottom of its mixer, between the
// crossfader assign buttons) that holds the crossfader exactly at the centre. While it is lit, neither a
// controller's crossfader (the DDJ-FLX4's or any other) nor dragging the on-screen one moves it. Unlocked, the
// controller's crossfader only takes over again once it reaches or passes where the crossfader is (the centre,
// unless it was dragged on screen since), so nothing jumps; until then the lock shows orange.
// It starts unlocked after every launch: a lock left on from an earlier session would look like a broken crossfader.
// How (no hooks.def call hooks, so install.sh is enough):
//  - every crossfader position reaches the mixer through DjMixerUnit::setCrossFaderPosition (virtual: via
//    DjEngineIF::setCrossFader from the on-screen fader, MIDI, automix and rekordbox's other paths): swapped in its
//    vtable, it gets the centre while locked, whatever the source;
//  - CrossFaderComp (the on-screen crossfader) gets each controller's crossfader move (eventAbsoluteValueChanged):
//    while locked it is dropped (its value kept for the pickup), so the on-screen fader stays put too;
//  - a drag of the on-screen fader (sliderValueChanged) puts the slider back at the centre before rekordbox reads it.
//   config: xfadelock=0 to disable (restart)
// macOS only for now (.mm): the Windows tables don't have these classes, and the button is an AppKit view.
#import <AppKit/AppKit.h>
#include "../core/rbmod.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <string.h>

using namespace rbm;

static volatile bool g_on = true;

// ---- rekordbox ---------------------------------------------------------------------------------------------------

// DjMixerUnit::setCrossFaderPosition(float 0..1, centre 0.5): in DjMixerUnit's own vtable and in that of its mixer
// control interface (at +0x180: what DjUnitAudioGraph::getMixerControl hands DjEngineIF::setCrossFader).
static const Addr A_mixerVt{"__ZTV11DjMixerUnit"};
static const Offset V_mixerSet{0x2d8, "__ZTV11DjMixerUnit", "vt:__ZN11DjMixerUnit21setCrossFaderPositionEf", 0};
static const Offset V_mixerSetCtl{0x680, "__ZTV11DjMixerUnit", "vt:__ZThn384_N11DjMixerUnit21setCrossFaderPositionEf", 0};
// CrossFaderComp: its device listener (+0x100, the __ZThn256_ thunk) and slider listener (+0xf0, __ZThn240_)
// interfaces have vtables of their own; the thunks jump straight to the method, so each call comes in once.
static const Addr A_compVt{"__ZTV14CrossFaderComp"};
static const Offset V_compMidi{0x1c0, "__ZTV14CrossFaderComp", "vt:__ZN14CrossFaderComp25eventAbsoluteValueChangedEPN6djplay12DeviceObjectEi", 0};
static const Offset V_compMidiL{0x270, "__ZTV14CrossFaderComp", "vt:__ZThn256_N14CrossFaderComp25eventAbsoluteValueChangedEPN6djplay12DeviceObjectEi", 0};
static const Offset V_compSlider{0x1a8, "__ZTV14CrossFaderComp", "vt:__ZN14CrossFaderComp18sliderValueChangedEPN4juce6SliderE", 0};
static const Offset V_compSliderL{0x1f8, "__ZTV14CrossFaderComp", "vt:__ZThn240_N14CrossFaderComp18sliderValueChangedEPN4juce6SliderE", 0};
constexpr size_t kDeviceListener = 256, kSliderListener = 240;  // as in the thunks' names (another offset, another name)
static const Offset O_xfDevice{0x2c8, "__ZN14CrossFaderComp25eventAbsoluteValueChangedEPN6djplay12DeviceObjectEi", "ldr x19", 0};  // CrossFaderComp: the crossfader's DeviceComponent
static const Offset O_xfSlider{0x260, "__ZN14CrossFaderComp17handleAsyncUpdateEv", "ldr x0", 0};  // its juce::Slider
static const Offset O_xfAt{0x308, "__ZN14CrossFaderComp17handleAsyncUpdateEv", "ldr x19", 0};  // double: the position (0..1) it last set
static const Addr A_tiDeviceObject{"__ZTIN6djplay12DeviceObjectE"};
static const Addr A_tiDeviceComponent{"__ZTIN6djplay15DeviceComponentE"};
static const Addr A_setCrossFader{"__ZN8djengine10DjEngineIF13setCrossFaderEd"};
static const Addr A_mainComponent{"__ZN13MainComponent11getInstanceEv"};
static const Addr A_numChildren{"__ZNK4juce9Component21getNumChildComponentsEv"};
static const Addr A_child{"__ZNK4juce9Component17getChildComponentEi"};
static const Addr A_screenBounds{"__ZNK4juce9Component15getScreenBoundsEv"};
static const Addr A_isShowing{"__ZNK4juce9Component9isShowingEv"};
static const Addr A_sliderValue{"__ZNK4juce6Slider8getValueEv"};
static const Addr A_sliderMin{"__ZNK4juce6Slider10getMinimumEv"};
static const Addr A_sliderMax{"__ZNK4juce6Slider10getMaximumEv"};
static const Addr A_sliderSet{"__ZN4juce6Slider8setValueEdNS_16NotificationTypeE"};
enum { kDontSend = 0, kSendSync = 2 };  // juce::NotificationType

using MixerSetFn = int (*)(void*, float);
using MidiFn = void (*)(void*, void*, int);
using SliderFn = void (*)(void*, void*);
static MixerSetFn o_mixerSet, o_mixerSetCtl;
static MidiFn o_midi, o_midiL;
static SliderFn o_slider, o_sliderL;

namespace {  // file-local
struct JRect { int x, y, w, h; };  // juce::Rectangle<int>
}  // namespace

// ---- the lock ----------------------------------------------------------------------------------------------------

enum { kFree, kLocked, kPickup };  // kPickup: unlocked, the controller's crossfader still on its way to the position
static std::atomic<int> g_state{kFree};
static std::atomic<double> g_hw{-1};  // where the controller's crossfader is (0..1), -1 not heard of yet
static bool g_ready;                  // the mixer is patched: the button is shown
constexpr double kNear = 0.02;        // pickup: this close counts as there (about 2.5 of the 7-bit steps)

static bool locked() { return g_on && g_state.load(std::memory_order_relaxed) == kLocked; }

static int mixerSet(void* self, float pos) { return o_mixerSet(self, locked() ? 0.5f : pos); }  // any thread
static int mixerSetCtl(void* self, float pos) { return o_mixerSetCtl(self, locked() ? 0.5f : pos); }

// Whether a controller's message may move the crossfader (whatever thread the controller's messages come on).
static bool midiPasses(char* comp, void* obj, int value) {
    if (!g_on || !obj) return true;
    void* dc = dynCast(obj, at<void*>(A_tiDeviceObject), at<void*>(A_tiDeviceComponent));
    if (!dc || dc != *(void**)(comp + O_xfDevice)) return true;  // an assign button, not the fader
    double v = std::clamp(value / 16383.0, 0.0, 1.0), prev = g_hw.exchange(v);
    int st = g_state.load();
    if (st == kFree) return true;
    if (st == kLocked) return false;
    // Picked up: there, or it was there or crossed it with this move; or, the first time it is heard of, a quick
    // move's step away. Where to: as rekordbox has it now (the centre, or where it was dragged on screen since).
    double want = *(double*)(comp + O_xfAt);
    bool near = std::fabs(v - want) <= kNear;
    bool from = prev >= 0 && (std::fabs(prev - want) <= kNear || (prev - want) * (v - want) < 0);
    bool first = prev < 0 && std::fabs(v - want) <= 2.5 * kNear;
    if (!(near || from || first)) return false;
    int expected = kPickup;
    if (g_state.compare_exchange_strong(expected, kFree))
        logf("xfadelock: the controller's crossfader picked up at %d%%: it moves the crossfader again", (int)lround(v * 100));
    return g_state.load() != kLocked;
}
static void midi(void* comp, void* obj, int value) {
    if (midiPasses((char*)comp, obj, value)) o_midi(comp, obj, value);
}
static void midiL(void* listener, void* obj, int value) {
    if (midiPasses((char*)listener - kDeviceListener, obj, value)) o_midiL(listener, obj, value);
}

// The slider's position as rekordbox reads it (linear, 0..1); setProportion moves it there (main thread).
static double proportion(void* s) {
    double lo = at<double (*)(void*)>(A_sliderMin)(s), hi = at<double (*)(void*)>(A_sliderMax)(s);
    return hi > lo ? (at<double (*)(void*)>(A_sliderValue)(s) - lo) / (hi - lo) : -1;
}
static void setProportion(void* s, double p, int notify) {
    double lo = at<double (*)(void*)>(A_sliderMin)(s), hi = at<double (*)(void*)>(A_sliderMax)(s);
    if (hi > lo && std::fabs(proportion(s) - p) > 1e-9) at<void (*)(void*, double, int)>(A_sliderSet)(s, lo + p * (hi - lo), notify);
}

// A drag while locked: back to the centre (silently) before rekordbox reads the slider, so it doesn't even show.
static void holdSlider(char* comp, void* slider) {
    if (locked() && slider == *(void**)(comp + O_xfSlider)) setProportion(slider, 0.5, kDontSend);
}
static void sliderChanged(void* comp, void* slider) {
    holdSlider((char*)comp, slider);
    o_slider(comp, slider);
}
static void sliderChangedL(void* listener, void* slider) {
    holdSlider((char*)listener - kSliderListener, slider);
    o_sliderL(listener, slider);
}

// rekordbox's on-screen crossfader (main thread), null if none.
static const char* className(void* c) { return *(const char**)((char*)(*(void***)c)[-1] + 8); }  // RTTI, mangled
static void* findCrossFader(void* c, int depth = 0) {
    if (!strcmp(className(c), "14CrossFaderComp")) return c;
    if (depth > 24) return nullptr;
    for (int i = 0, n = at<int (*)(void*)>(A_numChildren)(c); i < n; i++)
        if (void* r = findCrossFader(at<void* (*)(void*, int)>(A_child)(c, i), depth + 1)) return r;
    return nullptr;
}
static void* crossFader() {
    void* main = at<void* (*)()>(A_mainComponent)();
    return main ? findCrossFader(main) : nullptr;
}
static JRect screenBounds(void* c) { return at<JRect (*)(void*)>(A_screenBounds)(c); }
static bool showing(void* c) { return at<bool (*)(void*)>(A_isShowing)(c); }

static void setLocked(bool on) {  // main thread
    if (on) {
        g_state = kLocked;
        // To the centre now: through the slider (rekordbox then sets the mixer and its own copy), and the mixer itself.
        void* comp = crossFader();
        void* slider = comp ? *(void**)((char*)comp + O_xfSlider) : nullptr;
        if (slider) setProportion(slider, 0.5, kSendSync);
        if (void* e = engine()) at<int (*)(void*, double)>(A_setCrossFader)(e, 0.5);
        logf("xfadelock: crossfader locked at the centre");
        return;
    }
    double hw = g_hw.load();
    if (hw < 0 || std::fabs(hw - 0.5) <= kNear) {  // no controller crossfader heard of, or it is at the centre already
        g_state = kFree;
        logf("xfadelock: unlocked: the crossfader follows the fader again");
    } else {
        g_state = kPickup;
        logf("xfadelock: unlocked: follows the fader again once it passes the centre (the controller's is at %d%%)", (int)lround(hw * 100));
    }
}

// ---- the button ----------------------------------------------------------------------------------------------------

namespace {

NSWindow* mainWindow() {
    NSWindow* best = nil;
    for (NSWindow* w in NSApp.windows) {
        if (!w.visible || ![w.title isEqualToString:@"rekordbox"]) continue;
        if (!best || w.frame.size.width * w.frame.size.height > best.frame.size.width * best.frame.size.height) best = w;
    }
    return best;
}

// A juce screen rectangle in the window content's coordinates (juce's start at the main screen's top left).
NSRect onWindowRect(NSView* host, JRect r) {
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
    setLocked(g_state.load() != kLocked);
    self.state = g_state.load();
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

// Right of the crossfader, in the gap before the B side's assign buttons, on the fader's centre line. (rekordbox
// draws the fader's track about 3.5 pt inside the slider's bounds.)
static bool buttonRect(void* comp, JRect* out) {
    void* slider = *(void**)((char*)comp + O_xfSlider);
    if (!slider || !showing(slider)) return false;
    JRect cb = screenBounds(comp), sb = screenBounds(slider);
    int sliderEnd = sb.x + sb.w, right = cb.x + cb.w;
    for (int i = 0, n = at<int (*)(void*)>(A_numChildren)(comp); i < n; i++) {
        void* c = at<void* (*)(void*, int)>(A_child)(comp, i);
        if (c == slider || !showing(c)) continue;
        JRect r = screenBounds(c);
        if (r.x >= sliderEnd - 2 && r.x < right) right = r.x;
    }
    int from = sliderEnd - 3, to = right - 2, size = 14;
    if (to - from < 10) from = to - size;  // (no room: over the slider's end)
    size = std::min(size, std::max(10, to - from));
    *out = {from + (to - from - size) / 2, sb.y + sb.h / 2 - size / 2, size, size};
    return true;
}

static NSString* tipFor(int state) {
    switch (state) {
        case kLocked: return @"Crossfader locked at the centre: the controller's crossfader and the on-screen one can't move it. Click to unlock.";
        case kPickup: return @"Crossfader unlocked: the controller's crossfader takes over once it passes the centre. Click to lock again.";
        default: return @"Crossfader lock: click to hold the crossfader at the centre.";
    }
}

static void tick() {  // main thread, every 0.1 s
    if (!g_ready) return;
    NSView* host = mainWindow().contentView;
    void* comp = host ? crossFader() : nullptr;
    JRect r;
    if (!comp || !showing(comp) || !buttonRect(comp, &r)) {
        g_button.hidden = YES;
        return;
    }
    // While locked: the on-screen fader back at the centre if something moved it there (e.g. automix), through
    // rekordbox's own listener (which sets the mixer: centred again by the lock).
    void* slider = *(void**)((char*)comp + O_xfSlider);
    if (locked() && std::fabs(proportion(slider) - 0.5) > 1e-6) setProportion(slider, 0.5, kSendSync);
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
    NSRect frame = onWindowRect(host, r);
    if (!NSEqualRects(g_button.frame, frame)) g_button.frame = frame;
    g_button.hidden = NO;
    int st = g_state.load();
    if (g_button.state != st) {
        g_button.state = st;
        g_button.needsDisplay = YES;
    }
    if (![g_button.toolTip isEqualToString:tipFor(st)]) g_button.toolTip = tipFor(st);
}

// ---- install -------------------------------------------------------------------------------------------------------

// The original goes into *orig before the slot changes: another thread may call the replacement at once.
template <class F> static bool swap(const Addr& vt, const Offset& slot, F mine, F* orig) {
    void** p = (void**)(at<char*>(vt) + 0x10 + slot);
    *orig = (F)*p;
    return patchSlot(p, (void*)mine) != nullptr;
}

static void init() {
    if (!swap(A_mixerVt, V_mixerSet, &mixerSet, &o_mixerSet) || !swap(A_mixerVt, V_mixerSetCtl, &mixerSetCtl, &o_mixerSetCtl)) {
        logf("xfadelock: could not patch the mixer's crossfader: no lock");
        return;
    }
    g_ready = true;
    bool midiOk = swap(A_compVt, V_compMidi, &midi, &o_midi) && swap(A_compVt, V_compMidiL, &midiL, &o_midiL);
    bool dragOk = swap(A_compVt, V_compSlider, &sliderChanged, &o_slider) && swap(A_compVt, V_compSliderL, &sliderChangedL, &o_sliderL);
    if (!midiOk || !dragOk)
        logf("xfadelock: could not patch the on-screen crossfader (%s): the lock holds the sound, but that fader may show another position",
             !midiOk ? "controller moves" : "drags");
    logf("xfadelock: ready: the lock button is right of the on-screen crossfader (starts unlocked)");
}

RBM_MOD({.name = "xfadelock", .init = init, .tick = tick, .enabled = &g_on});
