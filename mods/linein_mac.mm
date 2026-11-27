// linein's audio inputs on macOS (mods/linein.h): CoreAudio's devices, a device's own IO callback writing the ring (the
// output streams left alone), and the permission macOS asks for once ("microphone": it covers every audio input).
#import <AVFoundation/AVFoundation.h>
#import <Foundation/Foundation.h>
#include "../core/rbmod.h"
#include "linein.h"
#include <CoreAudio/CoreAudio.h>
#include <algorithm>
#include <string>
#include <vector>

using namespace rbm;
using namespace linein;

namespace {

// The device's IO callback (a CoreAudio thread): its input channels into the ring, interleaved.
OSStatus captureIO(AudioObjectID, const AudioTimeStamp*, const AudioBufferList* in, const AudioTimeStamp*, AudioBufferList*, const AudioTimeStamp*, void* ctx) {
    Capture* c = (Capture*)ctx;
    if (!in || !in->mNumberBuffers || !in->mBuffers[0].mNumberChannels) return noErr;
    UInt32 frames = in->mBuffers[0].mDataByteSize / (sizeof(float) * in->mBuffers[0].mNumberChannels);
    if (!frames || frames > (UInt32)kRing / 2) return noErr;
    uint64_t w = c->written.load(std::memory_order_relaxed);
    size_t at = (size_t)(w & (kRing - 1)), first = std::min<size_t>(frames, kRing - at);
    float* ring = c->ring.data();
    int ch = 0, keep = c->channels;
    for (UInt32 b = 0; b < in->mNumberBuffers && ch < keep; b++) {
        const AudioBuffer& buf = in->mBuffers[b];
        const float* src = (const float*)buf.mData;
        UInt32 n = buf.mNumberChannels;
        if (!src || !n || buf.mDataByteSize / (sizeof(float) * n) < frames) { ch += n; continue; }
        for (UInt32 k = 0; k < n && ch < keep; k++, ch++) {
            // (stride copies: vDSP_mmov has none, and these are a few hundred frames)
            float* dst = ring + at * (size_t)keep + (size_t)ch;
            const float* s = src + k;
            for (size_t i = 0; i < first; i++) dst[i * (size_t)keep] = s[i * n];
            dst = ring + (size_t)ch;
            s += first * n;
            for (size_t i = 0; i < frames - first; i++) dst[i * (size_t)keep] = s[i * n];
        }
    }
    c->lastBlock.store(frames, std::memory_order_relaxed);
    c->written.store(w + frames, std::memory_order_release);
    return noErr;
}

template <class T> bool getProp(AudioObjectID obj, AudioObjectPropertySelector sel, AudioObjectPropertyScope scope, T* out) {
    AudioObjectPropertyAddress a{sel, scope, kAudioObjectPropertyElementMain};
    UInt32 size = sizeof(T);
    return AudioObjectGetPropertyData(obj, &a, 0, nullptr, &size, out) == noErr && size == sizeof(T);
}
std::string stringProp(AudioObjectID obj, AudioObjectPropertySelector sel) {
    CFStringRef s = nullptr;
    if (!getProp(obj, sel, kAudioObjectPropertyScopeGlobal, &s) || !s) return "";
    char buf[512] = "";
    CFStringGetCString(s, buf, sizeof buf, kCFStringEncodingUTF8);
    CFRelease(s);
    return buf;
}
int inputChannels(AudioObjectID dev) {
    AudioObjectPropertyAddress a{kAudioDevicePropertyStreamConfiguration, kAudioObjectPropertyScopeInput, kAudioObjectPropertyElementMain};
    UInt32 size = 0;
    if (AudioObjectGetPropertyDataSize(dev, &a, 0, nullptr, &size) != noErr || size < sizeof(AudioBufferList)) return 0;
    std::vector<char> buf(size);
    AudioBufferList* l = (AudioBufferList*)buf.data();
    if (AudioObjectGetPropertyData(dev, &a, 0, nullptr, &size, l) != noErr) return 0;
    int n = 0;
    for (UInt32 i = 0; i < l->mNumberBuffers; i++) n += (int)l->mBuffers[i].mNumberChannels;
    return n;
}

// The IO callback leaves the device's outputs alone (a device with both would otherwise be started for output too).
void outputsOff(AudioObjectID dev, AudioDeviceIOProcID proc) {
    AudioObjectPropertyAddress a{kAudioDevicePropertyIOProcStreamUsage, kAudioObjectPropertyScopeOutput, kAudioObjectPropertyElementMain};
    UInt32 size = 0;
    if (AudioObjectGetPropertyDataSize(dev, &a, 0, nullptr, &size) != noErr || size < sizeof(AudioHardwareIOProcStreamUsage)) return;
    std::vector<char> buf(size);
    AudioHardwareIOProcStreamUsage* u = (AudioHardwareIOProcStreamUsage*)buf.data();
    u->mIOProc = (void*)proc;
    if (AudioObjectGetPropertyData(dev, &a, 0, nullptr, &size, u) != noErr) return;
    for (UInt32 i = 0; i < u->mNumberStreams; i++) u->mStreamIsOn[i] = 0;
    AudioObjectSetPropertyData(dev, &a, 0, nullptr, size, u);
}
// Every input stream must hand the callback 32-bit floats (CoreAudio's own format for IO callbacks; a device that
// insists on integers there isn't taken).
bool floatInput(AudioObjectID dev) {
    AudioObjectPropertyAddress a{kAudioDevicePropertyStreams, kAudioObjectPropertyScopeInput, kAudioObjectPropertyElementMain};
    UInt32 size = 0;
    if (AudioObjectGetPropertyDataSize(dev, &a, 0, nullptr, &size) != noErr || !size) return false;
    std::vector<AudioObjectID> streams(size / sizeof(AudioObjectID));
    if (AudioObjectGetPropertyData(dev, &a, 0, nullptr, &size, streams.data()) != noErr) return false;
    for (AudioObjectID s : streams) {
        AudioStreamBasicDescription f{};
        if (!getProp(s, kAudioStreamPropertyVirtualFormat, kAudioObjectPropertyScopeGlobal, &f)) return false;
        if (f.mFormatID != kAudioFormatLinearPCM || !(f.mFormatFlags & kAudioFormatFlagIsFloat) || f.mBitsPerChannel != 32) return false;
    }
    return true;
}

bool g_micAsked, g_micDenied;

}  // namespace

namespace linein::sys {

std::vector<Input> inputs() {
    std::vector<Input> v;
    AudioObjectPropertyAddress a{kAudioHardwarePropertyDevices, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain};
    UInt32 size = 0;
    if (AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &a, 0, nullptr, &size) != noErr) return v;
    std::vector<AudioObjectID> ids(size / sizeof(AudioObjectID));
    if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &a, 0, nullptr, &size, ids.data()) != noErr) return v;
    ids.resize(size / sizeof(AudioObjectID));
    for (AudioObjectID id : ids) {
        int ch = inputChannels(id);
        if (ch < 1) continue;
        Float64 rate = 0;
        getProp(id, kAudioDevicePropertyNominalSampleRate, kAudioObjectPropertyScopeGlobal, &rate);
        Input in;
        in.id = id, in.name = stringProp(id, kAudioObjectPropertyName), in.uid = stringProp(id, kAudioDevicePropertyDeviceUID);
        in.channels = ch, in.rate = rate;
        v.push_back(std::move(in));
    }
    return v;
}

// macOS's permission for audio input ("microphone"): asked for the first time a deck has an input; without it every
// device delivers silence.
bool allowed(std::string* why) {
    AVAuthorizationStatus s = [AVCaptureDevice authorizationStatusForMediaType:AVMediaTypeAudio];
    if (s == AVAuthorizationStatusAuthorized) return true;
    if (s == AVAuthorizationStatusNotDetermined && !g_micAsked) {
        g_micAsked = true;
        [AVCaptureDevice requestAccessForMediaType:AVMediaTypeAudio completionHandler:^(BOOL) {}];
    } else if (s != AVAuthorizationStatusNotDetermined && !g_micDenied) {
        g_micDenied = true;
        logf("linein: macOS doesn't let rekordbox use audio inputs: allow it in System Settings > Privacy & Security > Microphone, then restart rekordbox");
    }
    *why = "waiting for macOS's permission to use audio inputs";
    return false;
}

std::unique_ptr<Capture> open(const Input& in, std::string* err) {
    AudioObjectID dev = (AudioObjectID)in.id;
    if (!floatInput(dev)) { *err = "its audio isn't in a format this can read"; return nullptr; }
    auto c = std::make_unique<Capture>();
    c->id = in.id, c->uid = in.uid, c->name = in.name, c->rate = in.rate;
    c->channels = std::min(in.channels, kMaxChannels);
    c->ring.assign((size_t)kRing * (size_t)c->channels, 0.f);
    // A small IO block: less to wait for. (CoreAudio runs the device at the smallest block any program asks for.)
    AudioValueRange range{};
    UInt32 want = 128;
    if (getProp(dev, kAudioDevicePropertyBufferFrameSizeRange, kAudioObjectPropertyScopeGlobal, &range))
        want = (UInt32)std::max(range.mMinimum, std::min(range.mMaximum, 128.0));
    AudioObjectPropertyAddress bs{kAudioDevicePropertyBufferFrameSize, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain};
    AudioObjectSetPropertyData(dev, &bs, 0, nullptr, sizeof want, &want);
    getProp(dev, kAudioDevicePropertyBufferFrameSize, kAudioObjectPropertyScopeGlobal, &c->block);
    if (!c->block) c->block = 512;
    AudioDeviceIOProcID proc = nullptr;
    OSStatus s = AudioDeviceCreateIOProcID(dev, captureIO, c.get(), &proc);
    if (s != noErr || !proc) { *err = "CoreAudio refused an IO callback (" + std::to_string((int)s) + ")"; return nullptr; }
    outputsOff(dev, proc);
    s = AudioDeviceStart(dev, proc);
    if (s != noErr) {
        AudioDeviceDestroyIOProcID(dev, proc);
        *err = "CoreAudio could not start it (" + std::to_string((int)s) + ")";
        return nullptr;
    }
    c->sys = (void*)proc;
    c->started = true;
    return c;
}

void stop(Capture* c) {
    if (!c->started) return;
    AudioDeviceIOProcID proc = (AudioDeviceIOProcID)c->sys;
    AudioDeviceStop((AudioObjectID)c->id, proc);
    AudioDeviceDestroyIOProcID((AudioObjectID)c->id, proc);
    c->sys = nullptr;
    c->started = false;
}

}  // namespace linein::sys
