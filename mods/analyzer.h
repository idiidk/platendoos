// analyzer <-> its window (analyzer_panel.mm).
#pragma once
#include <string>
#include <vector>

namespace analyzer {

// NotAnalysed / Analysed: a track of a playlist outside the run (from rekordbox's collection).
enum class State { Waiting, Loading, Analysing, Done, Skipped, Retry, Failed, NotAnalysed, Analysed };

struct TrackView {
    int row;
    State state;
    int deck;          // 0-based while loading/analysing, else the deck it was analysed on (-1 none)
    double seconds;    // time on the deck so far / it took
    std::string title, artist, uri;
    double bpm;        // rekordbox's, 0 if unknown
    std::string key;   // rekordbox's
    std::string cnnKey;  // betterkey's, in the same notation, with probability
    std::string genre;   // the Genre field, else the style the genres mod heard ("": none, or no genres mod)
    bool genreHeard = false;  // genre is what the model heard, not (yet) written in the Genre field
};

struct View {
    std::string playlistId, playlistName;  // the playlist shown ("" = the run's, before it is known)
    bool isRun = false;    // the run is on this playlist; the run fields below are for it
    bool known = true;     // false: the playlist's tracks aren't known yet (it has to be shown once)
    bool shown = false;    // the browser shows this playlist
    int analysed = 0;      // tracks with a current analysis (all of the playlist's tracks)
    bool exists = false;   // a run was started (it may have finished)
    bool running = false, paused = false, userPaused = false, stopping = false, force = false;
    std::string finishedWhy;
    std::string stopWhy;   // why the run stopped by itself ("" = finished, or stopped by the user)
    int rows = 0, done = 0, skipped = 0, failed = 0, active = 0, waiting = 0;
    int limitLeft = 0;     // seconds until Spotify's streaming limit is over (0: not limited)
    double holdWaitGb = 0;   // the run waits for another mod to let held tracks go, for this much (0: it doesn't wait)
    double holdToGoMb = 0, holdTotalMb = 0;  // of this wait: how much has to go before the run continues, and how much had to
    std::string holdWhat;    // what the run waits for ("": not known)
    std::string holdStep;    // what happens about it right now ("": not known)
    double elapsed = 0, eta = -1;  // seconds; eta -1 = unknown
    std::vector<TrackView> tracks;
};

View snapshot();                        // the run's playlist
View playlistView(const std::string& id);  // any playlist; the run's view if the run is on it

// The window's sidebar: the run's playlist first, then the playlists opened in the window.
struct PlaylistEntry {
    std::string id, name;
    bool running = false;   // the run is on it
    int tracks = 0, analysed = 0;
};
std::vector<PlaylistEntry> playlistEntries();
std::string runPlaylist();                    // id of the run's playlist, "" if none
void analyzePlaylist(const std::string& id, bool force);  // starts a run (one at a time)
void setUserPaused(bool paused);
void stop();
void retryFailed();
bool fastMode();
void setFastMode(bool on);
bool hasGenres();   // the genres mod is on
bool genreFirst();  // the genres mod's genres_on_analyse: a track's genre is detected and written when it is analysed
void setGenreFirst(bool on);

// Window. Opened from a playlist's menu, it shows that playlist (and selects it in the browser).
void showWindow(void* treeItem = nullptr);
void showWindowFor(const std::string& playlistId);  // same by id; "" = the run's playlist
void addWindowPlaylist(const std::string& id);  // list it in the window's sidebar (this session)
void refreshWindow();      // called by the analyzer while the window is open
bool windowVisible();

}  // namespace analyzer
