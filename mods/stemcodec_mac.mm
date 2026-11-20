// stemcodec_mac.mm: the Apple side of stemcodec.h: AAC through AudioToolbox's AudioConverter, and LZFSE
// through libcompression. A Windows backend (Media Foundation AAC, another lossless packer) implements the same
// stemcodec::backend functions; note an LZFSE chunk (RBS2) can only be read by a backend that has LZFSE.
#include "stemcodec.h"
#include <AudioToolbox/AudioToolbox.h>
#include <compression.h>
#include <Foundation/Foundation.h>
#include <algorithm>

namespace stemcodec::backend {

bool lzfseEncode(const uint8_t* src, size_t n, std::vector<uint8_t>& out) {
    out.resize(n + 4096);
    out.resize(compression_encode_buffer(out.data(), out.size(), src, n, nullptr, COMPRESSION_LZFSE));
    return !out.empty();
}
bool lzfseDecode(const uint8_t* src, size_t n, uint8_t* dst, size_t dstLen) {
    return compression_decode_buffer(dst, dstLen, src, n, nullptr, COMPRESSION_LZFSE) == dstLen;
}

namespace {

AudioStreamBasicDescription pcmFormat() {
    AudioStreamBasicDescription f = {};
    f.mSampleRate = 44100, f.mFormatID = kAudioFormatLinearPCM;
    f.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked;
    f.mChannelsPerFrame = 2, f.mBitsPerChannel = 32, f.mBytesPerFrame = 8, f.mFramesPerPacket = 1, f.mBytesPerPacket = 8;
    return f;
}
AudioStreamBasicDescription aacFormat() {
    AudioStreamBasicDescription f = {};
    f.mSampleRate = 44100, f.mFormatID = kAudioFormatMPEG4AAC, f.mFormatFlags = kMPEG4Object_AAC_LC;
    f.mChannelsPerFrame = 2, f.mFramesPerPacket = 1024;
    return f;
}

constexpr int kEdge = 2048;  // see aacEncode
struct EncIn { const float* data; UInt32 frames; bool given; };
OSStatus encFeed(AudioConverterRef, UInt32* packets, AudioBufferList* io, AudioStreamPacketDescription**, void* user) {
    EncIn* s = (EncIn*)user;
    if (s->given) return *packets = 0, noErr;  // end of input: the converter flushes its tail
    s->given = true;
    *packets = s->frames;
    io->mNumberBuffers = 1;
    io->mBuffers[0].mNumberChannels = 2;
    io->mBuffers[0].mDataByteSize = s->frames * 8;
    io->mBuffers[0].mData = (void*)s->data;
    return noErr;
}

}  // namespace

// One stereo stem (planar l, r, scaled already) as AAC packets.
bool aacEncode(const float* l, const float* r, int samples, int kbps, AacStream& out) {
    // The chunk is cut from the middle of a track, so a start from silence would spread quantisation noise over its
    // first frames. Both ends are extended with an odd reflection (continuous in level and slope), kEdge samples
    // each; the decoder drops the extension (prime = encoder delay + kEdge).
    std::vector<float> il((size_t)(samples + 2 * kEdge) * 2, 0.f);
    auto at = [&](int i, int c) { return c ? r[i] : l[i]; };
    for (int c = 0; c < 2; c++) {
        for (int i = 0; i < samples; i++) il[2 * (i + kEdge) + c] = at(i, c);
        for (int k = 1; k <= kEdge; k++) {
            int a = std::min(k, samples - 1);
            il[2 * (kEdge - k) + c] = 2 * at(0, c) - at(a, c);
            il[2 * (kEdge + samples - 1 + k) + c] = 2 * at(samples - 1, c) - at(samples - 1 - a, c);
        }
    }
    AudioStreamBasicDescription in = pcmFormat(), dst = aacFormat();
    AudioConverterRef c = nullptr;
    if (AudioConverterNew(&in, &dst, &c) != noErr) return false;
    UInt32 br = (UInt32)kbps * 1000, q = kAudioConverterQuality_Max;
    AudioConverterSetProperty(c, kAudioConverterEncodeBitRate, sizeof br, &br);
    AudioConverterSetProperty(c, kAudioConverterCodecQuality, sizeof q, &q);
    AudioConverterPrimeInfo pi = {};
    UInt32 sz = sizeof pi;
    AudioConverterGetProperty(c, kAudioConverterPrimeInfo, &sz, &pi);
    out.prime = pi.leadingFrames + kEdge;
    out.cookieLen = sizeof out.cookie;
    if (AudioConverterGetProperty(c, kAudioConverterCompressionMagicCookie, &(sz = sizeof out.cookie), out.cookie) != noErr)
        out.cookieLen = 0;
    else
        out.cookieLen = sz;
    UInt32 maxPacket = 0;
    sz = sizeof maxPacket;
    AudioConverterGetProperty(c, kAudioConverterPropertyMaximumOutputPacketSize, &sz, &maxPacket);
    if (!maxPacket || maxPacket > 65535) maxPacket = 8192;
    EncIn feed = {il.data(), (UInt32)(samples + 2 * kEdge), false};
    const UInt32 batch = 32;
    std::vector<uint8_t> buf((size_t)batch * maxPacket);
    std::vector<AudioStreamPacketDescription> pd(batch);
    bool ok = true;
    for (;;) {
        AudioBufferList bl;
        bl.mNumberBuffers = 1;
        bl.mBuffers[0] = {2, (UInt32)buf.size(), buf.data()};
        UInt32 packets = batch;
        OSStatus st = AudioConverterFillComplexBuffer(c, encFeed, &feed, &packets, &bl, pd.data());
        for (UInt32 p = 0; p < packets; p++) {
            if (pd[p].mDataByteSize > 65535) ok = false;
            out.sizes.push_back((uint16_t)pd[p].mDataByteSize);
            const uint8_t* b = buf.data() + pd[p].mStartOffset;
            out.bytes.insert(out.bytes.end(), b, b + pd[p].mDataByteSize);
        }
        if (st != noErr || packets == 0) break;
    }
    AudioConverterDispose(c);
    return ok && !out.sizes.empty() && (size_t)out.sizes.size() * 1024 >= (size_t)out.prime + samples;
}

namespace {

struct DecIn { const uint8_t* data; UInt32 bytes; AudioStreamPacketDescription* pd; UInt32 packets; bool given; };
OSStatus decFeed(AudioConverterRef, UInt32* packets, AudioBufferList* io, AudioStreamPacketDescription** pdOut, void* user) {
    DecIn* s = (DecIn*)user;
    if (s->given) return *packets = 0, noErr;
    s->given = true;
    *packets = s->packets;
    io->mNumberBuffers = 1;
    io->mBuffers[0].mNumberChannels = 0;
    io->mBuffers[0].mDataByteSize = s->bytes;
    io->mBuffers[0].mData = (void*)s->data;
    if (pdOut) *pdOut = s->pd;
    return noErr;
}

}  // namespace

// Exactly `samples` frames of one stem, times the stem's scale, into l and r.
bool aacDecode(const uint8_t* cookie, uint32_t cookieLen, uint32_t prime, int samples, const uint16_t* sizes,
               const uint8_t* data, size_t dataLen, int packets, float scale, float* l, float* r) {
    std::vector<AudioStreamPacketDescription> pd(packets);
    size_t off = 0;
    for (int p = 0; p < packets; p++) {
        pd[p] = {(SInt64)off, 0, sizes[p]};
        off += sizes[p];
    }
    if (off > dataLen || (size_t)packets * 1024 < (size_t)prime + samples) return false;
    AudioStreamBasicDescription src = aacFormat(), out = pcmFormat();
    AudioConverterRef c = nullptr;
    if (AudioConverterNew(&src, &out, &c) != noErr) return false;
    if (cookieLen) AudioConverterSetProperty(c, kAudioConverterDecompressionMagicCookie, cookieLen, cookie);
    UInt32 want = prime + samples;
    std::vector<float> pcm((size_t)want * 2);
    DecIn feed = {data, (UInt32)off, pd.data(), (UInt32)packets, false};
    AudioBufferList bl;
    bl.mNumberBuffers = 1;
    bl.mBuffers[0] = {2, want * 8, pcm.data()};
    UInt32 frames = want;
    OSStatus st = AudioConverterFillComplexBuffer(c, decFeed, &feed, &frames, &bl, nullptr);
    AudioConverterDispose(c);
    if ((st != noErr && st != 1) || frames < want) return false;
    const float* p = pcm.data() + (size_t)prime * 2;
    for (int i = 0; i < samples; i++) l[i] = p[2 * i] * scale, r[i] = p[2 * i + 1] * scale;
    return true;
}

}  // namespace stemcodec::backend
