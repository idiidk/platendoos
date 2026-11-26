// mac-only: an AppKit notification rekordbox listens to for every window, and two call sites of the mac binary; leave
// it out of a Windows build.
// relayout: two things rekordbox does again and again while its window is laid out.
//
// 1. It lays out its whole main window whenever *any* window is put on a screen: a pop-up menu, a drop-down, a
//    tooltip, Preferences. Its NSWindowListener observes NSWindowDidChangeScreenNotification for all windows
//    (object: nil) and MainComponent::windowDidChangeScreen() calls resized() without looking at which window it was
//    (windowDidChangeBackingProperties, next to it, does compare the window number). resized() runs
//    UiManager::resizeSkin over all four decks: 15-45 ms on the main thread before the menu or window shows.
//    Here the notification is passed on only when it is about the main window (moved to another screen).
//
// 2. A layout pass (a panel shown or hidden, another deck layout, a track loaded, the window resized) calls
//    CustomTextButton::setDrawablePosition on every hot cue pad, memory cue and lighting button, also the ones that
//    stay where they are. That function doesn't compare: it copies the button's two drawables, setDrawable copies them
//    again and createImage renders them into two new images and makes eight DrawableImages of those, after freeing the
//    eight it had. Each image is a CoreGraphics bitmap context, and freeing one walks CoreGraphics' list of all live
//    contexts (5700 of them in rekordbox: x_list_remove, 38% of the main thread while the window is resized).
//    Here the images are kept when nothing they are made from changed: the same drawables, position, button size and
//    display scale as when they were made.
//
// 3. rekordbox's main window holds 16,700 components and shows about 400: every panel exists all the time (64 sampler
//    slots, 512 user pads, the lighting panel, the grid and cue editors of all four decks), and a layout pass lays
//    them all out, shown or not. A quarter of a pass went to panels nobody sees. Worse, some option clicks
//    (Preferences > Controller > auto gain or needle lock, the beat count mode, the mixer mode, ...) tell the whole
//    window that its look changed (Component::sendLookAndFeelChange, rekordbox's "refresh everything"): 190-270 ms
//    on the main thread, half of it the 64 hidden sampler slots parsing their SVGs and rendering their buttons again.
//    Here a panel of the classes below that isn't showing skips its resized() or lookAndFeelChanged() and does it
//    when it is about to be seen: at its next paint, when it is made visible, when one of the two is called again
//    while it shows, or by the tick below (at most 0.1 s later, for a panel that shows without being painted).
//   config: relayout=0 to disable; relayout_hidden=0 lays hidden panels out as rekordbox does;
//   relayout_log=1 logs how many buttons kept their images; relayout_check=1 (for
//   testing) renders them all the same and logs every button whose new images differ from the ones it would have kept
#import <AppKit/AppKit.h>
#include "../core/rbmod.h"
#include <objc/runtime.h>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

using namespace rbm;

static volatile bool g_on = true;

static const Addr A_mainComponent{"__ZN13MainComponent11getInstanceEv"};
static const Addr A_getPeer{"__ZNK4juce9Component7getPeerEv"};
static const Addr A_windowNumber{"__ZN9OSXHelper15getWindowNumberEPN4juce13ComponentPeerE"};  // -1: no window

static IMP g_origScreen;
static int g_skipped;
static unsigned g_screens;  // counts changes of the displays, or of the main window's screen or scale

// Main thread (AppKit posts the notification where the window changed).
static void windowDidChangeScreen(id self, SEL cmd, NSNotification* note) {
    if (g_on && [note.object isKindOfClass:[NSWindow class]]) {
        void* main = at<void* (*)()>(A_mainComponent)();
        void* peer = main ? at<void* (*)(void*)>(A_getPeer)(main) : nullptr;
        int number = peer ? at<int (*)(void*)>(A_windowNumber)(peer) : -1;
        if (number >= 0 && number != (int)[(NSWindow*)note.object windowNumber]) {
            if (!g_skipped++) logf("relayout: another window's screen change no longer lays out the main window");
            return;
        }
    }
    g_screens++;  // the main window on another screen: another scale, maybe
    ((void (*)(id, SEL, NSNotification*))g_origScreen)(self, cmd, note);
}

// ---- 2: button images -----------------------------------------------------------------------------------------------

namespace {
static const char kSetPosition[] = "__ZN16CustomTextButton19setDrawablePositionERKN4juce9RectangleIfEE";
static const char kCreateImage[] = "__ZN16CustomTextButton11createImageEPKN4juce8DrawableES3_";
static const Offset O_position{0x2cc, kSetPosition, "add x0", 0, X86{0x2ec}};  // CustomTextButton: Rectangle<float>, where its drawable goes
static const Offset O_drawable{0x258, kSetPosition, "ldr x0", 0, X86{0x278}};  // its drawable (unique_ptr), the "on" one in the next word
static const Offset O_image{0x268, kCreateImage, "ldr x19", 0, X86{0x288}};    // the first of the four DrawableImages made of it
static const Offset O_imageOn{0x288, kCreateImage, "ldr x19", 12, X86{0x2a8}}; // and of the four made of the "on" drawable
static const Offset O_compWidth{0x40, "__ZN17ItemPageComponent7resizedEv", "ldr", 3};  // juce::Component: bounds {x, y, w, h}, w here

static const Offset O_drawableImage{0x160, "__ZN4juce13DrawableImage16setImageInternalERKNS_5ImageE", "ldr x0", 0, X86{0x190}};  // juce::DrawableImage: its Image
static const char kBitmapData[] = "__ZN4juce5Image10BitmapDataC1ERKS0_NS1_13ReadWriteModeE";
static const Addr A_bitmapData{kBitmapData};                    // juce::Image::BitmapData(const Image&, readOnly = 0)
static const Offset O_bitmapReleaser{0x28, kBitmapData, "str", 3};  // BitmapData {data, size, ...}: its unique_ptr<BitmapDataReleaser>

struct Bounds {
    int v[4];
    bool operator==(const Bounds& o) const { return !memcmp(v, o.v, sizeof v); }
};
static Bounds boundsOf(const void* comp) {
    Bounds b{};
    if (comp) memcpy(b.v, (const char*)comp + O_compWidth - 8, sizeof b.v);
    return b;
}
// What a button's images were made from (createImage's inputs: the two drawables and their bounds, the position, the
// button's own bounds, the display's scale), and the images themselves.
struct Made {
    void* drawable[2];
    void* image[2];
    Bounds drawableBounds[2], bounds;
    float position[4];
    unsigned screens;
    bool operator==(const Made& o) const {
        return drawable[0] == o.drawable[0] && drawable[1] == o.drawable[1] && image[0] == o.image[0] && image[1] == o.image[1] &&
               drawableBounds[0] == o.drawableBounds[0] && drawableBounds[1] == o.drawableBounds[1] && bounds == o.bounds &&
               !memcmp(position, o.position, sizeof position) && screens == o.screens;
    }
};
}  // namespace
static std::unordered_map<void*, Made> g_made;  // by button; main thread
static int g_same, g_differ;                    // relayout_check: buttons rendered again with the same / other pixels
static int g_kept, g_rendered;

static Made stateOf(void* button) {
    Made m{};
    char* b = (char*)button;
    m.drawable[0] = *(void**)(b + O_drawable), m.drawable[1] = *(void**)(b + O_drawable + 8);
    m.image[0] = *(void**)(b + O_image), m.image[1] = *(void**)(b + O_imageOn);
    for (int i = 0; i < 2; i++) m.drawableBounds[i] = boundsOf(m.drawable[i]);
    m.bounds = boundsOf(button);
    memcpy(m.position, b + O_position, sizeof m.position);
    m.screens = g_screens;
    return m;
}

// The pixels of the image a DrawableImage shows (empty: none). For relayout_check.
static std::vector<uint8_t> pixelsOf(void* drawableImage) {
    std::vector<uint8_t> out;
    const void* image = drawableImage ? (const char*)drawableImage + O_drawableImage : nullptr;
    if (!image || !*(void* const*)image) return out;
    char data[96] = {};
    at<void (*)(void*, const void*, int)>(A_bitmapData)(data, image, 0);
    const uint8_t* p = *(const uint8_t**)data;
    size_t n = *(size_t*)(data + 8);
    if (p && n && n < (64u << 20)) out.assign(p, p + n);
    if (void* releaser = *(void**)(data + O_bitmapReleaser)) (*(void (***)(void*))releaser)[1](releaser);  // (its deleting destructor)
    return out;
}

// CustomTextButton::setDrawable -> createImage (its only caller): after it, note what the images were made from.
extern "C" void rbm_hook_ctb_images(void* button, const void* drawable, const void* drawableOn) {
    RBM_ORIG(ctb_images, void (*)(void*, const void*, const void*))(button, drawable, drawableOn);
    if (![NSThread isMainThread]) return;
    if (g_made.size() > 20000) g_made.clear();  // (buttons that are gone stay in it: start over now and then)
    g_made[button] = stateOf(button);
    g_rendered++;
}

// CustomTextButton::setDrawablePosition -> setDrawable(copies of its own two drawables): the position is stored by
// then. Left out when the images there are the ones made from exactly this.
extern "C" void rbm_hook_ctb_position(void* button, const void* drawable, const void* drawableOn) {
    if (g_on && [NSThread isMainThread]) {
        auto it = g_made.find(button);
        if (it != g_made.end() && it->second == stateOf(button)) {
            g_kept++;
            if (!cfgInt("relayout_check", 0)) return;
            std::vector<uint8_t> was[2] = {pixelsOf(it->second.image[0]), pixelsOf(it->second.image[1])};
            RBM_ORIG(ctb_position, void (*)(void*, const void*, const void*))(button, drawable, drawableOn);
            Made now = stateOf(button);
            bool same = was[0] == pixelsOf(now.image[0]) && was[1] == pixelsOf(now.image[1]);
            (same ? g_same : g_differ)++;
            if (!same && g_differ <= 20) logf("relayout: check: a button's images differ from the ones kept (%d x %d, %zu and %zu bytes)", now.bounds.v[2], now.bounds.v[3], was[0].size(), was[1].size());
            return;
        }
    }
    RBM_ORIG(ctb_position, void (*)(void*, const void*, const void*))(button, drawable, drawableOn);
}

// ---- 3: hidden panels ------------------------------------------------------------------------------------------------

static const Addr A_isShowing{"__ZNK4juce9Component9isShowingEv"};
static const Addr A_numChildren{"__ZNK4juce9Component21getNumChildComponentsEv"};
static const Addr A_child{"__ZNK4juce9Component17getChildComponentEi"};
static const Addr A_desktop{"__ZN4juce7Desktop11getInstanceEv"};
static const Addr A_desktopComponent{"__ZNK4juce7Desktop12getComponentEi"};  // nullptr past the end

namespace {
enum { kResized = 1, kLookAndFeel = 2 };
struct LazyClass {
    const char* name;
    const Addr& vtable;
    const Offset &visibility, &lookAndFeel, &paint, &resized;  // juce::Component's slots in that vtable
    int defer;                                                 // what waits while an object of the class isn't showing
    void (*origVisibility)(void*) = nullptr;
    void (*origLookAndFeel)(void*) = nullptr;
    void (*origPaint)(void*, void*) = nullptr;
    void (*origResized)(void*) = nullptr;
    int deferred = 0, caughtUp = 0;
};
// A component with something left to do, and how the tick finds it again without touching freed memory: the way to it
// from the desktop (child indexes), walked through live components; it is used only if that way still ends at this
// address and at an object of the same class.
struct Waiting {
    int bits = 0, cls = 0;
    void* vptr = nullptr;
    std::vector<int> path;
    bool located = false, lost = false;
};
}  // namespace

// juce::Component's four slots in each class's vtable, from its address point (the vtable symbol + 0x10). Three of the
// classes have a virtual base, so their first slot is a word further on; the recipes find each by the method's name.
static const Addr A_vtSamplerSlot{"__ZTV18SamplerControlComp"};
static const Offset V_visSamplerSlot{0x70, "__ZTV18SamplerControlComp", "vt:__ZN18SamplerControlComp17visibilityChangedEv", 0};
static const Offset V_lafSamplerSlot{0xd0, "__ZTV18SamplerControlComp", "vt:__ZN18SamplerControlComp18lookAndFeelChangedEv", 0};
static const Offset V_paintSamplerSlot{0x100, "__ZTV18SamplerControlComp", "vt:__ZN18SamplerControlComp5paintERN4juce8GraphicsE", 0};
static const Offset V_resizedSamplerSlot{0x148, "__ZTV18SamplerControlComp", "vt:__ZN18SamplerControlComp7resizedEv", 0};
static const Addr A_vtSamplerPanel{"__ZTV19SamplerControlPanel"};
static const Offset V_visSamplerPanel{0x70, "__ZTV19SamplerControlPanel", "vt:__ZN4juce9Component17visibilityChangedEv", 0};
static const Offset V_lafSamplerPanel{0xd0, "__ZTV19SamplerControlPanel", "vt:__ZN19SamplerControlPanel18lookAndFeelChangedEv", 0};
static const Offset V_paintSamplerPanel{0x100, "__ZTV19SamplerControlPanel", "vt:__ZN19SamplerControlPanel5paintERN4juce8GraphicsE", 0};
static const Offset V_resizedSamplerPanel{0x148, "__ZTV19SamplerControlPanel", "vt:__ZN19SamplerControlPanel7resizedEv", 0};
static const Addr A_vtLighting{"__ZTVN6djplay20LightingControlPanelE"};
static const Offset V_visLighting{0x68, "__ZTVN6djplay20LightingControlPanelE", "vt:__ZN6djplay20LightingControlPanel17visibilityChangedEv", 0};
static const Offset V_lafLighting{0xc8, "__ZTVN6djplay20LightingControlPanelE", "vt:__ZN6djplay20LightingControlPanel18lookAndFeelChangedEv", 0};
static const Offset V_paintLighting{0xf8, "__ZTVN6djplay20LightingControlPanelE", "vt:__ZN6djplay20LightingControlPanel5paintERN4juce8GraphicsE", 0};
static const Offset V_resizedLighting{0x140, "__ZTVN6djplay20LightingControlPanelE", "vt:__ZN6djplay20LightingControlPanel7resizedEv", 0};
static const Addr A_vtUserPads{"__ZTVN6djplay11UserPadViewE"};
static const Offset V_visUserPads{0x70, "__ZTVN6djplay11UserPadViewE", "vt:__ZN4juce9Component17visibilityChangedEv", 0};
static const Offset V_lafUserPads{0xd0, "__ZTVN6djplay11UserPadViewE", "vt:__ZN6djplay11UserPadView18lookAndFeelChangedEv", 0};
static const Offset V_paintUserPads{0x100, "__ZTVN6djplay11UserPadViewE", "vt:__ZN4juce9Component5paintERNS_8GraphicsE", 0};
static const Offset V_resizedUserPads{0x148, "__ZTVN6djplay11UserPadViewE", "vt:__ZN6djplay11UserPadView7resizedEv", 0};
static const Addr A_vtHotCue{"__ZTVN6djplay10HotCueCompE"};
static const Offset V_visHotCue{0x68, "__ZTVN6djplay10HotCueCompE", "vt:__ZN4juce9Component17visibilityChangedEv", 0};
static const Offset V_lafHotCue{0xc8, "__ZTVN6djplay10HotCueCompE", "vt:__ZN6djplay10HotCueComp18lookAndFeelChangedEv", 0};
static const Offset V_paintHotCue{0xf8, "__ZTVN6djplay10HotCueCompE", "vt:__ZN6djplay10HotCueComp5paintERN4juce8GraphicsE", 0};
static const Offset V_resizedHotCue{0x140, "__ZTVN6djplay10HotCueCompE", "vt:__ZN6djplay10HotCueComp7resizedEv", 0};
static const Addr A_vtGridView{"__ZTVN6djplay13GridViewPanelE"};
static const Offset V_visGridView{0x70, "__ZTVN6djplay13GridViewPanelE", "vt:__ZN6djplay13GridViewPanel17visibilityChangedEv", 0};
static const Offset V_lafGridView{0xd0, "__ZTVN6djplay13GridViewPanelE", "vt:__ZN6djplay13GridViewPanel18lookAndFeelChangedEv", 0};
static const Offset V_paintGridView{0x100, "__ZTVN6djplay13GridViewPanelE", "vt:__ZN6djplay13GridViewPanel5paintERN4juce8GraphicsE", 0};
static const Offset V_resizedGridView{0x148, "__ZTVN6djplay13GridViewPanelE", "vt:__ZN6djplay13GridViewPanel7resizedEv", 0};
static const Addr A_vtPlayCue{"__ZTVN6djplay12PlayCuePanelE"};
static const Offset V_visPlayCue{0x68, "__ZTVN6djplay12PlayCuePanelE", "vt:__ZN6djplay12PlayCuePanel17visibilityChangedEv", 0};
static const Offset V_lafPlayCue{0xc8, "__ZTVN6djplay12PlayCuePanelE", "vt:__ZN6djplay12PlayCuePanel18lookAndFeelChangedEv", 0};
static const Offset V_paintPlayCue{0xf8, "__ZTVN6djplay12PlayCuePanelE", "vt:__ZN6djplay12PlayCuePanel5paintERN4juce8GraphicsE", 0};
static const Offset V_resizedPlayCue{0x140, "__ZTVN6djplay12PlayCuePanelE", "vt:__ZN6djplay12PlayCuePanel7resizedEv", 0};
#define LAZY(id, what) {#id, A_vt##id, V_vis##id, V_laf##id, V_paint##id, V_resized##id, what}
// What each does that is worth putting off (all of it looks only: bounds of children, SVGs, colours, tooltips):
//   sampler slots (64): lookAndFeelChanged parses 14 SVGs and renders 8 buttons each
//   sampler panel, lighting panel, user pad views (32, with 512 pads): resized lays out everything inside
//   hot cue pads, grid and cue editors: lookAndFeelChanged sets colours, translates tooltips, reloads the colour theme
static LazyClass g_lazy[] = {
    LAZY(SamplerSlot, kLookAndFeel), LAZY(SamplerPanel, kResized), LAZY(Lighting, kResized), LAZY(UserPads, kResized),
    LAZY(HotCue, kLookAndFeel),      LAZY(GridView, kLookAndFeel), LAZY(PlayCue, kLookAndFeel),
};
static const int kLazyClasses = sizeof g_lazy / sizeof *g_lazy;
static std::mutex g_waitLock;                    // (a paint may come on rekordbox's raster thread)
static std::unordered_map<void*, Waiting> g_waiting;
static volatile bool g_deferHidden = true;       // relayout_hidden, read in the tick

static bool showing(void* c) { return at<bool (*)(void*)>(A_isShowing)(c); }

// Does what `self` put off, if anything (self: a live object of class c).
static void catchUp(void* self, int c) {
    int bits = 0;
    {
        std::lock_guard<std::mutex> l(g_waitLock);
        auto it = g_waiting.find(self);
        if (it == g_waiting.end()) return;
        if (it->second.cls == c) bits = it->second.bits;  // (another class: an object long gone had this address)
        g_waiting.erase(it);
    }
    LazyClass& k = g_lazy[c];
    if (bits) k.caughtUp++;
    if (bits & kLookAndFeel) k.origLookAndFeel(self);
    if (bits & kResized) k.origResized(self);
}
// True if `what` was put off: self isn't showing.
static bool putOff(void* self, int c, int what) {
    if (!g_on || !g_deferHidden || !(g_lazy[c].defer & what) || showing(self)) return false;
    std::lock_guard<std::mutex> l(g_waitLock);
    if (g_waiting.size() > 8192) return false;  // (never seen: entries of objects that are gone and were never found)
    Waiting& w = g_waiting[self];
    if (w.cls != c || w.vptr != *(void**)self) w = Waiting{};
    w.cls = c, w.vptr = *(void**)self, w.bits |= what;
    g_lazy[c].deferred++;
    return true;
}
template <int C> static void lazyResized(void* self) {
    if (putOff(self, C, kResized)) return;
    catchUp(self, C);  // (a look change still waiting goes first, as it would have)
    g_lazy[C].origResized(self);
}
template <int C> static void lazyLookAndFeel(void* self) {
    if (putOff(self, C, kLookAndFeel)) return;
    g_lazy[C].origLookAndFeel(self);
    catchUp(self, C);
}
template <int C> static void lazyPaint(void* self, void* graphics) {
    catchUp(self, C);
    g_lazy[C].origPaint(self, graphics);
}
template <int C> static void lazyVisibility(void* self) {
    if (showing(self)) catchUp(self, C);
    g_lazy[C].origVisibility(self);
}

template <int C> static bool installLazy() {
    LazyClass& k = g_lazy[C];
    void** vt = (void**)(at<char*>(k.vtable) + 0x10);
    // The originals first: a slot may be called the moment it changes.
    k.origVisibility = (void (*)(void*))vt[k.visibility / 8], k.origLookAndFeel = (void (*)(void*))vt[k.lookAndFeel / 8];
    k.origPaint = (void (*)(void*, void*))vt[k.paint / 8], k.origResized = (void (*)(void*))vt[k.resized / 8];
    bool ok = patchSlot(&vt[k.paint / 8], (void*)&lazyPaint<C>) && patchSlot(&vt[k.visibility / 8], (void*)&lazyVisibility<C>);
    if (ok && (k.defer & kResized)) ok = patchSlot(&vt[k.resized / 8], (void*)&lazyResized<C>);
    if (ok && (k.defer & kLookAndFeel)) ok = patchSlot(&vt[k.lookAndFeel / 8], (void*)&lazyLookAndFeel<C>);
    return ok;
}
template <int... C> static bool installAllLazy(std::integer_sequence<int, C...>) { return (installLazy<C>() & ...); }

static void* componentAt(const std::vector<int>& path) {
    void* c = nullptr;
    for (size_t i = 0; i < path.size(); i++) {
        if (i == 0) c = at<void* (*)(void*, int)>(A_desktopComponent)(at<void* (*)()>(A_desktop)(), path[0]);
        else c = path[i] < at<int (*)(void*)>(A_numChildren)(c) ? at<void* (*)(void*, int)>(A_child)(c, path[i]) : nullptr;
        if (!c) return nullptr;
    }
    return c;
}
static void locate(void* c, std::vector<int>& path, std::unordered_set<void*>& wanted, int depth) {
    if (wanted.erase(c)) {
        std::lock_guard<std::mutex> l(g_waitLock);
        auto it = g_waiting.find(c);
        if (it != g_waiting.end()) it->second.path = path, it->second.located = true;
    }
    if (depth > 40 || wanted.empty()) return;
    for (int i = 0, n = at<int (*)(void*)>(A_numChildren)(c); i < n; i++) {
        void* ch = at<void* (*)(void*, int)>(A_child)(c, i);
        if (!ch) continue;
        path.push_back(i);
        locate(ch, path, wanted, depth + 1);
        path.pop_back();
        if (wanted.empty()) return;
    }
}
// The net under the three ways above: every tick, the waiting components that show by now. Main thread.
static void catchUpShowing() {
    struct Seen { void* obj; int cls; void* vptr; std::vector<int> path; };
    std::vector<Seen> known;
    std::unordered_set<void*> unknown;
    {
        std::lock_guard<std::mutex> l(g_waitLock);
        if (g_waiting.empty()) return;
        for (auto& [obj, w] : g_waiting) {
            if (w.located) known.push_back({obj, w.cls, w.vptr, w.path});
            else if (!w.lost) unknown.insert(obj);
        }
    }
    for (Seen& s : known) {
        void* c = componentAt(s.path);
        if (c == s.obj && *(void**)c == s.vptr) {
            if (showing(c)) catchUp(c, s.cls);
            continue;
        }
        std::lock_guard<std::mutex> l(g_waitLock);  // the window was rebuilt around it (or it is gone): look again
        auto it = g_waiting.find(s.obj);
        if (it != g_waiting.end() && it->second.located) it->second.located = false, unknown.insert(s.obj);
    }
    static double walkedAt;
    if (unknown.empty() || now() - walkedAt < 1) return;
    walkedAt = now();  // one walk of every window (16,700 components: about a millisecond), at most once a second
    void* desktop = at<void* (*)()>(A_desktop)();
    for (int i = 0;; i++) {
        void* top = at<void* (*)(void*, int)>(A_desktopComponent)(desktop, i);
        if (!top || unknown.empty()) break;
        std::vector<int> path{i};
        locate(top, path, unknown, 0);
    }
    std::lock_guard<std::mutex> l(g_waitLock);
    for (void* gone : unknown) {  // in no window: destroyed, or not on the desktop yet (then its paint catches up)
        auto it = g_waiting.find(gone);
        if (it != g_waiting.end()) it->second.lost = true;
    }
}

static void tick() {  // main thread, every 0.1 s
    g_deferHidden = cfgInt("relayout_hidden", 1) != 0;
    catchUpShowing();
    static int ticks, kept, rendered;
    if (++ticks % 10) return;
    bool log = cfgInt("relayout_log", 0);
    static int deferred[kLazyClasses], caught[kLazyClasses];
    std::string line;
    for (int c = 0; c < kLazyClasses; c++) {
        if (g_lazy[c].deferred != deferred[c] || g_lazy[c].caughtUp != caught[c]) {
            char part[96];
            snprintf(part, sizeof part, "%s%s %d put off, %d caught up", line.empty() ? "" : "; ", g_lazy[c].name, g_lazy[c].deferred - deferred[c], g_lazy[c].caughtUp - caught[c]);
            line += part;
        }
        deferred[c] = g_lazy[c].deferred, caught[c] = g_lazy[c].caughtUp;
    }
    if (log && !line.empty()) logf("relayout: hidden panels: %s", line.c_str());
    if (kept == g_kept && rendered == g_rendered) return;
    if (cfgInt("relayout_check", 0)) logf("relayout: check: %d buttons rendered again came out the same, %d different (since launch)", g_same, g_differ);
    else if (log) logf("relayout: %d buttons kept their images, %d rendered", g_kept - kept, g_rendered - rendered);
    else if (!kept && g_kept) logf("relayout: buttons that didn't change keep their images in a layout pass");
    kept = g_kept, rendered = g_rendered;
}

RBM_MOD({.name = "relayout", .init = [] {
             Method m = class_getInstanceMethod(objc_getClass("NSWindowListener"), @selector(windowDidChangeScreen:));
             if (!m) return logf("relayout: rekordbox's NSWindowListener not found, left as it is");
             g_origScreen = method_setImplementation(m, (IMP)windowDidChangeScreen);
             for (NSNotificationName name in @[NSApplicationDidChangeScreenParametersNotification, NSWindowDidChangeBackingPropertiesNotification])
                 [[NSNotificationCenter defaultCenter] addObserverForName:name object:nil queue:nil usingBlock:^(NSNotification*) { g_screens++; }];
             if (!installAllLazy(std::make_integer_sequence<int, kLazyClasses>{})) logf("relayout: a hidden panel's methods could not be replaced");
         }, .tick = tick, .enabled = &g_on});
