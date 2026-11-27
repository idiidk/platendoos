// artcache: rekordbox's artwork cache (cover art in the browser, Spotify playlist icons) keeps ~26 MB of images
// (~160 covers at 200 px), drops the 50 oldest whenever it's full, and downloads every image it no longer holds
// again, one at a time. Scrolling a long Spotify playlist or a library sync kept re-downloading the same covers.
// This lets it keep 256 MB in memory. The covers stay in memory only: nothing is written to disk.
//   config: artcache=0 to disable; artcache_mb=<memory cache size> (default 256)
#include "../core/rbmod.h"
#include <algorithm>
#include <atomic>
#include <filesystem>
#include <string>
#include <vector>
#include <string.h>

using namespace rbm;

// Its settings (core/settings.h): what the settings window and the config file's list show of them.
namespace opt {
const Setting mb{"artcache_mb", 256, {.label = "Memory for cover art", .unit = "MB", .min = 16, .max = 4096, .step = 16}};
}  // namespace opt

static const char* kAdd = "__ZN12ArtworkCache5Pimpl15addImageToCacheERKN4juce5ImageERKNS1_6StringE";
static const Offset O_items{0x3c8, kAdd, "add x19", 1};   // ArtworkCache::Pimpl: OwnedArray<Item> (oldest first)
static const Offset O_total{0x3d8, kAdd, "ldr x19", 0};   // ArtworkCache::Pimpl: bytes held (int)
static const Offset O_itemSize{0x10, kAdd, "str x20", 1}; // ArtworkCache::Pimpl::Item: its image's bytes (int)

static volatile bool g_on = true;

// addImageToCache has already subtracted the 50 items' sizes from the total when it calls this (under its lock).
extern "C" void rbm_hook_art_evict(void* array, int start, int n, bool deleteObjects) {
    char* pimpl = (char*)array - O_items;
    void** items = *(void***)array;
    int used = *(int*)((char*)array + 12);
    int64_t limit = (int64_t)opt::mb.number() << 20;
    int64_t removed = 0;
    for (int i = start; i < start + n && i < used; i++) removed += *(int*)((char*)items[i] + O_itemSize);
    int* total = (int*)(pimpl + O_total);
    if (g_on && *total + removed < std::min<int64_t>(limit, 1LL << 30)) {
        *total += (int)removed;
        return;
    }
    RBM_ORIG(art_evict, void (*)(void*, int, int, bool))(array, start, n, deleteObjects);
}

// juce::String / juce::URL arguments rekordbox passes on, and the std::unique_ptr<InputStream> it gets back
// (non-trivial, so returned via x8 like the real one).
namespace {
struct StreamPtr {
    void* p = nullptr;
    StreamPtr() {}
    ~StreamPtr() {}
    StreamPtr(const StreamPtr& o) : p(o.p) {}
};
}  // namespace
using CreateStream = StreamPtr (*)(void*, bool, void*, void*, void*, int, void*, int*, int, void*, void*, void*);

// ArtworkCache's download thread, and getFromURI (a deck's, on the main thread): rekordbox fetches a cover as it would.
extern "C" bool rbm_hook_art_download(const void* url, void* block, bool usePost) {
    return RBM_ORIG(art_download, bool (*)(const void*, void*, bool))(url, block, usePost);
}
extern "C" StreamPtr rbm_hook_art_stream(void* url, bool post, void* progress, void* context, void* headers, int timeout,
                                         void* responseHeaders, int* status, int redirects, void* a, void* b, void* c) {
    return RBM_ORIG(art_stream, CreateStream)(url, post, progress, context, headers, timeout, responseHeaders, status, redirects, a, b, c);
}

RBM_MOD({.name = "artcache", .enabled = &g_on,
         .title = "Cover art",
         .about = "Remembers far more cover art, so scrolling and loading decks don't download it again.",
         .area = "spotify", .order = 40});
