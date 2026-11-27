// badge: a "PLATENDOOS" chip, styled like rekordbox's own buttons, in rekordbox's bottom bar, next to its logo, so a
// modded rekordbox is easy to tell apart from the plain one. Hover for the versions; click for a small window
// (ui/about.html): the versions, how many mods are on, what waits for a restart, the log and the config, and the
// Preferences button. That opens the Preferences window (ui/settings.html), where everything is set: every mod's
// switch and settings as they declared them (core/settings.h), each written to the config file as it is changed,
// and the parts from settings::source (rekordbox's own preferences, mods/prefs.cpp).
// The chip is a native view over the main window (badge_view.h; no hooks for it). While rekordbox shows a message
// in that bar (they start right of the logo too), the chip steps aside.
// A mod's on/off switch in the config is read once at startup (modEnabled); when the file now says something else,
// the chip's icon pulses orange, its tooltip and the window say which mods change after a restart. The same for the
// few options that are read once at startup (ConfigOption::restart), and the window has a button that restarts
// rekordbox (plat::restartApp).
//   config: badge=0 to hide
#include "../core/rbmod.h"
#include "../core/webui.h"
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

web::Window* g_about;  // the chip's small window
web::Window* g_prefs;  // the Preferences window
std::string g_last, g_lastAbout;
void showPrefs();
std::string g_goCat, g_goPage;  // a category of rekordbox's preferences the page is to show (openPrefs), until it says it went
int g_goN;

// What a restart would change: the mods whose switch in the config now differs from what runs ("flx4 off",
// "genres on"; not those this rekordbox build can't run anyway), and the settings read once at startup that were
// changed since (by their label).
std::vector<std::string> pendingChanges() {
    std::vector<std::string> v;
    for (Mod& m : mods())
        if (!versionOff(m.name) && modSwitchNow(m.name) != modSwitchAtStart(m.name))
            v.push_back(std::string(m.title ? m.title : m.name) + (modSwitchNow(m.name) ? " on" : " off"));
    for (const std::string& key : settings::pendingKeys()) v.push_back(setting(key.c_str()).about.label);
    return v;
}
std::string joined(const std::vector<std::string>& v) {
    std::string s;
    for (auto& x : v) s += (s.empty() ? "" : ", ") + x;
    return s;
}

bool anyPlaying() {  // (the pages ask twice before a restart stops the music)
    for (int ch = 0; ch < deckCount(); ch++)
        if (deckPlaying(ch)) return true;
    return false;
}
std::string modsOn() {
    int on = 0, n = (int)mods().size();
    for (Mod& m : mods()) on += modEnabled(m.name);
    return std::to_string(on) + " of " + std::to_string(n) + " mods on";
}

// The small window: who is who, and what waits.
void refreshSmall() {
    if (!g_about || !web::visible(g_about)) return;
    std::vector<std::string> pend = pendingChanges();
    web::Json o;
    o.add("platendoos", PLATENDOOS_BUILD).add("rekordbox", plat::appVersion()).add("edition", editionName()).add("summary", modsOn());
    o.add("warn", checkedBuild() ? "" : "Untested rekordbox build: some mods may be off.");
    o.add("restart", pend.empty() ? std::string() : "Restart rekordbox to apply: " + joined(pend));
    o.add("on", true).add("playing", anyPlaying());
    std::string j = o.text();
    if (j == g_lastAbout) return;
    g_lastAbout = j;
    web::post(g_about, j);
}

// What a mod's switch says ("on", "off after a restart"...): state is the word the page styles it by.
void modState(const Mod& m, const char** state, std::string* text) {
    bool pend = !versionOff(m.name) && modSwitchNow(m.name) != modSwitchAtStart(m.name);
    *state = versionOff(m.name) ? "unavailable" : pend ? "pending" : !modEnabled(m.name) ? "config" : "on";
    *text = versionOff(m.name)     ? "not available in this rekordbox build"
            : pend                 ? std::string(modEnabled(m.name) ? "off" : "on") + " after a restart"
            : modEnabled(m.name)   ? "on"
            : offUnlessAsked(m.name) && cfgStr(m.name).empty() ? "off: not yet tested on this system"
                                                               : "off";
}
web::Json settingsOf(const char* page) {
    web::Json list = web::Json::array();
    for (const Setting* s : settings::of(page)) list.itemRaw(settings::json(*s));
    return list;
}

// Everything the Preferences window shows, from what the mods declared (core/settings.h): the areas, a page per mod
// (what it is, its switch, its settings and buttons), the lists drop-downs choose from. The page has no list of its own.
void refreshAbout() {
    refreshSmall();
    if (!g_prefs || !web::visible(g_prefs)) return;
    web::Json o;
    o.add("versions", versions());
    o.add("summary", modsOn() + (checkedBuild() ? "" : "  \xC2\xB7  untested rekordbox build: some mods may be off"));
    o.raw("areas", settings::areasJson());
    std::vector<Mod> sorted = mods();
    std::stable_sort(sorted.begin(), sorted.end(), [](const Mod& a, const Mod& b) { return a.order < b.order; });
    web::Json pages = web::Json::array();
    for (Mod& m : sorted) {
        const char* state;
        std::string text;
        modState(m, &state, &text);
        web::Json p;
        p.add("mod", m.name).add("area", m.area ? m.area : "").add("title", m.title ? m.title : m.name).add("about", m.about ? m.about : "");
        if (m.rekordboxPage) p.add("rb", m.rekordboxPage);
        p.add("state", state).add("text", text).add("on", modSwitchNow(m.name)).add("available", !versionOff(m.name));
        p.add("settings", settingsOf(m.name)).raw("actions", settings::actionsJson(m.name)).raw("live", settings::livesJson(m.name));
        std::string status = settings::statusOf(m.name);  // (settings::status: what runs now, in the mod's words)
        if (!status.empty()) p.add("status", status);
        pages.item(p);
    }
    o.add("pages", pages);
    o.add("about", settingsOf("about"));  // the settings that belong to no mod's page: the General page's
    o.raw("lists", settings::listsJson());
    std::vector<std::string> pend = pendingChanges();
    o.add("restart", pend.empty() ? std::string() : "Restart rekordbox to apply: " + joined(pend));
#ifdef _WIN32
    o.add("platform", "win");
#else
    o.add("platform", "mac");
#endif
    o.add("playing", anyPlaying());
    if (!g_goCat.empty()) o.add("go", web::Json().add("cat", g_goCat).add("page", g_goPage).add("n", g_goN));
    for (const settings::Source& src : settings::sources()) {  // pages from elsewhere (rekordbox's own preferences)
        std::string part = src.state ? src.state() : "";
        if (!part.empty()) o.raw(src.id, part);
    }
    std::string j = o.text();
    if (j == g_last) return;
    g_last = j;
    web::post(g_prefs, j);
}

const settings::Source* sourceOf(const std::string& id) {
    for (const settings::Source& src : settings::sources())
        if (id == src.id) return &src;
    return nullptr;
}
bool g_aboutOpen;  // (the sources were told it is open)
void tellSources(bool open) {
    if (open == g_aboutOpen) return;
    g_aboutOpen = open;
    for (const settings::Source& src : settings::sources())
        if (src.shown) src.shown(open);
    if (!open) settings::windowClosed();  // (a metronome switched on in the window stops with it)
}

// What both windows send.
bool commonMessage(const std::string& cmd) {
    if (cmd == "log") plat::openPath(plat::logFile());
    else if (cmd == "config") plat::openTextFile(plat::configDir() + "/config.txt");
    else if (cmd == "restart") {  // the windows' Restart rekordbox button
        bool ok = plat::restartApp();
        logf("badge: restart asked for in the settings window%s", ok ? "" : ": the helper that starts rekordbox again could not be started, nothing done");
    }
    else return false;
    return true;
}

void onSmallMessage(const std::string& m) {
    std::string cmd = web::jsonGet(m, "cmd");
    if (cmd == "ready") g_lastAbout.clear();
    else if (cmd == "close") { web::close(g_about); return; }
    else if (cmd == "prefs") { web::close(g_about); showPrefs(); return; }
    else commonMessage(cmd);
    refreshSmall();
}

void onAboutMessage(const std::string& m) {
    std::string cmd = web::jsonGet(m, "cmd");
    if (cmd == "ready") g_last.clear();
    else if (commonMessage(cmd)) { if (cmd == "restart") return; }
    else if (cmd == "close") { web::close(g_prefs); return; }
    else if (cmd == "went") { if (web::jsonGet(m, "n") == std::to_string(g_goN)) g_goCat.clear(); }
    else if (cmd == "live") settings::setLive(web::jsonGet(m, "id"), web::jsonGet(m, "on") == "true");  // (settings::liveSwitch)
    else if (cmd == "action") {  // a button of a mod's page (settings::action)
        settings::runAction(web::jsonGet(m, "id"));
        return;
    }
    else if (cmd == "set" || cmd == "unset") {  // a declared setting only
        std::string key = web::jsonGet(m, "key"), value = web::jsonGet(m, "value");
        const Setting& s = setting(key.c_str());
        if (!s.known || value.size() > 400 || value.find_first_of("\r\n") != std::string::npos) return;
        if (cmd == "unset") s.reset();
        else s.set(value);
    }
    else if (const settings::Source* src = sourceOf(cmd)) {  // a page from elsewhere: its own messages
        if (src->message) src->message(m);
    }
    else if (cmd == "mod") {  // a mod's own switch (back to what it is without a line: the line goes)
        std::string key = web::jsonGet(m, "key");
        bool known = false, on = web::jsonGet(m, "value") == "true";
        for (Mod& mod : mods()) known |= key == mod.name && !versionOff(mod.name);
        if (!known) return;
        if (on == !offUnlessAsked(key.c_str())) unsetCfg(key.c_str());
        else setCfg(key.c_str(), on ? "1" : "0");
    }
    refreshAbout();
}

void showAbout() {
    g_about = web::open("chip", "about", "platendoos", 340, 390, onSmallMessage);
    g_lastAbout.clear();
    refreshSmall();
}

void showPrefs() {
    g_prefs = web::open("prefs", "settings", "Preferences", 880, 620, onAboutMessage);
    web::setFloating(g_prefs, false);  // (a window to work in, like rekordbox's own: rekordbox may cover it)
    g_last.clear();
    tellSources(true);
    refreshAbout();
}

// Offered to other mods as "badge.openPrefs": the Preferences window, at a category of rekordbox's preferences
// (prefs' ids; "" stays where it is) and page ("" its first). False while badge is off.
bool openPrefs(const char* cat, const char* page) {
    if (!modEnabled("badge")) return false;
    if (cat && *cat) g_goCat = cat, g_goPage = page ? page : "", g_goN++;
    showPrefs();
    return true;
}
RBM_PROVIDE("badge", "badge.openPrefs", &openPrefs, nullptr);

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
    // Test commands (command-badge in the config folder): "about" opens the chip's window, "prefs" the Preferences
    // window; "state" also writes what the Preferences window is sent to badge-state.json there (to look at the page
    // outside rekordbox); "msg <json>" is handled as a message from the Preferences page.
    std::string cmd = devCommand("badge");
    if (cmd == "about") showAbout();
    if (cmd == "prefs" || cmd == "state") showPrefs();
    if (cmd == "state") plat::writeFileAtomic(plat::configDir() + "/badge-state.json", g_last.data(), g_last.size());
    if (cmd.rfind("msg ", 0) == 0) onAboutMessage(cmd.substr(4));  // "msg <json>": as if the page had sent it
    if (g_aboutOpen && (!g_prefs || !web::visible(g_prefs))) tellSources(false);  // the window was closed
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

RBM_MOD({.name = "badge", .init = init,
         .title = "PLATENDOOS chip",
         .about = "The chip in rekordbox's bottom bar: hover for the versions, click for a small window with a way to these Preferences.",
         .area = "about", .order = 10});
