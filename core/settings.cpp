// The settings SDK's implementation (settings.h): the registry of declared settings, their values through the config
// file (core.cpp's cfg*), and what the settings window and the config file's list are made from.
#include "rbmod.h"
#include "webui.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <algorithm>
#include <map>
#include <mutex>
#include <set>
#include <unordered_map>

namespace rbm {

uint64_t cfgGeneration();  // core.cpp: counts the changes of the config as it is read

namespace {

struct ListSource { std::string name; void (*fill)(std::vector<std::string>&); };
struct ActionEntry { std::string mod, id, label, help; void (*run)(); };

// Function-local statics: settings are declared as globals of many files, in no order the registry could count on.
std::vector<const Setting*>& registry() {
    static std::vector<const Setting*>* r = new std::vector<const Setting*>;
    return *r;
}
std::vector<ListSource>& lists() {
    static std::vector<ListSource>* l = new std::vector<ListSource>;
    return *l;
}
std::vector<ActionEntry>& actions() {
    static std::vector<ActionEntry>* a = new std::vector<ActionEntry>;
    return *a;
}
std::string numberText(double v) {
    char b[48];
    snprintf(b, sizeof b, "%.10g", v);
    return b;
}
#ifdef _WIN32
const char kPlatform[] = "win";
#else
const char kPlatform[] = "mac";
#endif
bool here(const Setting& s) { return !*s.about.platform || !strcmp(s.about.platform, kPlatform); }

}  // namespace

// ---- a setting --------------------------------------------------------------------------------------------------------

Setting::Setting(const char* key, Kind kind, std::string def, SettingAbout about, const char* file)
    : key(key), kind(kind), def(std::move(def)), about(std::move(about)), mod(file ? settings::modOfFile(file) : "") {
    if (file) registry().push_back(this);
    else known = false;
}
Setting::Setting(const char* key, bool def, SettingAbout about, const char* file) : Setting(key, Switch, def ? "1" : "0", std::move(about), file) {}
Setting::Setting(const char* key, int def, SettingAbout about, const char* file) : Setting(key, Integer, std::to_string(def), std::move(about), file) {}
Setting::Setting(const char* key, double def, SettingAbout about, const char* file) : Setting(key, Number, numberText(def), std::move(about), file) {}
Setting::Setting(const char* key, const char* def, SettingAbout about, const char* file) : Setting(key, Text, def ? def : "", std::move(about), file) {}

double Setting::number() const { return cfgNum(key, atof(def.c_str())); }
std::string Setting::text() const { return cfgStr(key, def.c_str()); }
std::string Setting::textAtStart() const { return cfgStrAtStart(key, def.c_str()); }
double Setting::numberAtStart() const {
    std::string v = textAtStart();
    return v.empty() ? atof(def.c_str()) : atof(v.c_str());
}
bool Setting::isSet() const { return cfgHas(key); }
void Setting::set(const std::string& value) const {
    if (!known) return;
    if (value == def) unsetCfg(key);  // (the default: no line of its own in the file)
    else setCfg(key, value);
}
void Setting::set(double value) const { set(numberText(value)); }
void Setting::reset() const {
    if (known) unsetCfg(key);
}
void Setting::onChange(void (*fn)()) const {
    if (listeners.empty()) seen = text();
    listeners.push_back(fn);
}

const Setting& setting(const char* key) {
    static std::mutex lock;
    static std::unordered_map<std::string, const Setting*> byKey;
    static size_t indexed = 0;
    static std::set<std::string> complained;
    static const Setting* none = new Setting("", "", SettingAbout{}, nullptr);
    std::lock_guard<std::mutex> l(lock);
    auto& all = registry();
    for (; indexed < all.size(); indexed++) byKey.emplace(all[indexed]->key, all[indexed]);  // (the first declaration of a key counts)
    auto it = byKey.find(key);
    if (it != byKey.end()) return *it->second;
    if (complained.insert(key).second) logf("settings: '%s' is read but no mod declares it: it reads as off / 0 / empty", key);
    return *none;
}

// Its settings (core/settings.h): what the settings window and the config file's list show of them.
namespace opt {  // (core's own: shown on the General page)
const Setting gpu{"gpu", true, {.label = "Run the models on the graphics card", .help = "Off: Beat This! and the genre model run on the processor, and stems come from rekordbox's own engine.", .restart = true, .page = "about"}};
}  // namespace opt

namespace settings {

static std::vector<const Area*>& areaList() {
    static std::vector<const Area*>* v = new std::vector<const Area*>;  // (filled by static initialisers: never destroyed)
    return *v;
}
Area::Area(const char* id, const char* title, const char* blurb, const char* icon, int order)
    : id(id), title(title), blurb(blurb), icon(icon), order(order) {
    for (const Area* a : areaList())
        if (!strcmp(a->id, id)) {  // (another mod of the area declared it first)
            if (strcmp(a->title, title) || strcmp(a->blurb, blurb) || strcmp(a->icon, icon) || a->order != order)
                fprintf(stderr, "settings: area '%s' is declared twice, differently: the first counts\n", id);
            return;
        }
    areaList().push_back(this);
}
std::vector<const Area*> areas() {
    std::vector<const Area*> v = areaList();
    std::stable_sort(v.begin(), v.end(), [](const Area* a, const Area* b) { return a->order < b->order; });
    return v;
}
const std::vector<const Setting*>& all() { return registry(); }

std::vector<const Setting*> of(const char* mod) {
    std::vector<const Setting*> v;
    for (const Setting* s : registry())
        if (here(*s) && !strcmp(*s->about.page ? s->about.page : s->mod.c_str(), mod)) v.push_back(s);
    return v;
}

std::string modOfFile(const char* file) {
    std::string f = file;
    // (the build maps the checkout's path away: a core file is "core/settings.cpp")
    if (f.find("/core/") != std::string::npos || f.find("\\core\\") != std::string::npos || !f.compare(0, 5, "core/") || !f.compare(0, 5, "core\\")) return "core";
    f = f.substr(f.find_last_of("/\\") + 1);
    f = f.substr(0, f.find('.'));
    for (const char* suffix : {"_panel", "_mac", "_win", "_ort", "_view", "_overlay", "_backend", "_art", "_chip", "_model"}) {
        size_t n = strlen(suffix);
        if (f.size() > n && !f.compare(f.size() - n, n, suffix)) f.resize(f.size() - n);
    }
    return f;
}

void list(const char* name, void (*fill)(std::vector<std::string>&)) { lists().push_back({name, fill}); }
static std::vector<Source>& sourceList() {
    static std::vector<Source>* v = new std::vector<Source>;
    return *v;
}
void source(const Source& s) { sourceList().push_back(s); }
const std::vector<Source>& sources() { return sourceList(); }
void action(const char* mod, const char* id, const char* label, const char* help, void (*run)()) { actions().push_back({mod, id, label, help, run}); }
static std::vector<LiveSwitch>& lives() {
    static std::vector<LiveSwitch>* v = new std::vector<LiveSwitch>;
    return *v;
}
void liveSwitch(const LiveSwitch& s) { lives().push_back(s); }
bool setLive(const std::string& id, bool on) {
    for (const LiveSwitch& s : lives())
        if (id == s.id && modEnabled(s.mod) && !(on && s.why && *s.why())) return s.set(on), true;
    return false;
}
void windowClosed() {
    for (const LiveSwitch& s : lives())
        if (s.whileOpen && modEnabled(s.mod) && s.on()) s.set(false);
}
std::string livesJson(const char* mod) {
    webui::Json j = webui::Json::array();
    for (const LiveSwitch& s : lives())
        if (!strcmp(s.mod, mod))
            j.item(webui::Json().add("id", s.id).add("label", s.label).add("help", s.help ? s.help : "").add("on", modEnabled(mod) && s.on())
                       .add("why", !modEnabled(mod) ? "The feature is off." : s.why ? s.why() : ""));
    return j.text();
}
static std::vector<std::pair<std::string, std::string (*)()>>& statuses() {
    static std::vector<std::pair<std::string, std::string (*)()>>* v = new std::vector<std::pair<std::string, std::string (*)()>>;
    return *v;
}
void status(const char* mod, std::string (*text)()) { statuses().push_back({mod, text}); }
std::string statusOf(const char* mod) {
    std::string out;
    if (!modEnabled(mod)) return out;
    for (auto& s : statuses())
        if (s.first == mod && s.second) {
            std::string t = s.second();
            if (!t.empty()) out += (out.empty() ? "" : "\n") + t;
        }
    return out;
}
bool runAction(const std::string& id) {
    for (ActionEntry& a : actions())
        if (a.id == id && modEnabled(a.mod.c_str())) return a.run(), true;
    return false;
}

std::string json(const Setting& s) {
    using webui::Json;
    const SettingAbout& a = s.about;
    const char* kind = !a.choices.empty() ? "choice" : *a.list ? "list" : s.kind == Setting::Switch ? "switch" : s.kind == Setting::Text ? "text" : "number";
    Json j;
    j.add("key", s.key).add("kind", kind).add("label", *a.label ? a.label : s.key).add("help", a.help);
    j.add("def", s.def).add("value", s.text()).add("changed", s.changed()).add("restart", a.restart).add("pending", s.pending());
    if (a.advanced) j.add("advanced", true);
    if (a.debug) j.add("debug", true);
    if (a.slider) j.add("slider", true);
    if (a.wide) j.add("wide", true);
    if (*a.unit) j.add("unit", a.unit);
    if (a.min == a.min && a.max == a.max) j.add("min", a.min).add("max", a.max);
    if (a.step > 0) j.add("step", a.step);
    if (*a.list) j.add("list", a.list);
    if (*a.line) j.add("line", a.line).add("column", a.column);
    if (*a.needs) j.add("needs", a.needs);
    if (*a.rekordboxPage) j.add("rb", a.rekordboxPage);
    if (!a.choices.empty()) {
        Json list = Json::array();
        for (const SettingChoice& c : a.choices) list.item(Json::array().item(c.value).item(c.label));
        j.add("choices", list);
    }
    return j.text();
}
std::string actionsJson(const char* mod) {
    webui::Json j = webui::Json::array();
    for (ActionEntry& a : actions())
        if (a.mod == mod) j.item(webui::Json().add("id", a.id).add("label", a.label).add("help", a.help));
    return j.text();
}
std::string listsJson() {
    webui::Json j;
    for (ListSource& l : lists()) {
        std::vector<std::string> items;
        l.fill(items);
        webui::Json list = webui::Json::array();
        for (const std::string& x : items) list.item(x);
        j.add(l.name.c_str(), list);
    }
    return j.text();
}
std::string areasJson() {
    webui::Json j = webui::Json::array();
    for (const Area* a : areas()) j.item(webui::Json().add("id", a->id).add("title", a->title).add("blurb", a->blurb).add("icon", a->icon));
    return j.text();
}

std::vector<std::string> pendingKeys() {
    std::vector<std::string> v;
    for (const Setting* s : registry())
        if (here(*s) && s->pending() && (s->mod == "core" || (!versionOff(s->mod.c_str()) && modSwitchNow(s->mod.c_str())))) v.push_back(s->key);
    return v;
}

// "# " + text, wrapped at about 100 columns; first: what the first line starts with, rest: the others.
static void comment(std::string& out, const std::string& text, const char* first, const char* rest) {
    std::string line = first;
    size_t start = line.size();
    for (size_t i = 0; i <= text.size();) {
        size_t sp = text.find(' ', i);
        if (sp == std::string::npos) sp = text.size();
        std::string word = text.substr(i, sp - i);
        if (line.size() > start && line.size() + 1 + word.size() > 100) out += line + "\n", line = rest, start = line.size();
        if (!word.empty()) line += (line.size() > start ? " " : "") + word;
        i = sp + 1;
    }
    if (line.size() > start) out += line + "\n";
}
// One setting's lines of the list: what it is, then "#key=default".
static void entry(std::string& out, const Setting& s) {
    const SettingAbout& a = s.about;
    std::string text = a.label;
    if (*a.help) text += std::string(text.empty() ? "" : ". ") + a.help;
    if (!a.choices.empty()) {
        text += std::string(text.empty() ? "" : " ") + "(";
        for (size_t i = 0; i < a.choices.size(); i++) text += std::string(i ? "; " : "") + (*a.choices[i].value ? a.choices[i].value : "empty") + ": " + a.choices[i].label;
        text += ")";
    } else if (s.kind == Setting::Switch) text += " (1 on, 0 off)";
    if (*a.unit) text += std::string(" [") + a.unit + "]";
    if (a.restart) text += " (applies after a restart)";
    comment(out, text, "#   ", "#   ");
    out += std::string("#") + s.key + "=" + s.def + "\n";
}
// Where a setting is listed: on the page it names, else with the mod that declared it ("core" and "about": general).
static std::string owner(const Setting& s) {
    std::string o = *s.about.page ? s.about.page : s.mod.c_str();
    return o == "core" || o == "about" ? "" : o;
}
std::string configList() {
    std::string out = "# platendoos options\n"
                      "# Every line starting with # is off. Remove the # to use a line, then change its value.\n"
                      "# <mod>=0 turns a mod off (<mod>=1 on); that applies after a restart of rekordbox. The values shown are the\n"
                      "# defaults. Every other option is re-read every few seconds. The settings window (the PLATENDOOS chip) sets\n"
                      "# the same things.\n";
    std::string general;  // the settings that belong to no mod (the core's own: the window's General page)
    for (const Setting* s : registry())
        if (owner(*s).empty() && here(*s)) entry(general, *s);
    if (!general.empty()) out += "\n# ---- general ----\n" + general;
    std::vector<Mod> sorted = mods();
    std::sort(sorted.begin(), sorted.end(), [](const Mod& a, const Mod& b) { return strcmp(a.name, b.name) < 0; });
    for (const Mod& m : sorted) {
        out += std::string("\n# ---- ") + m.name + " ----\n";
        std::string what = std::string(m.title ? m.title : "") + (m.title && m.about && *m.about ? ": " : "") + (m.about ? m.about : "");
        if (!what.empty()) comment(out, what, "# ", "# ");
        out += std::string("#") + m.name + "=" + (offUnlessAsked(m.name) ? "0" : "1") + "\n";
        for (const Setting* s : registry())
            if (owner(*s) == m.name && here(*s)) entry(out, *s);
    }
    return out;
}

void rememberStart() {
    std::set<std::string> keys;
    for (const Setting* s : registry()) {
        if (!keys.insert(s->key).second) logf("settings: '%s' is declared twice (the second in %s): the first counts", s->key, s->mod.c_str());
        if (*s->about.needs) {  // "key", "!key", "key=value", "key!=value": the key must be one somebody declared
            std::string k = s->about.needs + (s->about.needs[0] == '!');
            k = k.substr(0, k.find_first_of("!="));
            if (!setting(k.c_str()).known) logf("settings: '%s' needs '%s', which no mod declares", s->key, k.c_str());
        }
        if (!s->listeners.empty()) s->seen = s->text();
    }
}
void tick() {
    static uint64_t seenGeneration = ~0ull;
    uint64_t g = cfgGeneration();
    if (g == seenGeneration) return;
    seenGeneration = g;
    for (const Setting* s : registry()) {
        if (s->listeners.empty()) continue;
        std::string now = s->text();
        if (now == s->seen) continue;
        s->seen = now;
        for (auto fn : s->listeners) fn();
    }
}

}  // namespace settings
}  // namespace rbm
