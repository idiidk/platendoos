// analyzer window: progress of a playlist analysis run, per track, with pause / stop / retry.
// Shared by macOS and Windows: this builds the page's state (ui/analyzer.html, through core/webui.h) from the
// analyzer's data on each refresh and carries out the page's commands. Refreshed by the analyzer's tick.
#include "../core/rbmod.h"
#include "../core/webui.h"
#include "analyzer.h"
#include <algorithm>
#include <cstdarg>

using analyzer::State;
namespace web = rbm::webui;

namespace {  // file-local: another mod has types of the same names

web::Window* g_win;
std::string g_sel;        // the selected playlist's id
bool g_showFailed;
std::string g_last;       // the state last sent (an unchanged one isn't sent again)

const char* stateClass(State s) {
    switch (s) {
        case State::Waiting: return "waiting";
        case State::Loading: return "loading";
        case State::Analysing: return "analysing";
        case State::Done: return "done";
        case State::Skipped: return "skipped";
        case State::Retry: return "retry";
        case State::Failed: return "failed";
        case State::NotAnalysed: return "waiting";
        case State::Analysed: return "done";
    }
    return "waiting";
}

std::string fmt(const char* f, ...) __attribute__((format(printf, 1, 2)));
std::string fmt(const char* f, ...) {
    char b[512];
    va_list ap;
    va_start(ap, f);
    vsnprintf(b, sizeof b, f, ap);
    va_end(ap);
    return b;
}

std::string stateText(const analyzer::TrackView& t) {
    switch (t.state) {
        case State::Waiting: return "Waiting";
        case State::Loading: return t.deck >= 0 ? fmt("Loading on deck %d", t.deck + 1) : "Queued";
        case State::Analysing: return t.deck >= 0 ? fmt("Analysing on deck %d", t.deck + 1) : "Analysing";
        case State::Done: return "Analysed";
        case State::Skipped: return "Already analysed";
        case State::Retry: return "Will retry";
        case State::Failed: return "Failed";
        case State::NotAnalysed: return "Not analysed";
        case State::Analysed: return "Analysed";
    }
    return "";
}

std::string duration(double s) {
    if (s < 0) return "";
    int m = (int)(s / 60), sec = (int)s % 60;
    return m ? fmt("%d:%02d", m, sec) : fmt("%ds", sec);
}

// Select a playlist ("" = the run's, else keep the selection).
void select(const std::string& id) {
    std::string pick = id.empty() ? analyzer::runPlaylist() : id;
    if (!pick.empty() && pick != g_sel) {
        g_sel = pick;
        g_showFailed = false;
    }
}

void sendState() {
    if (!g_win) return;
    using web::jsonStr;

    // Sidebar: the playlists, with how far each is analysed; the selection stays (by id).
    std::vector<analyzer::PlaylistEntry> entries = analyzer::playlistEntries();
    bool runActive = false;
    std::string side = "[";
    bool found = false;
    for (const analyzer::PlaylistEntry& e : entries) found |= e.id == g_sel;
    if (!found && !entries.empty()) {  // gone: the run's playlist, else the first
        std::string run = analyzer::runPlaylist();
        g_sel = entries[0].id;
        for (const analyzer::PlaylistEntry& e : entries)
            if (e.id == run) g_sel = e.id;
        g_showFailed = false;
    }
    for (const analyzer::PlaylistEntry& e : entries) {
        std::string sub;
        if (e.running) sub = fmt("Analysing %d/%d", e.analysed, e.tracks), runActive = true;
        else if (e.tracks == 0) sub = "Not listed yet";
        else sub = fmt("%d/%d analysed", e.analysed, e.tracks);
        if (side.size() > 1) side += ",";
        side += "{\"id\":" + jsonStr(e.id) + ",\"name\":" + jsonStr(e.name) + ",\"sub\":" + jsonStr(sub) + "}";
    }
    side += "]";

    analyzer::View v = g_sel.empty() ? analyzer::snapshot() : analyzer::playlistView(g_sel);
    bool none = g_sel.empty() && !v.exists;
    bool empty = !v.known && !v.isRun;  // tracks not known: explain instead of the table
    int total = (int)v.tracks.size();

    std::string headline, detailText;
    bool haveDetail = false;
    if (none) {
        headline = "No playlist";
        detailText = "Open this window from a playlist's menu in rekordbox";
        haveDetail = true;
    } else if (!v.isRun) {
        headline = v.playlistName.empty() ? "Playlist" : v.playlistName;
        detailText = !v.known ? "Tracks not listed yet"
                              : fmt("%d of %d analysed%s", v.analysed, total, v.shown ? "  \xC2\xB7  shown in rekordbox" : "");
        haveDetail = true;
    } else if (v.stopping) headline = fmt("Stopping, finishing %d track%s", v.active, v.active == 1 ? "" : "s");
    else if (v.running && v.userPaused) headline = v.active ? "Pausing (finishing the current tracks)" : "Paused";
    else if (v.running && v.paused) headline = "Waiting: show the playlist again to continue";
    else if (v.running && v.rows == 0) headline = "Opening the playlist\xE2\x80\xA6";
    else if (v.running && v.holdWaitGb > 0) {  // how far the wait for held tracks is, counting up to where the run continues, and what happens right now
        double total = std::max(v.holdTotalMb, 1.0), done = std::min(total, std::max(0.0, total - v.holdToGoMb));
        std::string what = "Waiting for " + (v.holdWhat.empty() ? std::string("held tracks") : v.holdWhat);
        headline = total >= 1000 ? fmt("%s: %.2f of %.2f GB done", what.c_str(), done / 1000, total / 1000)
                                 : fmt("%s: %.0f of %.0f MB done", what.c_str(), done, total);
        if (!v.holdStep.empty()) headline += " (" + v.holdStep + ")";
    }
    else if (v.running && v.limitLeft > 0)
        headline = fmt("Spotify's streaming limit: waiting %d s", v.limitLeft);
    else if (v.running) headline = fmt("%s %d of %d", v.force ? "Re-analysing" : "Analysing", v.done + v.skipped + v.failed + v.active, v.rows);
    else if (!v.exists) headline = "No analysis yet";
    else if (v.finishedWhy == "done") headline = v.failed ? "Finished, with problems" : "Finished";
    else headline = v.stopWhy.empty() ? "Stopped" : "Stopped by itself";
    if ((v.stopping || !v.running) && v.isRun && !v.stopWhy.empty()) detailText = v.stopWhy, haveDetail = true;

    if (!haveDetail) {
        std::vector<std::string> parts;
        if (!v.playlistName.empty()) parts.push_back(v.playlistName);
        if (v.done) parts.push_back(fmt("%d analysed", v.done));
        if (v.skipped) parts.push_back(fmt("%d already analysed", v.skipped));
        if (v.failed) parts.push_back(fmt("%d failed", v.failed));
        if (v.waiting && v.exists) parts.push_back(fmt("%d to go", v.waiting));
        if (v.eta >= 0) parts.push_back("about " + duration(v.eta) + " left");
        if (v.exists) parts.push_back(duration(v.elapsed) + " elapsed");
        for (size_t i = 0; i < parts.size(); i++) detailText += (i ? "  \xC2\xB7  " : "") + parts[i];
    }

    double pMax, pVal;
    if (v.isRun) pMax = std::max(1, v.rows), pVal = v.done + v.skipped + v.failed;
    else pMax = std::max(1, total), pVal = v.analysed;

    bool run = v.isRun;
    std::string busy = runActive ? (run ? "This playlist is being analysed" : "Another playlist is being analysed") : "";
    std::string emptyText = g_showFailed
        ? "rekordbox hasn't loaded this playlist in its browser yet. Expand its folder in rekordbox's sidebar, then try again."
        : "rekordbox lists this playlist's tracks once it's shown. Open it in rekordbox to see them here.";

    std::string j = "{\"type\":\"state\",\"sel\":" + jsonStr(g_sel) + ",\"side\":" + side;
    j += ",\"headline\":" + jsonStr(headline) + ",\"detail\":" + jsonStr(detailText);
    j += fmt(",\"detailWarn\":%s", v.isRun && !v.stopWhy.empty() && (v.stopping || !v.running) ? "true" : "false");
    j += fmt(",\"none\":%s,\"empty\":%s", none ? "true" : "false", empty ? "true" : "false");
    j += ",\"emptyText\":" + jsonStr(emptyText);
    j += fmt(",\"progressMax\":%g,\"progressValue\":%g", pMax, pVal);
    j += ",\"pauseLabel\":" + jsonStr(v.userPaused ? "Resume" : "Pause");
    j += fmt(",\"pauseEnabled\":%s,\"stopEnabled\":%s,\"retryEnabled\":%s", (run && v.running && !v.stopping) ? "true" : "false",
             (run && v.running && !v.stopping) ? "true" : "false", (run && v.failed > 0 && !v.stopping) ? "true" : "false");
    j += fmt(",\"analyzeEnabled\":%s", (!runActive && !g_sel.empty()) ? "true" : "false");
    j += ",\"analyzeTip\":" + jsonStr(busy.empty() ? "Analyse the playlist's tracks that aren't analysed yet" : busy);
    j += ",\"reanalyzeTip\":" + jsonStr(busy.empty() ? "Analyse every track of the playlist again, also the analysed ones" : busy);
    j += fmt(",\"fast\":%s", analyzer::fastMode() ? "true" : "false");
    // A checkbox of another mod's own ("analyzer.option"; none of this tree has one).
    std::string optLabel, optTip;
    auto option = rbm::service<bool (*)(int, std::string*, std::string*)>("analyzer.option");
    bool optOn = option && option(-1, &optLabel, &optTip);
    j += option && !optLabel.empty() ? fmt(",\"option\":{\"on\":%s,\"label\":", optOn ? "true" : "false") + jsonStr(optLabel) + ",\"tip\":" + jsonStr(optTip) + "}"
                                     : std::string(",\"option\":null");
    j += fmt(",\"hasGenres\":%s,\"genreFirst\":%s", analyzer::hasGenres() ? "true" : "false", analyzer::genreFirst() ? "true" : "false");

    j += ",\"tracks\":[";
    for (int i = 0; i < total; i++) {
        const analyzer::TrackView& t = v.tracks[i];
        int a, b;
        bool ma, mb, ca, cb;
        // Highlight disagreement of betterkey's key with rekordbox's.
        bool differ = !t.cnnKey.empty() && rbm::parseKey(t.key.c_str(), &a, &ma, &ca) &&
                      rbm::parseKey(t.cnnKey.c_str(), &b, &mb, &cb) && (a != b || ma != mb);
        bool timed = t.state == State::Loading || t.state == State::Analysing || t.state == State::Done || t.state == State::Failed;
        if (i) j += ",";
        j += fmt("{\"row\":%d,\"state\":\"%s\",\"active\":%s", t.row, stateClass(t.state),
                 (t.state == State::Loading || t.state == State::Analysing) ? "true" : "false");
        j += ",\"status\":" + jsonStr(stateText(t));
        j += ",\"todo\":" + std::string((t.state == State::Waiting || t.state == State::Retry || t.state == State::Loading ||
                                         t.state == State::Analysing || t.state == State::NotAnalysed) ? "true" : "false");
        j += ",\"ok\":" + std::string((t.state == State::Done || t.state == State::Skipped || t.state == State::Analysed) ? "true" : "false");
        j += ",\"problem\":" + std::string((t.state == State::Failed || t.state == State::Retry) ? "true" : "false");
        j += ",\"title\":" + jsonStr(t.title.empty() ? t.uri : t.title);
        j += ",\"artist\":" + jsonStr(t.artist);
        j += ",\"genre\":" + jsonStr(t.genre) + fmt(",\"heard\":%s", t.genreHeard ? "true" : "false");
        j += ",\"bpm\":" + jsonStr(t.bpm > 0 ? fmt("%.2f", t.bpm) : "");
        j += ",\"key\":" + jsonStr(t.key) + ",\"cnn\":" + jsonStr(t.cnnKey);
        j += fmt(",\"differ\":%s", differ ? "true" : "false");
        j += ",\"time\":" + jsonStr(timed && t.seconds > 0 ? duration(t.seconds) : "") + "}";
    }
    j += "]}";
    if (j == g_last) return;
    g_last = j;
    web::post(g_win, j);
}

void onMessage(const std::string& m) {
    std::string cmd = web::jsonGet(m, "cmd");
    if (cmd == "ready") g_last.clear();
    else if (cmd == "select") {
        std::string id = web::jsonGet(m, "id");
        if (id.empty() || id == g_sel) return;
        g_sel = id;
        g_showFailed = false;
    } else if (cmd == "analyze") analyzer::analyzePlaylist(g_sel, false);
    else if (cmd == "reanalyze") analyzer::analyzePlaylist(g_sel, true);
    else if (cmd == "show") g_showFailed = !rbm::showPlaylistId(g_sel);
    else if (cmd == "pause") analyzer::setUserPaused(!analyzer::snapshot().userPaused);
    else if (cmd == "stop") analyzer::stop();
    else if (cmd == "retry") analyzer::retryFailed();
    else if (cmd == "fast") analyzer::setFastMode(web::jsonGet(m, "on") == "true");
    else if (cmd == "option") {
        if (auto option = rbm::service<bool (*)(int, std::string*, std::string*)>("analyzer.option")) option(web::jsonGet(m, "on") == "true", nullptr, nullptr);
    }
    else if (cmd == "genreFirst") analyzer::setGenreFirst(web::jsonGet(m, "on") == "true");
    else if (cmd == "close") { web::close(g_win); return; }
    sendState();
}

}  // namespace

namespace analyzer {

void showWindowFor(const std::string& id) {
    if (!id.empty()) addWindowPlaylist(id);
    select(id);
    g_win = web::open("analysis", "analyzer", "Playlist Analysis", 1100, 560, onMessage);
    sendState();
}

void showWindow(void* treeItem) {
    if (!treeItem) return showWindowFor("");
    std::string id = rbm::treeItemId(treeItem);
    rbm::showPlaylist(treeItem);
    showWindowFor(id);
}

void refreshWindow() { sendState(); }

// Gates the owner's periodic refresh: nothing is built while the window is closed, minimised or covered.
bool windowVisible() { return g_win && web::onScreen(g_win); }

}  // namespace analyzer
