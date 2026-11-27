// outputsync: rekordbox plays to two devices at once (separately, or through an aggregate device), e.g. with PC MASTER OUT the master
// to Bluetooth speakers and the headphone cue to a controller. macOS doesn't line an aggregate's devices up: the
// Bluetooth speakers sound a tenth of a second or more after the controller, so the headphones run ahead of the
// speakers. This delays the faster devices' audio to match the slowest one, so speakers and headphones line up.
// Bluetooth devices report their latency; the real delay is often longer, so it can be set by ear.
// Its row in Preferences > Audio also has a Refresh audio devices button: rekordbox looks for audio devices again and
// reconnects the audio (its own routine for waking from sleep), for a device that was plugged in or changed and
// didn't show up or stayed silent, without restarting rekordbox.
//   config: outputsync=0 to disable
//           outputsync_lineup=1  the switch in rekordbox's Preferences > Audio ("Line up speakers and headphones")
//           outputsync_offset=0  fine-tune in ms on top (+ = headphones later), the slider under that switch; there
//                                too a metronome switch: clicks on every output, to hear whether they line up
//           outputsync_ms=auto   auto: as the devices report; a number: delay the faster devices by that many ms
//                                (tune while listening to the speakers and headphones together; re-read live)
//           outputsync_clocks=1  log every 30 s how fast each device's clock really runs and how far they drift apart
//                                (measured: Bluetooth speakers vs an FLX4, 5 ppm = 0.3 ms a minute)
// Drift: separate devices run on their own clocks. In an aggregate, macOS locks them together only with drift
// compensation, which rekordbox's own aggregate leaves off; the mod turns it on.
// Platform note: this file is the CoreAudio backend of the mod (devices, aggregate IO callbacks, mach clocks) and
// stays Objective-C++ and macOS-only for now; a Windows port needs its own device layer (WASAPI) next to it.
#import <Foundation/Foundation.h>
#include "../core/rbmod.h"
#include <CoreAudio/CoreAudio.h>
#include <mach/mach_time.h>
#include <mach/mach.h>
#include <cmath>
#include <algorithm>
#include <atomic>
#include <cstring>
#include <string>
#include <functional>
#include <vector>

using namespace rbm;

// Its settings (core/settings.h): what the settings window and the config file's list show of them.
namespace opt {
const Setting lineup{"outputsync_lineup", true, {.label = "Line up speakers and headphones", .help = "Delays the quicker device so both sound at the same moment."}};
const Setting offset{"outputsync_offset", 0, {.label = "Fine-tune", .help = "To the right the headphones play later, to the left earlier.", .unit = "ms", .min = -300, .max = 300, .step = 1, .slider = true, .needs = "outputsync_lineup"}};
const Setting ms{"outputsync_ms", "auto", {.label = "Delay to use", .help = "auto: what the devices report. A number: delay the quicker devices by that many milliseconds.", .advanced = true, .needs = "outputsync_lineup"}};
const Setting clocks{"outputsync_clocks", false, {.label = "Log the devices' clocks every 30 seconds", .debug = true}};
}  // namespace opt

static volatile bool g_on = true;
static volatile bool g_broken = false;    // the device field isn't what it was: stay out of the way
static volatile bool g_lineUp = true;     // outputsync_lineup: the Preferences switch
static volatile int g_offsetMs = 0;       // outputsync_offset: the Preferences fine-tune slider (+ = headphones later)
static volatile bool g_metronome = false;  // the Preferences metronome switch (not saved)
static bool g_metroWindow = false;         // ...switched on in the platendoos Preferences window (which stops it when it closes)
static std::string g_planDesc;  // main thread: what replan last made of the devices ("" before the first)
static bool g_canLineUp;        // main thread: rekordbox plays to two or more devices now

namespace {

// One aggregate's output: per stream (= AudioBuffer in the IO callback), its channels and how many frames to delay
// it, with a ring buffer for that. Made on the main thread; the audio thread only reads it.
// The ring always has room for the longest delay, and every buffer goes through it, so the delay can change in
// place (the slider) without a gap of silence: only the read point moves.
struct StreamDelay {
    UInt32 channels = 0;
    volatile UInt32 delay = 0;  // frames; the main thread changes it in place
    std::vector<float> ring;    // (1 s + one block's room) x channels
    UInt32 pos = 0;             // write index (frames)
};
struct DevicePlan {
    AudioObjectID id = 0;
    double rate = 0;
    std::vector<StreamDelay> streams;  // the device's output buffers, in order
};
struct Plan {
    std::vector<DevicePlan> devices;
    std::string desc;
    std::string layout;  // devices and streams: a plan with the same layout is updated in place
};

}  // namespace

static std::atomic<Plan*> g_plan{nullptr};

// ---- CoreAudio ----------------------------------------------------------------------------

template <typename T> static bool getProp(AudioObjectID id, AudioObjectPropertySelector sel, AudioObjectPropertyScope scope, T* out) {
    AudioObjectPropertyAddress a{sel, scope, kAudioObjectPropertyElementMain};
    UInt32 size = sizeof(T);
    return AudioObjectGetPropertyData(id, &a, 0, nullptr, &size, out) == noErr;
}
static std::vector<AudioObjectID> getList(AudioObjectID id, AudioObjectPropertySelector sel, AudioObjectPropertyScope scope) {
    AudioObjectPropertyAddress a{sel, scope, kAudioObjectPropertyElementMain};
    UInt32 size = 0;
    if (AudioObjectGetPropertyDataSize(id, &a, 0, nullptr, &size) != noErr || !size) return {};
    std::vector<AudioObjectID> v(size / sizeof(AudioObjectID));
    if (AudioObjectGetPropertyData(id, &a, 0, nullptr, &size, v.data()) != noErr) return {};
    v.resize(size / sizeof(AudioObjectID));
    return v;
}
static std::string nameOf(AudioObjectID id) {
    CFStringRef s = nullptr;
    if (!getProp(id, kAudioObjectPropertyName, kAudioObjectPropertyScopeGlobal, &s) || !s) return "?";
    std::string r = [(__bridge NSString*)s UTF8String] ?: "?";
    CFRelease(s);
    return r;
}
static UInt32 streamChannels(AudioStreamID s) {
    AudioStreamBasicDescription f{};
    return getProp(s, kAudioStreamPropertyVirtualFormat, kAudioObjectPropertyScopeGlobal, &f) ? f.mChannelsPerFrame : 0;
}
// A device's output latency in frames: its own, its safety offset and its first output stream's.
static UInt32 outputLatency(AudioObjectID dev) {
    UInt32 lat = 0, safety = 0, stream = 0;
    getProp(dev, kAudioDevicePropertyLatency, kAudioObjectPropertyScopeOutput, &lat);
    getProp(dev, kAudioDevicePropertySafetyOffset, kAudioObjectPropertyScopeOutput, &safety);
    auto streams = getList(dev, kAudioDevicePropertyStreams, kAudioObjectPropertyScopeOutput);
    if (!streams.empty()) getProp(streams[0], kAudioStreamPropertyLatency, kAudioObjectPropertyScopeGlobal, &stream);
    return lat + safety + stream;
}

static std::string stringProp(AudioObjectID id, AudioObjectPropertySelector sel) {
    CFStringRef v = nullptr;
    if (!getProp(id, sel, kAudioObjectPropertyScopeGlobal, &v) || !v) return "";
    std::string r = [(__bridge NSString*)v UTF8String] ?: "";
    CFRelease(v);
    return r;
}

// Drift: an aggregate's devices each run on their own clock. macOS keeps the others locked to the clock device by
// resampling them only if their drift compensation is on, and rekordbox's own aggregate (PC MASTER OUT) has it off:
// the speakers then slowly drift against the headphones until macOS resyncs with a jump. Turned on here.
static void lockClocks(AudioObjectID aggregate) {
    std::string clock = stringProp(aggregate, kAudioAggregateDevicePropertyMainSubDevice);
    for (AudioObjectID sub : getList(aggregate, kAudioObjectPropertyOwnedObjects, kAudioObjectPropertyScopeGlobal)) {
        AudioClassID cls = 0;
        UInt32 drift = 0;
        if (!getProp(sub, kAudioObjectPropertyClass, kAudioObjectPropertyScopeGlobal, &cls) || cls != kAudioSubDeviceClassID) continue;
        if (!getProp(sub, kAudioSubDevicePropertyDriftCompensation, kAudioObjectPropertyScopeGlobal, &drift) || drift) continue;
        if (stringProp(sub, kAudioDevicePropertyDeviceUID) == clock) continue;
        UInt32 on = 1;
        AudioObjectPropertyAddress a{kAudioSubDevicePropertyDriftCompensation, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain};
        OSStatus err = AudioObjectSetPropertyData(sub, &a, 0, nullptr, sizeof on, &on);
        logf("outputsync: %s in %s: drift compensation %s", nameOf(sub).c_str(), nameOf(aggregate).c_str(),
             err == noErr ? "on (locked to the clock device, no more drifting)" : "could not be turned on");
    }
}

// The devices rekordbox plays to (its audio callbacks' devices) as a plan: every output stream with the delay that
// lines it up with the slowest device among them. An aggregate counts as its devices. Main thread.
namespace {
struct Seen {
    std::atomic<AudioObjectID> id{0};
    std::atomic<uint64_t> last{0};  // mach time of its latest callback: tick frees the slots of devices gone quiet
};
}  // namespace
static Seen g_seen[8];  // devices whose callback ran (audio thread adds them)

static Plan* makePlan(bool lineUp, const std::string& setting, int offsetMs) {
    // Each part's latency in seconds (its own rate) and the rate its callback device runs at (where the delay is
    // applied): devices at different rates can be lined up too.
    struct Part { AudioObjectID device; double rate; std::string name; double latency; std::vector<UInt32> channels; };
    std::vector<Part> parts;
    std::vector<AudioObjectID> done;
    for (auto& seen : g_seen) {
        AudioObjectID dev = seen.id.load();
        UInt32 running = 0, transport = 0;
        if (!dev || std::find(done.begin(), done.end(), dev) != done.end()) continue;
        done.push_back(dev);
        if (!getProp(dev, kAudioDevicePropertyDeviceIsRunningSomewhere, kAudioObjectPropertyScopeGlobal, &running) || !running) continue;
        getProp(dev, kAudioDevicePropertyTransportType, kAudioObjectPropertyScopeGlobal, &transport);
        Float64 rate = 0;
        getProp(dev, kAudioDevicePropertyNominalSampleRate, kAudioObjectPropertyScopeGlobal, &rate);
        if (!(rate > 0)) continue;
        std::vector<AudioObjectID> subs;
        if (transport == kAudioDeviceTransportTypeAggregate) {
            subs = getList(dev, kAudioAggregateDevicePropertyActiveSubDeviceList, kAudioObjectPropertyScopeGlobal);
            if (subs.size() > 1) lockClocks(dev);
        }
        else
            subs = {dev};
        for (AudioObjectID sub : subs) {
            Float64 subRate = 0;  // a sub-device's latency is in its own frames
            if (!getProp(sub, kAudioDevicePropertyNominalSampleRate, kAudioObjectPropertyScopeGlobal, &subRate) || !(subRate > 0)) subRate = rate;
            Part p{dev, rate, nameOf(sub), outputLatency(sub) / subRate, {}};
            for (AudioStreamID st : getList(sub, kAudioDevicePropertyStreams, kAudioObjectPropertyScopeOutput)) p.channels.push_back(streamChannels(st));
            if (!p.channels.empty()) parts.push_back(p);
        }
    }
    if (parts.size() < 2) return nullptr;
    double slowest = 0;
    for (auto& p : parts) slowest = std::max(slowest, p.latency);
    double ms = setting == "auto" || setting.empty() ? -1 : atof(setting.c_str());
    // Each part's delay (seconds): the faster ones wait for the slowest, plus the fine-tune offset. A negative result
    // (the offset wants the headphones earlier than that) delays the slowest device instead.
    std::vector<double> delays;
    double least = 0;
    for (auto& p : parts) {
        double d = 0;
        if (lineUp && p.latency != slowest) d = (ms >= 0 ? ms / 1000 : slowest - p.latency) + offsetMs / 1000.0;
        delays.push_back(d);
        least = std::min(least, d);
    }
    auto* plan = new Plan;
    char line[256];
    for (size_t i = 0; i < parts.size(); i++) {
        auto& p = parts[i];
        UInt32 delay = (UInt32)std::min(llround((delays[i] - least) * p.rate), (long long)p.rate);  // at most 1 s
        snprintf(line, sizeof line, "%s%s %.0f ms", plan->desc.empty() ? "" : "; ", p.name.c_str(), p.latency * 1000);
        plan->desc += line;
        if (delay) snprintf(line, sizeof line, ", delayed %.0f ms", delay / p.rate * 1000), plan->desc += line;
        if (plan->devices.empty() || plan->devices.back().id != p.device) {
            plan->devices.push_back({p.device, p.rate, {}});
            // the rate too: rings are sized for it, so a rate change makes a new plan
            plan->layout += "|" + std::to_string(p.device) + "@" + std::to_string((long)p.rate);
        }
        for (UInt32 ch : p.channels) {
            StreamDelay sd;
            sd.channels = ch;
            sd.delay = delay;
            plan->devices.back().streams.push_back(std::move(sd));
            plan->layout += "," + std::to_string(ch);
        }
    }
    if (!lineUp) plan->desc = "switched off in Preferences: nothing delayed";
    else if (offsetMs) snprintf(line, sizeof line, " (fine-tuned %+d ms)", offsetMs), plan->desc += line;
    return plan;
}

// ---- the audio thread ---------------------------------------------------------------------

// Each buffer the device gets: written into its ring, and the ring's frames from `delay` ago taken out instead.
static void delayStream(StreamDelay& s, AudioBuffer& b) {
    if (!b.mData || b.mNumberChannels != s.channels || !s.channels || s.ring.empty()) return;
    UInt32 frames = b.mDataByteSize / (sizeof(float) * s.channels);
    UInt32 size = (UInt32)(s.ring.size() / s.channels);
    UInt32 delay = s.delay;
    if (frames + delay > size) return;  // (a block larger than the room made for it)
    float* io = (float*)b.mData;
    for (UInt32 f = 0; f < frames; f++) {
        float* w = &s.ring[(size_t)s.pos * s.channels];
        float* r = &s.ring[(size_t)((s.pos + size - delay) % size) * s.channels];
        for (UInt32 c = 0; c < s.channels; c++) w[c] = io[c], io[c] = r[c];
        io += s.channels;
        s.pos = (s.pos + 1) % size;
    }
}

// The metronome: a short click every half second on every output, timed by the moment each buffer leaves for the
// device (outTime's host time), before the delay. With the devices lined up, speakers and headphones click together.
static double g_hostToSec;
static void addClicks(DevicePlan& d, const AudioTimeStamp* t, AudioBufferList* out) {
    if (!t || !(t->mFlags & kAudioTimeStampHostTimeValid) || d.rate <= 0 || !g_hostToSec) return;
    double t0 = (double)t->mHostTime * g_hostToSec;
    for (UInt32 i = 0; i < out->mNumberBuffers; i++) {
        AudioBuffer& b = out->mBuffers[i];
        if (!b.mData || !b.mNumberChannels) continue;
        UInt32 frames = b.mDataByteSize / (sizeof(float) * b.mNumberChannels);
        float* io = (float*)b.mData;
        for (UInt32 f = 0; f < frames; f++, io += b.mNumberChannels) {
            // A click every half second, the same instant on every device (the host's clock); every fourth one is
            // the "one": higher and a little longer, so it is clear which click of the speakers belongs to which of
            // the headphones when they are far apart.
            double at = t0 + f / d.rate, ph = fmod(at, 0.5);
            if (ph >= 0.04) continue;
            bool one = fmod(at, 2.0) < 0.5;
            float v = (float)((one ? 0.6 : 0.4) * sin(2 * M_PI * (one ? 2400 : 1200) * ph) * exp(-ph / (one ? 0.009 : 0.006)));
            for (UInt32 c = 0; c < b.mNumberChannels; c++) io[c] += v;
        }
    }
}

// Each device's clock against the Mac's: its sample time and host time, first and latest (audio thread writes).
namespace {
struct Clock {
    std::atomic<AudioObjectID> id{0};
    std::atomic<double> firstSample{0}, lastSample{0};
    std::atomic<uint64_t> firstHost{0}, lastHost{0};
};
}  // namespace
static Clock g_clock[8];
static void noteClock(AudioObjectID dev, const AudioTimeStamp* t) {
    if (!t || !(t->mFlags & kAudioTimeStampSampleHostTimeValid)) return;
    // its own slot first (tick frees slots, so a free one can come before it), then a free one
    for (auto& c : g_clock)
        if (c.id.load() == dev) {
            c.lastSample = t->mSampleTime, c.lastHost = t->mHostTime;
            return;
        }
    for (auto& c : g_clock) {
        AudioObjectID z = 0;
        if (!c.id.compare_exchange_strong(z, dev)) continue;
        c.firstSample = t->mSampleTime, c.firstHost = t->mHostTime;
        c.lastSample = t->mSampleTime, c.lastHost = t->mHostTime;
        return;
    }
}

typedef void (*CallbackFn)(void*, const AudioTimeStamp*, const AudioTimeStamp*, const AudioBufferList*, AudioBufferList*);
static std::atomic<int> g_inUse;  // the audio thread is using g_plan
// CoreAudioInternal: its device (set in its constructor, "stp wzr, w21, [x19, #0x64]"; no recipe kind reads a stp, so
// the main thread only believes a value that is a CoreAudio device).
static const size_t O_deviceId = 0x68;

extern "C" void rbm_hook_output_sync(void* self, const AudioTimeStamp* now, const AudioTimeStamp* outTime,
                                     const AudioBufferList* in, AudioBufferList* out) {
    RBM_ORIG(output_sync, CallbackFn)(self, now, outTime, in, out);
    if (!g_on || g_broken || !out || !self) return;
    AudioObjectID dev = *(AudioObjectID*)((char*)self + O_deviceId);
    // Its slot, or a free one. Looked up in all slots first: tick frees slots, so a free one can come before ours.
    // Only this device's callback adds it, so it never ends up in two slots.
    Seen* mine = nullptr;
    for (auto& s : g_seen)
        if (s.id.load() == dev) { mine = &s; break; }
    for (auto& s : g_seen) {
        if (mine) break;
        AudioObjectID z = 0;
        if (s.id.compare_exchange_strong(z, dev)) mine = &s;
    }
    if (!mine) return;
    mine->last.store(mach_absolute_time(), std::memory_order_relaxed);
    noteClock(dev, outTime);
    g_inUse++;
    if (Plan* p = g_plan.load())
        for (auto& d : p->devices)
            if (d.id == dev && out->mNumberBuffers == d.streams.size()) {
                if (g_metronome) addClicks(d, outTime, out);
                for (UInt32 i = 0; i < out->mNumberBuffers; i++) delayStream(d.streams[i], out->mBuffers[i]);
            }
    g_inUse--;
}

static void checkRow();
static void refreshAudio(const char* from);

// ---- main thread --------------------------------------------------------------------------

// Each device's actual sample rate (from its timestamps) and how far the devices drift apart.
static void logClocks() {
    static mach_timebase_info_data_t tb;
    if (!tb.denom) mach_timebase_info(&tb);
    std::string line;
    double rates[8] = {};
    int n = 0;
    for (auto& c : g_clock) {
        AudioObjectID id = c.id.load();
        if (!id) continue;
        double secs = (double)(c.lastHost.load() - c.firstHost.load()) * tb.numer / tb.denom / 1e9;
        if (secs < 5) continue;
        double rate = (c.lastSample.load() - c.firstSample.load()) / secs;
        Float64 nominal = 0;
        getProp(id, kAudioDevicePropertyNominalSampleRate, kAudioObjectPropertyScopeGlobal, &nominal);
        char one[160];
        snprintf(one, sizeof one, " %s %.3f Hz (%+.1f ppm) over %.0f s;", nameOf(id).c_str(), rate, nominal > 0 ? (rate / nominal - 1) * 1e6 : 0, secs);
        line += one;
        if (nominal > 0) rates[n++] = rate / nominal;
    }
    if (n >= 2) {
        char one[120];
        snprintf(one, sizeof one, " apart: %.1f ppm = %.1f ms a minute", (rates[0] - rates[1]) * 1e6, (rates[0] - rates[1]) * 60000);
        line += one;
    }
    if (!line.empty()) logf("outputsync: clocks:%s", line.c_str());
}

// Devices whose callback hasn't run for 10 s (unplugged, switched away from, a new aggregate made): their slots are
// freed, else after 8 device ids in a session new devices found no slot and nothing was lined up any more. A device
// that comes back takes a slot again on its next callback.
static void forgetQuiet() {
    uint64_t t = mach_absolute_time(), quiet = (uint64_t)(10 / g_hostToSec);
    auto stale = [&](uint64_t last) { return last < t && t - last > quiet; };  // (outTime's host time is ahead of now)
    for (auto& s : g_seen) {
        AudioObjectID v = s.id.load();
        if (v && stale(s.last.load(std::memory_order_relaxed))) s.id.compare_exchange_strong(v, 0);
    }
    for (auto& c : g_clock) {
        AudioObjectID v = c.id.load();
        if (v && stale(c.lastHost.load())) c.id.compare_exchange_strong(v, 0);
    }
}

// The plan for the devices running now and the settings. Same devices: the delays change in place (no gap).
static void replan() {
    static std::string lastDesc = "-";
    if (g_broken) return;
    g_lineUp = opt::lineup.integer() != 0;
    g_offsetMs = std::clamp(opt::offset.integer(), -500, 500);
    Plan* next = makePlan(g_lineUp, opt::ms.text(), g_offsetMs);
    Plan* cur = g_plan.load();
    std::string desc = next ? next->desc : "rekordbox plays to one device: nothing to line up";
    g_canLineUp = next != nullptr, g_planDesc = desc;
    if (next && cur && next->layout == cur->layout) {
        for (size_t d = 0; d < next->devices.size(); d++)
            for (size_t i = 0; i < next->devices[d].streams.size(); i++) cur->devices[d].streams[i].delay = next->devices[d].streams[i].delay;
        delete next;
    } else if (next || cur) {
        if (next)
            for (auto& d : next->devices)
                for (auto& st : d.streams) st.ring.assign((size_t)(d.rate + 8192) * st.channels, 0.f);
        g_plan = next;
        while (g_inUse) usleep(100);  // the audio thread is done with the old plan
        delete cur;
    }
    if (desc != lastDesc) logf("outputsync: %s", desc.c_str());
    lastDesc = desc;
}

static void tick() {
    static double at = -10, clocksAt = 0;
    if (devCommand("outputsync") == "refresh") refreshAudio("test command");
    if (!g_hostToSec) {
        mach_timebase_info_data_t tb;
        mach_timebase_info(&tb);
        g_hostToSec = (double)tb.numer / tb.denom / 1e9;
    }
    if (opt::clocks.integer() && now() - clocksAt > 30) clocksAt = now(), logClocks();
    checkRow();
    if (now() - at < 2) return;
    at = now();
    if (g_broken) return;
    static bool checked = false;
    if (!checked) {
        auto devices = getList(kAudioObjectSystemObject, kAudioHardwarePropertyDevices, kAudioObjectPropertyScopeGlobal);
        for (auto& s : g_seen) {
            AudioObjectID v = s.id.load();
            if (!v) continue;
            if (std::find(devices.begin(), devices.end(), v) == devices.end()) {
                logf("outputsync: %u isn't an audio device: rekordbox's audio callback changed, so this mod stays off", v);
                g_broken = true;
                return;
            }
            checked = true;
        }
    }
    if (checked) forgetQuiet();
    replan();
}


// ---- Preferences > Audio: the mod's row ---------------------------------------------------

// rekordbox's rows are PreferenceBaseItem classes (628 wide), stacked by ItemPageComponent::resized from its own list
// (each row's height + 4): a card in the page's colour with a 20 pt bold heading at 20,20 and its controls from
// 52,56 (as AudioSampleRate's constructor lays them out). The mod's row is one more of them, made the same way: a
// PreferenceBaseItem (which paints the card), the heading "Speakers and headphones" with "platendoos" at its right,
// the on/off switch in the look of rekordbox's own (as ExtensionEnableVideo), a fine-tune slider, a metronome
// switch, a line that says what is lined up now, and the Refresh button. It is added to the page after the
// AudioDevice row, and the page's resized (its vtable slot) is replaced by the same stacking with the row in its
// list place. The page object is the same for every Preferences category: on any other the row is taken out again.
// JUCE through rekordbox's own symbols.
static const Addr A_componentCtor{"__ZN4juce9ComponentC2Ev"};
static const Addr A_toggleCtor{"__ZN4juce12ToggleButtonC1ERKNS_6StringE"};
static const Addr A_sliderCtor{"__ZN4juce6SliderC1ENS0_11SliderStyleENS0_20TextEntryBoxPositionE"};
static const Addr A_labelCtor{"__ZN4juce5LabelC1ERKNS_6StringES3_"};
static const Addr A_rowCtor{"__ZN18PreferenceBaseItemC2Ev"};   // rekordbox's own row class: it paints the card's background
static const Addr A_fontCtor{"__ZN4juce4FontC1Efi"};           // juce::Font(float height, int style): 1 = bold
static const Addr A_fontDtor{"__ZN4juce4FontD1Ev"};
static const Addr A_labelSetFont{"__ZN4juce5Label7setFontERKNS_4FontE"};
static const Addr A_labelJustify{"__ZN4juce5Label20setJustificationTypeENS_13JustificationE"};
static const Addr A_labelSetText{"__ZN4juce5Label7setTextERKNS_6StringENS_16NotificationTypeE"};
static const Addr A_setEnabled{"__ZN4juce9Component10setEnabledEb"};
static const Addr A_removeChild{"__ZN4juce9Component20removeChildComponentEPS0_"};
static const Addr A_textButtonCtor{"__ZN4juce10TextButtonC1ERKNS_6StringE"};  // (a TextButton adds no fields to juce::Button: a ToggleButton's size)
// "Refresh audio devices": the list of devices is read again, the audio device closed, and the device that was
// selected opened anew, the way the Audio drop-down selects one (SettingIF::loadPreferredSettings_Audio). Not
// rekordbox's own reconnect (SettingIF::audioDeviceReconnect, what it runs after the computer wakes): that ends in
// its plug-and-play choice of a device, which took a virtual device over the chosen speakers here (2026-11-26) and
// kept it for the next launches.
static const Addr A_settingIF{"__ZN6djplay9SettingIF11getInstanceEv"};
static const Addr A_currentDevice{"SettingIF::getCurrentAudioDevice", [] {  // juce::String (SettingIF*, false): the output device's name; const from 7.2.20 on
    uintptr_t a = symbol("__ZNK6djplay9SettingIF21getCurrentAudioDeviceEb");
    return a ? a : symbol("__ZN6djplay9SettingIF21getCurrentAudioDeviceEb");
}};
static const Addr A_selectDevice{"__ZN6djplay9SettingIF27loadPreferredSettings_AudioERKN4juce6StringE"};  // bool: it opened
static const Addr A_cacheDevices{"__ZN6djplay9SettingIF30startCacheAvailableAudioDeviceEv"};
static const Addr A_audioObject{"__ZN8djengine10DjEngineIF20getAudioDeviceObjectEv"};   // rb::AudioDeviceManager*
static const Addr A_closeAudio{"__ZN2rb18AudioDeviceManager16closeAudioDeviceEv"};
static const Addr A_plugAndPlay{"__ZN6djplay9SettingIF11plugAndPlayEb"};
static const Addr A_muteOffCheck{"__ZN6djplay9SettingIF17startMuteOffCheckEv"};
static const Addr A_updateAllItem{"__ZN17ItemPageComponent13updateAllItemEv"};          // every row of a Preferences page reads its setting again
static const Addr A_addAndMakeVisible{"__ZN4juce9Component17addAndMakeVisibleEPS0_i"};
static const Addr A_setBounds{"__ZN4juce9Component9setBoundsEiiii"};
static const Addr A_setSize{"__ZN4juce9Component7setSizeEii"};
static const Addr A_setTopLeft{"__ZN4juce9Component18setTopLeftPositionEii"};
static const Addr A_isShowing{"__ZNK4juce9Component9isShowingEv"};
static const Addr A_setToggleState{"__ZN4juce6Button14setToggleStateEbNS_16NotificationTypeE"};
static const Addr A_getToggleState{"__ZNK4juce6Button14getToggleStateEv"};
static const Addr A_setRange{"__ZN4juce6Slider8setRangeEddd"};
static const Addr A_setValue{"__ZN4juce6Slider8setValueEdNS_16NotificationTypeE"};
static const Addr A_getValue{"__ZNK4juce6Slider8getValueEv"};
static const Addr A_setTextBoxStyle{"__ZN4juce6Slider15setTextBoxStyleENS0_20TextEntryBoxPositionEbii"};
static const Addr A_setSuffix{"__ZN4juce6Slider18setTextValueSuffixERKNS_6StringE"};
static const Addr A_setDoubleClick{"__ZN4juce6Slider25setDoubleClickReturnValueEbdNS_12ModifierKeysE"};
static const Addr A_setLookAndFeel{"__ZN4juce9Component14setLookAndFeelEPNS_11LookAndFeelE"};
static const Addr A_setColour{"__ZN4juce9Component9setColourEiNS_6ColourE"};
static const Addr A_findColour{"__ZNK4juce9Component10findColourEib"};
static const Addr A_defaultLnf{"__ZN4juce11LookAndFeel21getDefaultLookAndFeelEv"};
static const Addr A_lnfFindColour{"__ZNK4juce11LookAndFeel10findColourEi"};
static const Addr A_numChildren{"__ZNK4juce9Component21getNumChildComponentsEv"};
static const Addr A_child{"__ZNK4juce9Component17getChildComponentEi"};
static const Addr A_pageVtable{"__ZTV17ItemPageComponent"};
static const Addr A_pageResized{"__ZN17ItemPageComponent7resizedEv"};
static const char* kSwitchLnfGet = "__ZN4juce15SingletonHolderIN6djplay38PreferenceToggleAndColorBoxLookAndFeelENS_15CriticalSectionELb0EE3getEv";
static const Addr A_switchLnfGet{kSwitchLnfGet};
static const Addr A_switchLnf{"SingletonHolder<PreferenceToggleAndColorBoxLookAndFeel>", [] { return singleton("__ZN20ExtensionEnableVideoC2Ev", kSwitchLnfGet); }};
// std::function<void()> callbacks (libc++: a small buffer of three pointers, then __f_; 32 bytes on arm64, 48 on
// x86_64, where the buffer is 16-aligned): Button::onClick, Slider::onValueChange. The recipes read __f_.
static const Offset O_onClickF{0x118, "__ZN4juce6Button16sendClickMessageERKNS_12ModifierKeysE", "ldr x21", 7, X86{0x120}};
static const Offset O_onValueChangeF{0x118, "__ZN4juce6Slider5Pimpl17handleAsyncUpdateEv", "ldr", 7, X86{0x120}};
// Where __f_ is in a std::function: after the buffer, which is 3 pointers rounded up to max_align_t's alignment
// (0x18 on arm64, 0x20 on x86_64). Ours and rekordbox's are the same libc++ (std::__1).
static constexpr size_t kMaxAlign = alignof(std::max_align_t);
static constexpr size_t kFunctionF = (3 * sizeof(void*) + kMaxAlign - 1) / kMaxAlign * kMaxAlign;
static_assert(sizeof(std::function<void()>) == (kFunctionF + sizeof(void*) + kMaxAlign - 1) / kMaxAlign * kMaxAlign,
              "libc++ std::function: buffer, then __f_");
// ItemPageComponent's rows (juce::Array<Component*>: data, then the count at +0xc) and a component's width/height
static const Offset O_pageRows{0x100, "__ZN17ItemPageComponent7resizedEv", "add", 1};
static const Offset O_compHeight{0x44, "__ZN17ItemPageComponent7resizedEv", "ldr", 2};
static const Offset O_compWidth{0x40, "__ZN17ItemPageComponent7resizedEv", "ldr", 3};

enum { kAudioDeviceRow = 38 };  // PreferenceItem::Type AudioDevice
enum { kSwitchOn = 0x1006502, kSwitchOff = 0x1006503, kSwitchText = 0x1006501, kTextColour = 0x300000b };
enum { kLabelText = 0x1000281, kSliderBackground = 0x1001200, kSliderThumb = 0x1001300, kSliderTrack = 0x1001310,
       kSliderBoxText = 0x1001400, kSliderBoxBackground = 0x1001500, kSliderBoxOutline = 0x1001700 };
enum { kRowWidth = 628, kRowHeight = 250 };
enum { kButtonColour = 0x1000100, kButtonText = 0x1000102 };  // juce::TextButton::buttonColourId, textColourOffId

template <typename F> static F fn(const Addr& a) { return at<F>(a); }
static const char* className(void* c) { return *(const char**)((char*)(*(void***)c)[-1] + 8); }  // RTTI, mangled
static void* findToggle(void* c, int depth = 0) {  // one of rekordbox's own toggles on the page: its colours
    if (strstr(className(c), "N4juce12ToggleButtonE")) return c;
    if (depth > 6) return nullptr;
    int n = fn<int (*)(void*)>(A_numChildren)(c);
    for (int i = 0; i < n; i++)
        if (void* t = findToggle(fn<void* (*)(void*, int)>(A_child)(c, i), depth + 1)) return t;
    return nullptr;
}
static bool hasChild(void* parent, void* c) {
    int n = fn<int (*)(void*)>(A_numChildren)(parent);
    for (int i = 0; i < n; i++)
        if (fn<void* (*)(void*, int)>(A_child)(parent, i) == c) return true;
    return false;
}
static void setColour(void* c, int id, uint32_t argb) { fn<void (*)(void*, int, uint32_t)>(A_setColour)(c, id, argb); }
static void setBounds(void* c, int x, int y, int w, int h) { fn<void (*)(void*, int, int, int, int)>(A_setBounds)(c, x, y, w, h); }
static void add(void* parent, void* c) { fn<void (*)(void*, void*, int)>(A_addAndMakeVisible)(parent, c, -1); }
static void setToggle(void* b, bool on) { fn<void (*)(void*, bool, int)>(A_setToggleState)(b, on, 0); }  // dontSendNotification
static std::function<void()>& callback(void* c, const Offset& f) {
    return *(std::function<void()>*)((char*)c + f - kFunctionF);
}

// sizeof the juce objects the row makes (operator new before their constructors, every call in rekordbox; 7.2.19 and
// 7.2.20 alike). They hold std::functions, which are bigger on x86_64 (Intel Macs).
#if defined(__x86_64__)
enum : size_t { kToggleSize = 0x220, kLabelSize = 0x260, kSliderSize = 0x200 };
#else
enum : size_t { kToggleSize = 0x1f8, kLabelSize = 0x220, kSliderSize = 0x1a8 };
#endif

static void* g_switch;     // its juce::ToggleButtons and juce::Slider
static void* g_metroSwitch;
static void* g_slider;
static void* g_after;      // the page row the mod's row follows (AudioDevice)

static void* toggle(void* row, const char* text, int x, int y, void* lnf, void* sample, uint32_t textColour) {
    void* b = ::operator new(kToggleSize);
    memset(b, 0, kToggleSize);
    JStr t(text);
    fn<void (*)(void*, const JStr*)>(A_toggleCtor)(b, &t);
    add(row, b);
    setBounds(b, x, y, kRowWidth - 20 - x, 19);
    fn<void (*)(void*, void*)>(A_setLookAndFeel)(b, lnf);
    auto find = fn<uint32_t (*)(void*, int, bool)>(A_findColour);
    setColour(b, kSwitchOn, find(sample, kSwitchOn, false));
    setColour(b, kSwitchOff, find(sample, kSwitchOff, false));
    setColour(b, kSwitchText, textColour);
    return b;
}
// size: the font's height (0: the label's own); right: against the right edge.
static void* label(void* row, const char* text, int x, int y, int w, uint32_t colour, float size = 0, bool bold = false, bool right = false) {
    void* l = ::operator new(kLabelSize);
    memset(l, 0, kLabelSize);
    JStr name(""), t(text);
    fn<void (*)(void*, const JStr*, const JStr*)>(A_labelCtor)(l, &name, &t);
    add(row, l);
    setBounds(l, x, y, w, size > 16 ? 24 : 20);
    setColour(l, kLabelText, colour);
    if (size > 0) {
        void* font[2] = {};  // a juce::Font is one pointer
        fn<void (*)(void*, float, int)>(A_fontCtor)(font, size, bold ? 1 : 0);
        fn<void (*)(void*, const void*)>(A_labelSetFont)(l, font);
        fn<void (*)(void*)>(A_fontDtor)(font);
    }
    fn<void (*)(void*, int)>(A_labelJustify)(l, right ? 34 : 33);  // centredRight / centredLeft
    return l;
}
static void setLabel(void* l, const std::string& text) {
    JStr t(text.c_str());
    fn<void (*)(void*, const JStr*, int)>(A_labelSetText)(l, &t, 0);
}
static void setEnabled(void* c, bool on) { fn<void (*)(void*, bool)>(A_setEnabled)(c, on); }

// The row's Refresh button (and the test command "refresh" in command-outputsync). Main thread.
static void* g_row;
static void* g_page;
static void refreshAudio(const char* from) {
    void* settings = fn<void* (*)()>(A_settingIF)();
    void* e = engine();
    void* manager = e ? fn<void* (*)(void*)>(A_audioObject)(e) : nullptr;
    if (!settings || !manager) return;
    std::string before = memberString(A_currentDevice, settings, false);
    fn<void (*)(void*)>(A_cacheDevices)(settings);
    fn<void (*)(void*)>(A_closeAudio)(manager);
    JStr name(before.c_str());
    bool opened = !before.empty() && fn<bool (*)(void*, const JStr*)>(A_selectDevice)(settings, &name);
    // (the device is gone or won't open: rekordbox's own choice, as its drop-down does then)
    if (!opened) fn<void (*)(void*, bool)>(A_plugAndPlay)(settings, false);
    fn<void (*)(void*)>(A_muteOffCheck)(settings);
    std::string now = memberString(A_currentDevice, settings, false);
    logf("outputsync: audio devices looked for again and '%s' opened anew%s%s (%s)", before.c_str(), opened ? "" : ": it didn't open, rekordbox took ",
         opened ? "" : ("'" + now + "'").c_str(), from);
    if (g_page && g_row && fn<bool (*)(void*)>(A_isShowing)(g_row)) fn<void (*)(void*)>(A_updateAllItem)(g_page);  // the page shows what there is now
    if (auto reload = service<void (*)()>("prefs.reload")) reload();  // ...and the platendoos Preferences window's copy of it
    replan();
}

static void* g_fineLabel;   // the row's parts that are greyed out while there is nothing to line up
static void* g_fineHint;
static void* g_status;      // its line that says what is lined up now

static void* makeRow(void* page) {
    void* sample = findToggle(page);
    if (!sample) {
        logf("outputsync: no toggle on rekordbox's Audio page to take the look from: no Preferences row");
        return nullptr;
    }
    void* lnf = fn<void* (*)(uintptr_t)>(A_switchLnfGet)(A_switchLnf + g_slide);
    if (!lnf) return nullptr;
    void* row = calloc(1, 0x140);  // a PreferenceBaseItem is 0x100
    fn<void (*)(void*)>(A_rowCtor)(row);
    fn<void (*)(void*, int, int)>(A_setSize)(row, kRowWidth, kRowHeight);
    uint32_t text = fn<uint32_t (*)(void*, int)>(A_lnfFindColour)(fn<void* (*)()>(A_defaultLnf)(), kTextColour);
    uint32_t on = fn<uint32_t (*)(void*, int, bool)>(A_findColour)(sample, kSwitchOn, false);
    uint32_t off = fn<uint32_t (*)(void*, int, bool)>(A_findColour)(sample, kSwitchOff, false);
    uint32_t dim = (text & 0x00ffffff) | 0x99000000;

    // The heading, as rekordbox's rows have theirs, and whose settings these are.
    label(row, "Speakers and headphones", 20, 20, 380, text, 20, true);
    label(row, "platendoos \xC2\xB7 outputsync", 408, 22, 200, dim, 13, false, true);

    g_switch = toggle(row, "Line up speakers and headphones", 52, 58, lnf, sample, text);
    setToggle(g_switch, g_lineUp);
    callback(g_switch, O_onClickF) = [] {
        bool v = fn<bool (*)(void*)>(A_getToggleState)(g_switch);
        setCfg("outputsync_lineup", v ? "1" : "0");
        replan();
    };

    g_fineLabel = label(row, "Fine-tune", 80, 88, 80, text);
    void* sl = ::operator new(kSliderSize);
    memset(sl, 0, kSliderSize);
    fn<void (*)(void*, int, int)>(A_sliderCtor)(sl, 0, 2);  // LinearHorizontal, TextBoxRight
    add(row, sl);
    setBounds(sl, 164, 87, 424, 22);
    fn<void (*)(void*, double, double, double)>(A_setRange)(sl, -300, 300, 1);
    fn<void (*)(void*, int, bool, int, int)>(A_setTextBoxStyle)(sl, 2, false, 76, 20);
    JStr ms(" ms");
    fn<void (*)(void*, const JStr*)>(A_setSuffix)(sl, &ms);
    fn<void (*)(void*, bool, double, int)>(A_setDoubleClick)(sl, true, 0, 0);
    setColour(sl, kSliderBackground, off);
    setColour(sl, kSliderTrack, on);
    setColour(sl, kSliderThumb, 0xffffffff);
    setColour(sl, kSliderBoxText, text);
    setColour(sl, kSliderBoxBackground, 0x00000000);
    setColour(sl, kSliderBoxOutline, 0x00000000);
    fn<void (*)(void*, double, int)>(A_setValue)(sl, g_offsetMs, 0);
    g_slider = sl;
    callback(sl, O_onValueChangeF) = [] {
        int v = (int)lround(fn<double (*)(void*)>(A_getValue)(g_slider));
        if (v == g_offsetMs) return;
        setCfg("outputsync_offset", std::to_string(v));
        replan();
    };
    g_fineHint = label(row, "To the right the headphones play later, to the left earlier. Double-click for 0.", 80, 110, 520, dim, 13);

    g_metroSwitch = toggle(row, "Metronome on both, to tune by ear (every fourth click is higher)", 80, 140, lnf, sample, text);
    setToggle(g_metroSwitch, g_metronome);
    callback(g_metroSwitch, O_onClickF) = [] {
        g_metronome = g_canLineUp && fn<bool (*)(void*)>(A_getToggleState)(g_metroSwitch);
        g_metroWindow = false;
        logf("outputsync: metronome %s", g_metronome ? "on" : "off");
    };
    g_status = label(row, "", 52, 168, 556, dim, 13);

    // Refresh: the devices looked for again and the audio reconnected, without restarting rekordbox.
    void* rb = ::operator new(kToggleSize);
    memset(rb, 0, kToggleSize);
    JStr refresh("Refresh audio devices");
    fn<void (*)(void*, const JStr*)>(A_textButtonCtor)(rb, &refresh);
    add(row, rb);
    setBounds(rb, 52, 204, 190, 24);
    setColour(rb, kButtonColour, off);
    setColour(rb, kButtonText, text);
    callback(rb, O_onClickF) = [] { plat::onMain([] { refreshAudio("the button in Preferences"); }); };  // (after the click is done)
    label(row, "Looks for devices again and reconnects the audio: a moment of silence.", 254, 206, 354, dim, 13);
    return row;
}

// What the row shows of the devices: greyed out while rekordbox plays to one device (nothing to line up: no delay to
// set, and no metronome, which only sounds where there is something to compare), and the line that says what is
// lined up. Main thread; cheap when nothing changed.
static void showRowState() {
    static std::string shown = "\x01";
    static int shownCan = -1, shownLine = -1;
    if (!g_row) return;
    std::string line = g_canLineUp ? "Now: " + g_planDesc + "."
                                   : "Nothing to line up now: rekordbox plays to one device.";
    if (line != shown) shown = line, setLabel(g_status, line);
    if (shownCan == (int)g_canLineUp && shownLine == (int)g_lineUp) return;
    shownCan = g_canLineUp, shownLine = g_lineUp;
    setEnabled(g_slider, g_canLineUp && g_lineUp);
    setEnabled(g_fineLabel, g_canLineUp && g_lineUp);
    setEnabled(g_fineHint, g_canLineUp && g_lineUp);
    setEnabled(g_metroSwitch, g_canLineUp);
    if (!g_canLineUp && g_metronome) {
        g_metronome = false;
        setToggle(g_metroSwitch, false);
    }
}

// ItemPageComponent::resized, with the mod's row after g_after when it's on that page.
static void (*g_origResized)(void*);
static void pageResized(void* page) {
    if (page != g_page || !g_row || !hasChild(page, g_row)) return g_origResized(page);
    void** rows = *(void***)((char*)page + O_pageRows);
    int n = *(int*)((char*)page + O_pageRows + 0xc);
    int y = 0;
    bool placed = false;
    auto top = fn<void (*)(void*, int, int)>(A_setTopLeft);
    for (int i = 0; i < n; i++) {
        top(rows[i], 0, y);
        y += *(int*)((char*)rows[i] + O_compHeight) + 4;
        if (rows[i] == g_after) top(g_row, 0, y), y += kRowHeight + 4, placed = true;
    }
    if (!placed) top(g_row, 0, y), y += kRowHeight + 4;
    fn<void (*)(void*, int, int)>(A_setSize)(page, *(int*)((char*)page + O_compWidth), y);
}
static bool patchResized() {
    void** vt = at<void**>(A_pageVtable);
    void* orig = at<void*>(A_pageResized);
    for (int i = 0; vt && i < 128; i++) {
        if (vt[i] != orig) continue;
        g_origResized = (void (*)(void*))orig;
        if (!patchSlot(&vt[i], (void*)&pageResized)) break;
        return true;
    }
    logf("outputsync: ItemPageComponent::resized not found in its vtable: no Preferences row");
    return false;
}

extern "C" void rbm_hook_prefs_audio_row(void* page, const void* list) {
    RBM_ORIG(prefs_audio_row, void (*)(void*, const void*))(page, list);
    if (!g_on || g_broken || !page || !list) return;
    // The row's juce objects are made at sizes taken from the tested build (no recipe reads them): only there, and in
    // the checked one (7.2.20: the same sizes in every operator new before their constructors).
    if (!checkedBuild()) {
        static bool said;
        if (!said) said = true, logf("outputsync: untested rekordbox build: no Preferences row (the delay itself works)");
        return;
    }
    static int patched = -1;
    if (patched < 0) patched = patchResized();
    if (!patched) return;
    const int* types = *(const int* const*)list;
    int count = *(const int*)((const char*)list + 0xc);
    int after = -1;
    for (int i = 0; types && i < count; i++)
        if (types[i] == kAudioDeviceRow) after = i;
    if (after < 0) {
        // Another category's rows in the same page object: the mod's row goes (it used to stay, at the bottom of
        // every page), and the page is laid out as rekordbox does it.
        if (page == g_page && g_row && hasChild(page, g_row)) {
            fn<void (*)(void*, void*)>(A_removeChild)(page, g_row);
            g_page = nullptr;
            g_origResized(page);
        }
        return;
    }
    void** rows = *(void***)((char*)page + O_pageRows);
    int n = *(int*)((char*)page + O_pageRows + 0xc);
    if (!rows || after >= n) return;
    if (!g_row) g_row = makeRow(page);
    if (!g_row) return;
    setToggle(g_switch, g_lineUp);
    setToggle(g_metroSwitch, g_metronome);
    fn<void (*)(void*, double, int)>(A_setValue)(g_slider, g_offsetMs, 0);
    g_after = rows[after];
    g_page = page;
    add(page, g_row);
    showRowState();
    pageResized(page);
}

// The metronome stops when Preferences closes.
static void checkRow() {
    showRowState();
    if (g_metronome && !g_metroWindow && g_row && !fn<bool (*)(void*)>(A_isShowing)(g_row)) {
        g_metronome = false;
        setToggle(g_metroSwitch, false);
        logf("outputsync: metronome off (Preferences closed)");
    }
}

// A setting changed (the platendoos Preferences window, the config file): the delay follows at once, not at the next
// two-second replan, so the metronome moves while the Fine-tune slider is dragged; the dialog's own row shows it too.
static void settingChanged() {
    if (!g_on) return;
    replan();
    if (g_switch) setToggle(g_switch, g_lineUp);
    if (g_slider && (int)lround(fn<double (*)(void*)>(A_getValue)(g_slider)) != g_offsetMs) fn<void (*)(void*, double, int)>(A_setValue)(g_slider, g_offsetMs, 0);
}

// macOS's list of audio devices changed (a Bluetooth device connected, a device was plugged in or out): the plan is
// made again, and the platendoos Preferences window's copy of rekordbox's Audio page is made anew, whose device
// drop-down only follows rekordbox's list as it was when the page was made.
static void devicesChanged() {
    if (!g_on) return;
    replan();
    if (auto reload = service<void (*)()>("prefs.reload")) reload();
}

static void init() {
    for (const Setting* s : {&opt::lineup, &opt::offset, &opt::ms}) s->onChange(&settingChanged);
    AudioObjectPropertyAddress a{kAudioHardwarePropertyDevices, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain};
    AudioObjectAddPropertyListenerBlock(kAudioObjectSystemObject, &a, dispatch_get_main_queue(), ^(UInt32, const AudioObjectPropertyAddress*) { devicesChanged(); });
}

// What runs now, in the settings window under the mod's settings: the devices, their latencies and the delay worked
// out from them (the automatic part, which the Fine-tune slider adds to).
static std::string statusLine() {
    if (g_broken) return "Not working with this rekordbox build: nothing is delayed.";
    if (g_planDesc.empty()) return "";
    if (!g_canLineUp) return "Now: rekordbox plays to one device, nothing to line up. Choose a second output (such as PC MASTER OUT on Bluetooth speakers) in rekordbox's Audio settings.";
    if (!g_lineUp) return "Now: two devices, nothing delayed (Line up is off).";
    std::string ms = opt::ms.text();
    return "Now: " + g_planDesc + ".\n" + (ms == "auto" || ms.empty() ? "The delay is worked out from the latencies the devices report; Fine-tune adds to it."
                                                                         : "The delay is the one set under More options (Delay to use), plus Fine-tune.");
}
static int g_statusLine = (settings::status("outputsync", &statusLine), 0);

RBM_MOD({.name = "outputsync", .init = init, .tick = tick, .enabled = &g_on,
         .title = "Speakers and headphones",
         .about = "Lines up speakers and headphones when rekordbox plays to two devices, such as Bluetooth speakers and a controller.",
         .area = "decks", .order = 20, .rekordboxPage = "Audio/Configuration"});
// The metronome in the platendoos Preferences window too (settings::liveSwitch): the same clicks as the dialog's
// switch, off again when that window closes or rekordbox goes back to one device.
static int g_live = (settings::liveSwitch({.mod = "outputsync", .id = "outputsync.metronome",
                                           .label = "Metronome on both, to tune by ear",
                                           .help = "Clicks on every output, every fourth higher: move Fine-tune until they sound as one.",
                                           .on = [] { return (bool)g_metronome; },
                                           .set = [](bool on) {
                                               g_metronome = on && g_canLineUp;
                                               g_metroWindow = g_metronome;
                                               if (g_metroSwitch) setToggle(g_metroSwitch, g_metronome);
                                               logf("outputsync: metronome %s (the Preferences window)", g_metronome ? "on" : "off");
                                           },
                                           .why = [] { return g_canLineUp ? "" : "rekordbox plays to one device now: nothing to line up."; }}),
                    0);

// The settings window's Refresh audio devices button. Main thread.
static void refreshFromSettings() { refreshAudio("the settings window"); }
static int g_action = (settings::action("outputsync", "outputsync.refresh", "Refresh audio devices",
                                        "Looks for audio devices again and opens yours anew: for a device that was plugged in or changed and stays "
                                        "silent. A moment of silence.", &refreshFromSettings), 0);
