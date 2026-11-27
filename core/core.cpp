// rbmod core: hook dispatch, config, log, mod registry, timer and shared rekordbox helpers.
#include "rbmod.h"
#include <thread>
#include <deque>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <algorithm>
#include <atomic>
#include <filesystem>
#include <map>
#include <mutex>
#include "library.h"

namespace rbm {

// rekordbox's functions and fields, found at load (rbm::Addr, rbm::Offset; the values are 7.2.19's).
static const Addr A_stringCtorUTF8{"__ZN4juce6StringC1ENS_16CharPointer_UTF8E"};  // juce::String::String(CharPointer_UTF8)
static const Addr A_stringDtor{"__ZN4juce6StringD1Ev"};  // juce::String::~String()
static const Addr A_engineHolder{"SingletonHolder<DjEngineIF>", [] {
    return singleton("__ZN10OutputCompD2Ev", "__ZN4juce15SingletonHolderIN8djengine10DjEngineIFENS_15CriticalSectionELb0EE3getEv");
}};
static const Addr A_engineGet{"__ZN4juce15SingletonHolderIN8djengine10DjEngineIFENS_15CriticalSectionELb0EE3getEv"};  // SingletonHolder<...>::get()
static const Addr A_existMaster{"__ZNK8djengine10DjEngineIF11existMasterEv"};  // DjEngineIF::existMaster() const
static const Addr A_getMaster{"__ZNK8djengine10DjEngineIF9getMasterEv"};  // DjEngineIF::getMaster() const -> channel or -1
static const Addr A_getBpm{"__ZNK8djengine10DjEngineIF6getBpmENS_15EnPlayerChannelEb"};  // DjEngineIF::getBpm(channel, bool output) const -> BPM*100
static const Addr A_isLoaded{"__ZNK8djengine10DjEngineIF8isLoadedENS_15EnPlayerChannelE"};  // DjEngineIF::isLoaded(channel) const
static const Addr A_isLoading{"__ZNK8djengine10DjEngineIF9isLoadingENS_15EnPlayerChannelE"};  // DjEngineIF::isLoading(channel) const
static const Addr A_isPlaying{"__ZNK8djengine10DjEngineIF9isPlayingENS_15EnPlayerChannelE"};  // DjEngineIF::isPlaying(channel) const
static const Addr A_getUiPlayer{"__ZN6browse14PlayerAccessor11getUiPlayerEN6djplay9ePlayerIDE"};  // browse::PlayerAccessor::getUiPlayer(djplay::ePlayerID)
static const Addr A_unloadSongFile{"__ZN6djplay8UiPlayer14unloadSongFileEbbb"};  // djplay::UiPlayer::unloadSongFile(bool, bool, bool)
static const Addr A_uiSongInfo{"__ZN6djplay8UiPlayer11getSongInfoEv"};  // djplay::UiPlayer::getSongInfo()
static const Addr A_unload{"__ZN8djengine10DjEngineIF6unloadENS_15EnPlayerChannelE"};  // DjEngineIF::unload(channel) (as SpotifyBridge does on quit)
static const Addr A_getFilePath{"__ZNK8djengine10DjEngineIF11getFilePathENS_15EnPlayerChannelE"};  // DjEngineIF::getFilePath(channel) const -> juce::String
static const Offset O_engineChannels{0xdc, "__ZN8djengine10DjEngineIF6unloadENS_15EnPlayerChannelE", "ldr x20", 0};  // DjEngineIF: number of player channels
static const Addr A_smHolder{"SingletonHolder<StreamingManager>", [] {
    return singleton("__ZN11DetailTidalC2Ev", "__ZN4juce15SingletonHolderIN9streaming16StreamingManagerENS_15CriticalSectionELb1EE3getEv");
}};
static const Addr A_smGet{"__ZN4juce15SingletonHolderIN9streaming16StreamingManagerENS_15CriticalSectionELb1EE3getEv"};  // SingletonHolder<...>::get()
static const Addr A_isSpotifyProto{"__ZN9streaming16StreamingManager17isSpotifyProtocolERKN4juce6StringE"};  // StreamingManager::isSpotifyProtocol(String const&)
static const Addr A_addSeparator{"__ZN4juce9PopupMenu12addSeparatorEv"};  // juce::PopupMenu::addSeparator()
static const Addr A_addItemAction{"__ZN4juce9PopupMenu7addItemENS_6StringENSt3__18functionIFvvEEE"};  // juce::PopupMenu::addItem(String, std::function<void()>)
static const Addr A_desktopGet{"__ZN4juce7Desktop11getInstanceEv"};  // juce::Desktop::getInstance()
static const Addr A_desktopComp{"__ZNK4juce7Desktop12getComponentEi"};  // juce::Desktop::getComponent(int) const (nullptr past end)
static const Addr A_numChildren{"__ZNK4juce9Component21getNumChildComponentsEv"};  // juce::Component::getNumChildComponents() const
static const Addr A_child{"__ZNK4juce9Component17getChildComponentEi"};  // juce::Component::getChildComponent(int) const
static const Addr A_isShowing{"__ZNK4juce9Component9isShowingEv"};  // juce::Component::isShowing() const
static const Addr A_tiComponent{"__ZTIN4juce9ComponentE"};
static const Addr A_tiListViewer{"__ZTIN6browse10ListViewerE"};
static const Addr A_lvNumRows{"__ZN6browse10ListViewer10getNumRowsEv"};  // browse::ListViewer::getNumRows()
#ifdef _WIN32
// On Windows getNumRows is a TableListBoxModel method (its `this` is a secondary base of ListViewer), so the
// field it returns is read directly instead.
static const Offset O_lvNumRows{0x258, "__ZN6browse10ListViewer10getNumRowsEv", "ldr x0", 0};  // browse::ListViewer: number of rows
#endif
static const Addr A_lvSelectedRows{"__ZNK6browse10ListViewer24getSelectedRowDataTracksERN4juce5ArrayIPN2db12RowDataTrackENS1_20DummyCriticalSectionELi0EEE"};  // ListViewer::getSelectedRowDataTracks(Array<RowDataTrack*>&) const
static const Addr A_getRowByIndex{"__ZNK6browse10ListViewer13getRowByIndexEi"};  // browse::ListViewer::getRowByIndex(int) const -> RowDataTrack*
static const Addr A_ctvSelected{"__ZNK6browse14CustomTreeView15getSelectedItemEi"};  // browse::CustomTreeView::getSelectedItem(int) const
static const Addr A_ctviNumSub{"__ZNK6browse18CustomTreeViewItem14getNumSubItemsEv"};  // browse::CustomTreeViewItem::getNumSubItems() const
static const Addr A_ctviSub{"__ZNK6browse18CustomTreeViewItem10getSubItemEi"};  // browse::CustomTreeViewItem::getSubItem(int) const
static const Addr A_ctviSetOpen{"__ZN6browse18CustomTreeViewItem7setOpenEb"};  // browse::CustomTreeViewItem::setOpen(bool)
static const Addr A_ctviIdString{"__ZNK6browse18CustomTreeViewItem23getItemIdentifierStringEv"};  // browse::CustomTreeViewItem::getItemIdentifierString() const
static const Addr A_mediaNames{"GetMediaName's table", [] {  // BrowseMediaSelect::GetMediaName's juce::String table (21):
#if RBM_MAC_X86
    return symbol("__ZL15aStrButtonNames");  // the x86_64 slice has a symbol for it
#else
    const char* fn = "__ZN17BrowseMediaSelect12GetMediaNameE21mediaSelectButtonType";  // adrp/add x8, add x1, x8, #0x418
    size_t add;
    uintptr_t page = adrpAdd(fn, 0);
    return page && immediate(fn, "add x8", 1, &add) ? page + add : 0;
#endif
}};
static const Addr A_btvChangeMedia{"__ZN6browse16BrowseTreeViewer15ChangeMediaTypeE21mediaSelectButtonType"};  // browse::BrowseTreeViewer::ChangeMediaType(mediaSelectButtonType)
static const Offset O_ctvRoot{0x158, "__ZNK6browse14CustomTreeView15getSelectedItemEi", "ldr x0", 0};
static const Offset O_btvBasicView{0x148, "__ZNK6browse16BrowseTreeViewer13getListViewerEv", "ldr x0", 0};  // browse::BrowseTreeViewer: its BrowseBasicView
static const Addr A_bbvListViewer{"__ZNK6browse15BrowseBasicView13getListViewerEv"};  // browse::BrowseBasicView::getListViewer() const
static const Addr A_dbHolder{"SingletonHolder<DatabaseIF>", [] { return singleton("__ZN11DetailColorC2Ev", "__ZN4juce15SingletonHolderIN2db10DatabaseIFENS_15CriticalSectionELb0EE3getEv"); }};
static const Addr A_dbGet{"__ZN4juce15SingletonHolderIN2db10DatabaseIFENS_15CriticalSectionELb0EE3getEv"};  // SingletonHolder<db::DatabaseIF>::get()
static const Addr A_isCollectionSong{"__ZN2db10DatabaseIF16isCollectionSongERKN4juce6StringERNS_12RowDataTrackE"};  // DatabaseIF::isCollectionSong(String const&, RowDataTrack&)
static const Addr A_rowCtor{"__ZN2db12RowDataTrackC2Ev"};  // db::RowDataTrack::RowDataTrack()
static const Addr A_rowDtor{"__ZN2db12RowDataTrackD2Ev"};  // db::RowDataTrack::~RowDataTrack()
static const Addr A_rowCopy{"__ZN2db12RowDataTrackC2ERKS0_"};  // db::RowDataTrack::RowDataTrack(RowDataTrack const&)
static const Addr A_importBridge{"__ZN2db10DatabaseIF17importBridgeTrackENS_12_browse_typeERKN4juce10OwnedArrayINS_12RowDataTrackENS2_20DummyCriticalSectionEEEbb"};  // DatabaseIF::importBridgeTrack(_browse_type, OwnedArray<RowDataTrack> const&, bool, bool)
static const Addr A_browseType{"__ZNK6browse14BrowseProperty13getBrowseTypeEv"};  // browse::BrowseProperty::getBrowseType() const
static const Offset O_lvBrowseProp{0xf0, "__ZNK6browse10ListViewer14canPlayPreviewEv", "add x0", 0};  // browse::ListViewer: its BrowseProperty
static const Addr A_isAnalyzedOld{"__ZNK2db12RowDataTrack16isAnalyzedOldVerEb"};  // db::RowDataTrack::isAnalyzedOldVer(bool) const
static const Offset O_trackId{0x8, "__ZNK2db12RowDataTrack5refIDEv", "add x0", 0};  // RowDataTrack: database id
static const Offset O_trackFlags{0x3f8, "__ZNK2db12RowDataTrack17isAnalysisEnabledEv", "ldr x0", 1};  // RowDataTrack: bit 1 = analysed
static const size_t    S_rowDataTrack  = 0x4e0;        // sizeof(db::RowDataTrack) (the same on Windows: its allocations are `new` of 0x4e0 then the constructor)
static const Offset V_lvLoadTrack{0x428, "__ZTVN6browse10ListViewerE", "vt:__ZN6browse10ListViewer9loadTrackEN6djplay9ePlayerIDEibb", 0};  // ListViewer vtable: loadTrack(ePlayerID deck-1, row, bool, bool)
static const Offset O_trackPath{0x98, "__ZNK2db12RowDataTrack13isSpotifyFileEv", "add x0", 0};  // RowDataTrack: file path / streaming URI (juce::String)

intptr_t g_slide;

// UTF-8 (String(char const*) would read the bytes as Latin-1 and mangle non-ASCII paths and names).
JStr::JStr(const char* s) { at<void (*)(JStr*, const char*)>(A_stringCtorUTF8)(this, s); }
JStr::~JStr() { at<void (*)(JStr*)>(A_stringDtor)(this); }

// ---- config / log -------------------------------------------------------------------------

static std::map<std::string, std::string> g_cfg;  // g_cfgLock (read from any thread, replaced on the main thread)
static std::map<std::string, std::string> g_cfgAtStart;  // g_cfgLock: g_cfg as it was when the mods' switches were latched
static bool g_cfgStartKept;
static std::atomic<uint64_t> g_cfgGeneration{0};
static std::mutex g_cfgLock;
static double g_cfgCheckedAt = -100;
static FILE* g_log;

static std::string configFile() { return plat::configDir() + "/config.txt"; }

// Once per run: a config from before it had the .txt (Notepad can't open a name without one) is renamed.
static void prepareConfigFile() {
    namespace fs = std::filesystem;
    std::string path = configFile(), old = plat::configDir() + "/config";
    std::error_code ec;
    fs::create_directories(plat::fsPath(plat::configDir()), ec);
    if (fs::exists(plat::fsPath(old), ec) && !fs::exists(plat::fsPath(path), ec)) fs::rename(plat::fsPath(old), plat::fsPath(path), ec);
}
// The list of every mod and setting, all commented out (settings::configList), is added at the end of a config file
// that has none yet. Once the mods are registered (main thread, at startup).
static void addConfigList() {
    std::string path = configFile();
    bool listed = false, newlineAtEnd = true;
    if (FILE* f = plat::openFile(path, "r")) {
        char line[512];
        while (fgets(line, sizeof line, f)) {
            const char* text = !strncmp(line, "\xEF\xBB\xBF", 3) ? line + 3 : line;  // (Notepad's byte-order mark)
            if (!strncmp(text, "# platendoos options", 20)) listed = true;
            newlineAtEnd = strchr(line, '\n') != nullptr;
        }
        fclose(f);
    }
    if (listed) return;
    if (FILE* f = plat::openFile(path, "a")) {
        fprintf(f, "%s%s", newlineAtEnd ? "" : "\n", settings::configList().c_str());
        fclose(f);
    }
}

double now() { return plat::now(); }

void reloadConfig(bool force) {
    static std::once_flag prepared;
    std::call_once(prepared, prepareConfigFile);
    double t = now();
    if (!force && t - g_cfgCheckedAt < 2.0) return;
    g_cfgCheckedAt = t;
    FILE* f = plat::openFile(configFile(), "r");
    if (!f) return;
    std::map<std::string, std::string> c;
    char line[512];
    while (fgets(line, sizeof line, f)) {
        std::string s(line);
        if (!s.compare(0, 3, "\xEF\xBB\xBF")) s.erase(0, 3);  // (a byte-order mark before the first line)
        s.erase(s.find_last_not_of("\r\n") + 1);
        size_t eq = s.find('=');
        if (s.empty() || s[0] == '#' || eq == std::string::npos) continue;
        std::string k = s.substr(0, eq), v = s.substr(eq + 1);
        k.erase(0, k.find_first_not_of(" \t"));
        k.erase(k.find_last_not_of(" \t") + 1);
        v.erase(0, v.find_first_not_of(" \t"));  // "key = value" is the same as "key=value"
        v.erase(v.find_last_not_of(" \t") + 1);
        c[k] = v;
    }
    fclose(f);
    std::lock_guard<std::mutex> l(g_cfgLock);
    if (c != g_cfg) g_cfgGeneration++;
    g_cfg.swap(c);
}
uint64_t cfgGeneration() { return g_cfgGeneration; }

void setCfg(const char* key, const std::string& value) {
    std::string path = configFile();
    std::vector<std::string> lines;
    bool found = false;
    FILE* f = plat::openFile(path, "r");
    if (!f && plat::fileInfo(path, nullptr)) {
        // It is there but can't be read now (another program has it open): rewriting it from nothing would leave
        // this one key and lose every other setting.
        logf("config: %s can't be read, %s not saved", path.c_str(), key);
        return;
    }
    if (f) {
        char line[512];
        while (fgets(line, sizeof line, f)) {
            std::string s(line);
            s.erase(s.find_last_not_of("\r\n") + 1);
            size_t eq = s.find('=');
            std::string k = eq == std::string::npos || s[0] == '#' ? "" : s.substr(0, eq);
            k.erase(0, k.find_first_not_of(" \t"));
            if (!k.empty()) k.erase(k.find_last_not_of(" \t") + 1);
            if (k == key && found) continue;  // a duplicate (two rekordbox processes writing at once): dropped
            if (k == key) s = std::string(key) + "=" + value, found = true;
            lines.push_back(s);
        }
        fclose(f);
    }
    if (!found) {
        // A new line goes under the list's own "#key=default" line, where its description is; else at the end.
        std::string listed = std::string("#") + key + "=";
        auto at = std::find_if(lines.begin(), lines.end(), [&](const std::string& l) { return l.compare(0, listed.size(), listed) == 0; });
        lines.insert(at == lines.end() ? at : at + 1, std::string(key) + "=" + value);
    }
    // Written beside it and renamed over it: a full disk (or a crash) must not leave the config cut short or empty.
    std::string text;
    for (auto& l : lines) text += l + "\n";
    if (!plat::writeFileAtomic(path, text.data(), text.size())) logf("config: %s could not be written, %s not saved", path.c_str(), key);
    reloadConfig(true);
}

bool cfgHas(const char* key) {
    std::lock_guard<std::mutex> l(g_cfgLock);
    return g_cfg.count(key) > 0;
}

void unsetCfg(const char* key) {
    std::string path = configFile(), def = setting(key).def;
    FILE* f = plat::openFile(path, "r");
    if (!f) return;
    std::vector<std::string> lines;
    std::string listed = std::string("#") + key + "=";
    bool isListed = false, changed = false;
    char line[512];
    while (fgets(line, sizeof line, f)) {
        std::string s(line);
        s.erase(s.find_last_not_of("\r\n") + 1);
        if (s.compare(0, listed.size(), listed) == 0) isListed = true;
        lines.push_back(s);
    }
    fclose(f);
    for (auto it = lines.begin(); it != lines.end();) {
        size_t eq = it->find('=');
        std::string k = eq == std::string::npos || (*it)[0] == '#' ? "" : it->substr(0, eq);
        k.erase(0, k.find_first_not_of(" \t"));
        if (!k.empty()) k.erase(k.find_last_not_of(" \t") + 1);
        if (k != key) { ++it; continue; }
        changed = true;
        // (a config whose list has no line for it gets one in its place: the option stays findable in the file)
        if (isListed) it = lines.erase(it);
        else *it = listed + def, isListed = true, ++it;
    }
    if (!changed) return;
    std::string text;
    for (auto& l : lines) text += l + "\n";
    if (!plat::writeFileAtomic(path, text.data(), text.size())) logf("config: %s could not be written, %s not reset", path.c_str(), key);
    reloadConfig(true);
}

std::string cfgStr(const char* key, const char* def) {
    std::lock_guard<std::mutex> l(g_cfgLock);
    auto it = g_cfg.find(key);
    return it == g_cfg.end() ? def : it->second;
}

std::string cfgStrAtStart(const char* key, const char* def) {
    std::lock_guard<std::mutex> l(g_cfgLock);
    const auto& c = g_cfgStartKept ? g_cfgAtStart : g_cfg;
    auto it = c.find(key);
    return it == c.end() ? def : it->second;
}

double cfgNum(const char* key, double def) {
    std::lock_guard<std::mutex> l(g_cfgLock);
    auto it = g_cfg.find(key);
    return it == g_cfg.end() || it->second.empty() ? def : atof(it->second.c_str());
}


void checkBuild();

// A mod's switch (<mod>=0 / =1) is read once, at startup, and stays for the session: switching a mod live would
// leave its state behind (a running stem separation, multisort's header titles, the FLX4 overlay) and a mod
// switched on later would never have had init(). Other options are read live.
namespace {
std::mutex g_latchLock;
std::map<std::string, bool> g_latch;
std::atomic<bool> g_latched;  // start()'s main-thread step has recorded the switches (until then they're read live)
}  // namespace
bool offUnlessAsked(const char* mod) {
#ifdef _WIN32
    const char* here = "win";
#else
    const char* here = "mac";
#endif
    for (const Mod& m : mods())
        if (!strcmp(m.name, mod)) return m.untested && !strcmp(m.untested, here);
    return false;
}
bool modSwitchNow(const char* mod) { return cfgNum(mod, offUnlessAsked(mod) ? 0 : 1) != 0; }
bool modSwitchAtStart(const char* mod) {
    if (!g_latched) return modSwitchNow(mod);
    std::lock_guard<std::mutex> l(g_latchLock);
    auto it = g_latch.find(mod);
    if (it == g_latch.end()) it = g_latch.emplace(mod, modSwitchNow(mod)).first;  // a name that isn't a registered mod
    return it->second;
}
bool modEnabled(const char* mod) { return !versionOff(mod) && modSwitchAtStart(mod); }

std::string devCommand(const char* mod) {
    static std::map<std::string, double> lastRead;
    double t = now();
    double& at = lastRead[mod];
    if (t - at < 1) return "";
    at = t;
    std::string path = plat::configDir() + "/command-" + mod;
    FILE* f = plat::openFile(path, "r");
    if (!f) return "";
    char line[1100] = "";
    if (!fgets(line, sizeof line, f)) line[0] = 0;
    fclose(f);
    std::error_code ec;
    std::filesystem::remove(plat::fsPath(path), ec);
    std::string s = line;
    while (!s.empty() && strchr("\r\n \t", s.back())) s.pop_back();  // also cmd's "echo x > file" space
    logf("%s: command '%s'", mod, s.c_str());
    return s;
}

void logf(const char* fmt, ...) {
    static std::mutex lock;  // lines from different threads stay whole
    std::lock_guard<std::mutex> l(lock);
    if (!g_log) {
        // A new launch starts a new log once it has grown past 20 MB; the one before is kept as rekordbox-mods.1.log.
        std::error_code ec;
        auto path = plat::fsPath(plat::logFile());
        if (std::filesystem::file_size(path, ec) > 20u << 20 && !ec) {
            auto old = path;
            std::filesystem::rename(path, old.replace_extension(".1.log"), ec);
        }
        g_log = plat::openFile(plat::logFile(), "a");
    }
    if (!g_log) return;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(g_log, fmt, ap);
    va_end(ap);
    fputc('\n', g_log);
    fflush(g_log);
}

std::vector<Mod>& mods() {
    static std::vector<Mod> m;
    return m;
}

// ---- decks --------------------------------------------------------------------------------

void* engine() { return at<void* (*)(uintptr_t)>(A_engineGet)(A_engineHolder + g_slide); }

int deckCount() {
    void* e = engine();
    return e ? *(const int*)((const char*)e + O_engineChannels) : 0;
}

static bool validDeck(int ch) { return ch >= 0 && ch < deckCount(); }
bool deckLoaded(int ch) { return validDeck(ch) && at<bool (*)(void*, int)>(A_isLoaded)(engine(), ch); }
bool deckLoading(int ch) { return validDeck(ch) && at<bool (*)(void*, int)>(A_isLoading)(engine(), ch); }
bool deckPlaying(int ch) { return validDeck(ch) && at<bool (*)(void*, int)>(A_isPlaying)(engine(), ch); }
double deckBpm(int ch, bool output) {
    return validDeck(ch) ? at<float (*)(void*, int, bool)>(A_getBpm)(engine(), ch, output) * 0.01 : 0;
}
// Like the deck's eject button (UiPlayer::unloadSongFile, as ListViewer::unloadTrack does), so the deck
// display and track list update too; DjEngineIF::unload only empties the engine's player.
// Whether the deck shows a song (what its eject button acts on).
bool deckHasSong(int ch) {
    if (!validDeck(ch)) return false;
    void* ui = at<void* (*)(int)>(A_getUiPlayer)(ch);
    return ui && at<void* (*)(void*)>(A_uiSongInfo)(ui);
}

void deckUnload(int ch) {
    if (!validDeck(ch)) return;
    if (void* ui = at<void* (*)(int)>(A_getUiPlayer)(ch)) at<void (*)(void*, bool, bool, bool)>(A_unloadSongFile)(ui, true, false, false);
    else at<void (*)(void*, int)>(A_unload)(engine(), ch);
}

std::string deckPath(int ch) {
    if (!validDeck(ch)) return "";
    return memberString(A_getFilePath, engine(), ch);
}

double masterBpm() {
    void* e = engine();
    if (!e || !at<bool (*)(void*)>(A_existMaster)(e)) return 0;
    int ch = at<int (*)(void*)>(A_getMaster)(e);
    return ch < 0 ? 0 : deckBpm(ch, true);
}

bool isSpotify(const char* path) {
    if (!path || !*path) return false;
    void* sm = at<void* (*)(uintptr_t)>(A_smGet)(A_smHolder + g_slide);
    if (!sm) return false;
    JStr s(path);
    return at<bool (*)(void*, const JStr*)>(A_isSpotifyProto)(sm, &s);
}

// ---- UI -----------------------------------------------------------------------------------

static void walk(void* comp, const std::function<void(void*, void*)>& fn) {
    void* lv = dynCast(comp, at<void*>(A_tiComponent), at<void*>(A_tiListViewer));
    if (lv) { fn(comp, lv); return; }
    int n = at<int (*)(void*)>(A_numChildren)(comp);
    for (int i = 0; i < n; i++) {
        void* child = at<void* (*)(void*, int)>(A_child)(comp, i);
        if (child && at<bool (*)(void*)>(A_isShowing)(child)) walk(child, fn);
    }
}

// A search of the whole tree (5000 components in rekordbox's main window: about a millisecond) done once: the path of
// child indexes to the match is kept and walked the next time, a few steps. The whole search runs again when the path
// no longer ends at a match (another layout), and at most twice a second while there is none.
static bool searchComponent(void* c, const std::function<bool(void*)>& match, std::vector<int>& path, int depth) {
    if (match(c)) return true;
    if (depth > 24) return false;
    for (int i = 0, n = at<int (*)(void*)>(A_numChildren)(c); i < n; i++) {
        void* ch = at<void* (*)(void*, int)>(A_child)(c, i);
        if (!ch) continue;
        path.push_back(i);
        if (searchComponent(ch, match, path, depth + 1)) return true;
        path.pop_back();
    }
    return false;
}
static void* componentAt(void* c, const std::vector<int>& path) {
    for (int i : path) {
        if (i >= at<int (*)(void*)>(A_numChildren)(c)) return nullptr;
        c = at<void* (*)(void*, int)>(A_child)(c, i);
        if (!c) return nullptr;
    }
    return c;
}
void* findComponent(void* root, ComponentPath& where, const std::function<bool(void*)>& match) {
    if (!root) return nullptr;
    if (where.found) {
        void* c = componentAt(root, where.path);
        if (c && match(c)) return c;
        where.found = false;
    } else if (plat::monotonic() - where.searchedAt < 0.5) return nullptr;
    where.path.clear();
    where.searchedAt = plat::monotonic();
    where.found = searchComponent(root, match, where.path, 0);
    return where.found ? componentAt(root, where.path) : nullptr;
}

// Walks the live JUCE component tree, so only existing components are touched.
void forEachListViewer(const std::function<void(void*, void*)>& fn) {
    void* desktop = at<void* (*)()>(A_desktopGet)();
    for (int i = 0;; i++) {
        void* top = at<void* (*)(void*, int)>(A_desktopComp)(desktop, i);
        if (!top) break;
        if (at<bool (*)(void*)>(A_isShowing)(top)) walk(top, fn);
    }
}

#ifdef _WIN32
int listRows(void* lv) { return lv ? *(const int*)((const char*)lv + O_lvNumRows) : 0; }
#else
int listRows(void* lv) { return lv ? at<int (*)(void*)>(A_lvNumRows)(lv) : 0; }
#endif

void* mainListViewer() {
    void* best = nullptr;
    int bestRows = -1;
    forEachListViewer([&](void*, void* lv) {
        int n = listRows(lv);
        if (n > bestRows) { bestRows = n; best = lv; }
    });
    return best;
}

const char* listRow(void* lv, int row) { return lv ? at<const char* (*)(void*, int)>(A_getRowByIndex)(lv, row) : nullptr; }
const char* trackPath(const char* track) { return track ? *(const char* const*)(track + O_trackPath) : nullptr; }

void listLoadTrack(void* lv, int ch, int row) {
    void** vtbl = *(void***)lv;
    ((void (*)(void*, int, int, bool, bool))vtbl[V_lvLoadTrack / 8])(lv, ch, row, false, false);
}

void menuAddSeparator(Menu* m) { at<void (*)(Menu*)>(A_addSeparator)(m); }

// Set when one of the mods' items is picked. juce gives an item added with an action the result id -1 (MSVC) or
// 0, and rekordbox's own menus read -1 as one of theirs (the track list: "Analyse tracks", which asks for the
// analysis settings and queues rekordbox's own analysis next to the mod's), so the hooks hide it from them.
static bool g_menuPicked;

void menuAddItem(Menu* m, const char* text, std::function<void()> action) {
    action = [a = std::move(action)] {
        g_menuPicked = true;
        a();
    };
#ifdef _WIN32
    // By-value juce::String and std::function: passed by address of a temporary that the callee destroys (MSVC),
    // so they're built in raw storage and not destroyed here.
    alignas(JStr) char lb[sizeof(JStr)];
    alignas(std::function<void()>) char fb[sizeof(std::function<void()>)];
    at<void (*)(Menu*, JStr*, std::function<void()>*)>(A_addItemAction)(m, new (lb) JStr(text),
                                                                          new (fb) std::function<void()>(std::move(action)));
#else
    JStr label(text);
    // By-value juce::String and std::function: passed by address, destroyed by the caller (Itanium ABI).
    at<void (*)(Menu*, JStr*, std::function<void()>*)>(A_addItemAction)(m, &label, &action);
#endif
}

// The track menu while the mods add to it: the menu itself when their items go in a submenu (else null).
static Menu* g_menuMain;
bool menuGrouped() { return g_menuMain != nullptr; }
void menuAddMainItem(Menu* m, const char* text, std::function<void()> action) { menuAddItem(g_menuMain ? g_menuMain : m, text, std::move(action)); }
std::string menuWindowItem(const char* window, size_t tracks) {
    std::string w = window;
    if (!menuGrouped()) return "Open " + w + " for " + tracksText(tracks);
    if (!w.empty()) w[0] = (char)toupper((unsigned char)w[0]);
    return w;
}

// ---- tree (playlists) ----------------------------------------------------------------------

// rekordbox's playlist tree is a browse::CustomTreeView (its own copy of juce::TreeView). Its typeinfo is
// hidden, so it's recognised by name. It's found while hidden too (the tree can be folded away).
struct TypeName { const char* itanium; const char* msvc; };
static const TypeName kCustomTreeView{"N6browse14CustomTreeViewE", ".?AVCustomTreeView@browse@@"};
static const TypeName kBrowseTreeViewer{"N6browse16BrowseTreeViewerE", ".?AVBrowseTreeViewer@browse@@"};
static bool isA(const void* obj, const TypeName& name) {
#ifdef _WIN32
    // vtable[-1] is the RTTI complete object locator {signature, offset, cdOffset, type descriptor RVA,
    // class hierarchy RVA, own RVA}; the type descriptor is {vtable, spare, decorated name}.
    const unsigned char* col = (const unsigned char*)(*(void* const* const*)obj)[-1];
    if (!col) return false;
    uint32_t sig, td, self;
    memcpy(&sig, col, 4), memcpy(&td, col + 12, 4), memcpy(&self, col + 20, 4);
    if (sig != 1) return false;
    const char* base = (const char*)col - self;
    return !strcmp(base + td + 16, name.msvc);
#else
    const uintptr_t* ti = (const uintptr_t*)(*(void* const* const*)obj)[-1];
    // Apple arm64 flags non-unique type names in the name pointer's top bit.
    return ti && !strcmp((const char*)(ti[1] & ~(1ULL << 63)), name.itanium);
#endif
}
// Walking every component costs ~0.2 ms and the ticks look several up per second, so each search remembers
// where it found its components (child indices from the desktop) and follows that path again next time. The path
// is followed from the desktop down, so nothing stale is touched; a full walk runs when it no longer leads to the
// right kind of component, and every 10 s to notice new ones.
typedef std::vector<int> CompPath;
static void walkTrees(void* comp, CompPath& path, std::vector<CompPath>& out) {
    if (isA(comp, kCustomTreeView)) { out.push_back(path); return; }
    int n = at<int (*)(void*)>(A_numChildren)(comp);
    for (int i = 0; i < n; i++)
        if (void* child = at<void* (*)(void*, int)>(A_child)(comp, i)) {
            path.push_back(i);
            walkTrees(child, path, out);
            path.pop_back();
        }
}
static void* followPath(const CompPath& path) {
    void* desktop = at<void* (*)()>(A_desktopGet)();
    void* c = at<void* (*)(void*, int)>(A_desktopComp)(desktop, path[0]);
    if (!c || !at<bool (*)(void*)>(A_isShowing)(c)) return nullptr;
    for (size_t i = 1; c && i < path.size(); i++)
        c = path[i] < at<int (*)(void*)>(A_numChildren)(c) ? at<void* (*)(void*, int)>(A_child)(c, path[i]) : nullptr;
    return c;
}
static std::vector<void*> treeViews() {
    static std::vector<CompPath> paths;
    static double walkedAt;
    std::vector<void*> out;
    if (now() - walkedAt < 10) {
        for (const CompPath& p : paths) {
            void* c = followPath(p);
            if (!c || !isA(c, kCustomTreeView)) { out.clear(); break; }
            out.push_back(c);
        }
        if (out.size() == paths.size() && !paths.empty()) return out;
    }
    paths.clear();
    out.clear();
    walkedAt = now();
    void* desktop = at<void* (*)()>(A_desktopGet)();
    for (int i = 0;; i++) {
        void* top = at<void* (*)(void*, int)>(A_desktopComp)(desktop, i);
        if (!top) break;
        if (!at<bool (*)(void*)>(A_isShowing)(top)) continue;
        CompPath path{i};
        walkTrees(top, path, paths);
    }
    for (const CompPath& p : paths) out.push_back(followPath(p));
    return out;
}
static int numSub(void* item) { return at<int (*)(void*)>(A_ctviNumSub)(item); }
static void* sub(void* item, int i) { return at<void* (*)(void*, int)>(A_ctviSub)(item, i); }
static std::string itemId(void* item) { return memberString(A_ctviIdString, item); }
static bool findPath(void* item, void* target, std::string& path) {
    if (item == target) return true;
    for (int i = 0, n = numSub(item); i < n; i++) {
        size_t len = path.size();
        path += (path.empty() ? "" : "/") + std::to_string(i);
        if (findPath(sub(item, i), target, path)) return true;
        path.resize(len);
    }
    return false;
}

static void* findComp(void* comp, const TypeName& name, CompPath& path) {
    if (isA(comp, name)) return comp;
    int n = at<int (*)(void*)>(A_numChildren)(comp);
    for (int i = 0; i < n; i++)
        if (void* child = at<void* (*)(void*, int)>(A_child)(comp, i)) {
            path.push_back(i);
            if (void* r = findComp(child, name, path)) return r;
            path.pop_back();
        }
    return nullptr;
}
static void* browseTreeViewer() {
    const TypeName& kName = kBrowseTreeViewer;
    static CompPath last;
    static double walkedAt;
    if (!last.empty() && now() - walkedAt < 10)
        if (void* c = followPath(last); c && isA(c, kName)) return c;
    walkedAt = now();
    void* desktop = at<void* (*)()>(A_desktopGet)();
    for (int i = 0;; i++) {
        void* top = at<void* (*)(void*, int)>(A_desktopComp)(desktop, i);
        if (!top) break;
        if (!at<bool (*)(void*)>(A_isShowing)(top)) continue;
        last = {i};
        if (void* r = findComp(top, kName, last)) return r;
    }
    last.clear();
    return nullptr;
}
std::string selectedPlaylist() {
    auto trees = treeViews();
    for (size_t t = 0; t < trees.size(); t++) {
        void* root = *(void**)((char*)trees[t] + O_ctvRoot);
        void* sel = root ? at<void* (*)(void*, int)>(A_ctvSelected)(trees[t], 0) : nullptr;
        std::string path;
        if (sel && sel != root && findPath(root, sel, path)) return std::to_string(t) + ":" + path + "|" + itemId(sel);
    }
    return "";
}

static void* findItem(void* item, const std::string& id) {
    std::string mine = itemId(item);
    if (mine == id) return item;
    if (id.compare(0, mine.size() + 1, mine + "/")) return nullptr;  // not below this item
    for (int i = 0, n = numSub(item); i < n; i++)
        if (void* r = findItem(sub(item, i), id)) return r;
    return nullptr;
}

// Switches the browser to the source ("/Root/Spotify/...") an identifier belongs to.
static void switchMediaFor(const std::string& id) {
    if (id.compare(0, 6, "/Root/")) return;
    std::string media = id.substr(6, id.find('/', 6) - 6);
    for (int m = 0; m <= 20; m++)
        if (media == at<const char**>(A_mediaNames)[m]) {
            if (void* btv = browseTreeViewer()) at<void (*)(void*, int)>(A_btvChangeMedia)(btv, m);
            return;
        }
}

// Whether a tree (its root) is the browser's shown source for the playlist id: one of its top items is the id or holds
// it, e.g. "/Root/Spotify" for "/Root/Spotify/Your Library/dance" (ids outside a source count as shown). The
// collection's tree has several top items ("/Root/Collection", "/Root/Playlists", "/Root/Histories", ...): going by the
// first one alone, a collection playlist never counted as shown, and was never opened.
static bool showsSourceOf(void* root, const std::string& id) {
    int n = numSub(root);
    if (!n || id.compare(0, 6, "/Root/")) return true;
    for (int i = 0; i < n; i++) {
        std::string top = itemId(sub(root, i));
        if (id == top || !id.compare(0, top.size() + 1, top + "/")) return true;
    }
    return false;
}

int openPlaylist(const std::string& where) {
    size_t c = where.find(':'), bar = where.find('|');
    if (c == std::string::npos || bar == std::string::npos) return -1;
    auto trees = treeViews();
    size_t t = strtoul(where.c_str(), nullptr, 10);
    if (t >= trees.size()) return 0;
    void* item = *(void**)((char*)trees[t] + O_ctvRoot);
    // The browser shows one source at a time ("/Root/Spotify/..."): switch to the playlist's first.
    std::string id = where.substr(bar + 1);
    if (item && !showsSourceOf(item, id)) {
        switchMediaFor(id);
        return 0;
    }
    const char* p = where.c_str() + c + 1;
    void* parent = nullptr;
    while (item && p < where.c_str() + bar) {
        int i = (int)strtol(p, (char**)&p, 10);
        if (*p == '/') p++;
        at<void (*)(void*, bool)>(A_ctviSetOpen)(item, true);  // fills in its sub-items (may take a while)
        parent = item;
        if (i >= numSub(item)) {
            item = nullptr;  // fewer items than before: the id may still be among them
            break;
        }
        item = sub(item, i);
    }
    if (bar + 1 < where.size() && (!item || itemId(item) != id)) {
        // The playlists before it changed (one added or removed): look for its id among its siblings.
        item = nullptr;
        for (int i = 0, n = parent ? numSub(parent) : 0; i < n && !item; i++)
            if (itemId(sub(parent, i)) == id) item = sub(parent, i);
        if (!item) return 0;  // not (yet) there: Spotify lists fill in from the network; retried for a while
    }
    if (!item) return 0;
    openTreeItem(item);
    return 1;
}

std::string treeItemId(void* item) { return item ? itemId(item) : ""; }

// ---- collection ---------------------------------------------------------------------------

bool collectionTrack(const std::string& uri, CollectionTrack* out) {
    void* db = at<void* (*)(uintptr_t)>(A_dbGet)(A_dbHolder + g_slide);
    if (!db || uri.empty()) return false;
    alignas(16) static char row[S_rowDataTrack + 0x100];  // main thread only
    at<void (*)(void*)>(A_rowCtor)(row);
    JStr path(uri.c_str());
    bool found = at<bool (*)(void*, const void*, void*)>(A_isCollectionSong)(db, &path, row);
    if (found && out) {
        out->id = *(const unsigned*)(row + O_trackId);
        out->analysed = (*(const uint32_t*)(row + O_trackFlags) & 2) && !at<bool (*)(const void*, bool)>(A_isAnalyzedOld)(row, false);
        out->title = trackTitle(row);
        out->artist = trackArtist(row);
        out->bpm = trackBpm(row);
        const char* k = trackKey(row);
        out->key = k ? k : "";
    }
    at<void (*)(void*)>(A_rowDtor)(row);
    return found;
}

// ---- playlists ----------------------------------------------------------------------------

// The track list of the browser (not a palette or sub browser list).
void* browserListViewer() {
    void* btv = browseTreeViewer();
    void* basic = btv ? *(void**)((char*)btv + O_btvBasicView) : nullptr;
    return basic ? at<void* (*)(void*)>(A_bbvListViewer)(basic) : nullptr;
}

// Which playlist the browser shows: the tree whose selection changed last (the source tree and the
// playlist tree each keep one).
static std::string g_shown;
static double g_shownSince;
std::string shownPlaylist() {
    static std::map<void*, std::string> last;
    for (void* tv : treeViews()) {
        void* root = *(void**)((char*)tv + O_ctvRoot);
        void* sel = root ? at<void* (*)(void*, int)>(A_ctvSelected)(tv, 0) : nullptr;
        std::string id = sel && sel != root ? itemId(sel) : "";
        if (id == last[tv]) continue;
        last[tv] = id;
        if (!id.empty() && id != g_shown) g_shown = id, g_shownSince = now();
    }
    return g_shown;
}

static std::string g_pendingShow;  // shown once its tree item is loaded
static double g_pendingUntil;

void showPlaylist(void* treeItem) {
    if (!treeItem) return;
    g_pendingShow.clear();
    openTreeItem(treeItem);
}


bool showPlaylistId(const std::string& id) {
    if (isSelection(id)) {  // the playlist it was taken from; left alone while shown (selecting it again could reload the list)
        std::string parent = playlistParent(id);
        if (parent.empty()) return false;
        if (shownPlaylist() != parent) return showPlaylistId(parent);
        g_pendingShow.clear();
        return true;
    }
    for (void* tv : treeViews())
        if (void* root = *(void**)((char*)tv + O_ctvRoot))
            if (void* item = findItem(root, id)) {
                // A source's tree keeps its items while the browser shows another source (e.g. the Spotify tree
                // while the collection is shown): opening the item then changes nothing on screen. Switch to the
                // playlist's source first and open it once that tree is the shown one (recordShownPlaylist).
                if (!showsSourceOf(root, id)) {
                    switchMediaFor(id);
                    g_pendingShow = id;
                    g_pendingUntil = now() + 20;
                    return true;
                }
                g_pendingShow.clear();
                openTreeItem(item);
                return true;
            }
    // Not loaded: switch to its source and keep trying for a while (rows of a source load as it opens).
    if (g_pendingShow != id) switchMediaFor(id);
    g_pendingShow = id;
    g_pendingUntil = now() + 20;
    return false;
}

// Tracks of playlists seen this session (main thread), recorded while the browser shows them.
static std::map<std::string, PlaylistTracks> g_lists;
static plat::Queue& libraryQueue() {
    static plat::Queue* q = new plat::Queue("rbmods.library");
    return *q;
}

// Copies of streaming rows seen in the browser, so their tracks can be imported into the collection
// later whatever the browser shows (ListViewer::importTrackFromBridge only works on the list shown, but
// all it needs is the row and the list's browse type).
// Main thread only.
struct StreamingRow { int browseType; void* row; uint64_t seen; };
static std::map<std::string, StreamingRow> g_streamingRows;
static uint64_t g_streamingSeen;
static const size_t kStreamingRowsKept = 10000;  // ~2 KB each with their strings

// Browsing large Spotify libraries mustn't keep every row of the session: the tenth least recently shown go.
// Freeing a copy is safe: importBridgeTrack keeps no row (rekordbox's own ListViewer::importTrackFromBridge
// deletes its copy right after the call), and importTrack uses ours only during it, on this thread.
static void dropOldStreamingRows() {
    std::vector<uint64_t> seen;
    for (auto& kv : g_streamingRows) seen.push_back(kv.second.seen);
    std::nth_element(seen.begin(), seen.begin() + seen.size() / 10, seen.end());
    uint64_t keepFrom = seen[seen.size() / 10];
    for (auto it = g_streamingRows.begin(); it != g_streamingRows.end();) {
        if (it->second.seen >= keepFrom) { ++it; continue; }
        at<void (*)(void*)>(A_rowDtor)(it->second.row);
        ::operator delete(it->second.row);
        it = g_streamingRows.erase(it);
    }
}

static void keepStreamingRow(void* lv, const char* row, const char* uri) {
    if (strncmp(uri, "spotify:", 8)) return;
    auto it = g_streamingRows.find(uri);
    if (it != g_streamingRows.end()) {
        it->second.seen = ++g_streamingSeen;
        return;
    }
    if (g_streamingRows.size() >= kStreamingRowsKept) dropOldStreamingRows();
    void* copy = ::operator new(S_rowDataTrack);
    at<void (*)(void*, const void*)>(A_rowCopy)(copy, row);
    g_streamingRows[uri] = {at<int (*)(const void*)>(A_browseType)((char*)lv + O_lvBrowseProp), copy, ++g_streamingSeen};
}

bool canImportTrack(const std::string& uri) { return g_streamingRows.count(uri) != 0; }

bool importTrack(const std::string& uri) {
    auto it = g_streamingRows.find(uri);
    void* db = at<void* (*)(uintptr_t)>(A_dbGet)(A_dbHolder + g_slide);
    if (it == g_streamingRows.end() || !db) return false;
    struct { void** data; int allocated, used; } rows{&it->second.row, 1, 1};  // juce::OwnedArray<RowDataTrack>
    at<void (*)(void*, int, const void*, bool, bool)>(A_importBridge)(db, it->second.browseType, &rows, false, true);
    return true;
}

// What the last recordShownPlaylist pass settled on (playlist id + hash of its rows), so an unchanged list
// costs one allocation-free walk per second instead of building a string per row.
static std::string g_recordedId;
static uint64_t g_recordedHash;

static void recordShownPlaylist() {
    double t = now();
    if (!g_pendingShow.empty() && t > g_pendingUntil) g_pendingShow.clear();
    if (!g_pendingShow.empty()) {
        std::string id = g_pendingShow;
        for (void* tv : treeViews()) {
            void* root = *(void**)((char*)tv + O_ctvRoot);
            if (!root) continue;
            if (!showsSourceOf(root, id)) continue;  // not switched over yet
            if (void* item = findItem(root, id)) {
                g_pendingShow.clear();
                openTreeItem(item);
                break;
            }
            if (void* parent = findItem(root, id.substr(0, id.rfind('/'))))
                at<void (*)(void*, bool)>(A_ctviSetOpen)(parent, true);  // fills in its sub-items
        }
    }
    std::string id = shownPlaylist();
    if (id.empty() || t - g_shownSince < 1.5) return;  // the list is still switching over
    void* lv = browserListViewer();
    int n = listRows(lv);
    if (n <= 0) return;
    uint64_t hash = 14695981039346656037ull;  // FNV-1a over the row count and every row's path (empty while not paged in)
    auto mix = [&hash](uint8_t c) { hash = (hash ^ c) * 1099511628211ull; };
    for (int k = 0; k < 4; k++) mix((uint8_t)(n >> (8 * k)));
    for (int i = 0; i < n; i++) {
        const char* row = listRow(lv, i);
        const char* p = row ? trackPath(row) : nullptr;
        if (p) for (; *p; p++) mix((uint8_t)*p);
        mix(0);
    }
    if (hash == g_recordedHash && id == g_recordedId) return;  // same rows as when it was last recorded
    std::vector<std::string> uris(n);
    std::vector<const char*> rows;
    for (int i = 0; i < n; i++) {
        const char* row = listRow(lv, i);
        const char* p = row ? trackPath(row) : nullptr;
        if (!p || !*p) continue;  // not paged in yet
        uris[i] = p;
        keepStreamingRow(lv, row, p);
        rows.push_back(row);
    }
    // Right after a switch the list may still hold the previous playlist's rows. (Only then: a playlist with the same
    // tracks as another one, a copy of it, is recorded too once the list has had time to switch.)
    if (t - g_shownSince < 10)
        for (auto& kv : g_lists)
            if (kv.first != id && kv.second.uris == uris) return;
    PlaylistTracks& pl = g_lists[id];
    if (pl.uris == uris) {
        g_recordedId = id;
        g_recordedHash = hash;
        return;
    }
    // Only now that the list changed: reading every row's fields each second costs ~1% of a core on a
    // list of thousands.
    std::vector<library::Track> tracks;
    for (const char* row : rows) {
        library::Track tr;
        tr.uri = trackPath(row);
        tr.title = trackTitle(row);
        tr.artist = trackArtist(row);
        tr.bpm = trackBpm(row);
        if (const char* k = trackKey(row)) tr.key = k;
        tracks.push_back(tr);
    }
    pl.id = id;
    pl.name = id.substr(id.rfind('/') + 1);
    pl.uris = uris;
    pl.known = true;
    g_recordedId = id;
    g_recordedHash = hash;
    if ((int)tracks.size() < n) return;  // stored once every row is there
    library::Playlist lp{id, pl.name, uris};
    libraryQueue().async([tracks, lp] {
        library::putTracks(tracks);
        library::putPlaylist(lp);
    });
}

// ---- selections ---------------------------------------------------------------------------

// A few tracks of a list as a playlist of their own, so whatever works on a playlist by its id works on them. In
// memory only: recordShownPlaylist stores the playlist the tree shows, which is never one of these. Numbered from
// the clock at the first one, so an id left over from an earlier launch names none of this session's.
namespace {
struct Selection { std::string id, parent, name; std::vector<std::string> uris; };
std::map<std::string, Selection> g_selections;  // by id; main thread
const char kSelectionPrefix[] = "selection:";
const Selection* selectionOf(const std::string& id) {
    auto it = g_selections.find(id);
    return it == g_selections.end() ? nullptr : &it->second;
}
}  // namespace

bool isSelection(const std::string& id) { return !id.compare(0, sizeof kSelectionPrefix - 1, kSelectionPrefix); }

std::string playlistParent(const std::string& id) {
    if (!isSelection(id)) return id;
    const Selection* s = selectionOf(id);
    return s ? s->parent : "";
}

std::string playlistName(const std::string& id) {
    if (!isSelection(id)) return id.substr(id.rfind('/') + 1);
    const Selection* s = selectionOf(id);
    return s ? s->name : "Selection of an earlier session";
}

static std::atomic<bool> g_quitting{false};
bool quitting() { return g_quitting; }
static std::vector<void (*)()>& quitters() {
    static std::vector<void (*)()>* v = new std::vector<void (*)()>;
    return *v;
}
void atQuit(void (*fn)()) { quitters().push_back(fn); }
void parkIfQuitting() {
    while (g_quitting) std::this_thread::sleep_for(std::chrono::seconds(60));
}

void startupTask(std::function<void()> fn) {
    static std::mutex lock;
    static std::deque<std::function<void()>> tasks;
    static bool running;
    std::lock_guard<std::mutex> l(lock);
    tasks.push_back(std::move(fn));
    if (running) return;
    running = true;
    std::thread([] {
        for (;;) {
            std::function<void()> task;
            {
                std::lock_guard<std::mutex> l(lock);
                if (tasks.empty()) { running = false; return; }
                task = std::move(tasks.front());
                tasks.pop_front();
            }
            task();
        }
    }).detach();
}

std::string tracksText(size_t n) { return n == 1 ? "this track" : std::to_string(n) + " tracks"; }

std::string selectionPlaylist(const std::vector<std::string>& uris, void* lv) {
    if (uris.empty()) return "";
    void* browser = browserListViewer();
    if (!lv) lv = browser;
    // (a list that isn't the browser's, e.g. a palette: no playlist to show for it; its tracks still go by their URIs)
    std::string parent = lv == browser ? shownPlaylist() : "";
    for (const auto& [id, s] : g_selections)  // the same tracks again: the same playlist (one entry in a window's list)
        if (s.parent == parent && s.uris == uris) return id;
    // The rows as the list shows them now: kept for importing (a Spotify track that isn't in the collection needs its
    // row), and their titles stored for the windows (a long streaming list is only stored once all its pages are in).
    std::map<std::string, bool> want;
    for (const std::string& u : uris) want[u] = true;
    std::vector<library::Track> tracks;
    std::string title;  // of the first one
    for (int i = 0, n = listRows(lv); i < n; i++) {
        const char* row = listRow(lv, i);
        const char* p = row ? trackPath(row) : nullptr;
        auto it = p ? want.find(p) : want.end();
        if (it == want.end() || !it->second) continue;
        it->second = false;
        keepStreamingRow(lv, row, p);
        library::Track tr;
        tr.uri = p;
        tr.title = trackTitle(row);
        tr.artist = trackArtist(row);
        tr.bpm = trackBpm(row);
        if (const char* k = trackKey(row)) tr.key = k;
        if (uris[0] == p) title = tr.title;
        tracks.push_back(tr);
    }
    if (!tracks.empty()) libraryQueue().async([tracks] { library::putTracks(tracks); });
    if (CollectionTrack c; title.empty() && uris.size() == 1 && collectionTrack(uris[0], &c)) title = c.title;  // (not a row of the list)
    static uint64_t next = (uint64_t)time(nullptr);
    Selection s;
    s.id = kSelectionPrefix + std::to_string(next++);
    s.parent = parent;
    s.uris = uris;
    std::string from = parent.empty() ? "" : " from " + parent.substr(parent.rfind('/') + 1);
    if (uris.size() > 1) s.name = std::to_string(uris.size()) + " tracks" + from;
    else s.name = title.empty() ? "1 track" + from : title + " (1 track" + from + ")";
    g_selections[s.id] = s;
    return s.id;
}

std::vector<std::string> devTracks(const std::string& spec) {
    std::vector<std::string> out;
    if (spec.empty()) return out;
    void* lv = browserListViewer();
    auto row = [&](int i) {
        const char* p = i >= 0 && i < listRows(lv) ? trackPath(listRow(lv, i)) : nullptr;
        if (p && *p) out.push_back(p);
    };
    size_t digits = spec.find_first_not_of("0123456789");
    if (!spec.empty() && digits == std::string::npos) {  // "<n>"
        for (int i = 0, n = atoi(spec.c_str()); i < n; i++) row(i);
    } else if (!spec.compare(0, 5, "rows ")) {
        const char* p = spec.c_str() + 5;
        for (char* end; *p; p = end) {
            long i = strtol(p, &end, 10);
            if (end == p) break;
            row((int)i);
        }
    } else {
        // A file path has spaces of its own: split at tabs only then.
        bool file = spec[0] == '/' || (spec.size() > 2 && spec[1] == ':') || spec.find('\t') != std::string::npos;
        const char* sep = file ? "\t" : " \t";
        for (size_t a = spec.find_first_not_of(sep), b; a != std::string::npos; a = spec.find_first_not_of(sep, b)) {
            b = spec.find_first_of(sep, a);
            out.push_back(spec.substr(a, b == std::string::npos ? b : b - a));
        }
    }
    return out;
}

PlaylistTracks playlistTracks(const std::string& id) {
    PlaylistTracks pl;
    if (isSelection(id)) {
        pl.id = id;
        pl.name = playlistName(id);
        if (const Selection* s = selectionOf(id)) {
            pl.uris = s->uris;
            pl.known = true;
            pl.shown = !s->parent.empty() && shownPlaylist() == s->parent && now() - g_shownSince >= 1.5;
        }
        return pl;
    }
    auto it = g_lists.find(id);
    if (it != g_lists.end()) {
        pl = it->second;
    } else {
        library::Playlist lp;
        if (library::playlist(id, &lp)) pl.uris = lp.uris, pl.known = true;
        pl.id = id;
        pl.name = id.substr(id.rfind('/') + 1);
    }
    pl.shown = !id.empty() && shownPlaylist() == id && now() - g_shownSince >= 1.5;
    return pl;
}

// FolderListTreeViewItem's primary base is juce::TreeViewItem (offset 0); __dynamic_cast across the
// hidden typeinfos fails, so the pointer is used as-is. Virtual setSelected (vtable +0x20 on mac).
static const Offset V_tviSetSelected{0x20, "__ZTVN6browse18CustomTreeViewItemE", "vt:__ZN6browse18CustomTreeViewItem11setSelectedEbbN4juce16NotificationTypeE", 0};
std::string treeItemName(void* item) {
    std::string id = item ? itemId(item) : "";
    return id.substr(id.rfind('/') + 1);
}

void openTreeItem(void* tvi) { (*reinterpret_cast<void (***)(void*, bool, bool, int)>(tvi))[V_tviSetSelected / 8](tvi, true, true, 1); }

}  // namespace rbm

using namespace rbm;

extern "C" void* rbm_fill_hook_table();

// ---- between mods ---------------------------------------------------------------------------

namespace {
struct Provided { std::string mod; void* fn; volatile bool* enabled; };
std::mutex& providedLock() { static std::mutex m; return m; }
std::map<std::string, Provided>& providedMap() { static std::map<std::string, Provided> m; return m; }
}  // namespace

namespace rbm {
void provide(const char* mod, const char* name, void* fn, volatile bool* enabled) {
    std::lock_guard<std::mutex> l(providedLock());
    providedMap()[name] = {mod, fn, enabled};
}
void* provided(const char* name) {
    std::lock_guard<std::mutex> l(providedLock());
    auto it = providedMap().find(name);
    if (it == providedMap().end()) return nullptr;
    return !it->second.enabled || *it->second.enabled ? it->second.fn : nullptr;
}
const char* providerOf(const char* name) {
    std::lock_guard<std::mutex> l(providedLock());
    auto it = providedMap().find(name);
    return it == providedMap().end() ? "" : it->second.mod.c_str();  // map nodes are stable
}
}  // namespace rbm

// ---- playlist context menu ----------------------------------------------------------------

static void* g_menuTreeItem;  // FolderListTreeViewItem whose context menu is being built

// Wraps the call to BrowsePopupMenuManager::showTreeViewPopupMenu to remember which item was clicked.
extern "C" int rbm_hook_tree_showMenu(void* manager, const void* treeViewer, void* item) {
    g_menuTreeItem = item;
    int r = RBM_ORIG(tree_showMenu, int (*)(void*, const void*, void*))(manager, treeViewer, item);
    g_menuTreeItem = nullptr;
    return r;
}

// Wraps PopupMenu::show for the tree context menu so mods can append items (in their menuOrder).
extern "C" int rbm_hook_tree_popupShow(Menu* menu, int a, int b, int c, int d, void* callback) {
    // (core switched off for this rekordbox build: its functions may not be where the mods think; macOS can't unpatch)
    if (versionOff("core")) return RBM_ORIG(tree_popupShow, int (*)(Menu*, int, int, int, int, void*))(menu, a, b, c, d, callback);
    if (g_menuTreeItem) {
        reloadConfig();
        bool first = true;
        std::vector<Mod> ordered = mods();
        std::stable_sort(ordered.begin(), ordered.end(), [](const Mod& x, const Mod& y) { return x.menuOrder < y.menuOrder; });
        for (Mod& m : ordered) {
            if (!m.playlistMenu || !modEnabled(m.name)) continue;
            if (first) menuAddSeparator(menu);
            first = false;
            m.playlistMenu(menu, g_menuTreeItem);
        }
    }
    g_menuPicked = false;
    int r = RBM_ORIG(tree_popupShow, int (*)(Menu*, int, int, int, int, void*))(menu, a, b, c, d, callback);
    return g_menuPicked ? 0 : r;
}

// ---- track list context menu --------------------------------------------------------------

static void* g_menuListViewer;  // browse::ListViewer whose context menu is being built

// Wraps the call to BrowsePopupMenuManager::showListViewPopupMenu to remember which list was clicked.
extern "C" int rbm_hook_list_showMenu(void* manager, void* basicView, void* listViewer, int attribute, int selected) {
    g_menuListViewer = listViewer;
    int r = RBM_ORIG(list_showMenu, int (*)(void*, void*, void*, int, int))(manager, basicView, listViewer, attribute, selected);
    g_menuListViewer = nullptr;
    return r;
}

namespace rbm {
std::vector<std::string> selectedTracks(void* lv) {
    std::vector<std::string> out;
    if (!lv) return out;
    struct { const char** data; int allocated, used; } rows{};  // juce::Array<db::RowDataTrack*>
    at<void (*)(const void*, void*)>(A_lvSelectedRows)(lv, &rows);
    for (int i = 0; i < rows.used; i++)
        if (const char* p = rows.data[i] ? trackPath(rows.data[i]) : nullptr; p && *p) out.push_back(p);
    free(rows.data);  // juce's HeapBlock: malloc/realloc (Windows: the process heap, both CRTs); the rows belong to the list
    return out;
}
}  // namespace rbm

// Wraps PopupMenu::show for the track list's context menu so mods can append items (in their menuOrder).
extern "C" int rbm_hook_list_popupShow(Menu* menu, int a, int b, int c, int d, void* callback) {
    // (core switched off for this rekordbox build: its functions may not be where the mods think; macOS can't unpatch)
    if (versionOff("core")) return RBM_ORIG(list_popupShow, int (*)(Menu*, int, int, int, int, void*))(menu, a, b, c, d, callback);
    if (void* lv = g_menuListViewer) {
        reloadConfig();
        std::vector<std::string> uris = selectedTracks(lv);
        logf("rbmods: track menu, %d selected%s", (int)uris.size(), lv == browserListViewer() ? "" : " (not in the browser's list)");
        if (!uris.empty()) {
            bool first = true;
            std::vector<Mod> ordered = mods();
            std::stable_sort(ordered.begin(), ordered.end(), [](const Mod& x, const Mod& y) { return x.menuOrder < y.menuOrder; });
            // The selected tracks as a playlist, made once for all the mods' items: of the list the menu is on and the
            // playlist shown now (the same tracks give the same id each time).
            std::string selection;
            // tidymenu: the mods' items in a submenu of their own
            auto submenu = service<Menu* (*)()>("tidymenu.submenu");
            Menu* sub = submenu ? submenu() : nullptr;
            g_menuMain = sub ? menu : nullptr;
            for (Mod& m : ordered) {
                if (!m.trackMenu || !modEnabled(m.name)) continue;
                if (first) menuAddSeparator(menu), selection = selectionPlaylist(uris, lv);
                first = false;
                m.trackMenu(sub ? sub : menu, uris, selection);
            }
            g_menuMain = nullptr;
        }
        // tidymenu: the submenu hung in, and the menu shown without rekordbox's greyed-out items
        if (auto finish = service<Menu* (*)(Menu*)>("tidymenu.finish")) menu = finish(menu);
    }
    g_menuPicked = false;
    int r = RBM_ORIG(list_popupShow, int (*)(Menu*, int, int, int, int, void*))(menu, a, b, c, d, callback);
    if (auto done = service<void (*)()>("tidymenu.done")) done();
    return g_menuPicked ? 0 : r;
}

// ---- startup ------------------------------------------------------------------------------

namespace rbm {

// Development: a library path written to <config dir>/load is loaded into the running app and its
// extern "C" void rbm_experiment() called on the main thread (see tools/dev/hotload.sh).
// Each build needs a new file name: an already loaded library is returned again for the same path.
static void hotLoad() {
    static double checkedAt;
    if (now() - checkedAt < 0.5) return;
    checkedAt = now();
    std::string cmd = plat::configDir() + "/load", path;
    if (!plat::readFile(cmd, &path)) return;
    std::remove(plat::fsPath(cmd).string().c_str());
    path.erase(path.find_last_not_of("\r\n") + 1);
    auto fn = (void (*)())plat::loadLibraryFunction(path, "rbm_experiment");
    logf("hotload: %s %s", path.c_str(), fn ? "running" : "not loaded or has no rbm_experiment");
    if (fn) fn();
    logf("hotload: done");
}

// Development: "select <tracks>" in <configDir>/command-core (tracks: see devTracks in rbmod.h, e.g. "select 3" for the
// shown list's first three rows) makes a selection playlist of them, as the track list's menu does for the selected
// tracks, and logs its id: what the mods' "open <playlist id>" test commands then take.
static void coreCommand() {
    std::string c = devCommand("core");
    if (c.compare(0, 7, "select ")) return;
    std::vector<std::string> uris = devTracks(c.substr(7));
    std::string id = selectionPlaylist(uris);
    if (id.empty()) return logf("core: select: no such tracks in the list shown");
    logf("core: select: %s is '%s' (%d track%s of %s)", id.c_str(), playlistName(id).c_str(), (int)uris.size(),
         uris.size() == 1 ? "" : "s", playlistParent(id).empty() ? "no playlist" : playlistParent(id).c_str());
    for (size_t i = 0; i < uris.size() && i < 10; i++) logf("core: select:   %s", uris[i].c_str());
}

#ifdef _WIN32
void rememberColumnPlaces();  // columns.cpp
#endif
void retitleColumns();        // columns.cpp
static void tickAll() {
    reloadConfig();
    // This build lacks something the core needs (symbols.cpp checkBuild): every mod is off, and the core's own
    // work below calls rekordbox through addresses the table doesn't have.
    if (versionOff("core")) return;
    static double recordedAt;
    if (now() - recordedAt >= 1) recordedAt = now(), recordShownPlaylist();
    hotLoad();
    coreCommand();
    settings::tick();
    slowdrive::tick();
    for (Mod& m : mods()) {
        bool on = modEnabled(m.name);
        if (m.enabled) *m.enabled = on;
        if (m.tick && on) m.tick();
    }
    if (!columns().empty()) retitleColumns();
#ifdef _WIN32
    if (!columns().empty()) rememberColumnPlaces();
#endif
}

// Records the switch of every mod registered so far (a switch already recorded stays) and sets their enabled flags.
static void latchSwitches() {
    {
        std::lock_guard<std::mutex> l(g_latchLock);
        for (Mod& m : mods()) g_latch.emplace(m.name, modSwitchNow(m.name));
        g_latched = true;
    }
    {   // ... and the whole config as it is now, for the options that are read once (cfgStrAtStart)
        std::lock_guard<std::mutex> l(g_cfgLock);
        if (!g_cfgStartKept) g_cfgAtStart = g_cfg, g_cfgStartKept = true;
    }
    settings::rememberStart();
    for (Mod& m : mods())
        if (m.enabled) *m.enabled = modEnabled(m.name);
}

// Called by the platform's entry point (image_mac.mm, the Windows DLL) once rekordbox's image is known and
// the hooks are in place.
void start() {
    reloadConfig(true);
    // Windows: every mod is registered by now, so hooks firing during rekordbox's own startup see their flag.
    // macOS: start() runs from image_mac.mm's constructor, before the mods' static RBM_MOD registrations, so
    // mods() is still empty here; the main-thread step below does it for them.
    latchSwitches();
    plat::onQuit([] {
        g_quitting = true;
        logf("rbmods: rekordbox quits: the mods' own threads stop");
        for (void (*fn)() : quitters()) fn();  // (atQuit: before rekordbox's own shutdown, which is the next observer)
    });
    // Mods' init runs on the main thread once the app is up (all static registrations are done by then).
    plat::onMain([] {
        checkBuild();
        latchSwitches();  // after checkBuild, whose versionOff modEnabled counts
        addConfigList();
        std::string names;
        for (Mod& m : mods()) {
            names += std::string(names.empty() ? "" : ", ") + m.name + (modEnabled(m.name) ? "" : " (off)");
            if (m.init && modEnabled(m.name)) m.init();
        }
        logf("rbmods loaded (%s edition): %s", kPublicEdition ? "public" : "full", names.c_str());
        plat::everyOnMain(0.1, tickAll);
    });
}

}  // namespace rbm
