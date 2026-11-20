// genres: fills rekordbox's Genre field from what the music sounds like. A style model (Discogs-EffNet, see
// genres_model.h) listens to each track and names its three most likely styles out of 400 Discogs styles.
//  - Playlist menu "Open genres window": the playlist's tracks with their Genre now; Detect genres listens to them and
//    suggests a genre each (a dropdown of the three best guesses, with the model's confidence), as the specific style
//    ("Liquid Funk") or a broad genre ("Drum & Bass"). Apply writes the ticked ones; empty Genre fields only unless
//    Replace is on; Remove genres empties them all; Undo puts the old ones back.
//    Spotify tracks are read from the stream into memory by the spotifyaudio mod (or from a local file of the track,
//    rbmod.h "trackfiles"), and get a Genre field once they're in rekordbox's collection (analysed).
//  - What the model found is kept per track in the library (core/library.h), so it is there next time and for other mods.
//  - Fill on analysis ("Genre first" in the analysis window): when rekordbox analyses a track, the model listens to it
//    before its beat grid is made (bettergrid asks "genres.forTrack" with the analysis' audio and picks the tempo by
//    the answer), and an empty Genre field is filled with what it heard once the analysis is saved (core's analysis
//    hooks tell this mod when; without bettergrid the track is listened to then, a Spotify track as in the window).
//    Off by default; this window and the analysis window have the switch.
//  - Other mods: "genres.forTrack" gives a track's genre (stored, its Genre field, or detected from audio the caller
//    has; bettergrid picks the BPM with it), "genres.detect" the model's guesses for some audio.
// The model is CC BY-NC-SA 4.0 (MTG / Essentia): non-commercial use.
//   config: genres=0 to disable
//           genres_detail=style   style: the specific style ("Deep House"); broad: the broad genre ("House")
//           genres_on_analyse=0   1: fill empty Genre fields when rekordbox analyses a track
//           genres_warmup=1       0: the style model loads when it is first needed, not at launch
#include "../core/rbmod.h"
#include "../core/library.h"
#include "../core/webui.h"
#include "audiofile.h"
#include "genres_model.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <new>
#include <set>

using namespace rbm;
namespace web = rbm::webui;

namespace {  // file-local: another mod has types of the same names

const Addr A_dbHolder{"SingletonHolder<DatabaseIF>", [] { return singleton("__ZN11DetailColorC2Ev", "__ZN4juce15SingletonHolderIN2db10DatabaseIFENS_15CriticalSectionELb0EE3getEv"); }};
const Addr A_dbGet{"__ZN4juce15SingletonHolderIN2db10DatabaseIFENS_15CriticalSectionELb0EE3getEv"};
const Addr A_isCollectionSong{"__ZN2db10DatabaseIF16isCollectionSongERKN4juce6StringERNS_12RowDataTrackE"};
const Addr A_rowCtor{"__ZN2db12RowDataTrackC2Ev"};
const Addr A_rowDtor{"__ZN2db12RowDataTrackD2Ev"};
const size_t S_rowDataTrack = 0x4e0;
const Offset O_rowGenre{0x180, "__ZN2db15TrackListSorter12compareGenreEPNS_12RowDataTrackES2_", "add x1", 0};  // juce::String
// The Genre field, as rekordbox's genre editor (browse::GenreModifier::modifyGenre) writes it: ColName 16 is Genre.
// Also for a Spotify track's collection row: the path stores the field as given (only title, artist and the like are
// encrypted for Spotify rows, by modifySpotifyTrackData), skips the file tag (not an absolute path), and rekordbox's
// Spotify metadata refresh (SpotifyMetadataRefreshAsyncLoader) rewrites other fields, not the Genre.
const Addr A_modifyTrackData{"__ZN2db10DatabaseIF15modifyTrackDataERNS_12RowDataTrackENS_14ListDataCommon7ColNameERKN4juce6StringEbb"};
const int kColGenre = 16;
const float kSure = 0.2f;  // a guess under this isn't ticked (nor written on analysis)
const int kForTrackPatches = 64;  // genres.forTrack listens to at most this many 2 s patches: one model batch
const char* const kNoReader = "Needs the spotifyaudio mod";
typedef bool (*SpotifyReadFn)(const char* uri, std::shared_ptr<const std::vector<int16_t>>* stereo, int* rate, std::string* why);

volatile bool g_on;
void* database() { return at<void* (*)(uintptr_t)>(A_dbGet)(A_dbHolder + g_slide); }
bool isSpotifyUri(const std::string& uri) { return !uri.compare(0, 8, "spotify:"); }

std::unordered_map<std::string, std::string> g_field;  // Spotify URI -> its collection copy's Genre ("": none), once looked up (main thread)
double g_nextFill;

// A collection track's Genre field (main thread); false if the path isn't in the collection.
bool readGenre(const std::string& path, std::string* genre) {
    alignas(16) char row[S_rowDataTrack + 0x100];
    at<void (*)(void*)>(A_rowCtor)(row);
    JStr p(path.c_str());
    bool in = at<bool (*)(void*, const void*, void*)>(A_isCollectionSong)(database(), &p, row);
    if (in) {
        const char* g = *(const char* const*)(row + O_rowGenre);
        *genre = g ? g : "";
    }
    at<void (*)(void*)>(A_rowDtor)(row);
    return in;
}

bool writeGenre(const std::string& path, const std::string& genre) {  // main thread
    void* db = database();
    alignas(16) char row[S_rowDataTrack + 0x100];
    at<void (*)(void*)>(A_rowCtor)(row);
    JStr p(path.c_str());
    bool ok = at<bool (*)(void*, const void*, void*)>(A_isCollectionSong)(db, &p, row);
    if (ok) {
        JStr g(genre.c_str());
        at<void (*)(void*, void*, int, const void*, bool, bool)>(A_modifyTrackData)(db, row, kColGenre, &g, false, true);
        if (isSpotifyUri(path)) g_field[path] = genre, g_nextFill = 0;
    }
    at<void (*)(void*)>(A_rowDtor)(row);
    return ok;
}

// rekordbox shows a Spotify playlist's rows as Spotify lists them, with the BPM and key of the collection's copy but
// not its Genre: the column stays empty for a genre written here (the only way one gets there: the field can't be
// typed in on a streaming row). So the lists' Spotify rows get the collection's Genre from here: those of tracks this
// mod has listened to (only they can have one), looked up once (a few hundred rows per pass). Main thread.
void fillLists() {
    int lookups = 400;
    forEachListViewer([&](void* comp, void* lv) {
        bool changed = false;
        for (int i = 0, n = listRows(lv); i < n; i++) {
            const char* row = listRow(lv, i);
            const char* p = row ? trackPath(row) : nullptr;
            if (!p || strncmp(p, "spotify:", 8)) continue;
            auto it = g_field.find(p);
            if (it == g_field.end()) {
                if (lookups <= 0) continue;
                lookups--;
                std::string g;
                if (library::genre(p, nullptr)) readGenre(p, &g);
                it = g_field.emplace(p, g).first;
            }
            JStr* shown = (JStr*)(row + O_rowGenre);  // a juce::String
            if (it->second == (shown->text ? shown->text : "")) continue;
            shown->~JStr();
            new (shown) JStr(it->second.c_str());
            changed = true;
        }
        if (changed) repaintComponent(comp);
    });
}

// The Genre field from any thread: rekordbox's database is read on the main thread, so another thread asks it there
// and waits 2 s at most (false then, as if the track had none: the main thread may be waiting for the caller).
bool readGenreAnyThread(const std::string& uri, std::string* genre) {
    if (plat::isMainThread()) return readGenre(uri, genre);
    struct Answer { std::mutex m; std::condition_variable cv; bool done = false, in = false; std::string genre; };
    auto a = std::make_shared<Answer>();
    plat::onMain([a, uri] {
        std::string g;
        bool in = readGenre(uri, &g);
        std::lock_guard<std::mutex> l(a->m);
        a->in = in, a->genre = g, a->done = true;
        a->cv.notify_all();
    });
    std::unique_lock<std::mutex> l(a->m);
    if (!a->cv.wait_for(l, std::chrono::seconds(2), [&] { return a->done; })) return false;
    *genre = a->genre;
    return a->in;
}

bool broadDetail() { return cfgStr("genres_detail", "style") == "broad"; }

// The suggestions for a style at the chosen detail, best first, each with its score (a broad genre adds up the
// scores of its styles among the three).
std::vector<std::pair<std::string, float>> options(const genres::Style& s, bool broad) {
    std::vector<std::pair<std::string, float>> o;
    for (const genres::Guess& g : s.top) {
        const std::string& name = broad ? g.broad : g.style;
        if (name.empty()) continue;
        auto it = std::find_if(o.begin(), o.end(), [&](auto& x) { return x.first == name; });
        if (it == o.end()) o.push_back({name, g.score});
        else it->second = std::min(1.f, it->second + g.score);
    }
    std::stable_sort(o.begin(), o.end(), [](auto& a, auto& b) { return a.second > b.second; });
    return o;
}

// ---- what the model found, kept in the library -----------------------------------------------

bool stored(const std::string& uri, genres::Style* out) {
    library::TrackGenre g;
    if (!library::genre(uri, &g) || g.top[0].style.empty()) return false;
    genres::Style st;
    for (int k = 0; k < 3; k++) st.top[k] = {g.top[k].style, g.top[k].broad, (float)g.top[k].score};
    for (const genres::Guess& x : st.top)  // (the model's own pick looks at all 400 styles; close enough from the three)
        if (st.profile.empty()) st.profile = genres::family(x.style);
    *out = st;
    return true;
}

void store(const std::string& uri, const genres::Style& st) {
    library::TrackGenre g;
    g.uri = uri;
    for (int k = 0; k < 3; k++) g.top[k] = {st.top[k].style, st.top[k].broad, st.top[k].score};
    library::putGenre(g);
}

std::mutex g_detectLock;
std::condition_variable g_detectDone;
std::set<std::string> g_detecting;  // tracks the model listens to right now

// A track's styles, worked out once: run() listens to it unless a result is stored, and whoever asks for a track
// that is being listened to waits for that result instead of running the model again. A new result is stored.
bool detectOnce(const std::string& uri, genres::Style* out, std::string* why, const std::function<bool(genres::Style*, std::string*)>& run) {
    {
        std::unique_lock<std::mutex> l(g_detectLock);
        g_detectDone.wait(l, [&] { return !g_detecting.count(uri); });
        g_detecting.insert(uri);
    }
    bool ok = stored(uri, out);
    if (!ok && (ok = run(out, why))) store(uri, *out);
    {
        std::lock_guard<std::mutex> l(g_detectLock);
        g_detecting.erase(uri);
    }
    g_detectDone.notify_all();
    return ok;
}

// The file to read a track's audio from, now: a local file itself; a Spotify track its local file where a mod keeps one
// (asked right before reading: the file may be replaced later), else its URI, for its stream (the spotifyaudio mod
// reads it into memory). "" and why when there's none.
std::string audioFile(const std::string& uri, std::string* why) {
    if (!isSpotifyUri(uri)) return uri;
    std::string f;
    if (trackFile(uri.c_str(), &f) && !f.empty()) return f;
    if (provided("spotifyaudio.read")) return uri;
    *why = trackFilesOn() ? trackFilesText("genres.state") : kNoReader;
    return "";
}
// Whether that why means the track waits for audio to read (its local file, or the spotifyaudio mod).
bool waitsForAudio(const std::string& why) { return why == kNoReader || (!why.empty() && why == trackFilesText("genres.state")); }
// Reads what audioFile gave, as mono floats: a file, or a Spotify track's stream into memory.
bool readMono(const std::string& uri, const std::string& file, std::vector<float>* mono, int* rate) {
    if (!(isSpotifyUri(uri) && file == uri)) return audiofile::readMono(file, mono, rate);
    auto read = service<SpotifyReadFn>("spotifyaudio.read");
    std::shared_ptr<const std::vector<int16_t>> pcm;
    std::string why;
    if (!read || !read(uri.c_str(), &pcm, rate, &why)) return false;
    mono->resize(pcm->size() / 2);
    for (size_t i = 0; i < mono->size(); i++) (*mono)[i] = ((*pcm)[2 * i] + (*pcm)[2 * i + 1]) / 65536.f;
    return !mono->empty();
}

bool detectTrack(const std::string& uri, genres::Style* out, std::string* why) {
    std::string file = audioFile(uri, why);
    if (file.empty()) return false;
    std::vector<float> mono;
    int rate = 0;
    if (!readMono(uri, file, &mono, &rate)) return *why = "Couldn't read the audio", false;
    if (!genres::detect(mono, rate, out)) return *why = genres::modelAvailable() ? "Too short" : "No style model", false;
    return true;
}

// ---- the window ----------------------------------------------------------------------------

struct Row {
    std::string uri, title, artist, genre;  // genre: the Genre field now
    std::string state;  // "" not listened to yet, "Listening", or why there's no guess
    bool done = false, has = false, tick = false, touched = false;
    bool waiting = false;  // a Spotify track without audio to read: looked at again as local files come in
    bool outside = false;  // a Spotify track that isn't in the collection (no Genre field)
    bool notIn = false;    // not in the collection (any track): its state says so, which is no failure
    genres::Style style;
    std::string choice;  // the suggestion picked in the dropdown ("" = the best one)
};
struct Undo { std::string uri, old; };

web::Window* g_win;
std::mutex g_lock;  // g_rows, g_status, g_listening, g_wantDetect
std::vector<Row> g_rows;
std::vector<Undo> g_undo;
std::string g_playlistName, g_status;
bool g_replace, g_listening;  // g_listening: Detect genres is running
bool g_wantDetect;  // Detect genres was clicked: tracks readable later are listened to as they come in
std::atomic<bool> g_stopListening{false};  // Stop: the run ends after the track it is on
std::atomic<int> g_generation{0};
std::atomic<bool> g_dirty{false};
std::string g_playlistId;  // main thread
double g_nextPoll;          // main thread: when the window's rows are looked at again

// What Apply would write for a row ("" nothing), and how sure the model is of it (g_lock held).
std::string suggestion(const Row& r, float* score = nullptr) {
    if (!r.has) return "";
    auto o = options(r.style, broadDetail());
    if (o.empty()) return "";
    auto it = std::find_if(o.begin(), o.end(), [&](auto& x) { return x.first == r.choice; });
    if (it == o.end()) it = o.begin();
    if (score) *score = it->second;
    return it->first;
}
bool writes(const Row& r) {
    std::string s = suggestion(r);
    return r.tick && r.done && !s.empty() && s != r.genre && (r.genre.empty() || g_replace);
}
void autoTick(Row& r) {  // until the user ticks it: sure enough, and it would change an empty field (or Replace)
    if (r.touched) return;
    float score = 0;
    std::string s = suggestion(r, &score);
    r.tick = r.done && !s.empty() && score >= kSure && s != r.genre && (r.genre.empty() || g_replace);
}

std::string plural(int n, const char* one, const char* more) { return std::to_string(n) + (n == 1 ? one : more); }

void sendState() {
    if (!g_win || !web::visible(g_win)) return;
    using web::jsonStr;
    auto b = [](bool x) { return x ? "true" : "false"; };
    bool broad = broadDetail();
    bool canFetch = canFetchTrackFiles() && !g_playlistId.empty();
    bool filesOn = trackFilesOn();
    std::lock_guard<std::mutex> l(g_lock);
    int done = 0, toWrite = 0, withGenre = 0, waiting = 0, outside = 0;
    std::string rows = "[";
    for (size_t i = 0; i < g_rows.size(); i++) {
        Row& r = g_rows[i];
        autoTick(r);
        if (r.done) done++;
        if (writes(r)) toWrite++;
        if (!r.genre.empty()) withGenre++;
        if (r.waiting) waiting++;
        if (r.outside) outside++;
        float score = 0;
        std::string sug = suggestion(r, &score), opts = "[";
        for (auto& o : r.has ? options(r.style, broad) : std::vector<std::pair<std::string, float>>{}) {
            char sc[16];
            snprintf(sc, sizeof sc, "%.2f", o.second);
            opts += std::string(opts.size() > 1 ? "," : "") + "{\"n\":" + jsonStr(o.first) + ",\"s\":" + sc + "}";
        }
        opts += "]";
        char sc[16];
        snprintf(sc, sizeof sc, "%.2f", score);
        rows += std::string(i ? "," : "") + "{\"i\":" + std::to_string(i) + ",\"title\":" + jsonStr(r.title) + ",\"artist\":" + jsonStr(r.artist) +
                ",\"genre\":" + jsonStr(r.genre) + ",\"state\":" + jsonStr(r.state) + ",\"done\":" + b(r.done) + ",\"options\":" + opts +
                ",\"choice\":" + jsonStr(sug) + ",\"score\":" + sc + ",\"tick\":" + b(r.tick) + ",\"writes\":" + b(writes(r)) +
                ",\"waiting\":" + b(r.waiting) + ",\"notIn\":" + b(r.notIn) + "}";
    }
    rows += "]";
    // What keeps Spotify tracks from a genre, most important first.
    std::string note;
    if (waiting && !filesOn)
        note = "Spotify tracks are read from their stream by the spotifyaudio mod, which is switched off: remove spotifyaudio=0 from the config and restart rekordbox.";
    else if (waiting)  // (waiting for their local files: the words of the mod that keeps them)
        note = trackFilesText("genres.note", waiting);
    else if (outside)
        note = plural(outside, " Spotify track isn't", " Spotify tracks aren't") +
               " in rekordbox's collection yet, so there's no Genre field to fill: analyse the playlist first (Open analysis window).";
    bool waitingForFiles = g_wantDetect && !g_listening && waiting && filesOn;
    std::string j = "{\"name\":" + jsonStr(g_playlistName) + ",\"rows\":" + rows + ",\"status\":" + jsonStr(g_status);
    j += ",\"detail\":" + jsonStr(broad ? "broad" : "style") + ",\"replace\":" + b(g_replace) + ",\"onAnalyse\":" + b(cfgInt("genres_on_analyse", 0) != 0);
    j += std::string(",\"onAnalyseOk\":") + b(!versionOff("analysis")) + ",\"canUndo\":" + b(!g_undo.empty());
    j += ",\"done\":" + std::to_string(done) + ",\"total\":" + std::to_string(g_rows.size()) + ",\"listening\":" + b(g_listening) + ",\"toWrite\":" + std::to_string(toWrite) + ",\"withGenre\":" + std::to_string(withGenre);
    j += ",\"waiting\":" + std::to_string(waiting) + ",\"canFetch\":" + b(waiting && canFetch && filesOn) + ",\"waitingForFiles\":" + b(waitingForFiles) + ",\"spotifyNote\":" + jsonStr(note);
    if (filesOn)  // (the words of the mod that keeps local files)
        j += ",\"fetchLabel\":" + jsonStr(trackFilesText("button")) + ",\"fetchTip\":" + jsonStr(trackFilesText("buttonTip")) +
             ",\"waitingText\":" + jsonStr(trackFilesText("genres.waiting", waiting)) + ",\"missingState\":" + jsonStr(trackFilesText("genres.state")) +
             ",\"missingTip\":" + jsonStr(trackFilesText("genres.tip"));
    j += "}";
    web::post(g_win, j);
}

// "Detect genres": listens to the tracks not done yet, one at a time, until the playlist changes. Rows that become
// ready meanwhile (a local file came since) are taken too.
void startReading(int generation) {
    {
        std::lock_guard<std::mutex> l(g_lock);
        g_wantDetect = true, g_dirty = true;
        if (g_listening) return;
        g_listening = true, g_stopListening = false;
    }
    plat::background([generation] {
        plat::lowPriorityThread();
        for (;;) {
            std::string uri;
            size_t i = 0;
            {
                std::lock_guard<std::mutex> l(g_lock);
                if (generation != g_generation || g_stopListening) { g_listening = false, g_dirty = true; return; }
                while (i < g_rows.size() && g_rows[i].done) i++;
                if (i >= g_rows.size()) { g_listening = false, g_dirty = true; return; }
                uri = g_rows[i].uri;
                g_rows[i].state = "Listening";
                g_dirty = true;
            }
            genres::Style st;
            std::string why;
            bool ok = detectOnce(uri, &st, &why, [&](genres::Style* out, std::string* w) { return detectTrack(uri, out, w); });
            std::lock_guard<std::mutex> l(g_lock);
            // (the rows are another playlist's by now: a window opened on it lists it half a second later)
            if (generation != g_generation || i >= g_rows.size() || g_rows[i].uri != uri) { g_listening = false, g_dirty = true; return; }
            Row& r = g_rows[i];
            if (g_stopListening) {  // stopped while listening: this one stays undone
                r.state.clear(), g_listening = false, g_dirty = true;
                return;
            }
            r.style = st, r.has = ok, r.done = true, r.state = ok ? "" : why;
            r.waiting = !ok && waitsForAudio(why);  // (its local file went meanwhile)
            g_dirty = true;
        }
    });
}

// A playlist track as a row: its collection entry and Genre, a result found before, or why it can't be listened to (main thread).
Row makeRow(const std::string& uri) {
    Row r;
    r.uri = uri;
    bool spotify = isSpotifyUri(uri);
    CollectionTrack t;
    if (readGenre(uri, &r.genre) && collectionTrack(uri, &t)) r.title = t.title, r.artist = t.artist;
    else r.state = spotify ? "Not in the collection: analyse the playlist first" : "Not in the collection", r.done = r.notIn = true, r.outside = spotify;
    if (r.title.empty()) {
        library::Track lt;
        if (library::track(uri, &lt)) r.title = lt.title, r.artist = lt.artist;
    }
    if (r.title.empty()) r.title = uri.substr(uri.find_last_of("/\\") + 1);
    if (r.done) return r;
    if (stored(uri, &r.style)) r.has = r.done = true;  // listened to before
    else if (spotify) {
        std::string why;
        if (audioFile(uri, &why).empty()) r.state = why, r.done = r.waiting = true;
    }
    return r;
}

// The window's rows again (main thread, every 3 s while it shows): tracks rekordbox has listed since come in, and
// Spotify tracks waiting for a local file are taken once they have one.
void refreshRows() {
    int generation = g_generation;
    PlaylistTracks pt = playlistTracks(g_playlistId);
    std::vector<std::string> added, waiting;
    {
        std::lock_guard<std::mutex> l(g_lock);
        std::map<std::string, int> have;
        for (const Row& r : g_rows) {
            have[r.uri]++;
            if (r.waiting) waiting.push_back(r.uri);
        }
        for (const std::string& uri : pt.uris) {
            auto it = have.find(uri);
            if (it != have.end() && it->second > 0) it->second--;
            else added.push_back(uri);
        }
    }
    std::vector<Row> made, ready;  // (outside the lock: the database calls take a while on big lists)
    for (const std::string& uri : added) made.push_back(makeRow(uri));
    std::string file;
    for (const std::string& uri : waiting)
        if (trackFile(uri.c_str(), &file)) ready.push_back(makeRow(uri));
    bool read;
    {
        std::lock_guard<std::mutex> l(g_lock);
        if (generation != g_generation) return;
        for (Row& m : ready)
            for (Row& r : g_rows)
                if (r.uri == m.uri && r.waiting) {
                    if (!m.waiting) r = std::move(m), g_dirty = true;
                    break;
                }
        if (!made.empty()) g_dirty = true;
        for (Row& m : made) g_rows.push_back(std::move(m));
        std::string status = pt.known ? (g_rows.empty() ? "No tracks in this playlist" : "") : "rekordbox hasn't listed this playlist yet";
        bool listing = g_status.empty() || g_status == "No tracks in this playlist" || g_status == "rekordbox hasn't listed this playlist yet";
        if (listing && status != g_status) g_status = status, g_dirty = true;
        read = g_wantDetect && !g_listening && std::any_of(g_rows.begin(), g_rows.end(), [](const Row& r) { return !r.done; });
    }
    if (read) startReading(generation);
}

void openPlaylistId(const std::string& id, const std::string& name) {
    ++g_generation;
    g_playlistId = id;
    g_nextPoll = now() + 0.5;  // rekordbox lists a playlist once it's shown
    std::lock_guard<std::mutex> l(g_lock);
    g_rows.clear();
    g_playlistName = name;
    g_status.clear();
    g_wantDetect = false;
    g_dirty = true;
}

void apply() {
    std::lock_guard<std::mutex> l(g_lock);
    g_undo.clear();
    int n = 0;
    for (Row& r : g_rows) {
        if (!writes(r)) continue;
        std::string s = suggestion(r);
        if (!writeGenre(r.uri, s)) continue;
        g_undo.push_back({r.uri, r.genre});
        r.genre = s, n++;
    }
    g_status = "Wrote " + std::to_string(n) + " genre" + (n == 1 ? "" : "s") + ".";
    logf("genres: %s", g_status.c_str());
    g_dirty = true;
}

// Empties the Genre field of every track in the window (the page asks first); Undo puts them back. What the model
// found stays in the library.
void removeAll() {
    std::lock_guard<std::mutex> l(g_lock);
    g_undo.clear();
    int n = 0;
    for (Row& r : g_rows) {
        if (r.genre.empty() || !writeGenre(r.uri, "")) continue;
        g_undo.push_back({r.uri, r.genre});
        r.genre.clear(), n++;
    }
    g_status = "Removed " + std::to_string(n) + " genre" + (n == 1 ? "" : "s") + ".";
    logf("genres: %s", g_status.c_str());
    g_dirty = true;
}

void undo() {
    std::lock_guard<std::mutex> l(g_lock);
    for (const Undo& u : g_undo)
        if (writeGenre(u.uri, u.old))
            for (Row& r : g_rows)
                if (r.uri == u.uri) r.genre = u.old;
    g_status = "Undone.";
    g_undo.clear();
    g_dirty = true;
}

// The mod that keeps local files of Spotify tracks fetches the playlist's (and shows how far it is, as for the stems window).
void fetchFiles() {
    if (!g_playlistId.empty()) fetchTrackFiles(g_playlistId.c_str());
}

void openPlaylist(void* treeItem) {
    showPlaylist(treeItem);
    openPlaylistId(treeItemId(treeItem), treeItemName(treeItem));
}

void onMessage(const std::string& m) {
    std::string cmd = web::jsonGet(m, "cmd");
    auto on = [&] { return web::jsonGet(m, "on") == "true"; };
    if (cmd == "ready") g_dirty = true;
    else if (cmd == "detect") startReading(g_generation);
    else if (cmd == "stop") {
        std::lock_guard<std::mutex> l(g_lock);
        g_stopListening = true, g_wantDetect = false, g_dirty = true;
    } else if (cmd == "fetch") fetchFiles();
    else if (cmd == "detail") {
        setCfg("genres_detail", web::jsonGet(m, "value") == "broad" ? "broad" : "style");
        std::lock_guard<std::mutex> l(g_lock);
        for (Row& r : g_rows) r.choice.clear();
        g_dirty = true;
    } else if (cmd == "replace") { std::lock_guard<std::mutex> l(g_lock); g_replace = on(); g_dirty = true; }
    else if (cmd == "onAnalyse") { setCfg("genres_on_analyse", on() ? "1" : "0"); g_dirty = true; }
    else if (cmd == "tick" || cmd == "choice") {
        std::lock_guard<std::mutex> l(g_lock);
        size_t i = (size_t)atoi(web::jsonGet(m, "i").c_str());
        if (i < g_rows.size()) {
            if (cmd == "tick") g_rows[i].tick = on(), g_rows[i].touched = true;
            else g_rows[i].choice = web::jsonGet(m, "value"), g_rows[i].touched = false;  // a new pick is ticked again if sure
        }
        g_dirty = true;
    } else if (cmd == "all") {
        std::lock_guard<std::mutex> l(g_lock);
        for (Row& r : g_rows)
            if (r.done && r.has) r.tick = on(), r.touched = true;
        g_dirty = true;
    } else if (cmd == "apply") apply();
    else if (cmd == "undo") undo();
    else if (cmd == "removeAll") removeAll();
    else if (cmd == "close") web::close(g_win);
}

// treeItem: a playlist of the tree; null: `selection`, a few tracks as a playlist (core's selectionPlaylist), whose
// rows, Detect, Apply and the fetch button's playlist are then just those tracks.
void showWindow(void* treeItem, const std::string& selection = "") {
    g_win = web::open("genres", "genres", "Genres", 820, 600, onMessage);
    if (treeItem) openPlaylist(treeItem);
    else openPlaylistId(selection, playlistName(selection));
    g_dirty = true;
}

// On every playlist, Spotify ones too, as the other mods' windows (the window says what a Spotify track still needs).
void playlistMenu(Menu* menu, void* treeItem) {
    menuAddItem(menu, "Open genres window", [treeItem] { showWindow(treeItem); });
}

// The track list's menu: the same window on the selected tracks.
void trackMenu(Menu* menu, const std::vector<std::string>& uris, const std::string& selection) {
    menuAddItem(menu, menuWindowItem("genres window", uris.size()).c_str(), [selection] { showWindow(nullptr, selection); });
}

// ---- fill on analysis ----------------------------------------------------------------------

std::mutex g_qLock;
std::deque<std::string> g_queue;
std::set<std::string> g_pending;  // tracks on their way to the queue or in it: each once at a time
std::map<std::string, double> g_stageAt;  // of those: when rekordbox's analysis last reached a stage
bool g_working;

// Listens to the queued tracks (unless the model's result is stored: bettergrid may have asked during the analysis)
// and fills an empty Genre field with the best guess if it's sure enough.
void fillQueued() {
    plat::lowPriorityThread();
    for (;;) {
        std::string uri;
        {
            std::lock_guard<std::mutex> l(g_qLock);
            if (g_queue.empty()) { g_working = false; return; }
            uri = g_queue.front();
            g_queue.pop_front();
        }
        {
            std::lock_guard<std::mutex> l(g_qLock);
            g_pending.erase(uri), g_stageAt.erase(uri);
        }
        genres::Style st;
        std::string why;
        if (!detectOnce(uri, &st, &why, [&](genres::Style* out, std::string* w) { return detectTrack(uri, out, w); })) {
            if (isSpotifyUri(uri)) logf("genres: %s not filled on analysis: %s", uri.c_str(), why.c_str());
            continue;
        }
        auto o = options(st, broadDetail());
        if (o.empty() || o[0].second < kSure) continue;  // unsure: left for the window
        std::string genre = o[0].first;
        plat::onMain([uri, genre] {
            std::string field;
            CollectionTrack t;
            if (readGenre(uri, &field) && field.empty() && collectionTrack(uri, &t) && writeGenre(uri, genre))
                logf("genres: '%s' -> %s (on analysis)", t.title.c_str(), genre.c_str());
        });
    }
}

// Once rekordbox has saved the track's analysis (so the two don't write the track at once; a style bettergrid asked
// for during the analysis is stored by then): an
// empty Genre field is queued to be filled. Saved: the collection calls the track analysed, and its analysis hasn't
// reached a new stage for kQuiet (a track that is analysed again counts as analysed all along, and no stage is
// always the last one). Looks every 5 s for up to 10 minutes (main thread).
const double kQuiet = 8;  // s
void fillWhenAnalysed(const std::string& uri, int attempt) {
    CollectionTrack t;
    std::string field;
    bool fill = collectionTrack(uri, &t) && readGenre(uri, &field) && field.empty();  // else filled (by hand, or it never was empty)
    std::unique_lock<std::mutex> l(g_qLock);
    auto at = g_stageAt.find(uri);
    bool busy = !t.analysed || (at != g_stageAt.end() && now() - at->second < kQuiet);
    if (fill && busy && attempt < 120) {
        l.unlock();
        plat::onMainAfter(5, [uri, attempt] { fillWhenAnalysed(uri, attempt + 1); });
        return;
    }
    if (!fill) {
        g_pending.erase(uri), g_stageAt.erase(uri);
        return;
    }
    g_queue.push_back(uri);
    if (g_working) return;
    g_working = true;
    plat::background(fillQueued);
}

// "genres.analysing" (core's analysis hooks, on the analysis thread): rekordbox's analysis of this track reached a
// stage.
void analysing(const char* p) {
    if (!p || !*p || !cfgInt("genres_on_analyse", 0)) return;
    std::string uri = p;
    if (!isSpotifyUri(uri) && uri.find("://") != std::string::npos) return;  // other streaming services: nothing of theirs to listen to
    {
        std::lock_guard<std::mutex> l(g_qLock);
        g_stageAt[uri] = now();
        if (!g_pending.insert(uri).second) return;
    }
    plat::onMain([uri] { plat::onMainAfter(kQuiet, [uri] { fillWhenAnalysed(uri, 0); }); });
}

// ---- for other mods ------------------------------------------------------------------------

bool provideDetect(const std::vector<float>* mono, int rate, genres::Style* out) { return mono && genres::detect(*mono, rate, out); }

// "genres.forTrack" (genres.h): the Genre field (what the user typed, or what this mod wrote), else the stored style,
// else the model on the caller's audio (bounded to one batch of patches, on the calling thread), stored for next time.
// A model guess under kSure is no answer: callers act on it (bettergrid picks a tempo octave by it).
bool provideForTrack(const char* uri, const std::vector<float>* mono, int rate, std::string* genre) {
    if (!uri || !*uri || !genre) return false;
    std::string field;
    if (readGenreAnyThread(uri, &field) && !field.empty()) return *genre = field, true;
    genres::Style st;
    if (stored(uri, &st)) {
        if (st.top[0].score < kSure) return false;
        return *genre = st.top[0].style, true;
    }
    if (!mono || rate <= 0 || !genres::modelAvailable()) return false;
    std::string why;
    bool ran = false;
    double t0 = now();
    bool ok = detectOnce(uri, &st, &why, [&](genres::Style* out, std::string* w) {
        ran = true;
        if (genres::detect(*mono, rate, out, kForTrackPatches)) return true;
        *w = "too short";
        return false;
    });
    if (!ok) return false;
    if (ran) logf("genres: %s sounds like %s (%.0f%%, %.1fs)", uri, st.top[0].style.c_str(), st.top[0].score * 100, now() - t0);
    if (st.top[0].score < kSure) return false;
    *genre = st.top[0].style;
    return true;
}

// "genres.known" (genres.h): what is known without listening, for a window's Genre column (main thread).
bool provideKnown(const char* uri, std::string* genre, bool* written) {
    if (!uri || !*uri || !genre) return false;
    std::string field;
    if (readGenre(uri, &field) && !field.empty()) {
        if (written) *written = true;
        return *genre = field, true;
    }
    genres::Style st;
    if (!stored(uri, &st) || st.top[0].score < kSure) return false;
    if (written) *written = false;
    return *genre = st.top[0].style, true;
}

void tick() {
    if (now() >= g_nextFill) g_nextFill = now() + 0.5, fillLists();
    if (g_win && !g_playlistId.empty() && web::visible(g_win) && now() >= g_nextPoll) {
        g_nextPoll = now() + 3;
        refreshRows();
    }
    if (g_dirty.exchange(false)) sendState();
    // Test commands in <configDir>/command-genres: "detect <audio file>" logs the model's three best guesses;
    // "fortrack <path or spotify: URI>" asks genres.forTrack off the main thread (with the audio, when there's a file);
    // "open" opens the window on the playlist shown in the browser, "open <playlist id>" on that playlist or selection of
    // tracks ("selection:<n>", see selectionPlaylist in rbmod.h); "msg <json>" is as if the page had sent it.
    std::string c = devCommand("genres");
    if (c == "open" || !c.compare(0, 5, "open ")) {
        bool given = c.size() > 5;
        std::string id = given ? c.substr(5) : shownPlaylist();
        if (id.empty()) return logf("genres: test open: no playlist shown");
        if (given) showPlaylistId(id);
        g_win = web::open("genres", "genres", "Genres", 820, 600, onMessage);
        openPlaylistId(id, given ? playlistName(id) : id);
        logf("genres: test open: playlist %s", id.c_str());
    } else if (!c.compare(0, 4, "msg ")) {
        onMessage(c.substr(4));
    } else if (c == "state") {  // what the window shows, in short
        std::lock_guard<std::mutex> l(g_lock);
        int done = 0, has = 0, waiting = 0, outside = 0, ticked = 0;
        for (const Row& r : g_rows) done += r.done, has += r.has, waiting += r.waiting, outside += r.outside, ticked += r.tick;
        logf("genres: test state: %d rows, %d listened to (%d with a guess, %d ticked), %d waiting for audio, %d outside "
             "the collection; listening %d; status '%s'", (int)g_rows.size(), done, has, ticked, waiting, outside, (int)g_listening,
             g_status.c_str());
        int n = 0;
        for (const Row& r : g_rows) {
            if (!r.done || n++ >= 8) continue;
            float sc = 0;
            std::string sug = r.has ? suggestion(r, &sc) : "";
            logf("genres: test   '%s' (Genre '%s'): %s %s%s", r.title.c_str(), r.genre.c_str(), r.has ? sug.c_str() : r.state.c_str(),
                 r.has ? std::to_string((int)(sc * 100)).c_str() : "", r.has ? "%" : "");
        }
    } else if (!c.compare(0, 7, "detect ")) {
        std::string path = c.substr(7);
        plat::background([path] {
            genres::Style st;
            std::string why;
            double t0 = now();
            if (!detectTrack(path, &st, &why)) return logf("genres: test %s: %s", path.c_str(), why.c_str());
            std::string list;
            for (auto& [g, p] : options(st, broadDetail())) {
                char b[16];
                snprintf(b, sizeof b, " (%.2f)", p);
                list += " " + g + b;
            }
            logf("genres: test %s in %.1fs:%s", path.c_str(), now() - t0, list.c_str());
        });
    } else if (!c.compare(0, 9, "fortrack ")) {
        std::string uri = c.substr(9);
        plat::background([uri] {
            std::string why, file = audioFile(uri, &why), genre;
            std::vector<float> mono;
            int rate = 0;
            bool audio = !file.empty() && readMono(uri, file, &mono, &rate);
            double t0 = now();
            bool ok = provideForTrack(uri.c_str(), audio ? &mono : nullptr, rate, &genre);
            logf("genres: test forTrack %s: %s in %.1fs%s", uri.c_str(), ok ? genre.c_str() : "nothing known", now() - t0, audio ? "" : " (no audio)");
        });
    }
}

}  // namespace

RBM_PROVIDE("genres", "genres.detect", provideDetect, &g_on);
RBM_PROVIDE("genres", "genres.analysing", analysing, &g_on);
RBM_PROVIDE("genres", "genres.known", provideKnown, &g_on);
RBM_PROVIDE("genres", "genres.forTrack", provideForTrack, &g_on);
// The style model is loaded after the launch, off the main thread (after the other mods' models): the first track analysed (or loaded
// on a deck) then doesn't wait for it before its beat grid shows (bettergrid asks for the genre first; loading takes
// most of a second).
static void init() {
    startupTask([] {
        if (g_on && cfgInt("genres_warmup", 1)) genres::modelAvailable();
    });
}

RBM_MOD({.name = "genres", .init = init, .tick = tick, .playlistMenu = playlistMenu, .trackMenu = trackMenu, .enabled = &g_on, .menuOrder = 35});
