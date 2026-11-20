// multisort: sort the track list by several columns at once. A click on a column header cycles that column through
// ascending, descending and off, and every column that is on sorts along, in the order they were turned on: click
// BPM, then Key, and tracks are sorted by BPM and, within one BPM, by key. The first of them is rekordbox's own sort
// column (with juce's arrow); with two or more, each one's title shows its place and the others' their direction too
// ("BPM  1", "Key  ▼2"). A list sorted by its track order (#) starts over at the clicked column, as before.
//   config: multisort=0 for rekordbox's own one-column sort
//
// How: rekordbox's header click (BrowseListHeader::columnClicked) ends in setSortColumnId(column, forwards), with
// (0, true) for "off": that call is hooked and keeps this header's list of sorted columns, then sets rekordbox's sort
// column to the first one (or re-sorts when that stays). A sort builds a ListViewSorter, which compares by a chain of
// member function pointers ({address, this adjustment} pairs from +0x10, up to a null one): the first non-zero result
// decides, negated when the sort column is descending. (Windows: MSVC's member function pointers in this single-
// inheritance class are plain code addresses, 8 bytes, from +0x10; verified in tools/win/verify/multisort.out.json.) core's setCompFunc hook (one sort column) hands over to
// chain() below, which puts each other sorted column's compare function after the first one, through thunks that
// give each its own direction, then the first column's own tie-breakers (artist, album, ID).
#include "../core/rbmod.h"
#include <ctype.h>
#include <string.h>
#include <algorithm>
#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

using namespace rbm;

static const Addr A_getSortColumnId{"__ZNK4juce20TableHeaderComponent15getSortColumnIdEv"};
static const Addr A_isSortedForwards{"__ZNK4juce20TableHeaderComponent16isSortedForwardsEv"};
static const Addr A_reSortTable{"__ZN4juce20TableHeaderComponent11reSortTableEv"};  // async
static const Addr A_getColumnName{"__ZNK4juce20TableHeaderComponent13getColumnNameEi"};
static const Addr A_setColumnName{"__ZN4juce20TableHeaderComponent13setColumnNameEiRKNS_6StringE"};
static const Addr A_setCompFunc{"__ZN6browse14ListViewSorter11setCompFuncEi"};  // ListViewSorter::setCompFunc(column)
static const Offset O_viewerHeader{0x2a8, "__ZNK6browse10ListViewer14getTableHeaderEv", "ldr x0", 0};  // ListViewer: BrowseListHeader*
static const Offset O_sorterViewer{0x8, "__ZN6browse14ListViewSorter15compareElementsEii", "ldr x0", 0};  // ListViewSorter: ListViewer*
static const Offset O_sorterChain{0x18, "__ZN6browse14ListViewSorter15compareElementsEii", "add x19", 0};  // ListViewSorter: the chain + 8 (its loop reads [x22 - 8])
#ifdef _WIN32
static const int kWords = 1;  // a chain entry: its code address
static size_t chainStart() { return O_sorterChain; }  // (the table has the chain's start itself)
#else
static const int kWords = 2;  // a chain entry: {address, this adjustment * 2 (+ 1: a vtable offset)}
static size_t chainStart() { return O_sorterChain - 8; }
#endif
static const Offset O_sorterDir{0xb0, "__ZN6browse14ListViewSorter15compareElementsEii", "ldr x19", 1};  // ListViewSorter: 1 ascending, -1 descending (ends the chain)

static volatile bool g_on = true;

namespace {
struct SortKey { int col; bool up; };
// One link of the chain we build: rekordbox's compare function (fn, adj) or one of our columns (col), and
// +1/-1 to give it its own direction (the sorter negates every result when the first column is descending).
struct Level { uintptr_t fn = 0, adj = 0; int col = 0, sign = 1; };
struct Chain { Level levels[8]; };
}  // namespace

static const int kMaxKeys = 6;
static bool isTrackOrder(int col) { return col == 58 || col == 76; }  // the columns setCompFunc sorts by compareOrder (#)

static std::mutex g_lock;
static std::map<const void*, std::vector<SortKey>> g_keys;              // header -> its sorted columns, first = rekordbox's
static std::map<const void*, std::unique_ptr<Chain>> g_chains;         // sorter -> its links (rewritten in place)
static std::map<const void*, std::vector<int>> g_marked;                // header -> the columns whose titles we marked
static std::atomic<unsigned> g_gen{0};                                 // bumped when any chain is rewritten

// ---- the sorter -----------------------------------------------------------------------------

static Chain* chainOf(const void* sorter) {
    // A sort compares O(n log n) times on one thread with one sorter: the map is only read again when it changed.
    thread_local const void* lastSorter = nullptr;
    thread_local unsigned lastGen = ~0u;
    thread_local Chain* last = nullptr;
    unsigned gen = g_gen.load(std::memory_order_acquire);
    if (sorter != lastSorter || gen != lastGen) {
        std::lock_guard<std::mutex> l(g_lock);
        auto it = g_chains.find(sorter);
        last = it == g_chains.end() ? nullptr : it->second.get();
        lastSorter = sorter, lastGen = gen;
    }
    return last;
}

template <int L> static int link(char* sorter, const char* a, const char* b) {
    Chain* c = chainOf(sorter);
    if (!c) return 0;
    const Level& l = c->levels[L];
    if (l.col) return l.sign * compareColumn(l.col, a, b);
#ifdef _WIN32
    return l.sign * ((int (*)(char*, const char*, const char*))l.fn)(sorter, a, b);
#else
    // Itanium member function pointer: adj = this adjustment * 2, + 1 when fn is a vtable offset.
    char* self = sorter + (l.adj >> 1);
    uintptr_t fn = l.adj & 1 ? *(uintptr_t*)(*(char**)self + l.fn) : l.fn;
    return l.sign * ((int (*)(char*, const char*, const char*))fn)(self, a, b);
#endif
}
static void* const kLink[8] = {(void*)&link<0>, (void*)&link<1>, (void*)&link<2>, (void*)&link<3>,
                               (void*)&link<4>, (void*)&link<5>, (void*)&link<6>, (void*)&link<7>};

static void* headerOf(const void* sorter) {
    void* lv = *(void* const*)((const char*)sorter + O_sorterViewer);
    return lv ? *(void* const*)((const char*)lv + O_viewerHeader) : nullptr;
}

// core: setCompFunc(col) has just set the sorter's chain for its one sort column.
static void chain(void* sorter, int col) {
    if (!g_on || !sorter) return;
    std::vector<SortKey> keys;
    {
        std::lock_guard<std::mutex> l(g_lock);
        auto it = g_keys.find(headerOf(sorter));
        if (it == g_keys.end()) return;
        keys = it->second;
    }
    bool up = *(const int*)((const char*)sorter + O_sorterDir) > 0;
    if (keys.size() < 2 || keys[0].col != col || keys[0].up != up) return;  // one column, or the header changed meanwhile

    const int W = kWords;
    uintptr_t* slots = (uintptr_t*)((char*)sorter + chainStart());
    const int kSlots = (int)((O_sorterDir - chainStart()) / (8 * W));  // 10 in 7.2.19, the last one ends the chain
    auto isNull = [](const uintptr_t* s) { return !s[0] && (kWords == 1 || !(s[1] & 1)); };
    std::vector<uintptr_t> first(slots, slots + W * kSlots);  // the first column's chain: its function, then tie-breakers

    std::vector<Level> levels;
    for (size_t k = 1; k < keys.size() && levels.size() < 8; k++) {
        Level l;
        l.sign = keys[k].up == up ? 1 : -1;
        bool ours = false;
        for (const Column& c : columns()) ours |= c.id == keys[k].col;
        if (ours) {
            l.col = keys[k].col;
        } else {
            // rekordbox's own: its compare function is the first link setCompFunc makes for it (this also sets up
            // rekordbox's string cache for that column, as for a sort by it alone).
            at<void (*)(void*, int)>(A_setCompFunc)(sorter, keys[k].col);
            if (isNull(slots)) continue;  // a column rekordbox doesn't sort by
            l.fn = slots[0], l.adj = W == 2 ? slots[1] : 0;
        }
        levels.push_back(l);
    }

    // first column, the others' links, the first column's tie-breakers; then the null link.
    std::vector<uintptr_t> out;
    auto push = [&](const uintptr_t* e) { out.insert(out.end(), e, e + W); };
    push(&first[0]);
    for (size_t i = 0; i < levels.size(); i++) {
        uintptr_t e[2] = {(uintptr_t)kLink[i], 0};
        push(e);
    }
    for (int i = 1; i < kSlots && !isNull(&first[W * i]); i++) push(&first[W * i]);
    out.resize(W * (kSlots - 1));
    out.resize(W * kSlots, 0);  // the null entry
    {
        std::lock_guard<std::mutex> l(g_lock);
        auto& c = g_chains[sorter];
        if (!c) c.reset(new Chain());
        for (size_t i = 0; i < levels.size(); i++) c->levels[i] = levels[i];
    }
    g_gen.fetch_add(1, std::memory_order_release);
    std::copy(out.begin(), out.end(), slots);
}
RBM_PROVIDE("multisort", "multisort.chain", chain, &g_on);

// Whether a header's list sorts by this column, first or along (core's refreshColumn re-sorts it when the column's
// values change, e.g. Key @ Master when the master tempo moves).
static bool sortsBy(const void* header, int col) {
    std::lock_guard<std::mutex> l(g_lock);
    auto it = g_keys.find(header);
    if (it == g_keys.end()) return false;
    for (const SortKey& k : it->second)
        if (k.col == col) return true;
    return false;
}
RBM_PROVIDE("multisort", "multisort.sortsBy", sortsBy, &g_on);

// ---- the header -----------------------------------------------------------------------------

// A title as we mark it: "BPM  1", "Key  2"; and back. On macOS every sorted column is drawn as rekordbox draws its
// sort column (blue, its arrow: drawHeaderColumn below). On Windows the others show their direction in the title
// ("Key  ▼2") until that hook has its table entries.
static std::string marked(const std::string& title, size_t place, bool up) {
#ifdef _WIN32
    return title + (place == 0 ? "  " : up ? "  \u25B2" : "  \u25BC") + std::to_string(place + 1);
#else
    (void)up;
    return title + "  " + std::to_string(place + 1);
#endif
}
static std::string unmarked(const std::string& t) {
    size_t n = t.size();
    while (n && isdigit((unsigned char)t[n - 1])) n--;
    if (n == t.size()) return t;
    for (const char* m : {"  ", "  \u25B2", "  \u25BC"}) {
        size_t k = strlen(m);
        if (n >= k && !t.compare(n - k, k, m) && (k > 2 || n + 1 == t.size())) return t.substr(0, n - k);
    }
    return t;
}

// Each sorted column's place in its title, with two or more; the other columns we marked get their title back.
// Only changes what differs, so it can run every tick (rekordbox resets titles when it rebuilds a header).
static void showOrder(void* header, const std::vector<SortKey>& keys) {
    auto name = [&](int col) { return memberString(A_getColumnName, header, col); };
    auto setName = [&](int col, const std::string& n) {
        JStr s(n.c_str());
        at<void (*)(void*, int, const JStr*)>(A_setColumnName)(header, col, &s);
    };
    std::vector<int> was;
    {
        std::lock_guard<std::mutex> l(g_lock);
        std::vector<int>& m = g_marked[header];
        was = m;
        m.clear();
        if (keys.size() >= 2)
            for (const SortKey& k : keys) m.push_back(k.col);
    }
    for (int col : was) {
        bool still = false;
        for (const SortKey& k : keys) still |= keys.size() >= 2 && k.col == col;
        if (still) continue;
        std::string n = name(col), base = unmarked(n);
        if (base != n) setName(col, base);
    }
    if (keys.size() < 2) return;
    for (size_t i = 0; i < keys.size(); i++) {
        std::string n = name(keys[i].col), want = marked(unmarked(n), i, keys[i].up);
        if (n != want) setName(keys[i].col, want);
    }
}

// Every half second: a list sorted another way meanwhile (another playlist's saved sort, the # column's menu,
// a rebuilt header) drops its other columns, and titles rekordbox reset get their marks back. Only lists on
// screen are looked at (forEachListViewer walks the live components; a hidden list keeps its state).
static void tick() {
    static double last;
    if (now() - last < 0.5) return;
    last = now();
    forEachListViewer([](void*, void* lv) {
        void* header = *(void* const*)((const char*)lv + O_viewerHeader);
        if (!header) return;
        std::vector<SortKey> keys;
        {
            std::lock_guard<std::mutex> l(g_lock);
            auto it = g_keys.find(header);
            if (it == g_keys.end()) return;
            keys = it->second;
        }
        int cur = at<int (*)(void*)>(A_getSortColumnId)(header);
        bool curUp = at<bool (*)(void*)>(A_isSortedForwards)(header);
        if (keys.empty() || keys[0].col != cur || keys[0].up != curUp) {
            keys.clear();
            if (cur) keys.push_back({cur, curUp});
            std::lock_guard<std::mutex> l(g_lock);
            g_keys[header] = keys;
        }
        showOrder(header, keys);
    });
}

// BrowseListHeader::columnClicked -> setSortColumnId(column, forwards): (column, true) for a column the list isn't
// sorted by, (column, false) for the sort column when ascending, (0, true) when descending ("off").
extern "C" void rbm_hook_header_sortClick(void* header, int col, bool forwards) {
    auto orig = RBM_ORIG(header_sortClick, void (*)(void*, int, bool));
    if (!g_on) return orig(header, col, forwards);
    int cur = at<int (*)(void*)>(A_getSortColumnId)(header);
    bool curUp = at<bool (*)(void*)>(A_isSortedForwards)(header);
    int clicked = col ? col : cur;

    std::vector<SortKey> keys;
    {
        std::lock_guard<std::mutex> l(g_lock);
        keys = g_keys[header];
    }
    // Sorted another way meanwhile (another playlist's saved sort, the # column's menu): start from that.
    if (keys.empty() || keys[0].col != cur || keys[0].up != curUp) keys.clear();
    if (keys.empty() && cur) keys.push_back({cur, curUp});
    if (isTrackOrder(clicked) || (keys.size() == 1 && isTrackOrder(keys[0].col) && clicked != keys[0].col)) {
        // the track order isn't a column to sort along with others: rekordbox's own one-column cycle
        if (keys.empty() || keys[0].col != clicked) keys = {{clicked, true}};
        else if (keys[0].up) keys = {{clicked, false}};
        else keys.clear();
    } else {
        auto it = std::find_if(keys.begin(), keys.end(), [&](const SortKey& k) { return k.col == clicked; });
        if (it == keys.end()) {
            if ((int)keys.size() >= kMaxKeys) return;
            keys.push_back({clicked, true});
        } else if (it->up) {
            it->up = false;
        } else {
            keys.erase(it);
        }
    }
    {
        std::lock_guard<std::mutex> l(g_lock);
        g_keys[header] = keys;
    }
    showOrder(header, keys);
    if (keys.empty())
        orig(header, 0, true);
    else if (keys[0].col == cur && keys[0].up == curUp)
        at<void (*)(void*)>(A_reSortTable)(header);  // same first column: sort again, with the others
    else
        orig(header, keys[0].col, keys[0].up);
    std::string what;
    for (const SortKey& k : keys) what += " " + std::to_string(k.col) + (k.up ? "+" : "-");
    logf("multisort: sorted by%s", what.empty() ? " nothing" : what.c_str());
}

// The header's columns are drawn by rekordbox's browser look-and-feel, which shows a column as sorted (blue, with an
// arrow for the direction) from juce's column flags. The other sorted columns get those flags too, so they look like
// rekordbox's own sort column. Its drawTableHeaderColumn is replaced in the look-and-feel vtables, each entry keeping
// its original: juce calls it through TableHeaderComponent::LookAndFeelMethods (a thunk, the entries found in 7.2.19);
// a primary entry is replaced too should a build list one. Windows has no thunk: the function itself is the entry of
// the LookAndFeelMethods vtable (it takes that subobject as `this`), which follows the primary vtable in .rdata.
enum { kSortedForwards = 32, kSortedBackwards = 64 };  // juce::TableHeaderComponent::ColumnPropertyFlags
static const Addr A_lafVtable{"__ZTVN6browse17BrowseLookAndFeelE"};
static const Addr A_lafDarkVtable{"__ZTVN6browse26DarkFixedBrowseLookAndFeelE"};
static const Addr A_drawColumn{"__ZN6browse17BrowseLookAndFeel21drawTableHeaderColumnERN4juce8GraphicsERNS1_20TableHeaderComponentERKNS1_6StringEiiibbi"};
#ifndef _WIN32
static const Addr A_drawColumnThunk{"__ZThn152_N6browse17BrowseLookAndFeel21drawTableHeaderColumnERN4juce8GraphicsERNS1_20TableHeaderComponentERKNS1_6StringEiiibbi"};
#endif
using DrawColumn = void (*)(void*, void*, void*, const void*, int, int, int, bool, bool, int);
static DrawColumn g_drawOrig[2];  // 0: the primary entry's, 1: the thunk's

static int sortFlags(void* header, int col, int flags) {
    if (!g_on) return flags;
    std::lock_guard<std::mutex> l(g_lock);
    auto it = g_keys.find(header);
    if (it == g_keys.end() || it->second.size() < 2) return flags;
    for (size_t i = 1; i < it->second.size(); i++)
        if (it->second[i].col == col) return (flags & ~(kSortedForwards | kSortedBackwards)) | (it->second[i].up ? kSortedForwards : kSortedBackwards);
    return flags;
}
template <int N>
static void drawHeaderColumn(void* laf, void* g, void* header, const void* name, int col, int w, int h, bool over, bool down, int flags) {
    g_drawOrig[N](laf, g, header, name, col, w, h, over, down, sortFlags(header, col, flags));
}

static void init() {
#ifdef _WIN32
    uintptr_t fns[2] = {A_drawColumn + g_slide, 0};
#else
    uintptr_t fns[2] = {A_drawColumn + g_slide, A_drawColumnThunk + g_slide};
#endif
    void* mine[2] = {(void*)&drawHeaderColumn<0>, (void*)&drawHeaderColumn<1>};
    int patched = 0;
    for (const Addr* vt : {&A_lafVtable, &A_lafDarkVtable}) {
        void** p = (void**)(*vt + g_slide);
        for (int i = 2; i < 640; i++) {  // the whole vtable with its secondary bases' parts (LookAndFeelMethods from ~330)
            for (int k = 0; k < 2; k++) {
                if (!fns[k] || (uintptr_t)p[i] != fns[k]) continue;
                void* old = patchSlot(&p[i], mine[k]);
                if (old) g_drawOrig[k] = (DrawColumn)old, patched++;
            }
        }
    }
    if (patched) logf("multisort: other sorted columns drawn as rekordbox's sort column (%d entries)", patched);
    else logf("multisort: the header's look could not be hooked (%d entries): other sorted columns show as plain titles", patched);
}

RBM_MOD({.name = "multisort", .init = init, .tick = tick, .enabled = &g_on});
