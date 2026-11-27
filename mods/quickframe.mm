// mac-only: CAMetalLayer settings of rekordbox's Metal window; the Windows build draws through OpenGL.
// quickframe: everything in rekordbox's main window (buttons, lists, waveforms) is painted into one image and shown
// through a CAMetalLayer that rekordbox leaves at its defaults: three drawables, presented in step with the display.
// At the full frame rate the display thread fills that queue, so every frame waits behind two others: measured on a
// 120 Hz MacBook Pro, 36 ms from the start of a frame to the glass (4.3 refreshes) and 42-47 ms from a button's
// repaint() to the glass. What the queue buys is smooth scrolling: a frame that comes late is hidden by the ones
// waiting. So:
//   - two drawables instead of three, always: 28 ms from frame start, 32 ms from repaint, and the presented frames
//     exactly as regular as with three (95% one refresh apart, the rest two, in both);
//   - while no deck plays, nothing scrolls, and frames are handed to the window server as they are made
//     (displaySyncEnabled off): 4-7 ms from frame start to handed over, 8-12 ms from repaint. (When such a frame
//     reaches the glass isn't reported in this mode: the next refresh or the one after.) While a deck plays the
//     display's step is kept, for the waveforms: without it a late frame shows as a hitch (7-70 gaps of more than two
//     refreshes in 10 s of playing).
// The window is composited by the window server either way, so nothing tears.
//   config: quickframe=0 to disable; quickframe_drawables=3 keeps rekordbox's queue; quickframe_playing=1 hands
//   frames over at once while a deck plays too; quickframe_stopped=0 keeps the display's step always
#import <AppKit/AppKit.h>
#import <QuartzCore/QuartzCore.h>
#include "../core/rbmod.h"

using namespace rbm;

// Its settings (core/settings.h): what the settings window and the config file's list show of them.
namespace opt {
const Setting drawables{"quickframe_drawables", 2, {.label = "Frames queued for the display", .choices = {{"2", "2 (quicker)"}, {"3", "3 (rekordbox's own)"}}}};
const Setting stopped{"quickframe_stopped", true, {.label = "Show frames at once while nothing plays"}};
const Setting playing{"quickframe_playing", false, {.label = "Also while a deck plays", .help = "Quicker clicks, but a frame that is late then shows as a small stutter.", .needs = "quickframe_stopped"}};
}  // namespace opt

static __weak CAMetalLayer* g_layer;
static bool g_touched;  // we changed g_layer's settings (so there is something to put back)

static CAMetalLayer* metalLayerIn(CALayer* l, int depth) {
    if ([l isKindOfClass:[CAMetalLayer class]]) return (CAMetalLayer*)l;
    if (depth < 3)
        for (CALayer* s in l.sublayers)
            if (CAMetalLayer* m = metalLayerIn(s, depth + 1)) return m;
    return nil;
}
// rekordbox's one Metal layer: a sublayer of its main window's content view (MetalLayer::Pimpl::attachTo).
static CAMetalLayer* metalLayer() {
    if (CAMetalLayer* l = g_layer) return l;
    for (NSWindow* w in NSApp.windows) {
        if (!w.isVisible || !w.contentView.layer) continue;
        if (CAMetalLayer* m = metalLayerIn(w.contentView.layer, 0)) {
            g_layer = m, g_touched = false;
            return m;
        }
    }
    return nil;
}

static void apply(CAMetalLayer* l, int drawables, bool sync) {
    if (l.maximumDrawableCount != drawables) l.maximumDrawableCount = drawables;
    if (l.displaySyncEnabled != sync) l.displaySyncEnabled = sync;
}

static void tick() {  // main thread, every 0.1 s
    CAMetalLayer* l = metalLayer();
    if (!l) return;
    bool playing = false;
    for (int ch = 0; ch < deckCount() && ch < 8; ch++) playing |= deckPlaying(ch);
    int drawables = opt::drawables.integer() == 3 ? 3 : 2;  // (CAMetalLayer takes 2 or 3)
    bool sync = playing ? !opt::playing.integer() : !opt::stopped.integer();
    static int logged = -1;
    if (logged != drawables) {
        logged = drawables;
        logf("quickframe: %d frames queued for the display instead of %d; shown as they are made while no deck plays%s", drawables, (int)l.maximumDrawableCount,
             opt::stopped.integer() ? "" : " (off: quickframe_stopped=0)");
    }
    apply(l, drawables, sync);
    g_touched = true;
}

// Switched off: rekordbox's own settings back, once.
static void restore() {
    CAMetalLayer* l = g_layer;
    if (l && g_touched) apply(l, 3, true);
    g_touched = false;
}

RBM_MOD({.name = "quickframe", .init = [] {
             plat::everyOnMain(0.1, [] { modEnabled("quickframe") ? tick() : restore(); });
         },
         .title = "Quicker clicks",
         .about = "What you click shows on screen sooner: a shorter queue of frames.",
         .area = "speed", .order = 20});
