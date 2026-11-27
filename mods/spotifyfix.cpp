// spotifyfix: stops Spotify playlists from reloading in a loop, and rekordbox's Spotify client from deadlocking
// when a new client token arrives during a list refresh (see rbm_hook_spotify_token_set).
//
// Spotify's API returns a revision_id that flip-flops between two values for an unchanged playlist.
// rekordbox compares it (juce::operator!=) in four places in db::SpotifyBridge and on any difference
// throws the cached track list away and reloads it, endlessly. With this mod on, those revision
// comparisons report "unchanged"; track-list changes are still picked up by the normal refreshes.
//   config: spotifyfix=0 to restore the original behaviour
#include "../core/rbmod.h"
#include <atomic>
#include <chrono>
#include <string>
#include <thread>

using namespace rbm;
typedef bool (*StrNeFn)(const void*, const void*);

static volatile bool g_on = true;

static bool revisionDiffers(const void* a, const void* b, StrNeFn orig) { return g_on ? false : orig(a, b); }

extern "C" bool rbm_hook_spotify_rev_refresh(const void* a, const void* b) { return revisionDiffers(a, b, RBM_ORIG(spotify_rev_refresh, StrNeFn)); }
extern "C" bool rbm_hook_spotify_rev_library(const void* a, const void* b) { return revisionDiffers(a, b, RBM_ORIG(spotify_rev_library, StrNeFn)); }
extern "C" bool rbm_hook_spotify_rev_item1(const void* a, const void* b) { return revisionDiffers(a, b, RBM_ORIG(spotify_rev_item1, StrNeFn)); }
extern "C" bool rbm_hook_spotify_rev_item2(const void* a, const void* b) { return revisionDiffers(a, b, RBM_ORIG(spotify_rev_item2, StrNeFn)); }

// rekordbox's Spotify client can deadlock (all downloads and list loads stop, for good): a new client token
// arrives on a pool thread that holds the Client's lock and hands it to Service::setClientTokenDirect, which takes
// the Service's token lock; meanwhile a list refresh (SpotifyBridge::refreshListItem -> getExperiences ->
// Auth::getClientToken) holds that token lock and asks the Client for a token, which waits for the Client's lock.
// The token is handed over on another thread instead, after the Client's lock is released; getClientToken waits
// for it anyway (its event is signalled by setClientTokenDirect).
static void handedOver() {
    // rekordbox asks for the token before each Spotify request, so this is often: logged every 10 minutes.
    static int count;
    static double loggedAt = -1e9;
    count++;
    if (now() - loggedAt > 600) logf("spotifyfix: handed over %d client tokens", count), loggedAt = now(), count = 0;
}

#ifdef _WIN32
// Windows inlines onRequestClientTokenCallback and setClientTokenDirect into the C callback that
// Client::requestClientToken hands to the download library (callback(token, error, client)), so the hook takes
// that request call and swaps in a callback that runs rekordbox's on another thread.
typedef void (*TokenCb)(const char* token, int error, void* client);
typedef void* (*RequestFn)(void* handle, TokenCb cb, void* client);
static TokenCb g_cb;

static void tokenLater(const char* token, int error, void* client) {
    bool has = token != nullptr;
    std::string copy = has ? token : "";
    static plat::Queue q("rbm.spotifyfix.token");
    q.async([has, copy, error, client] {
        g_cb(has ? copy.c_str() : nullptr, error, client);
        handedOver();
    });
}

extern "C" void* rbm_hook_spotify_token_set(void* handle, TokenCb cb, void* client) {
    if (!g_on || !cb) return RBM_ORIG(spotify_token_set, RequestFn)(handle, cb, client);
    g_cb = cb;  // the same function from both call sites
    return RBM_ORIG(spotify_token_set, RequestFn)(handle, tokenLater, client);
}
#else
typedef void (*SetTokenFn)(void* service, const JStr& token);
extern "C" void rbm_hook_spotify_token_set(void* service, const JStr& token) {
    if (!g_on) return RBM_ORIG(spotify_token_set, SetTokenFn)(service, token);
    std::string copy = token.text ? token.text : "";
    static plat::Queue q("rbm.spotifyfix.token");
    q.async([service, copy] {
        JStr t(copy.c_str());
        RBM_ORIG(spotify_token_set, SetTokenFn)(service, t);
        handedOver();
    });
}
#endif

// rekordbox can also lock up for good when it logs out of Spotify while a track downloads (Spotify refuses the
// account, e.g. "403 permission_denied": SpotifyBridge::checkEligibilityFlow -> logout): the logout takes the
// Client's lock and then waits for the download's thread to end, and that thread needs the same lock (the SDK's
// HTTPS callback gives it up for the request and takes it again after). The window then waits for the download
// too. So a logout first makes the SDK's requests fail at once and waits until the download threads have ended
// (15 s at most), and only then runs: nothing is left for it to wait on with the lock held.
// (Windows: the same functions and callback table, found statically; not yet run against a real logout there.)
static const Addr A_serviceVt{"__ZTVN9streaming7spotify7ServiceE"};
static const Offset V_logOut{0xb8, "__ZTVN9streaming7spotify7ServiceE", "vt:__ZN9streaming7spotify7Service13requestLogOutEv", 0};
static const Addr A_jobVt{"__ZTVN9streaming7spotify21DownloadConnectionJobE"};
static const Offset V_jobRun{0x10, "__ZTVN9streaming7spotify21DownloadConnectionJobE", "vt:__ZN9streaming7spotify21DownloadConnectionJob3runEv", 0};
typedef void* (*SelfFn)(void*);
static SelfFn o_logOut, o_jobRun;
static std::atomic<bool> g_loggingOut;
static std::atomic<int> g_jobs, g_requests;  // download threads running; SDK requests inside rekordbox's callback
static thread_local bool t_job;

static void* jobRun(void* self) {
    g_jobs++, t_job = true;
    void* r = o_jobRun(self);
    t_job = false, g_jobs--;
    return r;
}
static void* logOut(void* self) {
    if (!g_on) return o_logOut(self);
    g_loggingOut = true;
    double t0 = now();
    int mine = t_job ? 1 : 0, jobs = g_jobs;
    while ((g_jobs > mine || g_requests > 0) && now() - t0 < 15) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    logf("spotifyfix: Spotify logout: %d downloads running, waited %.1fs for them to end%s", jobs - mine, now() - t0,
         g_jobs > mine || g_requests > 0 ? " (still running: logging out anyway)" : "");
    void* r = o_logOut(self);
    g_loggingOut = false;
    return r;
}
// For the hook on the SDK's HTTPS requests (spotifydl's): false = a logout is on, answer the request with an error.
static bool httpEnter() {
    g_requests++;
    if (!g_loggingOut || !g_on) return true;
    g_requests--;
    return false;
}
static void httpLeave() { g_requests--; }
RBM_PROVIDE("spotifyfix", "spotifyfix.httpEnter", httpEnter, &g_on);
RBM_PROVIDE("spotifyfix", "spotifyfix.httpLeave", httpLeave, &g_on);

static void init() {
    void** jvt = at<void**>(A_jobVt) + 2;
    void** svt = at<void**>(A_serviceVt) + 2;
    o_jobRun = (SelfFn)jvt[V_jobRun / 8], o_logOut = (SelfFn)svt[V_logOut / 8];  // (before the patches: calls may come in at once)
    if (!patchSlot(&jvt[V_jobRun / 8], (void*)&jobRun) || !patchSlot(&svt[V_logOut / 8], (void*)&logOut))
        logf("spotifyfix: could not patch the Spotify logout (it can still lock up during a download)");
}

// The comparisons run on worker threads, so they read a flag the core keeps in sync with the config.
RBM_MOD({.name = "spotifyfix", .init = init, .enabled = &g_on,
         .title = "Spotify fixes",
         .about = "Stops Spotify playlists from reloading themselves forever, and fixes a deadlock in rekordbox's Spotify client.",
         .area = "spotify", .order = 60});
