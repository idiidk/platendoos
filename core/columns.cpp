// Track list columns of our own (see Column in rbmod.h), track fields, keys and per-track notes.
#include "rbmod.h"
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <atomic>
#include <map>
#include <mutex>
#include <new>

namespace rbm {

static const Offset O_trackTitle{0x20, "__ZN2db15TrackListSorter12compareTitleEPNS_12RowDataTrackES2_", "add x1", 0};  // RowDataTrack: title (juce::String, compareTitle)
static const Offset O_trackArtist{0xd0, "__ZN2db15TrackListSorter13compareArtistEPNS_12RowDataTrackES2_", "add x1", 0};  // RowDataTrack: artist (juce::String, compareArtist)
static const Offset O_trackKey{0x210, "__ZN2db15TrackListSorter10compareKeyEPNS_12RowDataTrackES2_", "ldr x1", 0};  // RowDataTrack: key text (compareKey)
static const Offset O_trackBpm{0x3a8, "__ZN2db15TrackListSorter10compareBPMEPNS_12RowDataTrackES2_", "ldr x1", 0};  // RowDataTrack: BPM * 100 (compareBPM)
static const Offset O_viewerHeader{0x2a8, "__ZNK6browse10ListViewer14getTableHeaderEv", "ldr x0", 0};  // ListViewer: BrowseListHeader* (a juce::TableHeaderComponent)
static const Addr A_getSortColumnId{"__ZNK4juce20TableHeaderComponent15getSortColumnIdEv"};  // juce::TableHeaderComponent::getSortColumnId() const
static const Addr A_reSortTable{"__ZN4juce20TableHeaderComponent11reSortTableEv"};  // juce::TableHeaderComponent::reSortTable() (async)
static const Addr A_repaint{"__ZN4juce9Component7repaintEv"};  // juce::Component::repaint()
static const Addr A_addColumn{"__ZN4juce20TableHeaderComponent9addColumnERKNS_6StringEiiiiii"};  // TableHeaderComponent::addColumn(String const&, id, width, min, max, flags, index)
static const Addr A_setColumnName{"__ZN4juce20TableHeaderComponent13setColumnNameEiRKNS_6StringE"};  // TableHeaderComponent::setColumnName(id, String const&)
static const Addr A_indexOfColumn{"__ZNK4juce20TableHeaderComponent18getIndexOfColumnIdEib"};  // TableHeaderComponent::getIndexOfColumnId(id, onlyVisible) const
#ifdef _WIN32
static const Addr A_getColumnWidth{"__ZNK4juce20TableHeaderComponent14getColumnWidthEi"};  // TableHeaderComponent::getColumnWidth(id) const
static const Addr A_getColumnIdOfIndex{"__ZNK4juce20TableHeaderComponent18getColumnIdOfIndexEib"};  // TableHeaderComponent::getColumnIdOfIndex(index, onlyVisible) const
#endif
static const Addr A_setCompareWithArtistAlbum{"__ZN6browse14ListViewSorter32setCompareFuncWithArtistAndAlbumEMS0_FiPN2db12RowDataTrackES3_Eb"};  // ListViewSorter::setCompareFuncWithArtistAndAlbum(int (ListViewSorter::*)(RowDataTrack*, RowDataTrack*), bool)

static const int kNameRow = 16;  // BrowseHeaderManager name/width row lent to our columns (the Key column's)
// juce::TableHeaderComponent column flags: visible, resizable, draggable, on the column menu, sortable
static const int kColumnFlags = 1 | 2 | 4 | 8 | 16;

std::vector<Column>& columns() {
    static std::vector<Column> c;
    return c;
}
static Column* column(int id) {
    for (Column& c : columns())
        if (c.id == id) return &c;
    return nullptr;
}
static bool on(const Column& c) { return !c.enabled || *c.enabled; }

// ---- track fields -------------------------------------------------------------------------

static std::string str(const char* track, size_t off) {
    const char* s = track ? *(const char* const*)(track + off) : nullptr;
    return s ? s : "";
}
std::string trackTitle(const char* track) { return str(track, O_trackTitle); }
std::string trackArtist(const char* track) { return str(track, O_trackArtist); }
const char* trackKey(const char* track) { return track ? *(const char* const*)(track + O_trackKey) : nullptr; }
double trackBpm(const char* track) { return track ? *(const int*)(track + O_trackBpm) * 0.01 : 0; }

// ---- keys ---------------------------------------------------------------------------------

bool parseKey(const char* s, int* pc, bool* minor, bool* camelot) {
    if (!s) return false;
    while (*s == ' ') s++;
    if (*s >= '0' && *s <= '9') {
        int n = atoi(s);
        while (*s >= '0' && *s <= '9') s++;
        char l = *s & ~0x20;
        if (n < 1 || n > 12 || (l != 'A' && l != 'B')) return false;
        int majorPc = ((n - 8) * 7 % 12 + 12) % 12;  // 7 is its own inverse mod 12
        *minor = l == 'A';
        *pc = *minor ? (majorPc + 9) % 12 : majorPc;
        *camelot = true;
        return true;
    }
    static const int base[7] = {9, 11, 0, 2, 4, 5, 7};  // A..G
    char c = *s & ~0x20;
    if (c < 'A' || c > 'G') return false;
    int p = base[c - 'A'];
    s++;
    if (*s == '#' || !strncmp(s, "♯", 3)) { p++; s += (*s == '#') ? 1 : 3; }
    else if (*s == 'b' || !strncmp(s, "♭", 3)) { p--; s += (*s == 'b') ? 1 : 3; }
    *pc = (p + 12) % 12;
    *minor = *s == 'm' || *s == 'M';
    *camelot = false;
    return true;
}

std::string formatKey(int pc, bool minor, bool camelot) {
    char out[16];
    if (camelot) {
        int majorPc = minor ? (pc + 3) % 12 : pc;
        snprintf(out, sizeof out, "%d%c", (majorPc * 7 % 12 + 7) % 12 + 1, minor ? 'A' : 'B');
    } else {
        static const char* names[12] = {"C", "Db", "D", "Eb", "E", "F", "F#", "G", "Ab", "A", "Bb", "B"};
        static const char* mnames[12] = {"Cm", "C#m", "Dm", "Ebm", "Em", "Fm", "F#m", "Gm", "G#m", "Am", "Bbm", "Bm"};
        snprintf(out, sizeof out, "%s", minor ? mnames[pc] : names[pc]);
    }
    return out;
}

int camelotRank(int pc, bool minor) {
    int majorPc = minor ? (pc + 3) % 12 : pc;
    return ((majorPc * 7 % 12 + 7) % 12) * 2 + (minor ? 0 : 1);
}

// ---- notes --------------------------------------------------------------------------------

static std::mutex g_notesLock;
static std::map<std::string, std::map<std::string, std::string>> g_notes;

std::string trackNote(const std::string& uri, const char* name) {
    std::lock_guard<std::mutex> l(g_notesLock);
    auto it = g_notes.find(uri);
    if (it == g_notes.end()) return "";
    auto f = it->second.find(name);
    return f == it->second.end() ? "" : f->second;
}
void setTrackNote(const std::string& uri, const char* name, const std::string& value) {
    std::lock_guard<std::mutex> l(g_notesLock);
    g_notes[uri][name] = value;
}

// ---- columns ------------------------------------------------------------------------------

void refreshColumn(int id, bool resort) {
    forEachListViewer([id, resort](void* comp, void* lv) {
        void* header = *(void* const*)((const char*)lv + O_viewerHeader);
        if (!header || at<int (*)(void*, int, bool)>(A_indexOfColumn)(header, id, true) < 0) return;
        at<void (*)(void*)>(A_repaint)(comp);
        if (!resort) return;
        bool sorted = at<int (*)(void*)>(A_getSortColumnId)(header) == id;
        if (!sorted)  // one of multisort's other sort columns (e.g. Key @ Master within BPM)
            if (auto by = service<bool (*)(const void*, int)>("multisort.sortsBy")) sorted = by(header, id);
        if (sorted) at<void (*)(void*)>(A_reSortTable)(header);
    });
}

void repaintComponent(void* comp) { at<void (*)(void*)>(A_repaint)(comp); }

void showTrackBpm(const std::string& uri, double bpm) {
    int bpm100 = (int)lround(bpm * 100);
    forEachListViewer([&](void* comp, void* lv) {
        bool changed = false;
        for (int i = 0, n = listRows(lv); i < n; i++) {
            const char* row = listRow(lv, i);
            const char* p = row ? trackPath(row) : nullptr;
            if (!p || uri != p || *(const int*)(row + O_trackBpm) == bpm100) continue;
            *(int*)(row + O_trackBpm) = bpm100;
            changed = true;
        }
        if (changed) repaintComponent(comp);
    });
}

}  // namespace rbm

using namespace rbm;

// ListViewer::getString for plain-text cells.
#ifdef _WIN32
// MSVC: `this` in rcx, the hidden result pointer (the juce::String to construct) in rdx, returned in rax.
typedef JStr* (*GetStringFn)(const void* self, JStr* result, const void* track, int col);
extern "C" JStr* rbm_hook_paint_getString(const void* self, JStr* result, const void* track, int col) {
    Column* c = col >= 54 ? column(col) : nullptr;
    if (!c) return RBM_ORIG(paint_getString, GetStringFn)(self, result, track, col);
    return new (result) JStr(on(*c) ? c->text((const char*)track).c_str() : "");
}
#else
typedef JStr (*GetStringFn)(const void* self, const void* track, int col);
extern "C" JStr rbm_hook_paint_getString(const void* self, const void* track, int col) {
    Column* c = col >= 54 ? column(col) : nullptr;
    if (!c) return RBM_ORIG(paint_getString, GetStringFn)(self, track, col);
    return JStr(on(*c) ? c->text((const char*)track).c_str() : "");
}
#endif

// ListViewSorter compare functions are member functions (sorter, a, b); one thunk serves all our
// columns, by the column the sorter was last set to.
static std::mutex g_sorterLock;
static std::map<const void*, int> g_sorterColumn;
// strcasecmp (not on Windows)
static int compareNoCase(const char* a, const char* b) {
    for (;; a++, b++) {
        int x = tolower((unsigned char)*a), y = tolower((unsigned char)*b);
        if (x != y) return x - y;
        if (!x) return 0;
    }
}
namespace rbm {
int compareColumn(int id, const char* a, const char* b) {
    Column* c = column(id);
    if (!c) return 0;
    if (c->compare) return c->compare(a, b);
    return compareNoCase(c->text(a).c_str(), c->text(b).c_str());
}
}  // namespace rbm
static std::atomic<unsigned> g_sorterGen{0};  // bumped whenever a sorter is (re)set: remembered lookups are stale
[[maybe_unused]] static int compareThunk(const void* sorter, const char* a, const char* b) {
    // A sort compares O(n log n) times on one thread with one sorter: the map is only read again when the sorter
    // or anything in the map changed.
    thread_local const void* lastSorter = nullptr;
    thread_local unsigned lastGen = ~0u;
    thread_local int lastId = 0;
    unsigned gen = g_sorterGen.load(std::memory_order_acquire);
    if (sorter != lastSorter || gen != lastGen) {
        std::lock_guard<std::mutex> l(g_sorterLock);
        auto it = g_sorterColumn.find(sorter);
        if (it == g_sorterColumn.end()) return 0;
        lastSorter = sorter, lastGen = gen, lastId = it->second;
    }
    return compareColumn(lastId, a, b);
}

// ListViewSorter::setCompFunc(column): our columns sort like Lyricist's case, with the thunk.
static void setCompFunc(void* sorter, int col) {
    if (!column(col)) return RBM_ORIG(sorter_setCompFunc, void (*)(void*, int))(sorter, col);
    {
        std::lock_guard<std::mutex> l(g_sorterLock);
        g_sorterColumn[sorter] = col;
    }
    g_sorterGen.fetch_add(1, std::memory_order_release);
#ifdef _WIN32
    // MSVC: ListViewSorter has single inheritance, so its member function pointer is just the code address (one
    // register, setCompFunc passes it in rdx and the bool in r8); called with the sorter as `this` in rcx.
    at<void (*)(void*, void*, bool)>(A_setCompareWithArtistAlbum)(sorter, (void*)&compareThunk, true);
#else
    // Itanium: a member function pointer is {address, this adjustment}.
    at<void (*)(void*, void*, intptr_t, bool)>(A_setCompareWithArtistAlbum)(sorter, (void*)&compareThunk, 0, true);
#endif
}

// The sorter's compare chain for its column, then (multisort) the other columns sorted along with it.
extern "C" void rbm_hook_sorter_setCompFunc(void* sorter, int col) {
    if (versionOff("core")) return RBM_ORIG(sorter_setCompFunc, void (*)(void*, int))(sorter, col);  // (see core.cpp's menus)
    setCompFunc(sorter, col);
    if (auto more = service<void (*)(void*, int)>("multisort.chain")) more(sorter, col);
}

// BrowseHeaderManager::getColumnIndexByID: give our ids a name/width row so rekordbox keeps them in
// saved layouts (the header text is set separately).
typedef int (*ColumnIndexFn)(const void*, int);
#define COLUMN_INDEX_HOOK(n) \
    extern "C" int rbm_hook_column_index##n(const void* mgr, int id) { \
        return column(id) ? kNameRow : RBM_ORIG(column_index##n, ColumnIndexFn)(mgr, id); \
    }
COLUMN_INDEX_HOOK(1)
#ifndef _WIN32
COLUMN_INDEX_HOOK(2)
COLUMN_INDEX_HOOK(3)
COLUMN_INDEX_HOOK(4)
COLUMN_INDEX_HOOK(5)
#else
// windows-optional: column_index2 column_index3 column_index4 column_index5
// Windows inlines getColumnIndexByID into these four as a bounds-checked search that skips an unknown id
// (the layout restore and save, addColumn's defaults; getColumnName gives ""), so they need no hook, but our
// columns aren't in rekordbox's saved layout. So their place is kept here instead: the column left of them,
// their width and whether they're shown, in the config as column<id>=<left id>,<width>,<shown>, written when
// the user changes it in any list (checked once a second), and used by afterRestore.
namespace {
struct Place { int left = 0, width = 0; bool shown = true; };
bool operator!=(const Place& a, const Place& b) { return a.left != b.left || a.width != b.width || a.shown != b.shown; }

std::string placeKey(int id) { return "column" + std::to_string(id); }

bool savedPlace(int id, Place* p) {
    std::string v = cfgStr(placeKey(id).c_str());
    int shown = 1;
    if (sscanf(v.c_str(), "%d,%d,%d", &p->left, &p->width, &shown) < 2 || p->width <= 0) return false;
    p->shown = shown != 0;
    return true;
}

// Each list header's place of each column as last seen: a change there is the user's. A header seen for the
// first time, or just rebuilt by rekordbox, takes its place as it is.
std::map<std::pair<const void*, int>, Place> g_seen;
void forgetHeader(const void* header) {
    for (auto it = g_seen.begin(); it != g_seen.end();) it = it->first.first == header ? g_seen.erase(it) : std::next(it);
}

bool placeIn(void* header, int id, Place* p) {
    auto indexOf = at<int (*)(void*, int, bool)>(A_indexOfColumn);
    int i = indexOf(header, id, false);
    if (i < 0) return false;
    p->left = i > 0 ? at<int (*)(void*, int, bool)>(A_getColumnIdOfIndex)(header, i - 1, false) : 0;
    p->width = at<int (*)(void*, int)>(A_getColumnWidth)(header, id);
    p->shown = indexOf(header, id, true) >= 0;
    return p->width > 0;
}
}  // namespace

namespace rbm {
void rememberColumnPlaces() {
    static double next;
    if (now() < next) return;
    next = now() + 1;
    auto& seen = g_seen;
    std::map<std::pair<const void*, int>, Place> cur;
    forEachListViewer([&](void*, void* lv) {
        void* header = *(void* const*)((const char*)lv + O_viewerHeader);
        if (!header) return;
        for (Column& c : columns()) {
            Place p;
            if (!placeIn(header, c.id, &p)) continue;
            auto key = std::make_pair((const void*)header, c.id);
            cur[key] = p;
            auto it = seen.find(key);
            Place saved;
            if (it != seen.end() && it->second != p && (!savedPlace(c.id, &saved) || saved != p)) {
                char v[48];
                snprintf(v, sizeof v, "%d,%d,%d", p.left, p.width, p.shown ? 1 : 0);
                setCfg(placeKey(c.id).c_str(), v);
                logf("columns: column %d now after %d, %d wide%s", c.id, p.left, p.width, p.shown ? "" : ", hidden");
            }
        }
    });
    seen.swap(cur);
}
}  // namespace rbm
#endif

// A column's title is its mod's option (keycol_title, bettergrid_title...): afterRestore sets it when a list's header is
// built, which is once per list. Here a changed title goes to the headers that are there already (checked once a
// second, main thread), so the option applies as it is set.
namespace rbm {
void retitleColumns() {
    static double next;
    static std::map<int, std::string> shown;
    if (now() < next) return;
    next = now() + 1;
    for (Column& c : columns()) {
        if (!on(c)) continue;
        std::string t = c.title();
        auto it = shown.find(c.id);
        if (it == shown.end()) { shown[c.id] = t; continue; }  // (as the headers were built with)
        if (it->second == t) continue;
        it->second = t;
        int lists = 0;
        forEachListViewer([&](void*, void* lv) {
            void* header = *(void* const*)((const char*)lv + O_viewerHeader);
            if (!header || at<int (*)(void*, int, bool)>(A_indexOfColumn)(header, c.id, false) < 0) return;
            JStr title(t.c_str());
            at<void (*)(void*, int, const JStr*)>(A_setColumnName)(header, c.id, &title);
            lists++;
        });
        logf("columns: column %d is now titled '%s' (%d list%s)", c.id, t.c_str(), lists, lists == 1 ? "" : "s");
    }
}
}  // namespace rbm

// BrowseHeaderManager::restoreHeaderInfo(ViewType, TableHeaderComponent*, BrowseProperty const&, ArtworkStatus&)
// builds a list header from the saved layout (or the defaults). Afterwards: add our columns the layout
// doesn't have yet, and set their titles.
static void afterRestore(void* header) {
    if (!header) return;
#ifdef _WIN32
    forgetHeader(header);
#endif
    reloadConfig();
    auto indexOf = at<int (*)(void*, int, bool)>(A_indexOfColumn);
    for (Column& c : columns()) {
        JStr title(c.title().c_str());
        if (indexOf(header, c.id, false) >= 0) {
            at<void (*)(void*, int, const JStr*)>(A_setColumnName)(header, c.id, &title);
        } else if (on(c)) {
            int left = c.after, width = c.width, flags = kColumnFlags;
#ifdef _WIN32
            Place p;
            if (savedPlace(c.id, &p)) {
                left = p.left, width = p.width;
                if (!p.shown) flags &= ~1;
            }
            // (left 0: the first column)
            int after = left ? indexOf(header, left, false) : -1;
            if (left && after < 0) after = indexOf(header, c.after, false);
#else
            int after = indexOf(header, left, false);
#endif
            if (after < 0 && left) after = indexOf(header, 34, false);  // fall back to after Key
            at<void (*)(void*, const JStr*, int, int, int, int, int, int)>(A_addColumn)(
                header, &title, c.id, width, 3, -1, flags, after < 0 ? (left ? -1 : 0) : after + 1);
        }
    }
}
typedef void (*RestoreFn)(void*, int, void*, const void*, void*);
#define RESTORE_HOOK(n) \
    extern "C" void rbm_hook_header_restore##n(void* mgr, int view, void* header, const void* prop, void* art) { \
        RBM_ORIG(header_restore##n, RestoreFn)(mgr, view, header, prop, art); \
        afterRestore(header); \
    }
RESTORE_HOOK(1)
RESTORE_HOOK(2)
RESTORE_HOOK(3)
RESTORE_HOOK(4)
