// library: sqlite database of tracks, playlists and genres (see library.h).
// One connection, opened on first use and guarded by a mutex; if it cannot be opened every function
// does nothing (or returns false / empty). Schema version is kept in PRAGMA user_version.
#include "library.h"
#include "rbmod.h"
#include <sqlite3.h>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <mutex>

namespace rbm::library {

static std::mutex g_mu;
static sqlite3* g_db;
static bool g_tried;

static const int kSchemaVersion = 5;

static bool exec(const char* sql) {
    char* err = nullptr;
    if (sqlite3_exec(g_db, sql, nullptr, nullptr, &err) != SQLITE_OK) {
        logf("library: %s: %s", sql, err ? err : "error");
        sqlite3_free(err);
        return false;
    }
    return true;
}

// Brings the schema up to kSchemaVersion. Later versions add their migration steps here.
static bool migrate() {
    int version = 0;
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(g_db, "PRAGMA user_version", -1, &st, nullptr) == SQLITE_OK && sqlite3_step(st) == SQLITE_ROW)
        version = sqlite3_column_int(st, 0);
    sqlite3_finalize(st);
    if (version > kSchemaVersion) {
        logf("library: database is version %d, newer than %d", version, kSchemaVersion);
        return false;
    }
    if (version < 1) {
        if (!exec("BEGIN IMMEDIATE")) return false;
        bool ok =
            exec("CREATE TABLE IF NOT EXISTS tracks(uri TEXT PRIMARY KEY, title TEXT NOT NULL DEFAULT '', "
                 "artist TEXT NOT NULL DEFAULT '', album TEXT NOT NULL DEFAULT '', duration REAL NOT NULL DEFAULT 0, "
                 "bpm REAL NOT NULL DEFAULT 0, \"key\" TEXT NOT NULL DEFAULT '', updated_at REAL NOT NULL DEFAULT 0)") &&
            exec("CREATE TABLE IF NOT EXISTS playlists(id TEXT PRIMARY KEY, name TEXT NOT NULL DEFAULT '', "
                 "updated_at REAL NOT NULL DEFAULT 0)") &&
            exec("CREATE TABLE IF NOT EXISTS playlist_tracks(playlist_id TEXT NOT NULL, pos INTEGER NOT NULL, "
                 "uri TEXT NOT NULL, PRIMARY KEY(playlist_id, pos))") &&
            exec("CREATE INDEX IF NOT EXISTS playlist_tracks_uri ON playlist_tracks(uri)") &&
            exec("PRAGMA user_version = 1");
        if (!ok) {
            sqlite3_exec(g_db, "ROLLBACK", nullptr, nullptr, nullptr);
            return false;
        }
        if (!exec("COMMIT")) return false;
    }
    if (version < 3) {
        // Version 2 added local_files for a folder scan; version 3 drops it again.
        if (!exec("BEGIN IMMEDIATE")) return false;
        bool ok =
            exec("DROP TABLE IF EXISTS local_files") &&
            exec("PRAGMA user_version = 3");
        if (!ok) {
            sqlite3_exec(g_db, "ROLLBACK", nullptr, nullptr, nullptr);
            return false;
        }
        if (!exec("COMMIT")) return false;
    }
    if (version < 5) {
        // Version 5: the genres mod's detected styles per track (a new table; nothing else changes).
        if (!exec("BEGIN IMMEDIATE")) return false;
        bool ok =
            exec("CREATE TABLE IF NOT EXISTS genres(uri TEXT PRIMARY KEY, "
                 "style1 TEXT NOT NULL DEFAULT '', broad1 TEXT NOT NULL DEFAULT '', score1 REAL NOT NULL DEFAULT 0, "
                 "style2 TEXT NOT NULL DEFAULT '', broad2 TEXT NOT NULL DEFAULT '', score2 REAL NOT NULL DEFAULT 0, "
                 "style3 TEXT NOT NULL DEFAULT '', broad3 TEXT NOT NULL DEFAULT '', score3 REAL NOT NULL DEFAULT 0, "
                 "detected_at REAL NOT NULL DEFAULT 0)") &&
            exec("PRAGMA user_version = 5");
        if (!ok) {
            sqlite3_exec(g_db, "ROLLBACK", nullptr, nullptr, nullptr);
            return false;
        }
        if (!exec("COMMIT")) return false;
    }
    return true;
}

// The connection, opened on first use (g_mu held), or null.
static sqlite3* db() {
    if (g_tried) return g_db;
    g_tried = true;
    std::string path;
    if (const char* o = getenv("RBMODS_LIBRARY_DB")) path = o;  // for tests
    if (path.empty()) {
        std::string dir = plat::configDir();
        std::error_code ec;
        std::filesystem::create_directories(plat::fsPath(dir), ec);
        path = dir + "/library.sqlite";
    }
    if (sqlite3_open_v2(path.c_str(), &g_db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX, nullptr) != SQLITE_OK) {
        logf("library: cannot open %s: %s", path.c_str(), g_db ? sqlite3_errmsg(g_db) : "out of memory");
        sqlite3_close(g_db);
        g_db = nullptr;
        return nullptr;
    }
    sqlite3_busy_timeout(g_db, 5000);
    sqlite3_exec(g_db, "PRAGMA journal_mode = WAL", nullptr, nullptr, nullptr);
    sqlite3_exec(g_db, "PRAGMA synchronous = NORMAL", nullptr, nullptr, nullptr);
    if (!migrate()) {
        sqlite3_close(g_db);
        g_db = nullptr;
    }
    return g_db;
}

// A prepared statement, finalized on scope exit.
struct Stmt {
    sqlite3_stmt* s = nullptr;
    explicit Stmt(const char* sql) {
        if (sqlite3_prepare_v2(g_db, sql, -1, &s, nullptr) != SQLITE_OK) {
            logf("library: prepare failed (%s): %s", sql, sqlite3_errmsg(g_db));
            s = nullptr;
        }
    }
    ~Stmt() { sqlite3_finalize(s); }
    explicit operator bool() const { return s != nullptr; }
    void text(int i, const std::string& v) { sqlite3_bind_text(s, i, v.data(), (int)v.size(), SQLITE_TRANSIENT); }
    void num(int i, double v) { sqlite3_bind_double(s, i, v); }
    void integer(int i, long long v) { sqlite3_bind_int64(s, i, v); }
    bool row() { return sqlite3_step(s) == SQLITE_ROW; }
    bool done() {
        int r = sqlite3_step(s);
        if (r != SQLITE_DONE && r != SQLITE_ROW) {
            logf("library: %s", sqlite3_errmsg(g_db));
            return false;
        }
        return true;
    }
    std::string str(int i) {
        const char* t = (const char*)sqlite3_column_text(s, i);
        return t ? t : "";
    }
    double dbl(int i) { return sqlite3_column_double(s, i); }
    long long i64(int i) { return sqlite3_column_int64(s, i); }
};

// Runs fn inside a transaction (g_mu held); rolls back if it returns false.
template <class F>
static bool transaction(F fn) {
    if (!exec("BEGIN IMMEDIATE")) return false;
    if (fn() && exec("COMMIT")) return true;
    sqlite3_exec(g_db, "ROLLBACK", nullptr, nullptr, nullptr);
    return false;
}

static bool readTrack(const std::string& uri, Track* out) {
    Stmt q("SELECT title, artist, album, duration, bpm, \"key\", updated_at FROM tracks WHERE uri = ?");
    if (!q) return false;
    q.text(1, uri);
    if (!q.row()) return false;
    out->uri = uri;
    out->title = q.str(0);
    out->artist = q.str(1);
    out->album = q.str(2);
    out->duration = q.dbl(3);
    out->bpm = q.dbl(4);
    out->key = q.str(5);
    out->updatedAt = q.dbl(6);
    return true;
}

// Merges t into the stored track (g_mu held, inside a transaction).
static bool upsertTrack(const Track& t) {
    if (t.uri.empty()) return true;
    Track cur;
    bool changed = !readTrack(t.uri, &cur);
    cur.uri = t.uri;
    auto setS = [&](std::string& dst, const std::string& v) {
        if (!v.empty() && dst != v) dst = v, changed = true;
    };
    auto setN = [&](double& dst, double v) {
        if (v != 0 && dst != v) dst = v, changed = true;
    };
    setS(cur.title, t.title);
    setS(cur.artist, t.artist);
    setS(cur.album, t.album);
    setN(cur.duration, t.duration);
    setN(cur.bpm, t.bpm);
    setS(cur.key, t.key);
    if (!changed) return true;
    Stmt q("INSERT OR REPLACE INTO tracks(uri, title, artist, album, duration, bpm, \"key\", updated_at) VALUES(?,?,?,?,?,?,?,?)");
    if (!q) return false;
    q.text(1, cur.uri);
    q.text(2, cur.title);
    q.text(3, cur.artist);
    q.text(4, cur.album);
    q.num(5, cur.duration);
    q.num(6, cur.bpm);
    q.text(7, cur.key);
    q.num(8, now());
    return q.done();
}

void putTrack(const Track& t) {
    std::lock_guard<std::mutex> l(g_mu);
    if (!db()) return;
    transaction([&] { return upsertTrack(t); });
}

void putTracks(const std::vector<Track>& tracks) {
    if (tracks.empty()) return;
    std::lock_guard<std::mutex> l(g_mu);
    if (!db()) return;
    transaction([&] {
        for (const Track& t : tracks)
            if (!upsertTrack(t)) return false;
        return true;
    });
}

bool track(const std::string& uri, Track* out) {
    std::lock_guard<std::mutex> l(g_mu);
    if (!db()) return false;
    Track t;
    if (!readTrack(uri, &t)) return false;
    if (out) *out = t;
    return true;
}

void putPlaylist(const Playlist& p) {
    if (p.id.empty()) return;
    std::lock_guard<std::mutex> l(g_mu);
    if (!db()) return;
    transaction([&] {
        {
            Stmt q("INSERT OR REPLACE INTO playlists(id, name, updated_at) VALUES(?,?,?)");
            if (!q) return false;
            q.text(1, p.id);
            q.text(2, p.name);
            q.num(3, now());
            if (!q.done()) return false;
        }
        {
            Stmt q("DELETE FROM playlist_tracks WHERE playlist_id = ?");
            if (!q) return false;
            q.text(1, p.id);
            if (!q.done()) return false;
        }
        Stmt q("INSERT INTO playlist_tracks(playlist_id, pos, uri) VALUES(?,?,?)");
        if (!q) return false;
        for (size_t i = 0; i < p.uris.size(); i++) {
            sqlite3_reset(q.s);
            q.text(1, p.id);
            q.integer(2, (long long)i);
            q.text(3, p.uris[i]);
            if (!q.done()) return false;
        }
        return true;
    });
}

bool playlist(const std::string& id, Playlist* out) {
    std::lock_guard<std::mutex> l(g_mu);
    if (!db()) return false;
    Playlist p;
    {
        Stmt q("SELECT name, updated_at FROM playlists WHERE id = ?");
        if (!q) return false;
        q.text(1, id);
        if (!q.row()) return false;
        p.id = id;
        p.name = q.str(0);
        p.updatedAt = q.dbl(1);
    }
    Stmt q("SELECT uri FROM playlist_tracks WHERE playlist_id = ? ORDER BY pos");
    if (!q) return false;
    q.text(1, id);
    while (q.row()) p.uris.push_back(q.str(0));
    if (out) *out = std::move(p);
    return true;
}

std::vector<Playlist> playlists() {
    std::vector<Playlist> r;
    std::lock_guard<std::mutex> l(g_mu);
    if (!db()) return r;
    Stmt q("SELECT id, name, updated_at FROM playlists ORDER BY updated_at DESC, id");
    if (!q) return r;
    while (q.row()) {
        Playlist p;
        p.id = q.str(0);
        p.name = q.str(1);
        p.updatedAt = q.dbl(2);
        r.push_back(std::move(p));
    }
    return r;
}


// ---- genres -------------------------------------------------------------------------------

void putGenre(const TrackGenre& g) {
    if (g.uri.empty()) return;
    std::lock_guard<std::mutex> l(g_mu);
    if (!db()) return;
    Stmt q("INSERT OR REPLACE INTO genres(uri, style1, broad1, score1, style2, broad2, score2, style3, broad3, score3, detected_at) "
           "VALUES(?,?,?,?,?,?,?,?,?,?,?)");
    if (!q) return;
    q.text(1, g.uri);
    for (int k = 0; k < 3; k++) {
        q.text(2 + 3 * k, g.top[k].style);
        q.text(3 + 3 * k, g.top[k].broad);
        q.num(4 + 3 * k, g.top[k].score);
    }
    q.num(11, g.detectedAt ? g.detectedAt : (double)time(nullptr));
    q.done();
}

bool genre(const std::string& uri, TrackGenre* out) {
    std::lock_guard<std::mutex> l(g_mu);
    if (!db()) return false;
    Stmt q("SELECT style1, broad1, score1, style2, broad2, score2, style3, broad3, score3, detected_at FROM genres WHERE uri = ?");
    if (!q) return false;
    q.text(1, uri);
    if (!q.row()) return false;
    if (out) {
        out->uri = uri;
        for (int k = 0; k < 3; k++) out->top[k] = {q.str(3 * k), q.str(3 * k + 1), q.dbl(3 * k + 2)};
        out->detectedAt = q.dbl(9);
    }
    return true;
}

}  // namespace rbm::library
