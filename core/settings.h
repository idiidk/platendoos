// The settings SDK: how a mod has settings.
//
// A mod declares each setting once, as an object, with what it is to the people who set it:
//
//     namespace opt {
//     const Setting keep{"stemcache_keep", "all", {.label = "Save stems to disk", .help = "...",
//                        .choices = {{"all", "Everything"}, {"chosen", "Only what I separate"}, {"none", "Nothing"}}}};
//     const Setting maxGb{"stemcache_max_gb", 10, {.label = "Disk space for stems", .unit = "GB", .min = 0, .max = 2000}};
//     const Setting onDemand{"stemcache_ondemand", false, {.label = "Separate only when a stem control is used"}};
//     }
//
// and reads it through that object, wherever it needs the value: opt::keep.text(), opt::maxGb.number(),
// opt::onDemand.on(). That is all a setting needs. From the declarations the core
//   - keeps the value in step with the config file (config.txt in the mods' folder: "key=value", re-read every two
//     seconds, so a read is always what the file says, or the default; set() and reset() write the file),
//   - writes the file's commented list of every setting (a new config gets it),
//   - gives the settings window everything it shows: the window has no list of its own (mods/badge.cpp, ui/settings.html;
//     pages that aren't settings of a mod, such as rekordbox's own preferences, come from a settings::Source),
//   - knows what a restart would change (About::restart), and tells a mod when a value changed (onChange).
// The docs' tables are made from the same declarations (tools/build/settings_parse.py reads them from the sources, so
// keep a declaration a plain initializer: literals only).
//
// A mod says what it is itself in its RBM_MOD: .title, .about, .area (the window's sidebar: an Area some mod declares,
// below) and .untested (a platform it hasn't run on yet: off there until the config asks for it).
// Another file's setting is read by its key: setting("gpu").on(). A key nobody declared reads as off / 0 / "" and is
// logged once.
#pragma once
#include <cmath>
#include <string>
#include <vector>

namespace rbm {

struct SettingChoice {
    const char* value;  // as the config file has it
    const char* label;  // as the window shows it
};

// What a setting is to the people who set it. Only the label is needed.
struct SettingAbout {
    const char* label = "";
    const char* help = "";            // a sentence or two under the label
    const char* unit = "";            // after a number: "GB", "ms", "s"
    double min = NAN, max = NAN;      // a number's range (both, or neither)
    double step = 0;                  // its step (0: any)
    std::vector<SettingChoice> choices;  // a fixed set of values: a choice instead of a field
    const char* list = "";            // a list a mod gives at run time (settings::list): a drop-down of its entries
    bool slider = false;              // a number with a range, as a slider
    bool wide = false;                // a long text (a folder, a model id)
    bool restart = false;             // read once, when rekordbox starts: a change applies after a restart
    bool advanced = false;            // rarely needed: under "More options"
    bool debug = false;               // logging and test switches: under "Fault-finding"
    const char* line = "";            // settings with the same line share one line of a table ("Deck 1")...
    const char* column = "";          // ...each under its column's heading ("Gain")
    const char* page = "";            // shown on another mod's page ("about": the window's General page)
    const char* platform = "";        // "mac" / "win": only there (declared in a file both platforms build)
    // What this one only matters with, greyed out in the window otherwise: "key" (a switch that is on), "!key" (off),
    // "key=value" or "key!=value" (another setting's value).
    const char* needs = "";
    // "Audio/Configuration": shown on that page of rekordbox's own preferences in the window (as Mod::rekordboxPage,
    // for this one setting; in its mod's card when rekordbox's pages aren't there).
    const char* rekordboxPage = "";
};

class Setting {
public:
    enum Kind { Switch, Integer, Number, Text };
    Setting(const char* key, bool def, SettingAbout about, const char* file = __builtin_FILE());
    Setting(const char* key, int def, SettingAbout about, const char* file = __builtin_FILE());
    Setting(const char* key, double def, SettingAbout about, const char* file = __builtin_FILE());
    Setting(const char* key, const char* def, SettingAbout about, const char* file = __builtin_FILE());
    Setting(const Setting&) = delete;

    // The value now: what the config file says, else the default. Any thread.
    bool on() const { return number() != 0; }
    int integer() const { return (int)number(); }
    double number() const;
    std::string text() const;
    // The value as it was when rekordbox started: what a part that reads its setting once runs on.
    bool onAtStart() const { return numberAtStart() != 0; }
    int integerAtStart() const { return (int)numberAtStart(); }
    double numberAtStart() const;
    std::string textAtStart() const;

    bool isSet() const;                        // the config file has a line for it
    bool changed() const { return text() != def; }  // ...that says something else than the default
    bool pending() const { return about.restart && text() != textAtStart(); }  // a restart would change what runs
    // Writes the config file (the default takes its line out again). Main thread.
    void set(const std::string& value) const;
    void set(const char* value) const { set(std::string(value)); }
    void set(bool value) const { set(std::string(value ? "1" : "0")); }
    void set(int value) const { set(std::to_string(value)); }
    void set(double value) const;
    void reset() const;
    // fn runs on the main thread after the value changed (the file was edited, the window set it, set() was called).
    void onChange(void (*fn)()) const;

    const char* const key;
    const Kind kind;
    const std::string def;       // the default, as the config file would have it
    const SettingAbout about;
    const std::string mod;       // the mod of the file that declared it
    bool known = true;           // (false: the stand-in setting() gives for a key nobody declared)
    mutable std::vector<void (*)()> listeners;  // (core's: onChange's, and the value they were last called for)
    mutable std::string seen;

private:
    Setting(const char* key, Kind kind, std::string def, SettingAbout about, const char* file);
};

// A setting by its key (one another file declared).
const Setting& setting(const char* key);

namespace settings {

// An area of the window's sidebar, declared by a mod that is shown in it (any of them may; the first declaration of
// an id counts, so mods that share an area can each declare it and still build without the others):
//     const settings::Area area{"stems", "Stems", "How stems are separated and which are kept.", "<path d=.../>", 20};
// icon: SVG shapes drawn in a 16x16 box with a 1.6 stroke, no fill. A mod naming an area nobody declared is shown
// under "More".
struct Area {
    Area(const char* id, const char* title, const char* blurb, const char* icon, int order);
    Area(const Area&) = delete;
    const char* const id;
    const char* const title;
    const char* const blurb;
    const char* const icon;
    const int order;
};
std::vector<const Area*> areas();                 // the window's sidebar, in order
const std::vector<const Setting*>& all();         // every setting declared, in declaration order
std::vector<const Setting*> of(const char* mod);  // the ones shown on a mod's page (About::page counted)
std::string modOfFile(const char* file);          // mods/stemcache_panel.cpp -> "stemcache"

// A list a drop-down shows (SettingAbout::list): fill gives the entries there are now. Main thread.
void list(const char* name, void (*fill)(std::vector<std::string>& out));
// A button on a mod's page. run: main thread.
void action(const char* mod, const char* id, const char* label, const char* help, void (*run)());
bool runAction(const std::string& id);
// A switch on a mod's page whose state lives in the mod, not in the config file: something that runs while it is on
// (outputsync's metronome). on(): what it is now; set(): what was clicked; why(): "" when it can be used, else why not
// (the window greys it out and says so). whileOpen: switched off when the Preferences window closes. Main thread.
struct LiveSwitch {
    const char* mod;
    const char* id;
    const char* label;
    const char* help;
    bool (*on)();
    void (*set)(bool on);
    const char* (*why)() = nullptr;
    bool whileOpen = true;
};
void liveSwitch(const LiveSwitch& s);
bool setLive(const std::string& id, bool on);
void windowClosed();  // (the window closed: the whileOpen switches go off)
// A line a mod's page shows under its settings, worded at run time from what runs now (outputsync: which devices it
// lines up and by how much). "" shows nothing. Main thread.
void status(const char* mod, std::string (*text)());
std::string statusOf(const char* mod);

// Pages of the window that aren't the mods' settings: what another part has to set (rekordbox's own preferences,
// mods/prefs). The window gets state() under the source's id in what it is sent, and the source gets the page's
// messages whose "cmd" is its id. All on the main thread.
struct Source {
    const char* id;
    std::string (*state)();                    // JSON; "" while there is nothing to show
    void (*message)(const std::string& json);  // what the page sent it
    void (*shown)(bool open);                  // the window was opened / closed
};
void source(const Source& s);
const std::vector<Source>& sources();

// For the settings window (JSON): one setting; a mod's buttons ("[...]"); the lists ("{...}"); the areas ("[...]").
std::string json(const Setting& s);
std::string actionsJson(const char* mod);
std::string livesJson(const char* mod);
std::string listsJson();
std::string areasJson();
// What a restart would change among the settings: the keys of the pending ones (of mods that are switched on).
std::vector<std::string> pendingKeys();

// The config file's commented list of every mod and setting ("# platendoos options" ...), for a config without one.
std::string configList();
// Called by core: once at startup (the config as it is then), and on every tick (the listeners of changed settings).
void rememberStart();
void tick();

}  // namespace settings
}  // namespace rbm
