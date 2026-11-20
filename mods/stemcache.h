// stemcache <-> its window (stemcache_panel.mm).
#pragma once
#include <string>
#include <vector>

namespace stemcache {

// Unsupported: rekordbox doesn't separate this track (a streaming service other than Spotify, a file it can't read).
// NeedsFile: a Spotify track without its local file yet, where a mod keeps them (rbmod.h, "trackfiles"): stems ahead
// of time come from that file.
// DeckOnly: a Spotify track: it gets its stems when it loads onto a deck (separated in memory).
// NotSeparated / Separated: a track of a playlist outside the run (Separated: a run separated it before).
enum class State { Waiting, Separating, Done, FromDisk, Unsupported, NeedsFile, GaveUp, NotSeparated, Separated, DeckOnly };

struct TrackView {
    int row = 0;
    State state = State::Waiting;
    double seconds = 0;  // time it took / has been running
    int chunks = 0, want = 0;
    std::string title, artist, uri;
};

struct View {
    std::string playlistId, playlistName;
    bool isRun = false;    // the run is on this playlist; the run fields below are for it
    bool known = true;     // false: the playlist's tracks aren't known yet (it has to be shown once)
    bool shown = false;    // the browser shows this playlist
    bool exists = false;   // a run was started (it may have finished)
    bool running = false, collecting = false;
    std::string finishedWhy;  // "done", "stopped", "empty list", "playlist not shown", "disk full", "no folder", "no answer", "stems off"
    int rows = 0, done = 0, fromDisk = 0, unsupported = 0, needsFile = 0, deckOnly = 0, gaveUp = 0, active = 0, waiting = 0;
    int separated = 0;     // outside a run: tracks separated before
    double elapsed = 0, eta = -1;  // seconds; eta -1 = unknown
    std::vector<TrackView> tracks;
};

// What separating Spotify tracks needs, for the window to say.
struct Needs {
    bool stemsOn = true;        // rekordbox's STEMS (Preferences > Extensions) is on
    bool spotifyOn = true;      // stemcache_spotify isn't 0
    bool trackFiles = false;       // a mod keeping local files of Spotify tracks is on (rbmod.h, "trackfiles")
    std::string trackFilesMod;     // its name, even while it's off ("" if there is none)
    bool folderNoAnswer = false;   // stemcache_dir doesn't answer (its drive hung?): stems are neither read nor stored
    std::string folder;            // stemcache_dir
};
Needs needs();

View playlistView(const std::string& id);  // the run's view if the run is on it ("" = the run's playlist)
std::string runPlaylist();                 // "" if no run
void separatePlaylist(const std::string& id);  // starts a run (one at a time; shows the playlist in the browser)
void fetchFiles(const std::string& id);        // asks that mod to fetch the playlist's local files (rbm::fetchTrackFiles)
void stop();

// Window. Opened from a playlist's menu, it shows that playlist (and selects it in the browser).
void showWindow(void* treeItem = nullptr);
void showWindowFor(const std::string& playlistId);  // "" = the run's playlist
void refreshWindow();  // called by stemcache's tick while the window is open
bool windowVisible();

}  // namespace stemcache
