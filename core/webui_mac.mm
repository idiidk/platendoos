// rbm::webui on macOS: an NSPanel with a WKWebView (see webui.h).
#import <AppKit/AppKit.h>
#import <WebKit/WebKit.h>
#include "webui.h"
#include "rbmod.h"
#include <map>
#include <vector>

namespace rbm::webui {
struct Window;
}

// rekordbox has no Edit menu, so Cmd-C/V/X/A/Z reach no text field: the web view sends them itself.
@interface RBMWebView : WKWebView
@end
@implementation RBMWebView
- (BOOL)performKeyEquivalent:(NSEvent*)e {
    NSEventModifierFlags m = e.modifierFlags & NSEventModifierFlagDeviceIndependentFlagsMask;
    if (self.window.isKeyWindow && (m & NSEventModifierFlagCommand) && !(m & (NSEventModifierFlagControl | NSEventModifierFlagOption))) {
        NSString* k = e.charactersIgnoringModifiers.lowercaseString;
        SEL s = nil;
        bool shift = m & NSEventModifierFlagShift;
        if ([k isEqualToString:@"v"]) s = @selector(paste:);
        else if ([k isEqualToString:@"c"]) s = @selector(copy:);
        else if ([k isEqualToString:@"x"]) s = @selector(cut:);
        else if ([k isEqualToString:@"a"]) s = @selector(selectAll:);
        else if ([k isEqualToString:@"z"]) {
            // The page first: one with its own undo (rbm.undoKey, see common.js) takes it when no text field has focus.
            SEL u = shift ? @selector(redo:) : @selector(undo:);
            NSString* js = shift ? @"!!(window.rbm && rbm.undoKey && rbm.undoKey(true))" : @"!!(window.rbm && rbm.undoKey && rbm.undoKey(false))";
            WKWebView* me = self;
            [self evaluateJavaScript:js completionHandler:^(id r, NSError*) {
                if (!([r isKindOfClass:[NSNumber class]] && [r boolValue])) [NSApp sendAction:u to:nil from:me];
            }];
            return YES;
        }
        if (s && [NSApp sendAction:s to:nil from:self]) return YES;
    }
    return [super performKeyEquivalent:e];
}
@end

@interface RBMWebHost : NSObject <WKScriptMessageHandler, WKNavigationDelegate>
@property(assign) rbm::webui::Window* win;
@end

namespace rbm::webui {

struct Window {
    NSPanel* panel = nil;
    WKWebView* web = nil;
    RBMWebHost* host = nil;
    std::function<void(const std::string&)> onMessage;
    std::string page;
    bool loaded = false;
    std::vector<std::string> queue;  // posted before the page loaded
    double reloadedAt = -1e9;        // after its web content process went (seconds; the patcher links this file without core, so no rbm::now)
    bool stale = false;              // went again right after: reloaded by the next open()
};

namespace {
std::map<std::string, Window*>& windows() {
    static std::map<std::string, Window*> m;
    return m;
}

void deliver(Window* w, const std::string& json) {
    // (%@ of a UTF-8 NSString: %s would read the bytes as Mac Roman, "·" became "¬∑" and accents broke)
    NSString* js = [NSString stringWithFormat:@"window.rbm&&window.rbm.onState&&window.rbm.onState(%@);",
                                              [NSString stringWithUTF8String:json.c_str()] ?: @"null"];
    [w->web evaluateJavaScript:js completionHandler:nil];
}
// ui/<page>.html, from the start (also after its web content process went: a file URL is loaded again with its
// read access rather than reloaded).
void load(Window* w) {
    NSString* dir = [NSString stringWithUTF8String:(plat::resourcesDir() + "/ui").c_str()];
    NSString* file = [dir stringByAppendingPathComponent:[NSString stringWithFormat:@"%s.html", w->page.c_str()]];
    [w->web loadFileURL:[NSURL fileURLWithPath:file] allowingReadAccessToURL:[NSURL fileURLWithPath:dir isDirectory:YES]];
}
// The tool windows stay above rekordbox's own while rekordbox is the active app (floating level), and drop back
// to the normal level when another app is: they shouldn't float over everything else on the Mac.
void keepOnTop(NSPanel* p) {
    p.level = NSApp.isActive ? NSFloatingWindowLevel : NSNormalWindowLevel;
    static bool observing;
    if (observing) return;
    observing = true;
    auto relevel = ^(NSNotification*) {
        NSWindowLevel level = NSApp.isActive ? NSFloatingWindowLevel : NSNormalWindowLevel;
        for (auto& kv : windows()) kv.second->panel.level = level;
    };
    NSNotificationCenter* nc = NSNotificationCenter.defaultCenter;
    [nc addObserverForName:NSApplicationDidBecomeActiveNotification object:nil queue:NSOperationQueue.mainQueue usingBlock:relevel];
    [nc addObserverForName:NSApplicationDidResignActiveNotification object:nil queue:NSOperationQueue.mainQueue usingBlock:relevel];
}
}  // namespace

Window* open(const char* id, const char* page, const char* title, int width, int height,
             std::function<void(const std::string&)> onMessage) {
    auto it = windows().find(id);
    if (it != windows().end()) {
        Window* w = it->second;
        w->onMessage = std::move(onMessage);
        if (w->stale) {
            w->stale = false;
            w->reloadedAt = CFAbsoluteTimeGetCurrent();
            load(w);
        }
        [w->panel makeKeyAndOrderFront:nil];
        return w;
    }
    Window* w = new Window;
    w->onMessage = std::move(onMessage);
    w->page = page;
    NSRect frame = NSMakeRect(0, 0, width, height);
    NSPanel* p = [[NSPanel alloc] initWithContentRect:frame
                                            styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                                                      NSWindowStyleMaskResizable | NSWindowStyleMaskMiniaturizable
                                              backing:NSBackingStoreBuffered
                                                defer:NO];
    p.title = [NSString stringWithUTF8String:title];
    p.appearance = [NSAppearance appearanceNamed:NSAppearanceNameDarkAqua];
    p.releasedWhenClosed = NO;
    p.hidesOnDeactivate = NO;
    keepOnTop(p);
    p.backgroundColor = [NSColor colorWithSRGBRed:0.118 green:0.118 blue:0.118 alpha:1];
    p.minSize = NSMakeSize(MIN(width, 600), MIN(height, 360));
    [p setFrameAutosaveName:[@"rbmods." stringByAppendingString:[NSString stringWithUTF8String:id]]];
    if (!p.frameAutosaveName.length || NSEqualRects(p.frame, frame)) [p center];

    RBMWebHost* host = [RBMWebHost new];
    host.win = w;
    WKWebViewConfiguration* cfg = [WKWebViewConfiguration new];
    [cfg.userContentController addScriptMessageHandler:host name:@"rbm"];
    WKWebView* web = [[RBMWebView alloc] initWithFrame:p.contentView.bounds configuration:cfg];
    web.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
    web.navigationDelegate = host;
    [web setValue:@NO forKey:@"drawsBackground"];
    [p.contentView addSubview:web];
    w->panel = p;
    w->web = web;
    w->host = host;

    load(w);
    windows()[id] = w;
    [p makeKeyAndOrderFront:nil];
    return w;
}

void post(Window* w, const std::string& json) {
    if (!w) return;
    if (w->loaded) return deliver(w, json);
    // A page that never loads (a missing ui file) mustn't keep every post of the session: the newest are enough,
    // a page asks for the whole state anyway once it is up (rbm.ready()).
    if (w->queue.size() >= 32) w->queue.erase(w->queue.begin());
    w->queue.push_back(json);
}

bool visible(Window* w) { return w && w->panel.visible; }

bool onScreen(Window* w) {
    return visible(w) && !w->panel.miniaturized && (w->panel.occlusionState & NSWindowOcclusionStateVisible);
}

void close(Window* w) {
    if (w) [w->panel orderOut:nil];
}

}  // namespace rbm::webui

@implementation RBMWebHost
- (void)userContentController:(WKUserContentController*)c didReceiveScriptMessage:(WKScriptMessage*)m {
    if (!_win || !_win->onMessage || ![m.body isKindOfClass:[NSString class]]) return;
    _win->onMessage([(NSString*)m.body UTF8String]);
}
- (void)webView:(WKWebView*)wv didFinishNavigation:(WKNavigation*)nav {
    if (!_win) return;
    _win->loaded = true;
    std::vector<std::string> q;
    q.swap(_win->queue);
    for (const std::string& j : q) rbm::webui::deliver(_win, j);
}
- (void)webView:(WKWebView*)wv didFailProvisionalNavigation:(WKNavigation*)nav withError:(NSError*)error {
    if (_win) rbm::logf("webui: %s.html didn't load: %s", _win->page.c_str(), error.localizedDescription.UTF8String ?: "?");
}
- (void)webView:(WKWebView*)wv didFailNavigation:(WKNavigation*)nav withError:(NSError*)error {
    if (_win) rbm::logf("webui: %s.html failed: %s", _win->page.c_str(), error.localizedDescription.UTF8String ?: "?");
}
// WebKit's content process went (killed under memory pressure, or crashed): the page is blank until it is loaded
// again. Once it is back it asks for the whole state (rbm.ready()), so what was queued for the old page can go.
- (void)webViewWebContentProcessDidTerminate:(WKWebView*)wv {
    if (!_win) return;
    _win->loaded = false;
    _win->queue.clear();
    double t = CFAbsoluteTimeGetCurrent();
    bool again = t - _win->reloadedAt < 10;  // a page that takes it down each time isn't reloaded in a loop
    rbm::logf("webui: %s.html's web content process ended%s", _win->page.c_str(), again ? " again: reloaded when opened next" : ", reloading");
    if (again) {
        _win->stale = true;
        return;
    }
    _win->reloadedAt = t;
    rbm::webui::load(_win);
}
@end
