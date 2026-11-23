// rbmod: a small framework for mods injected into rekordbox (made for 7.2.19; macOS librbmods.dylib, arm64 and x86_64, Windows x64 rbmods.dll).
//
//  - Addresses: rekordbox's functions, vtables and fields are found by name and from its own code at load
//    (rbm::Addr, rbm::Offset below; on Windows from a per-build table), so an update that moves code doesn't break the mods; a mod whose
//    addresses or fields can't be found in the running build is switched off and the log says why.
//  - Hooks: call sites listed in ../hooks.def are routed by the patched binary to
//    extern "C" rbm_hook_<name> in this dylib; RBM_ORIG(name, FnType) gives the original target.
//  - Mods: each mods/*.cpp|.mm registers itself with RBM_MOD({...}) and gets init/tick callbacks and
//    can add items to the playlist (tree) context menu. Mods are switched on/off in the config.
//  - Config: <configDir>/config.txt (macOS: ~/Library/Application Support/rekordbox-mods), key=value lines, re-read every 2 s.
//  - Log: ~/Library/Logs/rekordbox-mods.log
#pragma once
#ifdef __OBJC__
#import <Foundation/Foundation.h>
#endif
#include "plat.h"
#include <stdint.h>
#include <stddef.h>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace rbm {

// ---- edition ------------------------------------------------------------------------------

// Which edition this build is (docs/editions.md): the full one (the default), or the public one (RBM_PUBLIC=1, built
// without private/), which never writes Spotify audio, decoded or derived (stems), to disk. A mod that behaves
// differently in the public edition has one switch on this, with a comment.
#ifdef RBM_PUBLIC
inline constexpr bool kPublicEdition = true;
#else
inline constexpr bool kPublicEdition = false;
#endif
inline const char* editionName() { return kPublicEdition ? "public" : "full (internal)"; }

// ---- addresses ----------------------------------------------------------------------------

extern intptr_t g_slide;
template <typename T> inline T at(uintptr_t a) { return (T)(a + g_slide); }

// 1 in rekordbox's x86_64 slice on macOS (an Intel Mac, or Rosetta: docs/intel-mac.md), else 0. Not __x86_64__ alone:
// the Windows build (clang-cl) defines that too.
#if defined(__APPLE__) && defined(__x86_64__)
#define RBM_MAC_X86 1
#else
#define RBM_MAC_X86 0
#endif

// An address in rekordbox, unslid (like the static addresses of the binary; at<T>() adds the slide). Found by
// its mangled symbol (plus an offset, e.g. a vtable's address point at +0x10; "stub:_x" is an imported
// function), or, for data without a symbol, by `find`, which decodes it from rekordbox's code (name then only
// says what it is, for the log). Resolved at load (core/symbols.cpp); 0 if it wasn't found, and then the mod of
// the file that declared it is switched off.
struct Addr {
    const char* name;
    intptr_t plus;
    uintptr_t (*find)();
    const char* file;
    mutable uintptr_t value = 0;
    Addr(const char* name, intptr_t plus = 0, const char* file = __builtin_FILE());
    Addr(const char* what, uintptr_t (*find)(), const char* file = __builtin_FILE());
    Addr(const Addr&) = delete;
    operator uintptr_t() const { return value; }
};

// A field offset (or other constant) inside rekordbox's objects, read from rekordbox's own code: the immediate
// of the nth instruction of `kind` in function `fn` (see tools/dev/recipes.py for the kinds; "vt:<method>" is the
// method's slot in the vtable fn). `expected` is its value in the tested build (7.2.19), which it starts as.
// In another build a field whose recipe doesn't resolve switches its mod off; tools/dev/recipes.py check tests
// every recipe against a binary.
//
// The recipes name arm64 instructions. In rekordbox's x86_64 code (an Intel Mac, docs/intel-mac.md) most fields
// have the same offset, and an Offset whose x86_64 value differs gives it as X86{value}: there the value is the
// arm64 recipe's result (read from the arm64 half of the same file) plus the difference to `expected`.
struct X86 {
    size_t value = 0;
    bool set = false;
    X86() = default;
    X86(size_t value) : value(value), set(true) {}
};
struct Offset {
    size_t expected;
    const char* fn;
    const char* kind;
    int nth;
    intptr_t x86Delta;  // the x86_64 value minus the arm64 one (0: the same)
    const char* file;
    mutable size_t value;
    mutable bool ok = true;
    Offset(size_t expected, const char* fn, const char* kind, int nth, const char* file = __builtin_FILE());
    Offset(size_t expected, const char* fn, const char* kind, int nth, X86 x86, const char* file = __builtin_FILE());
    Offset(const Offset&) = delete;
    operator size_t() const { return value; }
    // The tested build's value on this architecture: expected, plus the X86{} difference in the x86_64 slice.
    size_t expectedHere() const { return expected + (RBM_MAC_X86 ? x86Delta : 0); }
};

// Replaces a vtable slot (or any pointer in read-only data) with fn; returns the old value (nullptr if it
// couldn't be written).
void* patchSlot(void** slot, void* fn);

// delete of a rekordbox object through its virtual destructor (Itanium: the deleting destructor in slot 1;
// MSVC: the scalar deleting destructor in slot 0, flag 1 = free the memory).
inline void deleteObject(void* obj) {
#ifdef _WIN32
    (*reinterpret_cast<void* (***)(void*, unsigned)>(obj))[0](obj, 1);
#else
    (*reinterpret_cast<void (***)(void*)>(obj))[1](obj);
#endif
}

// dynamic_cast of a rekordbox object between two of its classes, by their typeinfo Addrs ("__ZTI...": on Windows
// the table maps them to the RTTI type descriptors); nullptr if obj isn't a dstType.
void* dynCast(void* obj, const void* srcType, const void* dstType);

// Reading rekordbox's code (unslid addresses), for Addr finders.
uintptr_t symbol(const char* name);                          // 0 if none
// What to add to the object pointer the mac code passes to a's function. 0 on mac. On Windows MSVC's member
// functions overriding a virtual of a non-primary base take that base's subobject as `this` (mac: the whole
// object, or for a __ZThn<N> thunk the object + N), so e.g. ((Fn)a)((char*)obj + thisDelta(a), ...).
// Only for a pointer the mod made the mac way: one rekordbox's Windows code hands over (e.g. getMixerState's
// IDjMixerState) already points at the subobject that Windows function takes.
intptr_t thisDelta(const Addr& a);
uint32_t insn(uintptr_t a);
uintptr_t callSite(const char* caller, const char* target, int nth);  // the nth "bl target" in caller, 0 if none
uintptr_t adrpAddBefore(uintptr_t site, int reg);            // address put in x<reg> by adrp + add just before site
uintptr_t adrpAdd(const char* fn, int nth);                  // address built by fn's nth adrp + add
uintptr_t singleton(const char* caller, const char* get);    // juce::SingletonHolder that caller's first get() is on
bool immediate(const char* fn, const char* kind, int nth, size_t* out);  // as Offset's recipes
// In rekordbox's x86_64 slice (RBM_MAC_X86, docs/intel-mac.md) the finders above that read arm64 instructions answer
// as follows:
//  - callSite: the nth "call target" (E8 rel32, found by a byte scan of the caller; "stub:_x" is the __stubs entry).
//    The compiler orders the calls differently in the two slices, so nth can differ from arm64's: check it there
//    (tools/dev/rbbin.py's Binary(data, CPU_X86_64).calls) before using the same nth.
//  - singleton: the holder's own symbol (every juce::SingletonHolder has one in both slices, e.g.
//    __ZN2db10DatabaseIF15singletonHolderE, named after get()'s class); caller isn't read.
//  - adrpAdd, adrpAddBefore: no arm64 code to read: 0 (a mod that needs one is off; use leaBefore or a symbol).
//    insn reads the 4 bytes at a, which are x86 code there.
//  - immediate (and every Offset): instruction recipes are read from the arm64 half of the same file on disk, "vt:"
//    recipes from the running x86_64 vtables; immediate() itself adds no X86{} difference.
// leaBefore: x86_64 only (0 on arm64). The address `lea <reg>, [rip + x]` puts in reg in the 48 bytes before site
// (the closest one), reg in x86 numbering: 0 rax, 1 rcx, 2 rdx, 3 rbx, 6 rsi, 7 rdi, 8-15 r8-r15. For an
// address passed to the call at site: arm64's x0, x1, x2, x3, x4, x5 are rdi (7), rsi (6), rdx (2), rcx (1), r8, r9
// when the function returns no struct (else x8's pointer takes rdi and the rest move up by one).
uintptr_t leaBefore(uintptr_t site, int reg);
std::string rekordboxBuild();                                // LC_UUID of the running rekordbox
bool testedBuild();                                          // the build the expected values come from
bool checkedBuild();                                         // that one, or a later build every mod was run on

enum HookId {
#define HOOK(name, caller, nth, count, target, kind) HK_##name,
#include "../hooks.def"
#undef HOOK
    HK_COUNT
};
extern const Addr kHookOrig[HK_COUNT];
bool hookPatched(HookId h);  // the patcher found its call site in this build
#define RBM_ORIG(name, FnType) (::rbm::at<FnType>(::rbm::kHookOrig[::rbm::HK_##name]))

// ---- juce::String -------------------------------------------------------------------------

// juce::String is a single pointer to its UTF-8 text (JStr builds it from UTF-8). Non-trivial, so returned via x8 like the real one.
struct JStr {
    const char* text;
    explicit JStr(const char* s);
    ~JStr();
    JStr(const JStr&) = delete;
    JStr& operator=(const JStr&) = delete;
};

// Calls a member function of rekordbox that returns a juce::String by value, and gives its text. The result
// goes through a hidden pointer: Itanium passes it first (x8 / rdi), MSVC after `this`.
template <class... A> std::string memberString(uintptr_t fn, const void* self, A... a) {
#ifdef _WIN32
    alignas(JStr) char buf[sizeof(JStr)];
    JStr* r = (JStr*)buf;
    at<void* (*)(const void*, JStr*, A...)>(fn)(self, r, a...);
    std::string out = r->text ? r->text : "";
    r->~JStr();
    return out;
#else
    JStr res = at<JStr (*)(const void*, A...)>(fn)(self, a...);
    return res.text ? res.text : "";
#endif
}

// ---- config / log -------------------------------------------------------------------------

void reloadConfig(bool force = false);  // cheap; re-reads the file at most every 2 s
std::string cfgStr(const char* key, const char* def = "");
double cfgNum(const char* key, double def);
inline int cfgInt(const char* key, int def) { return (int)cfgNum(key, def); }
void setCfg(const char* key, const std::string& value);  // rewrites that line of the config file
bool modEnabled(const char* mod);       // "<mod>=0" in the config disables a mod (default on), as does a build
                                        // in which its addresses aren't all found. Decided once at startup (the config as
                                        // it was then) and kept for the session: a changed switch applies after a restart.
bool modSwitchNow(const char* mod);     // the config's switch for the mod as it is now (live; versionOff not counted)
bool modSwitchAtStart(const char* mod); // the same, as it was when rekordbox started: what modEnabled follows
bool versionOff(const char* mod);       // off because this rekordbox build lacks something it needs
void logf(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
// Development: the line in <configDir>/command-<mod> (removed when read; at most once a second), "" if none.
// For scripted test runs, e.g. "open <playlist id>". Logged as "<mod>: command '...'".
std::string devCommand(const char* mod);
// Development: the tracks a test command names, of the list the browser shows: "<n>" its first n rows, "rows <a> <b> ..."
// those rows (0-based, as the analyzer's "rows" command logs them; rows not paged in are left out), else the paths /
// URIs themselves, with spaces between them (a file path is taken whole; tabs between several). Main thread.
// Core's own command file, <configDir>/command-core: "select <tracks>" makes a selection playlist of them
// (selectionPlaylist) and logs its id, for the mods' "open <playlist id>" commands.
std::vector<std::string> devTracks(const std::string& spec);
double now();                           // seconds (CFAbsoluteTime)

// ---- mods ---------------------------------------------------------------------------------

struct Menu;  // juce::PopupMenu
struct Mod {
    const char* name;
    void (*init)() = nullptr;                         // main thread, once after launch
    void (*tick)() = nullptr;                         // main thread, every 0.1 s (only while enabled)
    void (*playlistMenu)(Menu*, void* treeItem) = nullptr;  // append items to a playlist's context menu
    // append items to the track list's context menu; uris: the selected tracks (paths / streaming URIs), selection:
    // the same tracks as a playlist id (selectionPlaylist below), for the windows that open on a playlist
    void (*trackMenu)(Menu*, const std::vector<std::string>& uris, const std::string& selection) = nullptr;
    volatile bool* enabled = nullptr;                 // kept equal to modEnabled(name), for hooks on any thread
    int menuOrder = 50;                               // position of its playlist menu items (lower first)
};
std::vector<Mod>& mods();
struct ModRegistrar { ModRegistrar(Mod m) { mods().push_back(m); } };
#define RBM_CAT2(a, b) a##b
#define RBM_CAT(a, b) RBM_CAT2(a, b)
// RBM_MOD({.name = "x", .tick = ..., ...})
#define RBM_MOD(...) static ::rbm::ModRegistrar RBM_CAT(rbm_mod_, __LINE__)(::rbm::Mod __VA_ARGS__)

// ---- between mods -------------------------------------------------------------------------

// Mods don't call each other directly, so each still builds, runs and can be switched off on its own. A mod
// offers a function by name (at load, e.g. from a static initialiser); another looks it up when it needs it.
// provided() is null when the offering mod isn't built in or is switched off (its enabled flag, so any thread),
// and providerOf() names the mod, so the caller can say which mod a feature needs.
void provide(const char* mod, const char* name, void* fn, volatile bool* enabled);
void* provided(const char* name);
const char* providerOf(const char* name);  // the mod offering name, even while it's off ("" if none)
template <typename F> inline F service(const char* name) { return (F)provided(name); }
#define RBM_PROVIDE(mod, name, fn, enabled) \
    static int RBM_CAT(rbm_provide_, __LINE__) = (::rbm::provide(mod, name, (void*)(fn), enabled), 0)

// Services the analysis hooks in core call: see "analysis" below (timing.*, analysis.stage, bettergrid.prefetch, betterkey.*).
// Offered by quickdeck:
//   "analysis.stage"        void (const void* thread, const char* uri, const char* stage)  (called by core's hooks) an analysis stage starts
//   "quickdeck.earlyGrid"   void (const void* thread, const char* uri, const void* beats)   bettergrid: Beat This!'s grid
//                           (a juce::Array<DetailedBeatInfo>) while rekordbox's beat job still runs: show it on the deck

// Offered by multisort:
//   "multisort.chain"       void (void* sorter, int column)  after ListViewSorter::setCompFunc(column): add the other
//                           columns the list is sorted by to the sorter's compare chain

// Offered by spotifyfix (spotifydl gates its HTTPS requests with them):
//   "spotifyfix.httpEnter"  bool ()  false: a Spotify logout is on, answer the request with an error; true: call httpLeave after
//   "spotifyfix.httpLeave"  void ()

// Offered by tidymenu (core's track-menu hook calls them, main thread):
//   "tidymenu.submenu"      Menu* ()  an empty menu for the mods' items of the track menu being built (null: none)
//   "tidymenu.finish"       Menu* (Menu* menu)  the menu is built: hangs the submenu in it, gives the menu to show
//                           (a copy without the disabled items, or menu)
//   "tidymenu.done"         void ()  it was shown

// Offered by genres (see mods/genres.h):
//   "genres.detect"         bool (const std::vector<float>* mono, int rate, Style* out)  any thread; false: no model
//   "genres.analysing"      void (const char* path)  rekordbox's analysis of this track reached a stage (core calls it)
//   "genres.known"          bool (const char* uri, std::string* genre, bool* written)  the Genre field, else the style heard
//   "genres.forTrack"       bool (const char* uri, const std::vector<float>* mono, int rate, std::string* genre)  the
//                           track's genre: rekordbox's Genre field, else the stored style, else detected from mono (if
//                           given); an unsure model guess counts as none

// Offered by streamio:
//   "streamio.passAudio"    bool (const void** reader, const int16_t** l, const int16_t** r, size_t* n)  on an analysis
//                           thread, until its phrase passes are done: the 16-bit samples its audio pass read (from frame 0)

// Offered by spotifyaudio:
//   "spotifyaudio.read"   bool (const char* uri, std::shared_ptr<const std::vector<int16_t>>* stereo, int* rate,
//                         std::string* why)  a Spotify track's whole audio, 16-bit stereo interleaved, decoded from
//                         the stream into memory (never on disk); blocks; not on the main thread
// Local files of streaming tracks: a mod may keep a streaming track's whole audio as a local file (none of this tree
// does). The mods that read a whole track (analyzer, autocue, genres, stemcache) take that file when there is one and
// the stream otherwise, and their windows can ask the mod to fetch a playlist's files ahead of time. Offered by it:
//   "trackfiles.file"     bool (const char* uri, std::string* path)  the track's file now (path may be null); ask right
//                         before opening it (the file may be replaced by another of the same audio later)
//   "trackfiles.fetch"    void (const char* playlistId)  fetch the playlist's files ahead of time, showing how far it is
//   "trackfiles.text"     std::string (const char* key, int n)  the windows' words for it, also while the mod is off
//                         (the keys are the callers': "button", "stems.state", ...; n: a count where one goes in)
// The helpers below are false, "" or do nothing without such a mod.
inline bool trackFile(const char* uri, std::string* path) {
    auto f = service<bool (*)(const char*, std::string*)>("trackfiles.file");
    return f && uri && f(uri, path);
}
inline bool trackFilesOn() { return provided("trackfiles.file") != nullptr; }
inline const char* trackFilesMod() { return providerOf("trackfiles.file"); }  // its name, even while it's off ("" none)
inline bool canFetchTrackFiles() { return provided("trackfiles.fetch") != nullptr; }
inline void fetchTrackFiles(const char* playlistId) {
    if (auto f = service<void (*)(const char*)>("trackfiles.fetch")) f(playlistId);
}
inline std::string trackFilesText(const char* key, int n = 0) {
    auto f = service<std::string (*)(const char*, int)>("trackfiles.text");
    return f ? f(key, n) : std::string();
}
// Offered by analyzer:
//   "analyzer.status"           bool (std::string* runPlaylist, int* finished, int* rows)  true while a run is going;
//                                                                              the run's playlist and its progress
//   "analyzer.analyzePlaylist"  void (const char* playlistId, bool force)      start a run (one at a time)
//   "analyzer.stop"             void ()                                        stop the run
//   "analyzer.analysing"        bool (const char* uri)                         a run of this mod is analysing the track
// Offered to analyzer by another mod (none of this tree), each optional:
//   "analyzer.option"           bool (int set, std::string* label, std::string* tip)  a checkbox of its own in the
//                               analysis window: set -1 asks (label, tip), 0 / 1 sets it; returns whether it's on
//   "analyzer.holdBack"         bool (const char* uri, double* gb)  uri null: whether it holds tracks back now (gb: how
//                               much it waits for); else whether the run should hold this track back while it does
//   "analyzer.holdProgress"     void (double* toGoMb, std::string* what, std::string* step)  while it holds tracks
//                               back: how much still has to go, what the run waits for, and what happens right now
//   "analyzer.diskUse"          bool (std::string* hint)  a run writes more than rekordbox's analysis files (the
//                               analyze_min_free_gb then counts in full); hint: added to the "disk nearly full" message
// Offered by betterstems:
//   "betterstems.active"        bool ()                                        the HTDemucs model is on and loaded
//   "betterstems.version"       std::string ()                                 its identity for cache keys ("" if not active)
//   "betterstems.separate"      bool (const float* inL, const float* inR, int n, float* const out[8])  any thread
// Offered by stemcache:
//   "stemcache.cached"          bool (const float* inL, const float* inR, int inSamples, float* const out[8])  a stored
//                               chunk's stems (other, vocals, drums, bass; L, R each), false if not on disk; any thread
//   "stemcache.store"           void (same arguments)  stores a chunk separated with betterstems' model; any thread
//   "stemcache.storeSpotify"    void (same arguments)  the same for a Spotify track's chunk: kept in memory
//                               (stemcache_memory_mb), never on disk

// ---- UI -----------------------------------------------------------------------------------

// Work a mod does once after the launch to be ready (loading a model, its first run): the tasks of all mods run one
// after another on one background thread, in the order they are handed in. (Each on a thread of its own, three Core
// ML models loading for the GPU at once took 20-60 s instead of 1-3 s each, and their first runs 6-9 s instead of
// 0.5 s.) Something that needs a model sooner loads it itself, as before.
void startupTask(std::function<void()> fn);

// rekordbox is quitting (macOS: from NSApplicationWillTerminate on). exit() then destroys the dylib's statics (its
// mutexes too: a lock after that aborts), so a thread of a mod's own that runs for the app's lifetime calls
// parkIfQuitting() in its loop and stops there for good. Windows: never set (ExitProcess ends the threads first).
bool quitting();
void parkIfQuitting();

void menuAddSeparator(Menu* m);
void menuAddItem(Menu* m, const char* text, std::function<void()> action);
std::string tracksText(size_t n);       // "this track" / "5 tracks": how the track menu's items name the selection
// The track list's menu (Mod::trackMenu). With the tidymenu mod the Menu* a mod gets is a submenu ("platendoos") that
// gathers the mods' window items; without it, the menu itself.
//  - menuWindowItem: the name of an item that opens a mod's window on the selection: "Genres window" in the submenu,
//    "Open genres window for this track" in the menu itself (window: "genres window", "stems window").
//  - menuAddMainItem: an item for the menu itself, next to the submenu: what acts right there ("Re-analyze track").
bool menuGrouped();
std::string menuWindowItem(const char* window, size_t tracks);
void menuAddMainItem(Menu* m, const char* text, std::function<void()> action);
void openTreeItem(void* treeItem);      // selects a playlist in the tree (shows it in the track list)
std::string treeItemName(void* treeItem);  // its name as shown in the tree
// The selected playlist in the browser's tree as "tree:index/index/...|/Root/Spotify/..." (its position and
// identifier; "" if none), and opening it again (switching the browser to its source first): 1 opened,
// 0 not there (yet: tree items fill in as their parents open), -1 it's a different item.
std::string selectedPlaylist();
int openPlaylist(const std::string& where);
void forEachListViewer(const std::function<void(void* comp, void* listViewer)>& fn);
void* mainListViewer();                 // showing browse::ListViewer with the most rows
int listRows(void* lv);
const char* listRow(void* lv, int row);  // db::RowDataTrack* (nullptr if not paged in)
const char* trackPath(const char* track);  // RowDataTrack file path / streaming URI
std::vector<std::string> selectedTracks(void* lv);  // the list's selected tracks (paths / URIs), in list order
void listLoadTrack(void* lv, int ch, int row);  // as dragging the row onto deck ch+1

// The collection's copy of a track (by path / streaming URI), from rekordbox's database (main thread).
struct CollectionTrack {
    unsigned id = 0;
    bool analysed = false;      // has a current analysis
    std::string title, artist;
    double bpm = 0;             // 0 unknown
    std::string key;            // "" unknown
};
bool collectionTrack(const std::string& uri, CollectionTrack* out);

// Playlists by tree identifier ("/Root/Spotify/Your Library/Minimal Techno Mix"). rekordbox only lists
// a playlist's tracks while the browser shows it; core records them (title, artist, ...) as they load,
// keeps them in the library database (core/library.h), and serves them from there afterwards.
struct PlaylistTracks {
    std::string id, name;
    std::vector<std::string> uris;  // in order
    bool shown = false;             // the browser shows it now (uris may still grow as rows load)
    bool known = false;             // false: never seen, no tracks known
};
void* browserListViewer();                            // the browser's track list (browse::ListViewer)
std::string treeItemId(void* treeItem);               // its identifier
void showPlaylist(void* treeItem);                    // select it in the browser (recorded from then on)
bool showPlaylistId(const std::string& id);           // same by identifier, if its tree item is loaded
std::string shownPlaylist();                          // identifier of the playlist shown, "" if none
PlaylistTracks playlistTracks(const std::string& id);
std::string playlistName(const std::string& id);      // its name alone (no tracks copied)
// A few tracks of a list as a playlist of their own ("selection:<n>"), so a window that opens on a playlist opens on
// them: playlistTracks gives just these tracks (in the order given, known, name "5 tracks from <playlist>", shown
// while the browser shows the playlist they were taken from) and showPlaylistId shows that playlist. For this
// session only and never stored; an id from an earlier launch is an unknown playlist without tracks.
// A mod that walks the browser's rows for a playlist, or compares its id with shownPlaylist(), asks isSelection /
// playlistParent: a selection's tracks are known (nothing to wait for or to add as rows load), and the list that
// shows them is the parent's.
// lv: the list they are selected in (null: the browser's, the playlist shown). Main thread; "" if uris is empty.
std::string selectionPlaylist(const std::vector<std::string>& uris, void* lv = nullptr);
bool isSelection(const std::string& id);              // by the id alone (any thread)
std::string playlistParent(const std::string& id);    // the playlist a selection is of ("" if none, or the selection
                                                      // isn't this session's); any other id as it is
// Imports a streaming track into the collection (asynchronously) from its row as seen in a playlist this
// session, whatever the browser shows now. False if no list has shown it yet.
bool canImportTrack(const std::string& uri);
bool importTrack(const std::string& uri);

// Track fields (db::RowDataTrack*).
std::string trackTitle(const char* track);
std::string trackArtist(const char* track);
const char* trackKey(const char* track);   // rekordbox's key text as shown ("8A" or "Am"), may be null
double trackBpm(const char* track);        // 0 if unknown

// Keys: pitch class (C=0) + minor; text either Camelot ("8A") or classic ("Am", "F#").
bool parseKey(const char* s, int* pc, bool* minor, bool* camelot);
std::string formatKey(int pc, bool minor, bool camelot);
int camelotRank(int pc, bool minor);       // sort order like rekordbox's Key column (1A, 1B, 2A ...)

// Per-track notes shared between mods (e.g. a detected key), by track path / streaming URI.
std::string trackNote(const std::string& uri, const char* name);
void setTrackNote(const std::string& uri, const char* name, const std::string& value);

// ---- track list columns -------------------------------------------------------------------

// A column of our own in the track lists. It uses a column id rekordbox leaves unused (54-57), which
// rekordbox then saves/restores with the layout and offers in the header's right-click menu. Lists
// built without it get it added after `after` (while the column's mod is enabled).
struct Column {
    int id;
    std::string (*title)();
    std::string (*text)(const char* track);                  // cell text (main thread)
    int (*compare)(const char* a, const char* b) = nullptr;  // sort order (any thread); default: by text
    int width = 110;
    int after = 34;                                          // column id to insert after (34 = Key)
    volatile bool* enabled = nullptr;
};
std::vector<Column>& columns();
struct ColumnRegistrar { ColumnRegistrar(Column c) { columns().push_back(c); } };
#define RBM_COLUMN(...) static ::rbm::ColumnRegistrar RBM_CAT(rbm_col_, __LINE__)(::rbm::Column __VA_ARGS__)
// Repaints lists showing the column; re-sorts those sorted by it if resort.
void refreshColumn(int id, bool resort = false);
void showTrackBpm(const std::string& uri, double bpm);  // the BPM the lists' rows of this track show (after it changed in the collection; main thread)
void repaintComponent(void* comp);      // a juce::Component (a list viewer's, from forEachListViewer), after changing its rows
// Our column id's comparison of two tracks (RowDataTrack*), as its sort uses it (0 for an unknown id).
int compareColumn(int id, const char* a, const char* b);

// ---- Spotify readers (core/spotifyread.cpp) -----------------------------------------------
// Makes rekordbox's Spotify readers return a track's audio to its end also when they were opened while Spotify still
// downloaded it (see the file). The first call installs it (main thread, at startup); false if this rekordbox build
// lacks what it needs.
bool spotifyReadersKeepUp();

// ---- juce threads (core/jucethread.cpp) ---------------------------------------------------
// While one exists, juce::Thread::getCurrentThread() on this thread (one the mods started) returns a stand-in that
// was never asked to stop, instead of null: for rekordbox code that calls threadShouldExit() on it unguarded
// (7.2.20's Spotify download). Does nothing on a juce thread, or where this rekordbox build lacks what it needs.
class JuceThreadScope {
public:
    JuceThreadScope();
    ~JuceThreadScope();
    JuceThreadScope(const JuceThreadScope&) = delete;
private:
    void* entry_ = nullptr;
};

// ---- analysis (core/analysis.cpp) ---------------------------------------------------------
// rekordbox's track analysis (CBeatAnalyzeThread::analyze) is shared by several mods that don't know each other:
// core owns what they share, and each mod's own part is a service looked up by name (null when that mod isn't built
// in or is switched off), so any of them can be left out. Core's hooks call:
//   "timing.mark"        void (*)(const char* uri, const char* stage, bool fresh)  timing: a stage was reached on the
//                        analysis thread (fresh = the first mark of an analysis); stages: "start", "poll" (waiting for
//                        the stream download), "beat job", "phrase", "key", "similarity", "seek table"
//   "timing.wait"        void (*)(const char* uri, int ms)                         timing: the analysis slept ms waiting for the stream
//   "timing.report"      std::string (*)(const std::string& uri, double since, double loadedAt)   see analysisTiming
//   "analysis.stage"     void (*)(const void* thread, const char* uri, const char* stage)   quickdeck: each stage but "poll", on the analysis thread
//   "bettergrid.prefetch" void (*)(const void* thread)                             just before rekordbox's beat job
//   "bettergrid.stage"   void (*)(const void* thread, const char* stage)           bettergrid: each stage but "poll", on the analysis thread
//   "genres.analysing"   void (*)(const char* uri)                                 genres: each stage but "poll", on the analysis thread
//   "betterkey.keyJob"   void (*)(const void* thread, const std::string& uri, void* keyInfo)   just before the key job
//   "betterkey.prefetch" void (*)(const void* thread)                              the analyzer: an analysis it queued starts
//                                                                                  (the key CNN starts next to the audio and beat passes)

// Marks the start of a stage of rekordbox's analysis on a CBeatAnalyzeThread (called on that thread; a stage of
// one analysis counts once). Does nothing when no mod listens.
void analysisMark(const void* thread, const char* stage);
#if RBM_MAC_X86
// Intel Macs: the analysis thread (CBeatAnalyzeThread) this call runs on, for the hooks inside its analyze() that take
// it from a register on arm64 (x86_64 keeps it in a stack slot); nullptr on any other thread.
const void* analysisThreadHere();
#endif
// The track URI the analysis thread works on; false if unknown (or this rekordbox build lacks the recipe).
bool analysisSong(const void* thread, std::string* uri);
// Stages of the track's analyses so far ("..." marks the stage still running), "" if none seen or timing isn't running.
inline std::string analysisTiming(const std::string& uri, double since, double loadedAt = -1) {  // analyses started after since; times from loadedAt (default since)
    auto f = service<std::string (*)(const std::string&, double, double)>("timing.report");
    return f ? f(uri, since, loadedAt) : "";
}

// A track's whole audio (mono, its own sample rate), decoded once with rekordbox's reader (or taken from the audio
// pass, "streamio.passAudio") and shared by the mods that analyse it (bettergrid at the beat stage, betterkey at the
// key stage). Blocks until decoded; any thread. Null if this rekordbox build lacks the reader.
// Kept for a few minutes or until analysisAudioDone(uri).
std::shared_ptr<const std::vector<float>> analysisAudio(const std::string& uri, int* rate);
void analysisAudioDone(const std::string& uri);
// An analysis is at its beat stage right now (any thread): what a deck waits for to show the track's beat grid. Heavy
// work that can wait a few seconds (stem separation) holds back while it is true.
bool analysisBeatStage();

// ---- decks --------------------------------------------------------------------------------

void* engine();                         // djengine::DjEngineIF
int deckCount();
bool deckLoaded(int ch);
bool deckLoading(int ch);
bool deckPlaying(int ch);
double deckBpm(int ch, bool output);    // current BPM (output = with tempo applied)
double masterBpm();                     // 0 if no master
std::string deckPath(int ch);           // loaded file path / streaming URI, "" if empty
bool deckHasSong(int ch);
void deckUnload(int ch);
bool isSpotify(const char* path);

// ---- a slow drive that is read and written at once -------------------------------------------

// A cache folder may be on a USB stick, which serves its requests one after the other: a read waits behind every write
// that is queued, and a stick that has taken a lot at once writes one request a second (measured: a 29 MB read took
// 135 s behind what was being stored). So what a mod writes to such a drive in the background goes
// through here: one piece at a time for all mods together, none started while something reads from the drive or a
// deck is loading, a rest after each piece that grows with the time the drive took for it, and, once the drive has
// been slow, nothing at all while a deck plays. Whoever needs the data sooner keeps it on the internal disk meanwhile
// (the mods' staging folders).
namespace slowdrive {
// Something the DJ waits for reads from the drive now, or is about to (a track handed to a deck or an
// analysis): no new piece is started for this long. Any thread.
void reading(double seconds = 3);
// The same for the time a read of the mod's own takes (stored stems).
struct Reading {
    explicit Reading(bool on = true);
    ~Reading();
    Reading(const Reading&) = delete;
    Reading& operator=(const Reading&) = delete;
private:
    bool on_;
};
// A background writer's turn at the drive: the constructor waits until no read is under way or expected, no deck is
// loading (nor, on a drive that has been slow, playing), no other writer has the turn and the rest after the last
// piece is over, which can be as long as a set lasts; the caller then writes one piece
// (1 MB at most, flushed: File::direct or sync) and ends the turn. on = false: no waiting at all (a folder on an
// internal disk), so call sites need no second path.
struct Turn {
    explicit Turn(bool on = true);
    ~Turn();
    void wrote(size_t bytes) { bytes_ += bytes; }  // how much the piece was (for the pace)
    Turn(const Turn&) = delete;
    Turn& operator=(const Turn&) = delete;
private:
    bool on_;
    double from_ = 0;
    size_t bytes_ = 0;
};
bool quiet();          // nothing reads or is about to (work that can wait starts only then)
bool deckPlays();      // a deck is playing (as the main thread last saw, ten times a second; any thread)
void tick();           // core, main thread: looks at the decks
}  // namespace slowdrive

}  // namespace rbm
