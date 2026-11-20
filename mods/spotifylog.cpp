// spotifylog: rekordbox's Spotify log (Pioneer/rekordbox6/Spotify.log in rekordbox's log folder) is capped at 1 MB, and
// after every batch of lines StreamingLog trims it by reading the whole file and writing it out again: up to
// ~2 MB of disk writes a second while the library syncs. This trims it at most every 10 minutes instead (the file
// can grow a little past the cap in between).
//   config: spotifylog=0 to disable
#include "../core/rbmod.h"

using namespace rbm;

static volatile bool g_on = true;

// StreamingLog's own thread.
extern "C" void rbm_hook_log_trim(const void* file, int64_t maxSize) {
    static double trimmedAt = -1e9;
    if (g_on && now() - trimmedAt < 600) return;
    trimmedAt = now();
    RBM_ORIG(log_trim, void (*)(const void*, int64_t))(file, maxSize);
}

RBM_MOD({.name = "spotifylog", .enabled = &g_on});
