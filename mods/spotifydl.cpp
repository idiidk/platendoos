// spotifydl: watches the Spotify SDK's HTTPS requests, which go through rekordbox's GET callback
// (HttpsHalHelper::getCallback, synchronous: it answers before it returns). Measuring deck loads showed where a
// Spotify track's time goes: the SDK resolves the track (some requests then hang for a 10 s timeout, once twice
// in a row) and downloads the file in 512 KB pieces read one after another from one connection, ~10 MB/s when the
// CDN is quick but 0.3-1.5 s a piece when it isn't. This logs
//  - any request that takes over a second or fails (host and path, no query: the URLs carry access tokens),
//  - per track download: time to the first byte, total time, MB and how long the pieces kept the SDK waiting.
//   config: spotifydl=0 to disable
//           spotifydl_debug=1   also log every audio piece, and the first audio answer in full
#include "../core/rbmod.h"
#include <string.h>
#ifdef _WIN32
#define strcasecmp _stricmp
#endif
#include <map>
#include <mutex>

using namespace rbm;

// Its settings (core/settings.h): what the settings window and the config file's list show of them.
namespace opt {
const Setting debug{"spotifydl_debug", false, {.label = "Also log every audio piece", .debug = true}};
}  // namespace opt

static volatile bool g_on = true;

namespace {
struct SpHTTPHeader { const char* key; const char* value; };
typedef void (*GetDone)(int err, int curl, const char* msg, int status, const SpHTTPHeader* hdrs, unsigned nhdrs,
                        const unsigned char* body, unsigned len, void* ctx);
struct Call {
    GetDone cb;
    void* ctx;
    double t0;
    int err = 0, curl = 0, status = 0;
    unsigned len = 0;
    bool answered = false;
};
struct Download { double start = 0, firstByte = 0, blocked = 0; long long bytes = 0; int pieces = 0; };
}  // namespace

static std::mutex g_lock;
static std::map<std::string, Download> g_downloads;  // by audio file id

static void done(int err, int curl, const char* msg, int status, const SpHTTPHeader* hdrs, unsigned nhdrs,
                 const unsigned char* body, unsigned len, void* ctx) {
    Call* c = (Call*)ctx;
    c->err = err, c->curl = curl, c->status = status, c->len = len, c->answered = true;
    static bool shown;
    if (!shown && opt::debug.integer() && status == 206) {
        shown = true;
        logf("spotifydl: answer: err %d, %d, msg %s, status %d, %u bytes, %u headers", err, curl, msg ? msg : "(null)", status, len, nhdrs);
        for (unsigned i = 0; hdrs && i < nhdrs; i++) logf("spotifydl:   %s: %s", hdrs[i].key ? hdrs[i].key : "?", hdrs[i].value ? hdrs[i].value : "");
    }
    c->cb(err, curl, msg, status, hdrs, nhdrs, body, len, c->ctx);
}

// "https://host/a/b/c?query" -> "host/a/b" (no query, at most two path parts)
static std::string where(const char* url) {
    const char* p = strstr(url, "://");
    p = p ? p + 3 : url;
    std::string s;
    int slashes = 0;
    for (; *p && *p != '?'; p++) {
        if (*p == '/' && ++slashes > 2) break;
        s += *p;
    }
    return s;
}

static void get(const char* url, const SpHTTPHeader* hdrs, unsigned nhdrs, unsigned timeout, GetDone cb, void* ctx, void* client);

// The hook itself is also spotifyfix's gate (whether or not this mod is on): during a Spotify logout the request
// is answered with an error at once, so the download ends instead of locking up with the logout (see spotifyfix).
extern "C" void rbm_hook_sp_https_get(const char* url, const SpHTTPHeader* hdrs, unsigned nhdrs, unsigned timeout,
                                      GetDone cb, void* ctx, void* client) {
    auto enter = service<bool (*)()>("spotifyfix.httpEnter");
    auto leave = service<void (*)()>("spotifyfix.httpLeave");
    if (enter && leave) {
        if (!enter()) {
            if (cb) cb(1, 0, "", 0, nullptr, 0, nullptr, 0, ctx);
            return;
        }
        get(url, hdrs, nhdrs, timeout, cb, ctx, client);
        leave();
    } else {
        get(url, hdrs, nhdrs, timeout, cb, ctx, client);
    }
}

static void get(const char* url, const SpHTTPHeader* hdrs, unsigned nhdrs, unsigned timeout, GetDone cb, void* ctx, void* client) {
    auto orig = RBM_ORIG(sp_https_get, void (*)(const char*, const SpHTTPHeader*, unsigned, unsigned, GetDone, void*, void*));
    if (!g_on || !url) return orig(url, hdrs, nhdrs, timeout, cb, ctx, client);
    long long from = -1, to = -1;
    for (unsigned i = 0; hdrs && i < nhdrs; i++)
        if (hdrs[i].key && !strcasecmp(hdrs[i].key, "range") && hdrs[i].value) sscanf(hdrs[i].value, "bytes=%lld-%lld", &from, &to);
    const char* a = strstr(url, "/audio/");
    std::string id = a ? std::string(a + 7, strcspn(a + 7, "?/")).substr(0, 12) : "";

    Call* c = new Call{cb, ctx, now()};
    orig(url, hdrs, nhdrs, timeout, done, c, client);
    double took = now() - c->t0;
    if (!c->answered) {  // not answered before returning (not seen so far): leave the call to its callback
        logf("spotifydl: %s returned after %.1fs without an answer yet", where(url).c_str(), took);
        return;
    }
    bool failed = c->err || c->status >= 400 || c->status == 0;
    if (!a || failed) {
        if (took > 1 || failed)
            logf("spotifydl: %s took %.1fs: status %d, error %d/%d", where(url).c_str(), took, c->status, c->err, c->curl);
    } else if (from >= 0) {
        if (opt::debug.integer()) logf("spotifydl: %s bytes %lld-%lld: %u in %.3fs", id.c_str(), from, to, c->len, took);
        std::lock_guard<std::mutex> l(g_lock);
        if (from == 0) g_downloads[id] = Download{c->t0, c->t0 + took};
        auto it = g_downloads.find(id);
        if (it != g_downloads.end()) {
            Download& d = it->second;
            d.pieces++, d.bytes += c->len;
            if (from > 0) d.blocked += took;
            // The SDK asks for 128 KB, then 512 KB pieces, and only the rest of the file at the end.
            if (from > 0 && ((to >= 0 && to - from + 1 < 524288) || (long long)c->len < to - from + 1)) {
                logf("spotifydl: %s downloaded %.1f MB in %.1fs (first byte after %.2fs; %d pieces kept it waiting %.1fs)",
                     id.c_str(), d.bytes / 1e6, now() - d.start, d.firstByte - d.start, d.pieces, d.blocked);
                g_downloads.erase(it);
            }
        }
    }
    delete c;
}

// The window's sidebar area this mod is shown in (core/settings.h: the first declaration of an area counts).
namespace { const rbm::settings::Area kArea{"spotify", "Spotify",
    "Spotify playlists, their audio and their cover art.",
    "<path d=\"M3 5.5c3.5-1.3 7-1 10 .8M3.8 8.3c2.9-1 5.8-.7 8.2.7M4.8 11c2.1-.6 4.1-.4 5.9.5\"/>", 40}; }

RBM_MOD({.name = "spotifydl", .enabled = &g_on,
         .title = "Spotify load times",
         .about = "Logs where a Spotify track's load time goes.",
         .area = "spotify", .order = 90});
