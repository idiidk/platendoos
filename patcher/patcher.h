// platendoos patcher: installs platendoos for the rekordbox on this computer, from the files it carries (the mods,
// their models and pages; never any of rekordbox's files).
//   macOS:   makes ~/Applications/platendoos.app: a copy of /Applications/rekordbox 7/rekordbox.app, patched to load
//            librbmods.dylib (patcher/macho.h), with the FLX4 mapping and an inverted icon, signed for this Mac.
//   Windows: puts platendoos.exe, rbmods.dll and rbmods\ in %LOCALAPPDATA%\Programs\platendoos with a Start menu
//            shortcut; rekordbox's files stay as they are, except the FLX4 mapping (with permission; the original
//            is kept next to it).
// The window is ui/patcher.html (core/webui.h); patcher.cpp is the shared logic, install_mac.mm / install_win.cpp
// the platform parts.
#pragma once
#include <functional>
#include <set>
#include <string>
#include <vector>

namespace patcher {

// One line of the window's check list, or one mod.
struct Check {
    std::string state;  // ok, warn, fail, off (a mod not made for this platform)
    std::string label;
    std::string detail;
};

struct Target {
    bool found = false;
    std::string app;      // rekordbox's app (macOS) or rekordbox.exe (Windows)
    std::string version;  // e.g. 7.2.19
    std::string build;    // LC_UUID (macOS) / <TimeDateStamp>-<SizeOfImage> (Windows)
    bool tested = false;  // the build platendoos was tested with
    bool supported = false;
    std::string problem;  // why it can't be installed ("" if it can)
    std::vector<Check> notes;   // for the window: only what the user needs to know or do, in plain words
    std::vector<Check> checks;  // the technical details (build, hook sites, table...): --check only
    std::vector<Check> mods;    // per mod: will it work here
    std::string table;    // Windows: an address table the patcher made for this build (patcher/retable.h), for install
    std::string dest;     // where platendoos goes
    bool installed = false;
};

struct Options {
    std::set<std::string> off;  // mods turned off (the config's <mod>=0); the rest are on
    bool flx4 = true;     // the DDJ-FLX4 mapping for four decks and stems
    bool desktop = false; // Windows: a desktop shortcut too
};

// A progress line for the window and how far the whole job is (0..1); any thread.
using Step = std::function<void(const std::string& line, double progress)>;

// Platform parts.
Target find();
std::string install(const Target& t, const Options& o, const Step& step);  // "" or what went wrong (background thread)
std::string uninstall(const Target& t, const Step& step);
void launch(const Target& t);
void reveal(const std::string& path);
void onMain(std::function<void()> fn);
void background(std::function<void()> fn);
void quit();
std::string platformName();  // "macOS" / "Windows"
std::string payloadDir();    // the files platendoos is made from
std::string patcherVersion();
std::string configFile();    // platendoos's config (core/core.cpp): mac ~/Library/Application Support/rekordbox-mods/config.txt,
                             // win %APPDATA%\rekordbox-mods\config.txt

// Shared (patcher.cpp). The per-mod list from which hooks this rekordbox has (found[i] for hooks.def's hook i;
// mac: which platform).
std::vector<Check> modChecks(const std::vector<bool>& found, bool mac);
std::string humanSize(double bytes);

// Shared: opens the window and looks for rekordbox (patcher.cpp).
void start();
bool windowOpen();  // the app quits once the window is closed (and nothing is being installed)
bool busy();
// Without a window: --check prints what the window would show, --install / --uninstall do it (for testing and
// scripts; PLATENDOOS_DEST=<path> installs elsewhere, PLATENDOOS_SOURCE=<rekordbox.app, on Windows rekordbox.exe> takes
// that rekordbox instead of the installed one). The exit status.
int cli(const std::string& cmd);

}  // namespace patcher
