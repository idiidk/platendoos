// Windows: the stemcodec.h backend. LZFSE from the reference library (third_party/lzfse, the same format as macOS'
// libcompression); AAC-LC through Media Foundation's AAC encoder and decoder (built into Windows).
//   - Windows' encoder takes 16-bit PCM and goes up to 192 kbps (24000 bytes/s): higher settings (stemcache_quality
//     256, the default) are encoded at 192. 192 kbps AAC-LC per stereo stem is near-transparent, and a stem is only
//     heard while another is turned down.
//   - The encoder's delay (where the music starts in the decoded output) is measured once with a test signal and
//     stored in the chunk as `prime`, as the Mac stores AudioConverter's 2112, so chunks stay sample exact. Raw
//     packets need no cookie (AAC-LC, 44.1 kHz, stereo is fixed): Windows writes none and ignores the Mac's.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mferror.h>
#include <mftransform.h>
#include "stemcodec.h"
#include "../core/rbmod.h"
#include <cmath>
#include <mutex>
extern "C" {
#include "../third_party/lzfse/lzfse.h"
}

#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfuuid.lib")

namespace stemcodec::backend {

bool lzfseEncode(const uint8_t* src, size_t n, std::vector<uint8_t>& out) {
    std::vector<uint8_t> scratch(lzfse_encode_scratch_size());
    out.resize(n + n / 8 + 4096);
    size_t got = lzfse_encode_buffer(out.data(), out.size(), src, n, scratch.data());
    out.resize(got);
    return got > 0;
}
bool lzfseDecode(const uint8_t* src, size_t n, uint8_t* dst, size_t dstLen) {
    std::vector<uint8_t> scratch(lzfse_decode_scratch_size());
    // (one byte of room: a stream decoding to more than dstLen comes out as dstLen + 1, not as an exact fit)
    std::vector<uint8_t> buf(dstLen + 1);
    size_t got = lzfse_decode_buffer(buf.data(), buf.size(), src, n, scratch.data());
    if (got != dstLen) return false;
    memcpy(dst, buf.data(), dstLen);
    return true;
}

namespace {

// CLSID_AACMFTEncoder, CLSID_CMSAACDecMFT (wmcodecdsp.h; spelled out to need no extra uuid library)
const CLSID kEncoder = {0x93AF0C51, 0x2275, 0x45d2, {0xA3, 0x5B, 0xF2, 0xBA, 0x21, 0xCA, 0xED, 0x00}};
const CLSID kDecoder = {0x32d186a7, 0x218f, 0x4c75, {0x88, 0x76, 0xdd, 0x77, 0x27, 0x3a, 0x89, 0x99}};
constexpr int kRate = 44100, kEdge = 2048;  // kEdge: as stemcodec_mac.mm (see aacEncode there)

template <class T>
struct Ptr {
    T* p = nullptr;
    Ptr() = default;
    Ptr(const Ptr&) = delete;
    ~Ptr() { if (p) p->Release(); }
    T** operator&() { return &p; }
    T* operator->() const { return p; }
    explicit operator bool() const { return p; }
};

struct Com {
    bool com;
    Com() {
        com = SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED));
        static std::once_flag once;
        std::call_once(once, [] { MFStartup(MF_VERSION, MFSTARTUP_LITE); });
    }
    ~Com() { if (com) CoUninitialize(); }
};

void say(const char* msg) {  // each message once
    static std::mutex m;
    static std::vector<const char*> said;
    std::lock_guard<std::mutex> lock(m);
    if (std::find(said.begin(), said.end(), msg) != said.end()) return;
    said.push_back(msg);
    rbm::logf("%s", msg);
}

bool pcmType(IMFMediaType** out, bool isFloat) {
    if (FAILED(MFCreateMediaType(out))) return false;
    IMFMediaType* t = *out;
    t->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
    t->SetGUID(MF_MT_SUBTYPE, isFloat ? MFAudioFormat_Float : MFAudioFormat_PCM);
    int bytes = isFloat ? 4 : 2;
    t->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, bytes * 8);
    t->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, kRate);
    t->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, 2);
    t->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, 2 * bytes);
    t->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, kRate * 2 * bytes);
    return true;
}

bool aacType(IMFMediaType** out, UINT32 bytesPerSec, bool decoder) {
    if (FAILED(MFCreateMediaType(out))) return false;
    IMFMediaType* t = *out;
    t->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
    t->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_AAC);
    t->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
    t->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, kRate);
    t->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, 2);
    t->SetUINT32(MF_MT_AAC_PAYLOAD_TYPE, 0);  // raw packets
    t->SetUINT32(MF_MT_AAC_AUDIO_PROFILE_LEVEL_INDICATION, 0x29);
    if (bytesPerSec) t->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, bytesPerSec);
    if (decoder) {
        // HEAACWAVEINFO's fields after WAVEFORMATEX (payload 0 = raw, profile 0x29), then the AudioSpecificConfig:
        // AAC-LC (2), 44.1 kHz (index 4), 2 channels -> 0x12 0x10
        uint8_t ud[14] = {0, 0, 0x29, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x12, 0x10};
        t->SetBlob(MF_MT_USER_DATA, ud, sizeof ud);
    }
    return true;
}

bool makeSample(const void* data, DWORD bytes, LONGLONG time, LONGLONG dur, IMFSample** out) {
    Ptr<IMFMediaBuffer> b;
    if (FAILED(MFCreateSample(out)) || FAILED(MFCreateMemoryBuffer(bytes, &b))) return false;
    BYTE* p = nullptr;
    if (FAILED(b->Lock(&p, nullptr, nullptr))) return false;
    memcpy(p, data, bytes);
    b->Unlock();
    b->SetCurrentLength(bytes);
    (*out)->AddBuffer(b.p);
    (*out)->SetSampleTime(time);
    (*out)->SetSampleDuration(dur);
    return true;
}

// Pulls what the transform has ready; each output sample's bytes go to got(). False on an error.
template <class F>
bool drainOutput(IMFTransform* t, F got) {
    MFT_OUTPUT_STREAM_INFO info{};
    t->GetOutputStreamInfo(0, &info);
    bool provides = info.dwFlags & (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES | MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES);
    DWORD size = std::max<DWORD>(info.cbSize, 1 << 16);
    for (;;) {
        Ptr<IMFSample> s;
        Ptr<IMFMediaBuffer> b;
        MFT_OUTPUT_DATA_BUFFER db{};
        if (!provides) {
            if (FAILED(MFCreateSample(&s)) || FAILED(MFCreateMemoryBuffer(size, &b))) return false;
            s->AddBuffer(b.p);
            db.pSample = s.p;
        }
        DWORD status = 0;
        HRESULT hr = t->ProcessOutput(0, 1, &db, &status);
        if (db.pEvents) db.pEvents->Release();
        if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) return true;
        if (hr == MF_E_TRANSFORM_STREAM_CHANGE) continue;
        if (FAILED(hr)) return false;
        IMFSample* out = db.pSample;
        Ptr<IMFMediaBuffer> cb;
        if (out && SUCCEEDED(out->ConvertToContiguousBuffer(&cb))) {
            BYTE* p = nullptr;
            DWORD n = 0;
            if (SUCCEEDED(cb->Lock(&p, nullptr, &n))) {
                got(p, n);
                cb->Unlock();
            }
        }
        if (provides && out) out->Release();
    }
}

// Raw AAC packets for interleaved 16-bit stereo; false on an error.
bool encodeRaw(const std::vector<int16_t>& pcm, UINT32 bytesPerSec, std::vector<std::vector<uint8_t>>& packets) {
    Ptr<IMFTransform> t;
    if (FAILED(CoCreateInstance(kEncoder, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&t)))) return false;
    Ptr<IMFMediaType> in, out;
    if (!aacType(&out, bytesPerSec, false) || FAILED(t->SetOutputType(0, out.p, 0))) return false;
    if (!pcmType(&in, false) || FAILED(t->SetInputType(0, in.p, 0))) return false;
    t->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
    t->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
    auto got = [&](const BYTE* p, DWORD n) { packets.emplace_back(p, p + n); };
    const size_t frames = pcm.size() / 2, step = 4096;
    for (size_t at = 0; at < frames; at += step) {
        size_t n = std::min(step, frames - at);
        Ptr<IMFSample> s;
        if (!makeSample(pcm.data() + at * 2, (DWORD)(n * 4), (LONGLONG)(at * 10000000ull / kRate), (LONGLONG)(n * 10000000ull / kRate), &s))
            return false;
        HRESULT hr;
        while ((hr = t->ProcessInput(0, s.p, 0)) == MF_E_NOTACCEPTING)
            if (!drainOutput(t.p, got)) return false;
        if (FAILED(hr) || !drainOutput(t.p, got)) return false;
    }
    t->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
    t->ProcessMessage(MFT_MESSAGE_COMMAND_DRAIN, 0);
    return drainOutput(t.p, got);
}

// Every decoded frame of the packets (interleaved float stereo); false on an error.
bool decodeRaw(const uint16_t* sizes, const uint8_t* data, int packets, std::vector<float>& pcm) {
    Ptr<IMFTransform> t;
    if (FAILED(CoCreateInstance(kDecoder, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&t)))) return false;
    Ptr<IMFMediaType> in, out;
    if (!aacType(&in, 0, true) || FAILED(t->SetInputType(0, in.p, 0))) return false;
    bool isFloat = pcmType(&out, true) && SUCCEEDED(t->SetOutputType(0, out.p, 0));
    if (!isFloat) {
        Ptr<IMFMediaType> o16;
        if (!pcmType(&o16, false) || FAILED(t->SetOutputType(0, o16.p, 0))) return false;
    }
    t->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
    t->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
    auto got = [&](const BYTE* p, DWORD n) {
        if (isFloat) pcm.insert(pcm.end(), (const float*)p, (const float*)(p + n));
        else for (DWORD i = 0; i + 1 < n; i += 2) pcm.push_back(*(const int16_t*)(p + i) / 32768.f);
    };
    size_t off = 0;
    for (int k = 0; k < packets; k++) {
        Ptr<IMFSample> s;
        if (!makeSample(data + off, sizes[k], (LONGLONG)k * 1024 * 10000000ll / kRate, 1024 * 10000000ll / kRate, &s)) return false;
        off += sizes[k];
        HRESULT hr;
        while ((hr = t->ProcessInput(0, s.p, 0)) == MF_E_NOTACCEPTING)
            if (!drainOutput(t.p, got)) return false;
        if (FAILED(hr) || !drainOutput(t.p, got)) return false;
    }
    t->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
    t->ProcessMessage(MFT_MESSAGE_COMMAND_DRAIN, 0);
    return drainOutput(t.p, got);
}

// Where the music starts in what the decoder returns for this encoder's packets: a noise burst through both,
// found again by cross-correlation. Measured once (-1: couldn't).
int codecDelay() {
    static int delay = -2;
    static std::once_flag once;
    std::call_once(once, [] {
        const int n = 16384, at = 4096;
        std::vector<int16_t> pcm((size_t)n * 2, 0);
        std::vector<float> ref(n, 0.f);
        uint32_t seed = 12345;
        for (int i = at; i < at + 4096; i++) {
            seed = seed * 1664525u + 1013904223u;
            float v = ((int)(seed >> 9) - (1 << 22)) / (float)(1 << 22) * 0.4f;
            ref[i] = v;
            pcm[2 * i] = pcm[2 * i + 1] = (int16_t)lrintf(v * 32767);
        }
        std::vector<std::vector<uint8_t>> packets;
        std::vector<float> out;
        if (!encodeRaw(pcm, 24000, packets) || packets.empty()) return void(delay = -1);
        std::vector<uint16_t> sizes;
        std::vector<uint8_t> bytes;
        for (auto& p : packets) sizes.push_back((uint16_t)p.size()), bytes.insert(bytes.end(), p.begin(), p.end());
        if (!decodeRaw(sizes.data(), bytes.data(), (int)sizes.size(), out)) return void(delay = -1);
        size_t frames = out.size() / 2;
        double best = 0;
        int lag = -1;
        for (int d = 0; d <= 6144; d++) {
            double c = 0, e = 0;
            for (int i = at; i < at + 4096 && i + d < (int)frames; i++) c += ref[i] * out[2 * (i + d)], e += (double)out[2 * (i + d)] * out[2 * (i + d)];
            double score = e > 0 ? c / std::sqrt(e) : 0;
            if (score > best) best = score, lag = d;
        }
        delay = lag;
        rbm::logf("stemcache: Windows' AAC encoder delay %d samples", delay);
    });
    return delay;
}

}  // namespace

bool aacEncode(const float* l, const float* r, int samples, int kbps, AacStream& out) {
    Com com;
    // 12000 / 16000 / 20000 / 24000 bytes per second are what the encoder takes: the highest at or below kbps
    UINT32 bytesPerSec = kbps >= 192 ? 24000 : kbps >= 160 ? 20000 : kbps >= 128 ? 16000 : 12000;
    if (kbps > 192) say("stemcache: AAC stems at 192 kbps (the highest Windows' encoder makes)");
    int delay = codecDelay();
    if (delay < 0) {
        say("stemcache: Windows' AAC encoder isn't available (stored lossless instead)");
        return false;
    }
    // the chunk extended at both ends by an odd reflection, as on macOS
    std::vector<int16_t> il((size_t)(samples + 2 * kEdge) * 2, 0);
    auto at = [&](int i, int c) { return c ? r[i] : l[i]; };
    auto q = [](float v) { return (int16_t)lrintf(std::max(-1.f, std::min(1.f, v)) * 32767); };
    for (int c = 0; c < 2; c++) {
        for (int i = 0; i < samples; i++) il[2 * (i + kEdge) + c] = q(at(i, c));
        for (int k = 1; k <= kEdge; k++) {
            int a = std::min(k, samples - 1);
            il[2 * (kEdge - k) + c] = q(2 * at(0, c) - at(a, c));
            il[2 * (kEdge + samples - 1 + k) + c] = q(2 * at(samples - 1, c) - at(samples - 1 - a, c));
        }
    }
    std::vector<std::vector<uint8_t>> packets;
    if (!encodeRaw(il, bytesPerSec, packets)) return false;
    for (auto& p : packets) {
        if (p.size() > 65535) return false;
        out.sizes.push_back((uint16_t)p.size());
        out.bytes.insert(out.bytes.end(), p.begin(), p.end());
    }
    out.prime = (uint32_t)(delay + kEdge);
    out.cookieLen = 0;
    return !out.sizes.empty() && (size_t)out.sizes.size() * 1024 >= (size_t)out.prime + samples;
}

bool aacDecode(const uint8_t*, uint32_t, uint32_t prime, int samples, const uint16_t* sizes, const uint8_t* data,
               size_t dataLen, int packets, float scale, float* l, float* r) {
    Com com;
    size_t total = 0;
    for (int p = 0; p < packets; p++) total += sizes[p];
    if (total > dataLen) return false;
    std::vector<float> pcm;
    if (!decodeRaw(sizes, data, packets, pcm)) {
        say("stemcache: Windows' AAC decoder failed");
        return false;
    }
    if (pcm.size() / 2 < (size_t)prime + samples) return false;
    const float* p = pcm.data() + (size_t)prime * 2;
    for (int i = 0; i < samples; i++) l[i] = p[2 * i] * scale, r[i] = p[2 * i + 1] * scale;
    return true;
}

}  // namespace stemcodec::backend
