// See audiofile.h: AudioToolbox.
#import <Foundation/Foundation.h>
#import <AudioToolbox/AudioToolbox.h>
#include <string.h>
#include <algorithm>
#include <vector>
#include "audiofile.h"

namespace audiofile {

namespace {
NSURL* urlFor(const std::string& path) { return [NSURL fileURLWithPath:[NSString stringWithUTF8String:path.c_str()]]; }

AudioStreamBasicDescription pcm16(double rate) {
    AudioStreamBasicDescription f = {};
    f.mSampleRate = rate;
    f.mFormatID = kAudioFormatLinearPCM;
    f.mFormatFlags = kAudioFormatFlagIsSignedInteger | kAudioFormatFlagIsPacked;
    f.mChannelsPerFrame = 2;
    f.mBitsPerChannel = 16;
    f.mBytesPerFrame = 4;
    f.mFramesPerPacket = 1;
    f.mBytesPerPacket = 4;
    return f;
}

// Reads up to `want` frames; the FLAC decoder returns a packet's worth at a time, so keep reading until
// the buffer is full or the file ends (got < want).
bool readFrames(ExtAudioFileRef f, int16_t* buf, UInt32 want, UInt32* got) {
    UInt32 have = 0;
    while (have < want) {
        AudioBufferList abl = {};
        abl.mNumberBuffers = 1;
        abl.mBuffers[0].mNumberChannels = 2;
        abl.mBuffers[0].mDataByteSize = (want - have) * 4;
        abl.mBuffers[0].mData = buf + (size_t)have * 2;
        UInt32 n = want - have;
        if (ExtAudioFileRead(f, &n, &abl) != noErr) return false;
        if (n == 0) break;
        have += n;
    }
    *got = have;
    return true;
}
}  // namespace

// (The threads that call these have no autorelease pool of their own, so each opens one.)

bool facts(const std::string& path, int* rate, long long* frames) {
    @autoreleasepool {
        ExtAudioFileRef f = nullptr;
        NSURL* url = urlFor(path);
        if (ExtAudioFileOpenURL((__bridge CFURLRef)url, &f) != noErr) return false;
        AudioStreamBasicDescription fmt = {};
        SInt64 len = 0;
        UInt32 sz = sizeof fmt, lsz = sizeof len;
        bool ok = ExtAudioFileGetProperty(f, kExtAudioFileProperty_FileDataFormat, &sz, &fmt) == noErr &&
                  ExtAudioFileGetProperty(f, kExtAudioFileProperty_FileLengthFrames, &lsz, &len) == noErr;
        ExtAudioFileDispose(f);
        if (ok) *rate = (int)fmt.mSampleRate, *frames = len;
        return ok;
    }
}

bool probe(const std::string& path, Info* out) {
    @autoreleasepool {
        AudioFileID af = nullptr;
        if (AudioFileOpenURL((__bridge CFURLRef)urlFor(path), kAudioFileReadPermission, 0, &af) != noErr) return false;
        Float64 seconds = 0;
        UInt32 bitRate = 0;
        AudioStreamBasicDescription fmt = {};
        UInt32 sz = sizeof seconds;
        AudioFileGetProperty(af, kAudioFilePropertyEstimatedDuration, &sz, &seconds);
        sz = sizeof bitRate;
        AudioFileGetProperty(af, kAudioFilePropertyBitRate, &sz, &bitRate);
        sz = sizeof fmt;
        AudioFileGetProperty(af, kAudioFilePropertyDataFormat, &sz, &fmt);
        AudioFileClose(af);
        out->seconds = seconds, out->bitRate = bitRate, out->sampleRate = fmt.mSampleRate;
        return true;
    }
}

bool readStereo16(const std::string& path, std::vector<int16_t>* out, int* rate) {
    @autoreleasepool {
        ExtAudioFileRef f = nullptr;
        if (ExtAudioFileOpenURL((__bridge CFURLRef)urlFor(path), &f) != noErr) return false;
        AudioStreamBasicDescription src = {};
        UInt32 sz = sizeof src;
        bool ok = ExtAudioFileGetProperty(f, kExtAudioFileProperty_FileDataFormat, &sz, &src) == noErr && src.mSampleRate > 0;
        AudioStreamBasicDescription pcm = pcm16(src.mSampleRate);
        ok = ok && ExtAudioFileSetProperty(f, kExtAudioFileProperty_ClientDataFormat, sizeof pcm, &pcm) == noErr;
        if (ok) {
            *rate = (int)src.mSampleRate;
            out->clear();
            static const UInt32 kChunk = 1 << 16;
            std::vector<int16_t> x((size_t)kChunk * 2);
            for (;;) {
                UInt32 n = 0;
                if (!readFrames(f, x.data(), kChunk, &n)) { ok = false; break; }
                if (!n) break;
                out->insert(out->end(), x.begin(), x.begin() + (size_t)n * 2);
            }
        }
        ExtAudioFileDispose(f);
        return ok && !out->empty();
    }
}

bool readMono(const std::string& path, std::vector<float>* out, int* rate) {
    std::vector<int16_t> x;
    if (!readStereo16(path, &x, rate)) return false;
    out->resize(x.size() / 2);
    for (size_t i = 0; i < out->size(); i++) (*out)[i] = (x[2 * i] + x[2 * i + 1]) / 65536.f;
    return true;
}

bool sameSamples(const std::string& a, const std::string& b) {
    @autoreleasepool {
        ExtAudioFileRef fa = nullptr, fb = nullptr;
        NSURL* ua = urlFor(a);
        NSURL* ub = urlFor(b);
        bool same = false;
        do {
            if (ExtAudioFileOpenURL((__bridge CFURLRef)ua, &fa) != noErr || ExtAudioFileOpenURL((__bridge CFURLRef)ub, &fb) != noErr) break;
            AudioStreamBasicDescription ra = {}, rb = {};
            UInt32 sz = sizeof ra;
            SInt64 la = 0, lb = 0;
            UInt32 lsz = sizeof la;
            if (ExtAudioFileGetProperty(fa, kExtAudioFileProperty_FileDataFormat, &sz, &ra) != noErr) break;
            sz = sizeof rb;
            if (ExtAudioFileGetProperty(fb, kExtAudioFileProperty_FileDataFormat, &sz, &rb) != noErr) break;
            if (ExtAudioFileGetProperty(fa, kExtAudioFileProperty_FileLengthFrames, &lsz, &la) != noErr) break;
            lsz = sizeof lb;
            if (ExtAudioFileGetProperty(fb, kExtAudioFileProperty_FileLengthFrames, &lsz, &lb) != noErr) break;
            if (ra.mSampleRate != rb.mSampleRate || la != lb || la <= 0) break;
            AudioStreamBasicDescription pcm = pcm16(ra.mSampleRate);
            if (ExtAudioFileSetProperty(fa, kExtAudioFileProperty_ClientDataFormat, sizeof pcm, &pcm) != noErr ||
                ExtAudioFileSetProperty(fb, kExtAudioFileProperty_ClientDataFormat, sizeof pcm, &pcm) != noErr) break;
            static const UInt32 kChunk = 1 << 16;
            std::vector<int16_t> x((size_t)kChunk * 2), y((size_t)kChunk * 2);
            SInt64 total = 0;
            same = true;
            for (;;) {
                UInt32 na = 0, nb = 0;
                if (!readFrames(fa, x.data(), kChunk, &na) || !readFrames(fb, y.data(), kChunk, &nb) || na != nb ||
                    memcmp(x.data(), y.data(), (size_t)na * 4) != 0) { same = false; break; }
                if (na == 0) break;
                total += na;
            }
            if (total != la) same = false;
        } while (false);
        if (fa) ExtAudioFileDispose(fa);
        if (fb) ExtAudioFileDispose(fb);
        return same;
    }
}

}  // namespace audiofile
