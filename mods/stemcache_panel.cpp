// stemcache window: a playlist's tracks and their stems, the actions (separate, stop, fetch local files first)
// and the progress of a run, per track. Shared by macOS and Windows: this builds the page's state
// (ui/stemcache.html, through core/webui.h) and carries out its commands. Refreshed by stemcache's tick.
#include "../core/rbmod.h"
#include "../core/webui.h"
#include "stemcache.h"
#include <algorithm>

using stemcache::State;
namespace web = rbm::webui;

namespace {  // file-local: another mod has types of the same names

web::Window* g_win;
std::string g_last;        // the state last sent
std::string g_playlistId;  // the playlist shown ("" = the run's)

const char* stateClass(State s) {
    switch (s) {
        case State::Waiting: return "waiting";
        case State::Separating: return "separating";
        case State::Done: return "done";
        case State::FromDisk: return "fromdisk";
        case State::Unsupported: return "unsupported";
        case State::NeedsFile: return "needsfile";
        case State::GaveUp: return "gaveup";
        case State::NotSeparated: return "waiting";
        case State::Separated: return "separated";
        case State::DeckOnly: return "unsupported";
    }
    return "";
}

std::string stateText(const stemcache::TrackView& t) {
    switch (t.state) {
        case State::Waiting: return "Waiting";
        case State::Separating:
            return t.want > 0 ? "Separating " + std::to_string(std::min(t.chunks, t.want)) + "/" + std::to_string(t.want) : "Separating";
        case State::Done: return "Separated";
        case State::FromDisk: return "Already separated";
        case State::Unsupported: return "Not supported";
        case State::NeedsFile: return rbm::trackFilesText("stems.state");
        case State::GaveUp: return "Gave up";
        case State::NotSeparated: return "Not separated";
        case State::Separated: return "Separated";
        case State::DeckOnly: return "On a deck";
    }
    return "";
}

std::string duration(double s) {
    if (s < 0) return "";
    int m = (int)(s / 60), sec = (int)s % 60;
    char b[32];
    if (m) snprintf(b, sizeof b, "%d:%02d", m, sec);
    else snprintf(b, sizeof b, "%ds", sec);
    return b;
}

std::string plural(int n, const char* what) { return std::to_string(n) + " " + what; }

void sendState() {
    if (!g_win || !web::visible(g_win)) return;
    using web::jsonStr;
    stemcache::View v = stemcache::playlistView(g_playlistId);
    stemcache::Needs needs = stemcache::needs();
    bool otherRun = !v.isRun && !stemcache::runPlaylist().empty() && stemcache::playlistView("").running;
    std::string headline;
    if (!v.isRun) headline = v.playlistName.empty() ? "Stems" : v.playlistName;
    else if (v.running && v.collecting) headline = "Reading the playlist\xE2\x80\xA6";
    else if (v.running) headline = "Separating " + std::to_string(std::min(v.rows, v.done + v.fromDisk + v.gaveUp + v.active)) + " of " + std::to_string(v.rows);
    else if (v.finishedWhy == "done" && v.rows && v.done + v.fromDisk == 0) headline = "Nothing to separate";
    else if (v.finishedWhy == "done") headline = v.gaveUp ? "Finished, with problems" : "Finished";
    else if (v.finishedWhy == "stopped") headline = "Stopped";
    else if (v.finishedWhy == "empty list") headline = "The playlist is empty";
    else if (v.finishedWhy == "playlist not shown") headline = "The playlist didn't open";
    else if (v.finishedWhy == "disk full") headline = "Stopped, the disk is nearly full";
    else if (v.finishedWhy == "no folder") headline = "Stopped, the stems folder is not there (drive not connected?)";
    else if (v.finishedWhy == "no answer") headline = "Stopped, the stems folder doesn't answer (drive hung?)";
    else if (v.finishedWhy == "stems off") headline = "Stopped, STEMS is off";
    else headline = "Finished";

    std::vector<std::string> parts;
    if (v.isRun && !v.playlistName.empty()) parts.push_back(v.playlistName);
    if (!v.isRun) parts.push_back(plural(v.rows, v.rows == 1 ? "track" : "tracks"));
    if (v.separated) parts.push_back(plural(v.separated, "separated before"));
    if (v.done) parts.push_back(plural(v.done, "separated"));
    if (v.fromDisk) parts.push_back(plural(v.fromDisk, "already separated"));
    if (v.gaveUp) parts.push_back(plural(v.gaveUp, "gave up"));
    if (v.needsFile) parts.push_back(std::to_string(v.needsFile) + " " + rbm::trackFilesText("stems.count", v.needsFile));
    if (v.deckOnly) parts.push_back(plural(v.deckOnly, v.deckOnly == 1 ? "Spotify track: stems on a deck" : "Spotify tracks: stems on a deck"));
    if (v.unsupported) parts.push_back(plural(v.unsupported, "not supported"));
    if (v.running && v.waiting + v.active) parts.push_back(plural(v.waiting + v.active, "to go"));
    if (v.running && v.eta >= 0) parts.push_back("about " + duration(v.eta) + " left");
    if (v.isRun) parts.push_back(duration(v.elapsed) + " elapsed");
    std::string detail;
    for (size_t i = 0; i < parts.size(); i++) detail += (i ? "  \xC2\xB7  " : "") + parts[i];

    // What stands in the way, most important first.
    std::string note;
    if (!needs.stemsOn)
        note = "rekordbox's STEMS is off. Turn it on in Preferences > Extensions > STEMS to separate stems.";
    else if (needs.folderNoAnswer)
        note = "The stems folder " + needs.folder + " doesn't answer (drive hung?). Stems are separated as without the cache, and nothing is stored, until it does.";
    else if (v.needsFile)  // (the words of the mod that keeps the files: its own name in them when it's off)
        note = rbm::trackFilesText(needs.trackFiles ? "stems.note" : "stems.off", v.needsFile);
    else if (v.deckOnly)
        note = "Spotify tracks get their stems when they load onto a deck: separated from the stream and kept in memory, never stored on disk.";
    else if (v.unsupported && !needs.spotifyOn)
        note = "Stems for Spotify tracks are switched off (stemcache_spotify=0 in the config).";
    else if (v.unsupported)
        note = std::to_string(v.unsupported) + (v.unsupported == 1 ? " track" : " tracks") +
               " can't be separated: rekordbox doesn't offer STEMS for other streaming services' tracks or files it can't read.";
    else if (otherRun)
        note = "Another playlist is being separated; one playlist at a time.";
    else if (!v.known)
        note = "Its tracks show once rekordbox has listed the playlist.";

    int handled = v.done + v.fromDisk + v.gaveUp + v.unsupported + v.needsFile + v.deckOnly;
    double cur = 0;  // the track in progress counts by its chunks
    for (const stemcache::TrackView& t : v.tracks)
        if (t.state == State::Separating && t.want > 0) cur = std::min(1.0, (double)t.chunks / t.want);

    auto b = [](bool x) { return x ? "true" : "false"; };
    char num[64];
    std::string j = "{\"headline\":" + jsonStr(headline) + ",\"detail\":" + jsonStr(detail) + ",\"note\":" + jsonStr(note);
    snprintf(num, sizeof num, ",\"progressMax\":%d,\"progressValue\":%g", std::max(1, v.rows), handled + cur);
    j += num;
    j += std::string(",\"progressShown\":") + b(v.isRun) + ",\"stopEnabled\":" + b(v.isRun && v.running);
    j += std::string(",\"separateEnabled\":") + b(needs.stemsOn && !needs.folderNoAnswer && !v.running && !otherRun && !v.playlistId.empty() && (v.rows > 0 || !v.known));
    j += std::string(",\"fetchShown\":") + b(v.needsFile && rbm::canFetchTrackFiles()) + ",\"fetchLabel\":" + jsonStr(rbm::trackFilesText("button"));
    j += std::string(",\"running\":") + b(v.running);
    j += ",\"tracks\":[";
    for (size_t i = 0; i < v.tracks.size(); i++) {
        const stemcache::TrackView& t = v.tracks[i];
        if (i) j += ",";
        j += "{\"row\":" + std::to_string(t.row + 1) + ",\"title\":" + jsonStr(t.title) + ",\"artist\":" + jsonStr(t.artist);
        j += ",\"state\":" + jsonStr(stateText(t)) + ",\"cls\":\"" + stateClass(t.state) + "\"";
        j += ",\"time\":" + jsonStr(t.seconds > 0 ? duration(t.seconds) : "") + "}";
    }
    j += "]}";
    if (j == g_last) return;
    g_last = j;
    web::post(g_win, j);
}

void onMessage(const std::string& m) {
    std::string cmd = web::jsonGet(m, "cmd");
    if (cmd == "ready") g_last.clear();
    else if (cmd == "stop") stemcache::stop();
    else if (cmd == "separate") {
        g_playlistId = stemcache::playlistView(g_playlistId).playlistId;
        stemcache::separatePlaylist(g_playlistId);
    } else if (cmd == "fetch") stemcache::fetchFiles(stemcache::playlistView(g_playlistId).playlistId);
    else if (cmd == "close") { web::close(g_win); return; }
    sendState();
}

}  // namespace

namespace stemcache {

void showWindowFor(const std::string& id) {
    g_playlistId = id;
    g_win = web::open("stemcache", "stemcache", "Stem Separation", 780, 520, onMessage);
    g_last.clear();
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

}  // namespace stemcache
