// badge: a "PLATENDOOS" chip, styled like rekordbox's own buttons, in rekordbox's bottom bar, next to its logo, so a
// modded rekordbox is easy to tell apart from the plain one. Hover for the versions; click for a window with every
// mod, whether it's on, and the log and config (ui/about.html). The window is also where the mods are set: every
// mod's switch and its options, as the config file lists them (core's configSections), each written to that file
// as it is changed.
// The chip is a native view over the main window (badge_view.h; no hooks for it). While rekordbox shows a message
// in that bar (they start right of the logo too), the chip steps aside.
// A mod's on/off switch in the config is read once at startup (modEnabled); when the file now says something else,
// the chip's icon pulses orange, its tooltip and the window say which mods change after a restart.
//   config: badge=0 to hide
#include "../core/rbmod.h"
#include "../core/webui.h"
#include "../core/defaultoff.h"
#include "badge_view.h"
#include <string.h>

using namespace rbm;
namespace web = rbm::webui;

#ifndef PLATENDOOS_BUILD
#define PLATENDOOS_BUILD "dev"
#endif

// ---- the window the chip opens -------------------------------------------------------------

namespace {  // file-local

// (with the edition, docs/editions.md: "public" or "full (internal)")
std::string versions() { return "rekordbox " + plat::appVersion() + "  \xC2\xB7  platendoos " PLATENDOOS_BUILD "  \xC2\xB7  " + editionName(); }

web::Window* g_about;
std::string g_last;

// The mods whose switch in the config now differs from what runs ("flx4 off", "genres on"); not those this
// rekordbox build can't run anyway.
std::vector<std::string> pendingChanges() {
    std::vector<std::string> v;
    for (Mod& m : mods())
        if (!versionOff(m.name) && modSwitchNow(m.name) != modSwitchAtStart(m.name))
            v.push_back(std::string(m.name) + (modSwitchNow(m.name) ? " on" : " off"));
    return v;
}
std::string joined(const std::vector<std::string>& v) {
    std::string s;
    for (auto& x : v) s += (s.empty() ? "" : ", ") + x;
    return s;
}

void refreshAbout() {
    if (!g_about || !web::visible(g_about)) return;
    using web::jsonStr;
    int on = 0, n = (int)mods().size();
    for (Mod& m : mods()) on += modEnabled(m.name);
    std::string j = "{\"versions\":" + jsonStr(versions());
    j += ",\"summary\":" + jsonStr(std::to_string(on) + " of " + std::to_string(n) + " mods on" +
                                   (checkedBuild() ? "" : "  \xC2\xB7  untested rekordbox build: some mods may be off"));
    j += ",\"mods\":[";
    bool first = true;
    for (Mod& m : mods()) {
        bool pend = !versionOff(m.name) && modSwitchNow(m.name) != modSwitchAtStart(m.name);
        const char* state = versionOff(m.name) ? "unavailable" : pend ? "pending" : !modEnabled(m.name) ? "config" : "on";
        std::string text = versionOff(m.name) ? "off: not available in this build"
                           : pend             ? std::string(modEnabled(m.name) ? "on \xE2\x86\x92 off" : "off \xE2\x86\x92 on") + " after restart"
                           : modEnabled(m.name) ? "on"
                           : offUnlessAsked(m.name) && cfgStr(m.name).empty()
                               ? std::string("off: not yet tested on Windows (") + m.name + "=1 to try)"
                               : std::string("off in config (") + m.name + "=0)";
        j += std::string(first ? "" : ",") + "{\"name\":" + jsonStr(m.name) + ",\"state\":\"" + state + "\",\"text\":" + jsonStr(text);
        j += std::string(",\"on\":") + (modSwitchNow(m.name) ? "true" : "false") + ",\"available\":" + (versionOff(m.name) ? "false" : "true");
        // Its options, as the config file lists them, with what the file says now.
        const ConfigSection* sec = nullptr;
        for (const ConfigSection& c : configSections())
            if (c.mod == m.name) sec = &c;
        j += ",\"summary\":" + jsonStr(sec ? sec->summary : "") + ",\"options\":[";
        for (size_t i = 0; sec && i < sec->options.size(); i++) {
            const ConfigOption& o = sec->options[i];
            j += std::string(i ? "," : "") + "{\"key\":" + jsonStr(o.key) + ",\"def\":" + jsonStr(o.def) + ",\"text\":" + jsonStr(o.text);
            j += ",\"value\":" + jsonStr(cfgStr(o.key.c_str(), o.def.c_str())) + ",\"set\":" + (cfgHas(o.key.c_str()) ? "true" : "false");
            j += std::string(",\"kind\":\"") + (o.kind == ConfigOption::Switch ? "switch" : o.kind == ConfigOption::Choice ? "choice" : "text") + "\",\"choices\":[";
            for (size_t c = 0; c < o.choices.size(); c++) j += (c ? "," : "") + jsonStr(o.choices[c]);
            j += "]}";
        }
        j += "]}";
        first = false;
    }
    j += "]";
    std::vector<std::string> pend = pendingChanges();
    j += ",\"restart\":" + jsonStr(pend.empty() ? "" : "Restart rekordbox to apply: " + joined(pend));
    j += "}";
    if (j == g_last) return;
    g_last = j;
    web::post(g_about, j);
}

void onAboutMessage(const std::string& m) {
    std::string cmd = web::jsonGet(m, "cmd");
    if (cmd == "ready") g_last.clear();
    else if (cmd == "log") plat::openPath(plat::logFile());
    else if (cmd == "config") plat::openTextFile(plat::configDir() + "/config.txt");
    else if (cmd == "close") { web::close(g_about); return; }
    else if (cmd == "set" || cmd == "unset" || cmd == "mod") {
        // Only what the config's own list has: a mod's switch, or one of its options.
        std::string key = web::jsonGet(m, "key"), value = web::jsonGet(m, "value");
        bool known = false;
        for (Mod& mod : mods()) known |= cmd == "mod" && key == mod.name && !versionOff(mod.name);
        for (const ConfigSection& sec : configSections())
            for (const ConfigOption& o : sec.options) known |= cmd != "mod" && key == o.key;
        if (!known || value.size() > 400 || value.find_first_of("\r\n") != std::string::npos) return;
        if (cmd == "unset") unsetCfg(key.c_str());
        else if (cmd == "mod") {  // (back to what it is without a line: the line goes)
            bool on = value == "true";
            if (on == !offUnlessAsked(key.c_str())) unsetCfg(key.c_str());
            else setCfg(key.c_str(), on ? "1" : "0");
        }
        else setCfg(key.c_str(), value);
    }
    refreshAbout();
}

void showAbout() {
    g_about = web::open("mods", "about", "platendoos", 900, 640, onAboutMessage);  // ("mods": not the size remembered for the small window it was)
    g_last.clear();
    refreshAbout();
}

}  // namespace

// ---- rekordbox's status bar -----------------------------------------------------------------

// rekordbox's status bar (common::StatusBar's component), for its messages.
static const char* kStatusBarGet = "__ZN4juce15SingletonHolderIN6common9StatusBarENS_15CriticalSectionELb0EE3getEv";
static const Addr A_statusBarGet{kStatusBarGet};
static const Addr A_statusBar{"SingletonHolder<StatusBar>", [] { return singleton("__ZN13MainComponent13getBrowseRectEv", kStatusBarGet); }};
static const Addr A_statusBarComponent{"__ZNK6common9StatusBar12getComponentENS0_15StatusBarWindowE"};
static const Addr A_numChildren{"__ZNK4juce9Component21getNumChildComponentsEv"};
static const Addr A_child{"__ZNK4juce9Component17getChildComponentEi"};
static const Addr A_isShowing{"__ZNK4juce9Component9isShowingEv"};
static const Addr A_screenBounds{"__ZNK4juce9Component15getScreenBoundsEv"};
static const Addr A_labelText{"__ZNK4juce5Label7getTextEb"};

static badgeview::Bar screenBounds(void* c) {
    badgeview::Bar r{};
#ifdef _WIN32
    at<void* (*)(void*, badgeview::Bar*)>(A_screenBounds)(c, &r);  // (16 bytes: MSVC returns it through a hidden pointer)
#else
    r = at<badgeview::Bar (*)(void*)>(A_screenBounds)(c);
#endif
    return r;
}

// Whether c is exactly a juce::Label (by its RTTI name).
static bool isLabel(void* c) {
#ifdef _WIN32
    const char* col = *(const char**)((*(void***)c) - 1);       // the complete object locator
    const char* base = col - *(const uint32_t*)(col + 20);      // pSelf: the locator's own offset
    return !strcmp(base + *(const uint32_t*)(col + 12) + 16, ".?AVLabel@juce@@");
#else
    return !strcmp(*(const char**)((char*)(*(void***)c)[-1] + 8), "N4juce5LabelE");
#endif
}

// The status bar's component of the main window (null if not showing), and whether rekordbox shows a message in
// it where the chip is: a label there with text.
static void* statusBar(bool* message) {
    *message = false;
    void* bar = at<void* (*)(uintptr_t)>(A_statusBarGet)(A_statusBar + g_slide);
    void* comp = bar ? at<void* (*)(void*, int)>(A_statusBarComponent)(bar, 0) : nullptr;  // the main window's
    if (!comp || !at<bool (*)(void*)>(A_isShowing)(comp)) return nullptr;
    badgeview::Bar b = screenBounds(comp);
    for (int i = 0, n = at<int (*)(void*)>(A_numChildren)(comp); i < n; i++) {
        void* c = at<void* (*)(void*, int)>(A_child)(comp, i);
        if (!isLabel(c) || !at<bool (*)(void*)>(A_isShowing)(c)) continue;
        if (screenBounds(c).x - b.x >= 210) continue;  // right of the chip (it ends 210 pt in)
        if (!memberString(A_labelText, c, false).empty()) *message = true;
    }
    return comp;
}

static void update() {
    // Test commands (command-badge in the config folder): "about" opens the window; "state" also writes what the
    // window is sent to badge-state.json there (to look at the page outside rekordbox); "msg <json>" is handled as a
    // message from the page.
    std::string cmd = devCommand("badge");
    if (cmd == "about" || cmd == "state") showAbout();
    if (cmd == "state") plat::writeFileAtomic(plat::configDir() + "/badge-state.json", g_last.data(), g_last.size());
    if (cmd.rfind("msg ", 0) == 0) onAboutMessage(cmd.substr(4));  // "msg <json>": as if the page had sent it
    if (!modEnabled("badge")) return badgeview::update(false, false, nullptr, "", false);
    bool message = false;
    void* comp = statusBar(&message);
    badgeview::Bar bar = comp ? screenBounds(comp) : badgeview::Bar{};
    std::vector<std::string> pend = pendingChanges();
    badgeview::update(true, message, comp ? &bar : nullptr,
                      versions() + (checkedBuild() ? "" : " (untested rekordbox build: some mods may be off)") +
                          (pend.empty() ? "" : "\nRestart rekordbox to apply: " + joined(pend)),
                      !pend.empty());
    refreshAbout();
}

// The status bar is about to show or hide its message: update the chip right after, before the next paint.
#ifdef _WIN32
extern "C" void* rbm_hook_status_text(const void* label, void* result, bool useEdited) {  // (MSVC: String result via a hidden pointer)
    plat::onMain(update);
    return RBM_ORIG(status_text, void* (*)(const void*, void*, bool))(label, result, useEdited);
}
#else
extern "C" JStr rbm_hook_status_text(const void* label, bool useEdited) {
    plat::onMain(update);
    return RBM_ORIG(status_text, JStr (*)(const void*, bool))(label, useEdited);
}
#endif

// A new message fades in.
extern "C" void rbm_hook_status_fade(void* animator, void* component, int ms) {
    RBM_ORIG(status_fade, void (*)(void*, void*, int))(animator, component, ms);
    update();
}

// Its own timer rather than the mods' tick, which stops while the mod is off and so couldn't take it away.
static void init() {
    badgeview::setOnClick(showAbout);
    plat::everyOnMain(1, update);
}

RBM_MOD({.name = "badge", .init = init});
