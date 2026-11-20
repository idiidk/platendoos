// keycolumn: a new "Key @ Master" column in the track list.
//
// Shows each track's key transposed by the pitch change it would get when played (key lock off) at
// the master deck's current BPM, e.g. "8A  +1.2". The column sorts by the transposed key like the Key
// column does, and lists sorted by it re-sort live while the master tempo moves.
//   config: keycolumn=0 to disable
//           keycol_title=Key @ Master   header name
#include "../core/rbmod.h"
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <atomic>

using namespace rbm;

static const int kColumnId = 55;  // unused by rekordbox

static volatile bool g_on = true;
static double g_lastMaster;   // master BPM used for painting
// Master BPM the lists are sorted by: set (on the main thread, where rekordbox sorts) only when a re-sort is asked for,
// so a sort never sees two values (an inconsistent comparator can make rekordbox's sort run past the list).
static std::atomic<double> g_sortedMaster{0};
static double g_timerMaster;  // master BPM the lists were last refreshed for
static double g_refreshedAt;

// Transposed key of a track at master tempo.
static bool keyAtMaster(const char* track, double mb, std::string* out, int* rank, bool* camelot) {
    int pc; bool minor;
    double tb = trackBpm(track);
    if (mb <= 0 || tb <= 0 || !parseKey(trackKey(track), &pc, &minor, camelot)) return false;
    pc = ((pc + (int)lround(12.0 * log2(mb / tb))) % 12 + 12) % 12;
    *out = formatKey(pc, minor, *camelot);
    *rank = camelotRank(pc, minor);
    return true;
}

static std::string text(const char* track) {
    std::string k;
    int rank; bool camelot;
    double mb = g_lastMaster;
    if (!keyAtMaster(track, mb, &k, &rank, &camelot)) return "";
    char buf[64];
    snprintf(buf, sizeof buf, "%s  %+.1f", k.c_str(), 12.0 * log2(mb / trackBpm(track)));
    return buf;
}

// The transposed key's sort rank (no text: sorting compares thousands of pairs), or false without a key or BPM.
static bool rankAtMaster(const char* track, double mb, int* rank) {
    int pc; bool minor, camelot;
    double tb = trackBpm(track);
    if (mb <= 0 || tb <= 0 || !parseKey(trackKey(track), &pc, &minor, &camelot)) return false;
    pc = ((pc + (int)lround(12.0 * log2(mb / tb))) % 12 + 12) % 12;
    *rank = camelotRank(pc, minor);
    return true;
}

static int compare(const char* a, const char* b) {
    int ra = 0, rb = 0;
    static std::atomic<bool> offMain{false};  // (g_sortedMaster counts on sorts running on the main thread)
    if (!plat::isMainThread() && !offMain.exchange(true)) logf("keycolumn: Key @ Master sorted off the main thread");
    double mb = g_sortedMaster.load(std::memory_order_relaxed);
    bool oka = rankAtMaster(a, mb, &ra), okb = rankAtMaster(b, mb, &rb);
    if (!oka) return okb ? -1 : 0;
    if (!okb) return 1;
    return ra - rb;
}

static std::string title() { return cfgStr("keycol_title", "Key @ Master"); }

// Follow the master deck: when its BPM moves, repaint the lists showing the column (at most every 0.25 s) and re-sort
// those sorted by it (a whole sort on the main thread: at most once a second while it moves, and once when it settles).
static double g_sortedAt;
static void tick() {
    double mb = masterBpm(), t = now();
    g_lastMaster = mb;
    bool moved = fabs(mb - g_timerMaster) >= 0.05;
    if (moved && t - g_refreshedAt >= 0.25) {
        g_timerMaster = mb;
        g_refreshedAt = t;
        bool sort = t - g_sortedAt >= 1;
        if (sort) g_sortedMaster = mb, g_sortedAt = t;
        refreshColumn(kColumnId, sort);
    } else if (!moved && fabs(mb - g_sortedMaster.load()) >= 0.05 && t - g_refreshedAt >= 0.3) {  // settled: the last sort
        g_sortedMaster = mb, g_sortedAt = t;
        refreshColumn(kColumnId, true);
    }
}

// Windows: the column needs columns.cpp's hooks (header restore, column ids, painting, sorting); without them the
// mod is off. (column_index2-5 aren't needed there: see columns.cpp.)
#ifdef _WIN32
namespace rbm { void modOffForHooks(const char* mod); }
static void init() {
    const HookId need[] = {HK_paint_getString, HK_sorter_setCompFunc, HK_header_restore1, HK_header_restore2, HK_header_restore3,
                           HK_header_restore4, HK_column_index1};
    for (HookId h : need)
        if (!hookPatched(h)) {
            logf("keycolumn: off, a rekordbox hook it needs has no site in this build (hook #%d)", (int)h);
            rbm::modOffForHooks("keycolumn");
            g_on = false;
            return;
        }
}
#else
static void init() {}
#endif

RBM_COLUMN({.id = kColumnId, .title = title, .text = text, .compare = compare, .enabled = &g_on});
RBM_MOD({.name = "keycolumn", .init = init, .tick = tick, .enabled = &g_on});
