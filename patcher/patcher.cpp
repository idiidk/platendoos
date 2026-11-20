// platendoos patcher: the window's state and commands, shared by macOS and Windows (see patcher.h).
#include "patcher.h"
#include "../core/defaultoff.h"
#include "../core/webui.h"
#include "patcher_gen.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <filesystem>

namespace web = rbm::webui;

namespace patcher {

std::string humanSize(double bytes) {
    char b[32];
    if (bytes >= 1e9) snprintf(b, sizeof b, "%.1f GB", bytes / 1e9);
    else snprintf(b, sizeof b, "%.0f MB", bytes / 1e6);
    return b;
}

std::vector<Check> modChecks(const std::vector<bool>& found, bool mac) {
    using namespace gen;
    const char* const* hookMod = mac ? kHookMod_mac : kHookMod_win;
    auto missing = [&](const char* mod) {
        std::vector<std::string> m;
        for (int i = 0; i < kHooks; i++)
            if (!strcmp(hookMod[i], mod) && (i >= (int)found.size() || !found[i])) m.push_back(kHookName[i]);
        return m;
    };
    auto list = [](const std::vector<std::string>& v) {
        std::string s;
        for (size_t i = 0; i < v.size() && i < 4; i++) s += (i ? ", " : "") + v[i];
        if (v.size() > 4) s += " and " + std::to_string(v.size() - 4) + " more";
        return s;
    };
    std::vector<std::string> core = missing("core");
    std::vector<Check> out;
    for (const Mod& m : kMods) {
        Check c{"ok", m.name, m.summary};
        if (!(mac ? m.mac : m.win)) {
            c.state = "off";
            c.detail = mac ? "Windows only" : "macOS only for now";
        } else if (!core.empty()) {
            c.state = "fail";
            c.detail = "platendoos can't start in this rekordbox (missing " + list(core) + ")";
        } else {
            std::vector<std::string> miss = missing(m.name);
            if (!strcmp(m.name, "keycolumn"))
                for (auto& h : missing("columns")) miss.push_back(h);
            if (!miss.empty()) {
                // macOS: a hook left out is just never called; Windows switches the whole mod off (core/image_win.cpp)
                c.state = mac ? "warn" : "fail";
                c.detail = std::string(mac ? "may not work fully" : "will be off") + ": this rekordbox lacks " + list(miss);
            }
        }
        if (c.state == "ok" && rbm::offUnlessAsked(m.name))
            c.detail += " (off by default: not yet tested on Windows; tick it to try)";
        out.push_back(c);
    }
    return out;
}

FILE* openUtf8(const std::string& path, const char* mode) {
#ifdef _WIN32
    std::wstring m(mode, mode + strlen(mode));
    return _wfopen(std::filesystem::u8path(path).c_str(), m.c_str());
#else
    return fopen(path.c_str(), mode);
#endif
}

// The config's lines, and the mods it turns off (<mod>=0, as core's modEnabled reads it).
std::vector<std::string> readConfig() {
    {   // a config from before it had the .txt
        namespace fs = std::filesystem;
        std::error_code ec;
        fs::path now = fs::u8path(configFile()), old = now;
        old.replace_extension();
        if (fs::exists(old, ec) && !fs::exists(now, ec)) fs::rename(old, now, ec);
    }
    std::vector<std::string> lines;
    if (FILE* f = openUtf8(configFile(), "r")) {
        char line[1024];
        while (fgets(line, sizeof line, f)) {
            std::string s(line);
            s.erase(s.find_last_not_of("\r\n") + 1);
            lines.push_back(s);
        }
        fclose(f);
    }
    return lines;
}
std::string keyOf(const std::string& line) {
    size_t eq = line.find('=');
    if (line.empty() || line[0] == '#' || eq == std::string::npos) return "";
    std::string k = line.substr(0, eq);
    k.erase(0, k.find_first_not_of(" \t"));
    if (!k.empty()) k.erase(k.find_last_not_of(" \t") + 1);
    return k;
}
// (Mods that are off unless asked for, core/defaultoff.h, count as off until the config says <mod>=1.)
std::set<std::string> modsOff() {
    std::set<std::string> off;
    for (auto& m : gen::kMods)
        if (rbm::offUnlessAsked(m.name)) off.insert(m.name);
    for (auto& l : readConfig()) {
        std::string k = keyOf(l);
        for (auto& m : gen::kMods)
            if (k == m.name && atof(l.substr(l.find('=') + 1).c_str()) == 0 && !l.substr(l.find('=') + 1).empty()) off.insert(k);
            else if (k == m.name) off.erase(k);
    }
    return off;
}
// Writes the choices: <mod>=0 for the mods turned off, <mod>=1 where the config had one off that is on now (and
// for a mod that is off unless asked for, when it is ticked); every other line stays.
bool saveChoices(const Options& o) {
    const std::set<std::string>& off = o.off;
    std::vector<std::string> lines = readConfig();
    std::set<std::string> done;
    for (auto& l : lines) {
        std::string k = keyOf(l);
        for (auto& m : gen::kMods)
            if (k == m.name) l = k + (off.count(k) ? "=0" : "=1"), done.insert(k);
    }
    for (auto& k : off)
        if (!done.count(k) && !rbm::offUnlessAsked(k.c_str())) lines.push_back(k + "=0");  // (off anyway without a line)
    for (auto& m : gen::kMods)
        if (rbm::offUnlessAsked(m.name) && !off.count(m.name) && !done.count(m.name)) lines.push_back(std::string(m.name) + "=1");
    std::string path = configFile();
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::u8path(path).parent_path(), ec);
    FILE* f = openUtf8(path, "w");
    if (!f) return false;
    for (auto& l : lines) fprintf(f, "%s\n", l.c_str());
    return fclose(f) == 0;
}

namespace {

web::Window* g_win;
Target g_target;
Options g_opt;
std::vector<std::string> g_steps;
double g_progress = 0;
std::string g_phase = "finding";  // finding, ready, working, done, failed, removed
std::string g_error;

std::string checksJson(const std::vector<Check>& v) {
    using web::jsonStr;
    std::string j = "[";
    for (size_t i = 0; i < v.size(); i++)
        j += std::string(i ? "," : "") + "{\"state\":" + jsonStr(v[i].state) + ",\"label\":" + jsonStr(v[i].label) +
             ",\"detail\":" + jsonStr(v[i].detail) + "}";
    return j + "]";
}

void send() {
    using web::jsonStr;
    auto b = [](bool x) { return x ? "true" : "false"; };
    const Target& t = g_target;
    std::string j = "{\"platform\":" + jsonStr(platformName()) + ",\"patcher\":" + jsonStr(patcherVersion() + (strcmp(gen::kEdition, "public") ? " (full, internal)" : ""));
    j += std::string(",\"found\":") + b(t.found) + ",\"app\":" + jsonStr(t.app) + ",\"version\":" + jsonStr(t.version);
    j += ",\"build\":" + jsonStr(t.build) + ",\"tested\":" + b(t.tested) + ",\"supported\":" + b(t.supported);
    j += ",\"problem\":" + jsonStr(t.problem) + ",\"dest\":" + jsonStr(t.dest);
    j += std::string(",\"installed\":") + b(t.installed) + ",\"phase\":" + jsonStr(g_phase) + ",\"error\":" + jsonStr(g_error);
    j += std::string(",\"flx4\":") + b(g_opt.flx4) + ",\"desktop\":" + b(g_opt.desktop);
    j += ",\"off\":[";
    for (auto it = g_opt.off.begin(); it != g_opt.off.end(); ++it) j += (it == g_opt.off.begin() ? "" : ",") + jsonStr(*it);
    j += "]";
    j += ",\"notes\":" + checksJson(t.notes) + ",\"mods\":" + checksJson(t.mods);
    j += ",\"progress\":" + std::to_string(g_progress) + ",\"steps\":[";
    for (size_t i = 0; i < g_steps.size(); i++) j += (i ? "," : "") + jsonStr(g_steps[i]);
    j += "]}";
    web::post(g_win, j);
}

void step(const std::string& s, double progress) {
    onMain([s, progress] {
        if (!s.empty()) g_steps.push_back(s);
        g_progress = progress;
        send();
    });
}

void refind(const std::string& phase) {
    g_phase = "finding";
    send();
    background([phase] {
        Target t = find();
        onMain([t, phase] {
            g_target = t;
            if (g_phase == "finding") g_phase = phase;
            send();
        });
    });
}

void run(bool remove) {
    g_phase = "working";
    g_steps.clear();
    g_error.clear();
    g_progress = 0;
    send();
    Target t = g_target;
    Options o = g_opt;
    background([t, o, remove] {
        std::string err = remove ? uninstall(t, step) : install(t, o, step);
        if (err.empty() && !remove) {
            bool ok = saveChoices(o);
            std::string line = o.off.empty() ? "Every mod on" : std::to_string(o.off.size()) + (o.off.size() == 1 ? " mod" : " mods") + " turned off";
            step(ok ? line + " (change it any time in the badge's window or the config)" : "Your mod choices couldn't be saved in " + configFile(), 1);
        }
        Target now = find();
        onMain([err, remove, now] {
            g_error = err;
            g_target = now;
            if (err.empty()) g_progress = 1;
            g_phase = !err.empty() ? "failed" : remove ? "removed" : "done";
            send();
        });
    });
}

void onMessage(const std::string& m) {
    std::string cmd = web::jsonGet(m, "cmd");
    bool busy = g_phase == "working" || g_phase == "finding";
    if (cmd == "option") {
        std::string name = web::jsonGet(m, "name");
        bool on = web::jsonGet(m, "on") == "true";
        if (name == "flx4") g_opt.flx4 = on;
        if (name == "desktop") g_opt.desktop = on;
    } else if (cmd == "mod" && !busy) {
        std::string name = web::jsonGet(m, "name");
        if (web::jsonGet(m, "on") == "true") g_opt.off.erase(name);
        else g_opt.off.insert(name);
    } else if (cmd == "allmods" && !busy) {
        if (web::jsonGet(m, "on") == "true") g_opt.off.clear();
        else for (auto& md : gen::kMods) g_opt.off.insert(md.name);
    } else if (cmd == "install" && !busy && g_target.supported) {
        return run(false);
    } else if (cmd == "uninstall" && !busy && g_target.installed) {
        return run(true);
    } else if (cmd == "again" && !busy) {
        g_steps.clear();
        g_error.clear();
        g_progress = 0;
        return refind("ready");
    } else if (cmd == "open") {
        launch(g_target);
    } else if (cmd == "reveal") {
        reveal(g_target.dest);
    } else if (cmd == "close") {
        if (g_phase != "working") quit();
        return;
    }
    send();
}

}  // namespace

void start() {
    g_opt.off = modsOff();
    g_win = web::open("patcher", "patcher", "platendoos patcher", 640, 720, onMessage);
    refind("ready");
}

int cli(const std::string& cmd) {
    Target t = find();
    printf("platendoos patcher %s, %s edition\n", patcherVersion().c_str(), gen::kEdition);
    auto print = [](const char* head, const std::vector<Check>& v) {
        printf("%s\n", head);
        for (auto& c : v) printf("  [%-4s] %-14s %s\n", c.state.c_str(), c.label.c_str(), c.detail.c_str());
    };
    print("rekordbox:", t.checks);
    print("for the user:", t.notes);
    print("mods:", t.mods);
    std::string offList;
    for (auto& m : modsOff()) offList += " " + m;
    printf("mods turned off:%s\n", offList.empty() ? " none" : offList.c_str());
    printf("destination: %s%s\n", t.dest.c_str(), t.installed ? " (installed)" : "");
    if (!t.problem.empty()) printf("problem: %s\n", t.problem.c_str());
    if (cmd == "--check") return t.supported ? 0 : 1;
    Step step = [](const std::string& s, double p) { if (!s.empty()) printf("%3d%%  %s\n", (int)(p * 100), s.c_str()), fflush(stdout); };
    std::string err;
    if (cmd == "--install") {
        if (!t.supported) return 1;
        Options o;
        o.off = modsOff();
        if (const char* f = getenv("PLATENDOOS_FLX4")) o.flx4 = strcmp(f, "0") != 0;
        err = install(t, o, step);
        if (err.empty() && !saveChoices(o)) err = "the mod choices couldn't be saved in " + configFile();
    } else if (cmd == "--uninstall") {
        err = uninstall(t, step);
    } else {
        printf("usage: --check | --install | --uninstall\n");
        return 2;
    }
    if (!err.empty()) printf("failed: %s\n", err.c_str());
    return err.empty() ? 0 : 1;
}

bool windowOpen() { return web::visible(g_win); }
bool busy() { return g_phase == "working"; }

}  // namespace patcher
