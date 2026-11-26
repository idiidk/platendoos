// second command channel (cmd2.txt): playlists, deck load/play/unload
#import <Foundation/Foundation.h>
#include <thread>
#include "rbmod.h"
using namespace rbm;
#define D "/tmp/rbm-uimon/"
#define GEN2 3
static double wall() { timespec t; clock_gettime(CLOCK_REALTIME, &t); return t.tv_sec + t.tv_nsec * 1e-9; }
static FILE* g_out;
static void out(const char* f, ...) { va_list a; va_start(a, f); vfprintf(g_out, f, a); va_end(a); fputc('\n', g_out); fflush(g_out); }
static int curGen() { FILE* f = fopen(D "gen2.txt", "r"); int g = 0; if (f) { fscanf(f, "%d", &g); fclose(f); } return g; }
static void run(const std::string& l) {
    std::string c = l.substr(0, l.find(' ')), r = l.find(' ') == std::string::npos ? "" : l.substr(l.find(' ') + 1);
    if (c == "pl") { bool ok = showPlaylistId(r); out("%.3f PL %s -> %d (shown %s)", wall(), r.c_str(), ok, shownPlaylist().c_str()); }
    else if (c == "openpl") { int rr = openPlaylist(r); out("%.3f OPENPL %d %s", wall(), rr, r.c_str()); }
    else if (c == "selpl") out("%.3f SELPL %s", wall(), selectedPlaylist().c_str());
    else if (c == "rows") { void* lv = mainListViewer(); int n = lv ? listRows(lv) : -1; out("%.3f ROWS %d", wall(), n); for (int i = 0; i < n && i < atoi(r.c_str()); i++) { const char* row = listRow(lv, i); const char* p = row ? trackPath(row) : nullptr; out("  row %d %s", i, p ? p : "-"); } }
    else if (c == "load") { int row, deck; sscanf(r.c_str(), "%d %d", &row, &deck); void* lv = mainListViewer(); out("%.3f LOAD row %d deck %d", wall(), row, deck); listLoadTrack(lv, deck - 1, row); }
    else if (c == "play" || c == "pause" || c == "unload") {
        int ch = atoi(r.c_str()) - 1; void* e = engine(); out("%.3f %s deck %d", wall(), c.c_str(), ch + 1);
        if (c == "play") at<void (*)(void*, int, bool)>(symbol("__ZN8djengine10DjEngineIF4playENS_15EnPlayerChannelEb"))(e, ch, false);
        else if (c == "pause") at<void (*)(void*, int)>(symbol("__ZN8djengine10DjEngineIF5pauseENS_15EnPlayerChannelE"))(e, ch);
        else at<void (*)(void*, int)>(symbol("__ZN8djengine10DjEngineIF6unloadENS_15EnPlayerChannelE"))(e, ch);
    }
    else if (c == "state") { for (int d = 0; d < deckCount(); d++) out("%.3f DECK %d loaded %d playing %d %s", wall(), d + 1, deckLoaded(d), deckPlaying(d), deckPath(d).c_str()); }
    else out("%.3f cmd2 unknown %s", wall(), l.c_str());
}
extern "C" void rbm_experiment() {
    g_out = fopen(D "stalls.txt", "a");
    std::thread([] {
        while (curGen() == GEN2) {
            FILE* f = fopen(D "cmd2.txt", "r");
            if (f) { std::string s; char b[4096]; while (fgets(b, sizeof b, f)) s += b; fclose(f); remove(D "cmd2.txt");
                size_t i = 0; while (i < s.size()) { size_t j = s.find('\n', i); if (j == std::string::npos) j = s.size(); std::string l = s.substr(i, j - i); i = j + 1;
                    if (l.rfind("sleep ", 0) == 0) { usleep((useconds_t)(atof(l.c_str() + 6) * 1e6)); continue; }
                    if (l.empty()) continue; auto* L = new std::string(l); plat::onMain([L] { run(*L); delete L; }); usleep(20000); } }
            usleep(30000);
        }
    }).detach();
    logf("cmd2: started");
}
