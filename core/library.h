// library: the mods' own database of tracks, playlists and genres (sqlite, in plat::configDir()/library.sqlite).
// rekordbox only knows a Spotify track while some list shows it (or once it is in the collection); this keeps what the
// mods learn about each track, keyed by its URI, so they can show and use it without rekordbox.
// Thread-safe: every function may be called from any thread.
#pragma once
#include <string>
#include <vector>

namespace rbm::library {

struct Track {
    std::string uri;            // "spotify:track:..." or a file path
    std::string title, artist, album;
    double duration = 0;        // seconds, 0 unknown
    double bpm = 0;             // rekordbox's, 0 unknown
    std::string key;            // rekordbox's key text, "" unknown
    double updatedAt = 0;       // rbm::now() of the last change
};

// Adds the track or updates it: only fields that are set (non-empty / non-zero) overwrite stored ones.
void putTrack(const Track& t);
void putTracks(const std::vector<Track>& tracks);  // one transaction
bool track(const std::string& uri, Track* out);

// A playlist as last seen: its tree identifier ("/Root/Spotify/Your Library/Minimal Techno Mix"),
// name and track URIs in order (replaces the stored list).
struct Playlist {
    std::string id, name;
    std::vector<std::string> uris;
    double updatedAt = 0;
};
void putPlaylist(const Playlist& p);
bool playlist(const std::string& id, Playlist* out);
std::vector<Playlist> playlists();  // without uris, most recently updated first


// A track's styles as the genres mod's model heard them (kept apart from rekordbox's Genre field, which a Spotify
// track may not have and the user may have typed): what other mods ask for through "genres.forTrack".
struct GenreGuess {
    std::string style;          // the Discogs style ("Drum n Bass"), "" none
    std::string broad;          // its broad genre ("Drum & Bass")
    double score = 0;           // 0..1
};
struct TrackGenre {
    std::string uri;
    GenreGuess top[3];          // best first
    double detectedAt = 0;      // seconds since 1970
};
void putGenre(const TrackGenre& g);  // insert or replace
bool genre(const std::string& uri, TrackGenre* out);

}  // namespace rbm::library
