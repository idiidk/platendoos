// spotifyaudio: a Spotify track's whole audio, in memory, for the mods that listen to whole tracks in their windows
// (autocue, genres). Decoded from the stream with rekordbox's own reader when such a mod asks (as rekordbox's analysis
// reads it), kept for a while in memory, and never written to disk (docs/editions.md). A local file of the track, where
// a mod keeps one (rbmod.h, "trackfiles"), is read by those mods themselves.
// The last tracks decoded are kept (spotifyaudio_mb, 16-bit stereo: ~50 MB for a 5-minute track), so a window that
// reads a track again (autocue's Analyse, then listening) doesn't stream it twice. Gone when rekordbox quits.
//   config: spotifyaudio=0 to disable
//           spotifyaudio_mb=256    memory for the tracks decoded last (0: none kept)
//           spotifyaudio_wait=90   seconds to keep trying to open a track's stream (Spotify's streaming limit)
// Offers (see rbmod.h, "between mods"):
//   "spotifyaudio.read"  bool (const char* uri, std::shared_ptr<const std::vector<int16_t>>* stereo, int* rate,
//                        std::string* why)  the track's audio, 16-bit stereo interleaved (L, R); blocks until it is
//                        decoded (seconds, longer while Spotify still downloads it); never on the main thread
#include "../core/rbmod.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

using namespace rbm;

static volatile bool g_on = true;

namespace {  // file-local: another mod has types of the same names

const Addr A_pcmReaderCtor{"__ZN6rbxfrm17PcmReaderTemplateINS_6Float2EEC1ERKN4juce6StringEiNS3_6Thread8PriorityEiiPKNS_18AudioReaderFactoryE"};  // PcmReaderTemplate<Float2>(String const&, int, Priority, int, int, factory)
const Addr A_pcmGetStreamAt{"__ZN6rbxfrm17PcmReaderTemplateINS_6Float2EE11getStreamAtEiPfS3_i"};  // PcmReaderTemplate<Float2>::getStreamAt(int, float*, float*, int)
const Addr A_readerRate{"__ZNK6rbxfrm15ReaderOwnerTmplINS_6Float2EE17getFileSampleRateEv"};  // ReaderOwnerTmpl<Float2>::getFileSampleRate() const
const Addr A_readerChannels{"__ZNK6rbxfrm15ReaderOwnerTmplINS_6Float2EE13getChannelNumEv"};  // ReaderOwnerTmpl<Float2>::getChannelNum() const
const Offset S_pcmReader{0x2a8, "__ZN8analyzer18CBeatAnalyzeThread7analyzeEv", "movz x0", 0};  // sizeof(PcmReaderTemplate<Float2>): analyze() news one before constructing it
const Offset O_readerOwner{0x1d8, "__ZN6rbxfrm17PcmReaderTemplateINS_6Float2EE6unloadEv", "add x19", 1};  // PcmReaderTemplate: its ReaderOwnerTmpl
const Offset O_readerLength{0x1f8, "__ZN6rbxfrm17PcmReaderTemplateINS_6Float2EE6unloadEv", "str x19", 0};  // PcmReaderTemplate: length in frames (int)

typedef std::shared_ptr<const std::vector<int16_t>> Pcm;
struct Kept { Pcm pcm; int rate = 0; double usedAt = 0; };

std::mutex g_lock;  // g_kept, g_decoding
std::condition_variable g_done;
std::map<std::string, Kept> g_kept;
std::set<std::string> g_decoding;
size_t g_keptBytes;

void trim(double cap) {  // g_lock held: the least recently used go first
    while ((double)g_keptBytes > cap && !g_kept.empty()) {
        auto oldest = g_kept.begin();
        for (auto it = g_kept.begin(); it != g_kept.end(); ++it)
            if (it->second.usedAt < oldest->second.usedAt) oldest = it;
        g_keptBytes -= oldest->second.pcm->size() * sizeof(int16_t);
        g_kept.erase(oldest);
    }
}

// Decodes the track with rekordbox's reader. Opening a stream that Spotify hasn't buffered yet fails (the reader's
// length stays 0), and Spotify refuses new streams for a while after many in a short time: keep trying, 5x a second
// at first and then once a second, for spotifyaudio_wait seconds.
bool decode(const std::string& uri, std::vector<int16_t>* out, int* rate, std::string* why) {
    // (rekordbox 7.2.20 asks the juce thread it runs on whether to stop while it waits for the download: this thread
    // isn't one of juce's)
    JuceThreadScope onJuceThread;
    double t0 = now(), maxWait = cfgNum("spotifyaudio_wait", 90);
    bool waited = false;
    while (g_on && now() - t0 < maxWait) {
        JStr path(uri.c_str());
        void* r = ::operator new(S_pcmReader);
        at<void (*)(void*, const JStr*, int, int, int, int, const void*)>(A_pcmReaderCtor)(r, &path, 0, 0, 0xd55, 0, nullptr);
        int length = *(const int*)((const char*)r + O_readerLength);
        if (length <= 0) {
            deleteObject(r);  // virtual deleting destructor
            if (!waited) logf("spotifyaudio: waiting for the stream of %s", uri.c_str()), waited = true;
            std::this_thread::sleep_for(std::chrono::milliseconds(now() - t0 < 5 ? 200 : 1000));
            continue;
        }
        void* owner = (char*)r + O_readerOwner;
        *rate = at<int (*)(void*)>(A_readerRate)(owner);
        int channels = at<int (*)(void*)>(A_readerChannels)(owner);
        if (*rate > 0) length = std::min(length, *rate * 60 * 30);  // (a mix longer than half an hour is cut)
        out->assign((size_t)length * 2, 0);
        float L[512], R[512];
        // 16-bit as rekordbox's stems are hashed by stemcache (x clamped to -1..1, times 32767, rounded), so a chunk
        // autocue separates from this is the same chunk as a deck's
        auto q = [](float x) { return (int16_t)std::nearbyint(std::max(-1.f, std::min(1.f, x)) * 32767.f); };
        for (int pos = 0; pos < length && g_on; pos += 512) {
            int n = std::min(512, length - pos);
            at<int (*)(void*, int, float*, float*, int)>(A_pcmGetStreamAt)(r, pos, L, R, 512);
            for (int i = 0; i < n; i++) {
                (*out)[(size_t)(pos + i) * 2] = q(L[i]);
                (*out)[(size_t)(pos + i) * 2 + 1] = q(channels >= 2 ? R[i] : L[i]);
            }
        }
        deleteObject(r);
        if (*rate <= 0) return *why = "Couldn't read the stream", false;
        logf("spotifyaudio: decoded %s in memory (%.0f s of audio, %.1f s)", uri.c_str(), (double)length / *rate, now() - t0);
        return true;
    }
    *why = g_on ? "Spotify didn't stream the track" : "Switched off";
    return false;
}

bool read(const char* uriC, Pcm* stereo, int* rate, std::string* why) {
    std::string uri = uriC ? uriC : "", dummy;
    if (!why) why = &dummy;
    if (!g_on || uri.compare(0, 8, "spotify:")) return *why = "Not a Spotify track", false;
    if (!spotifyReadersKeepUp()) return *why = "This rekordbox build can't be read by the mods", false;
    {
        std::unique_lock<std::mutex> l(g_lock);
        g_done.wait(l, [&] { return !g_decoding.count(uri); });  // (one decode per track at a time)
        auto it = g_kept.find(uri);
        if (it != g_kept.end()) {
            it->second.usedAt = now();
            *stereo = it->second.pcm, *rate = it->second.rate;
            return true;
        }
        g_decoding.insert(uri);
    }
    auto pcm = std::make_shared<std::vector<int16_t>>();
    bool ok = decode(uri, pcm.get(), rate, why);
    {
        std::lock_guard<std::mutex> l(g_lock);
        g_decoding.erase(uri);
        double cap = std::max(0.0, cfgNum("spotifyaudio_mb", 256)) * 1e6;
        if (ok && (double)(pcm->size() * sizeof(int16_t)) <= cap) {
            g_kept[uri] = {pcm, *rate, now()};
            g_keptBytes += pcm->size() * sizeof(int16_t);
            trim(cap);
        }
    }
    g_done.notify_all();
    if (ok) *stereo = pcm;
    return ok;
}

void init() {
    if (!spotifyReadersKeepUp()) logf("spotifyaudio: this rekordbox build's Spotify readers can't be made to keep up: Spotify tracks can't be read");
}

void tick() {
    static double nextAt;
    if (now() < nextAt) return;
    nextAt = now() + 5;
    std::lock_guard<std::mutex> l(g_lock);
    trim(std::max(0.0, cfgNum("spotifyaudio_mb", 256)) * 1e6);  // (a smaller limit set meanwhile)
}

}  // namespace

RBM_PROVIDE("spotifyaudio", "spotifyaudio.read", read, &g_on);
RBM_MOD({.name = "spotifyaudio", .init = init, .tick = tick, .enabled = &g_on});
