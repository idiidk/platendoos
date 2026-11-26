// uimon: a main-thread stall monitor and a command channel that drives rekordbox's UI from inside, for measuring how
// long the UI takes (see README.md here). Hot-loaded: uimon.sh start. Writes /tmp/rbm-uimon/stalls.txt:
//   STALL n ms    a job posted to the main thread every 5 ms waited that long (10 ms or more)
//   BUSY n ms     one turn of the main run loop took that long (8 ms or more)
//   SUM ...       on the command "sum <label>": main thread awake time, frames (nextDrawable calls) since the last sum;
//                 frames.txt then holds each frame's start, return and presented time
// Commands (one per line in /tmp/rbm-uimon/cmd.txt, "sleep <s>" between them): click|dclick|move|down|up <x> <y>
// (window points from the top left), key <keycode> <mods> [chars], menu <Menu/Item>, closewin <title>,
// resize <steps> <dx> <dy> <ms>, repaint <class>, tree <depth>, sum <label>. Synthesized events: no HID input, so
// plat::idleSeconds() doesn't see them.
#import <AppKit/AppKit.h>
#import <QuartzCore/QuartzCore.h>
#import <Metal/Metal.h>
#include <objc/runtime.h>
#include <pthread.h>
#include <mutex>
#include <atomic>
#include <thread>
#include <typeinfo>
#include <map>
#include <algorithm>
#include <cxxabi.h>
#include "rbmod.h"
using namespace rbm;
#define D "/tmp/rbm-uimon/"
#ifndef GEN
#define GEN 5
#endif
static const Addr A_numChildren{"__ZNK4juce9Component21getNumChildComponentsEv"};
static const Addr A_child{"__ZNK4juce9Component17getChildComponentEi"};
static const Addr A_bounds{"__ZNK4juce9Component15getScreenBoundsEv"};
static const Addr A_showing{"__ZNK4juce9Component9isShowingEv"};
static const Addr A_deskGet{"__ZN4juce7Desktop11getInstanceEv"};
static const Addr A_deskN{"__ZNK4juce7Desktop16getNumComponentsEv"};
static const Addr A_deskC{"__ZNK4juce7Desktop12getComponentEi"};
static const Addr A_setBounds{"__ZN4juce9Component9setBoundsEiiii"};
static const Addr A_repaint{"__ZN4juce9Component7repaintEv"};
struct R { int x, y, w, h; };
static double wall() { timespec t; clock_gettime(CLOCK_REALTIME, &t); return t.tv_sec + t.tv_nsec * 1e-9; }
static FILE* g_out;
static void out(const char* f, ...) { va_list a; va_start(a, f); vfprintf(g_out, f, a); va_end(a); fputc('\n', g_out); fflush(g_out); }
static int curGen() { FILE* f = fopen(D "gen.txt", "r"); int g = 0; if (f) { fscanf(f, "%d", &g); fclose(f); } return g; }
static std::string cls(void* c) {
    void* vt = *(void**)c; auto* ti = ((const std::type_info**)vt)[-1];
    int st; char* n = abi::__cxa_demangle(ti->name(), 0, 0, &st); std::string s = n ? n : ti->name(); free(n); return s;
}
static void tree(void* c, int d, int maxd, FILE* f) {
    R r = at<R (*)(const void*)>(A_bounds)(c);
    int n = at<int (*)(void*)>(A_numChildren)(c);
    fprintf(f, "%*s%s [%d,%d %dx%d] show=%d kids=%d\n", d * 2, "", cls(c).c_str(), r.x, r.y, r.w, r.h, (int)at<bool (*)(void*)>(A_showing)(c), n);
    if (d < maxd) for (int i = 0; i < n; i++) tree(at<void* (*)(void*, int)>(A_child)(c, i), d + 1, maxd, f);
}
static const Addr A_dismiss{"__ZN4juce9PopupMenu21dismissAllActiveMenusEv"};
static void* g_desk;
static void deskTop(int maxd, FILE* f) {
    void* dk = at<void* (*)()>(A_deskGet)();
    int n = at<int (*)(void*)>(A_deskN)(dk);
    for (int i = 0; i < n; i++) { fprintf(f, "TOP %d\n", i); tree(at<void* (*)(void*, int)>(A_deskC)(dk, i), 0, maxd, f); }
}
static NSWindow* winAt(NSPoint screen) {
    for (NSWindow* w in NSApp.orderedWindows) if (w.isVisible && NSPointInRect(screen, w.frame)) return w;
    return nil;
}
static NSEvent* mev(NSWindow* win, NSPoint scr, NSEventType t, int mods = 0, int cc = 1) {
    NSPoint p = [win convertPointFromScreen:scr];
    return [NSEvent mouseEventWithType:t location:p modifierFlags:mods timestamp:NSProcessInfo.processInfo.systemUptime windowNumber:win.windowNumber context:nil eventNumber:0 clickCount:cc pressure:1];
}
static NSPoint toCocoa(double x, double y) { return NSMakePoint(x, NSScreen.screens[0].frame.size.height - y); }
static void sumAndReset(const char*); static void* findCls(void*, const char*); static double mediaOff();
static void ctlSend(const std::string& line) {
    std::vector<std::string> a; { size_t i = 0; while (i < line.size()) { size_t j = line.find(' ', i); if (j == std::string::npos) j = line.size(); if (j > i) a.push_back(line.substr(i, j - i)); i = j + 1; } }
    if (a.empty()) return;
    auto num = [&](size_t i) { return i < a.size() ? atof(a[i].c_str()) : 0.0; };
    if (a[0] == "sum") { sumAndReset(a.size() > 1 ? a[1].c_str() : ""); return; }
    if (a[0] == "repaint") { void* dk = at<void* (*)()>(A_deskGet)(); int n = at<int (*)(void*)>(A_deskN)(dk); for (int i = 0; i < n; i++) if (void* c = findCls(at<void* (*)(void*, int)>(A_deskC)(dk, i), a[1].c_str())) { out("%.3f REPAINT %s media=%.4f", wall(), cls(c).c_str(), CACurrentMediaTime() + mediaOff()); at<void (*)(void*)>(A_repaint)(c); return; } out("%.3f no comp %s", wall(), a[1].c_str()); return; }
    if (a[0] == "resize") {  // resize <steps> <dx> <dy> <interval ms>: steps of setBounds on MainAppWindow, then back
        void* dk = at<void* (*)()>(A_deskGet)(); int n = at<int (*)(void*)>(A_deskN)(dk); void* mw = nullptr;
        for (int i = 0; i < n; i++) { void* c = at<void* (*)(void*, int)>(A_deskC)(dk, i); if (cls(c) == "MainAppWindow") mw = c; }
        if (!mw) { out("%.3f no MainAppWindow", wall()); return; }
        R r = at<R (*)(const void*)>(A_bounds)(mw); int steps = (int)num(1), dx = (int)num(2), dy = (int)num(3); double iv = num(4) / 1000;
        out("%.3f RESIZE start %d steps (%d,%d) from %dx%d", wall(), steps, dx, dy, r.w, r.h);
        for (int k = 1; k <= 2 * steps; k++) {
            int j = k <= steps ? k : 2 * steps - k;
            dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(k * iv * NSEC_PER_SEC)), dispatch_get_main_queue(), ^{ at<void (*)(void*, int, int, int, int)>(A_setBounds)(mw, r.x, r.y, r.w + j * dx, r.h + j * dy); });
        }
        return;
    }
    if (a[0] == "mark") { out("%.3f MARK %s", wall(), line.c_str() + 5); return; }
    if (a[0] == "tree") { FILE* f = fopen(D "tree.txt", "w"); deskTop((int)num(1), f); fclose(f); out("%.3f tree written", wall()); return; }
    if (a[0] == "wins") { FILE* f = fopen(D "wins.txt", "w"); for (NSWindow* w in NSApp.orderedWindows) fprintf(f, "%s title='%s' vis=%d frame=%.0f,%.0f %.0fx%.0f key=%d\n", object_getClassName(w), w.title.UTF8String, w.isVisible, w.frame.origin.x, w.frame.origin.y, w.frame.size.width, w.frame.size.height, w.isKeyWindow); fclose(f); return; }
    if (a[0] == "menus") {
        FILE* f = fopen(D "menus.txt", "w");
        std::function<void(NSMenu*, int)> dump = [&](NSMenu* m, int d) {
            for (NSMenuItem* it in m.itemArray) { fprintf(f, "%*s%s | key='%s' mods=%lx en=%d sel=%s\n", d * 2, "", it.title.UTF8String, it.keyEquivalent.UTF8String, (long)it.keyEquivalentModifierMask, it.enabled, it.action ? sel_getName(it.action) : "-"); if (it.submenu) dump(it.submenu, d + 1); }
        };
        dump(NSApp.mainMenu, 0); fclose(f); return;
    }
    if (a[0] == "closewin") { NSString* t = [NSString stringWithUTF8String:line.c_str() + 9]; for (NSWindow* w in NSApp.windows) if (w.isVisible && [w.title isEqualToString:t]) { out("%.3f CLOSEWIN %s", wall(), line.c_str() + 9); [[w standardWindowButton:NSWindowCloseButton] performClick:nil]; return; } out("%.3f no window %s", wall(), line.c_str()+9); return; }
    if (a[0] == "front") { for (NSWindow* w in NSApp.windows) if (w.isVisible && !strcmp(object_getClassName(w), "JuceNSWindow")) {} ; return; }
    if (a[0] == "click" || a[0] == "dclick" || a[0] == "move" || a[0] == "down" || a[0] == "up") {
        NSPoint s = toCocoa(num(1), num(2)); NSWindow* w = winAt(s);
        if (!w) { out("%.3f no window at %g,%g", wall(), num(1), num(2)); return; }
        [w orderFrontRegardless];
        int mods = a.size() > 3 ? (int)num(3) : 0;
        if (a[0] == "move") { NSView* v = [w.contentView hitTest:[w.contentView convertPoint:[w convertPointFromScreen:s] fromView:nil]]; NSEvent* e = mev(w, s, NSEventTypeMouseMoved, mods); [v mouseMoved:e]; return; }
        if (a[0] == "down") { [w sendEvent:mev(w, s, NSEventTypeLeftMouseDown, mods)]; return; }
        if (a[0] == "up") { [w sendEvent:mev(w, s, NSEventTypeLeftMouseUp, mods)]; return; }
        int cc = a[0] == "dclick" ? 2 : 1;
        out("%.3f CLICK %g,%g (win %s)", wall(), num(1), num(2), w.title.UTF8String);
        [w sendEvent:mev(w, s, NSEventTypeLeftMouseDown, mods, cc)];
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 60 * NSEC_PER_MSEC), dispatch_get_main_queue(), ^{ [w sendEvent:mev(w, s, NSEventTypeLeftMouseUp, mods, cc)]; });
        return;
    }
    if (a[0] == "key") {  // key <keycode> <mods bitmask: 1 shift 2 ctrl 4 alt 8 cmd> <chars>
        int kc = (int)num(1), m = (int)num(2); NSEventModifierFlags f = 0; if (m & 1) f |= NSEventModifierFlagShift; if (m & 2) f |= NSEventModifierFlagControl; if (m & 4) f |= NSEventModifierFlagOption; if (m & 8) f |= NSEventModifierFlagCommand;
        NSString* ch = a.size() > 3 ? [NSString stringWithUTF8String:a[3].c_str()] : @"";
        NSWindow* w = NSApp.keyWindow ?: NSApp.mainWindow; if (!w) for (NSWindow* x in NSApp.orderedWindows) if (x.isVisible) { w = x; break; }
        out("%.3f KEY %d mods %d to window %s", wall(), kc, m, w ? w.title.UTF8String : "(none)");
        NSEvent* e = [NSEvent keyEventWithType:NSEventTypeKeyDown location:NSZeroPoint modifierFlags:f timestamp:NSProcessInfo.processInfo.systemUptime windowNumber:w.windowNumber context:nil characters:ch charactersIgnoringModifiers:ch isARepeat:NO keyCode:kc];
        [NSApp sendEvent:e];
        NSEvent* u = [NSEvent keyEventWithType:NSEventTypeKeyUp location:NSZeroPoint modifierFlags:f timestamp:NSProcessInfo.processInfo.systemUptime windowNumber:w.windowNumber context:nil characters:ch charactersIgnoringModifiers:ch isARepeat:NO keyCode:kc];
        [NSApp sendEvent:u]; return;
    }
    if (a[0] == "menu") {  // menu <path with / > e.g. menu rekordbox/Preferences...
        std::string p = line.substr(5); NSMenu* m = NSApp.mainMenu; NSMenuItem* it = nil;
        size_t i = 0;
        while (i <= p.size()) { size_t j = p.find('/', i); if (j == std::string::npos) j = p.size(); NSString* t = [NSString stringWithUTF8String:p.substr(i, j - i).c_str()]; it = nil;
            for (NSMenuItem* x in m.itemArray) if ([x.title hasPrefix:t]) { it = x; break; }
            if (!it) { out("%.3f menu item %s not found", wall(), p.c_str()); return; }
            m = it.submenu; i = j + 1; if (!m) break; }
        out("%.3f MENU %s", wall(), p.c_str());
        NSMenu* par = it.menu; [par performActionForItemAtIndex:[par indexOfItem:it]]; return;
    }
    if (a[0] == "setsize") { void* dk = at<void* (*)()>(A_deskGet)(); void* c = at<void* (*)(void*, int)>(A_deskC)(dk, (int)num(1)); at<void (*)(void*, int, int, int, int)>(A_setBounds)(c, (int)num(2), (int)num(3), (int)num(4), (int)num(5)); return; }
    out("%.3f unknown command %s", wall(), line.c_str());
}

// ---- frame / runloop instrumentation ----
struct Frame { double call, ret, pres; char thr[24]; };
static std::mutex g_fm; static std::vector<Frame> g_frames; static std::atomic<long> g_nd{0};
static std::atomic<double> g_awake{0}; static std::atomic<long> g_iters{0}; static double g_iterStart;
static std::atomic<bool> g_inst{false};
static double mediaOff() { return wall() - CACurrentMediaTime(); }
static void installInstr() {
    if (g_inst.exchange(true)) return;
    Method m = class_getInstanceMethod([CAMetalLayer class], @selector(nextDrawable));
    IMP orig = method_getImplementation(m);
    id (^blk)(id) = ^id(id self) {
        double c = CACurrentMediaTime();
        id d = ((id(*)(id, SEL))orig)(self, @selector(nextDrawable));
        double r = CACurrentMediaTime();
        long idx; { std::lock_guard<std::mutex> l(g_fm); g_frames.push_back({c, r, 0, {0}}); idx = (long)g_frames.size() - 1; pthread_getname_np(pthread_self(), g_frames.back().thr, 24); }
        if (d) [(id<MTLDrawable>)d addPresentedHandler:^(id<MTLDrawable> dd) { std::lock_guard<std::mutex> l(g_fm); if (idx < (long)g_frames.size()) g_frames[idx].pres = dd.presentedTime; }];
        return d;
    };
    method_setImplementation(m, imp_implementationWithBlock(blk));
    CFRunLoopObserverRef ob = CFRunLoopObserverCreateWithHandler(0, kCFRunLoopAfterWaiting | kCFRunLoopBeforeWaiting, true, 0, ^(CFRunLoopObserverRef, CFRunLoopActivity a) {
        double t = CACurrentMediaTime();
        if (a == kCFRunLoopAfterWaiting) g_iterStart = t;
        else if (g_iterStart > 0) { double d = t - g_iterStart; g_awake = g_awake + d; g_iters++; if (d > 0.008) out("%.3f BUSY %.1f ms (one runloop turn)", t + mediaOff() - d, d * 1000); g_iterStart = 0; }
    });
    CFRunLoopAddObserver(CFRunLoopGetMain(), ob, kCFRunLoopCommonModes);
}
static double g_sumT0 = 0;
static void sumAndReset(const char* label) {
    double now_ = wall(); std::vector<Frame> fr; { std::lock_guard<std::mutex> l(g_fm); fr.swap(g_frames); }
    double dt = now_ - g_sumT0; double aw = g_awake.exchange(0); long it = g_iters.exchange(0);
    std::vector<double> iv; double blocked = 0; std::map<std::string, int> thr;
    for (size_t i = 0; i < fr.size(); i++) { blocked += fr[i].ret - fr[i].call; thr[fr[i].thr]++; if (i) iv.push_back((fr[i].call - fr[i - 1].call) * 1000); }
    std::sort(iv.begin(), iv.end());
    std::string ts; for (auto& p : thr) ts += p.first + ":" + std::to_string(p.second) + " ";
    out("%.3f SUM %s over %.2f s: main awake %.0f ms (%.1f%%) in %ld runloop turns; nextDrawable calls %zu (%.1f/s) blocked total %.1f ms; intervals ms p50 %.1f p95 %.1f max %.1f; threads %s", now_, label, dt, aw * 1000, 100 * aw / dt, it, fr.size(), fr.size() / dt, blocked * 1000, iv.empty() ? 0 : iv[iv.size() / 2], iv.empty() ? 0 : iv[iv.size() * 95 / 100], iv.empty() ? 0 : iv.back(), ts.c_str());
    FILE* f = fopen(D "frames.txt", "w"); double off = mediaOff(); for (auto& x : fr) fprintf(f, "%.4f %.4f %.4f %s\n", x.call + off, x.ret + off, x.pres ? x.pres + off : 0, x.thr); fclose(f);
    g_sumT0 = now_;
}
static void* findCls(void* c, const char* sub) {
    if (cls(c).find(sub) != std::string::npos && at<bool (*)(void*)>(A_showing)(c)) return c;
    int n = at<int (*)(void*)>(A_numChildren)(c);
    for (int i = 0; i < n; i++) if (void* r = findCls(at<void* (*)(void*, int)>(A_child)(c, i), sub)) return r;
    return nullptr;
}
extern "C" void rbm_experiment() {
    installInstr(); g_sumT0 = wall();
    g_out = fopen(D "stalls.txt", "a");
    std::thread([] {
        int myGen = GEN; (void)myGen;
        // monitor
        std::thread([] {
            while (curGen() == GEN) {
                std::atomic<int>* done = new std::atomic<int>(0); double t0 = wall(); double* tr = new double(0);
                plat::onMain([done, tr] { *tr = wall(); done->store(1); });
                while (!done->load() && curGen() == GEN) usleep(1000);
                double t1 = wall();
                if (done->load() && t1 - t0 > 0.010) out("%.3f STALL %.1f ms (ran %.1f ms after post)", t0, (t1 - t0) * 1000, (*tr - t0) * 1000);
                if (!done->load()) { usleep(20000); } // leak the job's flag intentionally (cannot free while queued)
                usleep(5000);
            }
        }).detach();
        // commands
        while (curGen() == GEN) {
            FILE* f = fopen(D "cmd.txt", "r");
            if (f) {
                std::string s; char b[4096]; while (fgets(b, sizeof b, f)) s += b; fclose(f); remove(D "cmd.txt");
                size_t i = 0; while (i < s.size()) { size_t j = s.find('\n', i); if (j == std::string::npos) j = s.size(); std::string l = s.substr(i, j - i); i = j + 1;
                    if (l.rfind("sleep ", 0) == 0) { usleep((useconds_t)(atof(l.c_str() + 6) * 1e6)); continue; }
                    if (l.empty()) continue;
                    auto* L = new std::string(l); plat::onMain([L] { ctlSend(*L); delete L; }); usleep(20000); }
            }
            usleep(30000);
        }
        out("%.3f gen %d exits", wall(), GEN);
    }).detach();
    logf("mon: started");
}
