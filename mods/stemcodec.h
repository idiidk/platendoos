// stemcodec: the on-disk format of a stemcache chunk (header-only; stemcache.cpp uses it). The AAC encoder/decoder
// and the LZFSE packer are a backend (stemcodec::backend, below), implemented for macOS in stemcodec_mac.mm.
// A chunk is n planar float channels (stereo pairs, one pair per stem) of `samples` samples at 44.1 kHz.
//   "RBS2" lossless: 16-bit PCM scaled per channel, packed (mid/side, order-2 prediction, byte planes, LZFSE).
//   "RBS3" lossy: one AAC-LC stream per stem, raw packets (no m4a): header, then per stem the packet sizes (u16)
//          and the packet bytes. Raw packets decoded with AudioConverter are sample exact: the encoder's priming
//          (kAudioConverterPrimeInfo leadingFrames, 2112) is stored in the header and dropped by the decoder, the
//          input is extended at both ends by an odd reflection (kEdge samples, dropped again: a chunk starts and ends
//          mid-music, and a step from silence would smear noise over its edges), so the packets always cover
//          prime + samples, and the decoder returns exactly `samples`. Float input above +-1 is scaled down per stem before encoding (the scale is
//          stored, and applied back by the decoder), so the encoder never sees a level it might clip.
// Checked: "RBC2" / "RBC3" are the same with a CRC-32 right after the header, of the header and everything after the
// CRC, so a chunk whose payload didn't reach the disk (a stick pulled without ejecting) is never served.
// decode() reads all four; encode(kbps = 0) writes RBC2, otherwise RBC3.
#pragma once
#include "../core/plat.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace stemcodec {

// The codec backend: what the platform provides (macOS: stemcodec_mac.mm).
namespace backend {
// LZFSE-compress n bytes; false if that fails. lzfseDecode: exactly dstLen bytes must come out.
bool lzfseEncode(const uint8_t* src, size_t n, std::vector<uint8_t>& out);
bool lzfseDecode(const uint8_t* src, size_t n, uint8_t* dst, size_t dstLen);

// One stereo stem as raw AAC-LC packets (see the format description above).
struct AacStream {
    std::vector<uint16_t> sizes;
    std::vector<uint8_t> bytes;
    uint32_t prime = 0, cookieLen = 0;
    uint8_t cookie[64];
};
// l, r: planar, scaled already. Encodes with kEdge reflected samples at both ends (prime includes them).
bool aacEncode(const float* l, const float* r, int samples, int kbps, AacStream& out);
// Exactly `samples` frames of one stem, times scale, into l and r.
bool aacDecode(const uint8_t* cookie, uint32_t cookieLen, uint32_t prime, int samples, const uint16_t* sizes,
               const uint8_t* data, size_t dataLen, int packets, float scale, float* l, float* r);
}  // namespace backend

namespace {

// CRC-32 (zlib's polynomial), slicing by 8: zlib isn't linked on Windows, and this runs on rekordbox's separation
// threads (~1 ms for a 1.5 MB chunk; a byte at a time took ~4).
struct CrcTable {
    uint32_t t[8][256];
    constexpr CrcTable() : t() {
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            t[0][i] = c;
        }
        for (int k = 1; k < 8; k++)
            for (int i = 0; i < 256; i++) t[k][i] = t[0][t[k - 1][i] & 0xff] ^ (t[k - 1][i] >> 8);
    }
};
constexpr CrcTable kCrc;
// Continues crc (start with 0) over n more bytes.
uint32_t crc32(uint32_t crc, const void* data, size_t n) {
    const uint8_t* p = (const uint8_t*)data;
    crc = ~crc;
    for (; n >= 8; n -= 8, p += 8) {
        uint32_t a = crc ^ ((uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24);
        crc = kCrc.t[7][a & 0xff] ^ kCrc.t[6][(a >> 8) & 0xff] ^ kCrc.t[5][(a >> 16) & 0xff] ^ kCrc.t[4][a >> 24] ^
              kCrc.t[3][p[4]] ^ kCrc.t[2][p[5]] ^ kCrc.t[1][p[6]] ^ kCrc.t[0][p[7]];
    }
    for (; n; n--, p++) crc = kCrc.t[0][(crc ^ *p) & 0xff] ^ (crc >> 8);
    return ~crc;
}

struct Hdr2 {
    char magic[4];  // "RBS2"
    uint32_t outputs, samples, packed;
    float scale[8];  // per output channel: int16 * scale = sample
};
struct Hdr3 {
    char magic[4];  // "RBS3"
    uint32_t outputs, samples;
    uint16_t codec, kbps;  // codec 1 = AAC-LC, kbps per stereo stem
    uint32_t prime;        // decoded frames to drop at the start
    uint32_t cookieLen;
    uint8_t cookie[64];    // the encoder's magic cookie
    uint32_t packets[4];   // per stem
    float scale[4];        // per stem: decoded sample * scale = sample
};

// ---- lossless ---------------------------------------------------------------------------

// The 16-bit channels (stereo pairs) as packed bytes: each pair as mid and side, each channel as the residual of
// an order-2 prediction, zigzag-coded, in three byte planes; then LZFSE.
std::vector<uint8_t> pack(const std::vector<int16_t>& pcm, int n, int samples) {
    size_t total = (size_t)n * samples;
    std::vector<uint8_t> planes(3 * total);
    std::vector<int32_t> x(samples);
    for (int o = 0; o < n; o++) {
        const int16_t* a = pcm.data() + (size_t)(o & ~1) * samples;
        const int16_t* b = a + samples;
        for (int i = 0; i < samples; i++) {
            int32_t side = a[i] - b[i];
            x[i] = o & 1 ? side : b[i] + (side >> 1);
        }
        for (int i = 0; i < samples; i++) {
            int32_t p = i > 1 ? 2 * x[i - 1] - x[i - 2] : i ? x[i - 1] : 0;
            int32_t e = x[i] - p;
            uint32_t z = (uint32_t)(e << 1) ^ (uint32_t)(e >> 31);
            size_t k = (size_t)o * samples + i;
            planes[k] = z & 0xff, planes[total + k] = (z >> 8) & 0xff, planes[2 * total + k] = (z >> 16) & 0xff;
        }
    }
    std::vector<uint8_t> out;
    backend::lzfseEncode(planes.data(), planes.size(), out);
    return out;
}
bool unpack(const uint8_t* data, size_t len, int n, int samples, std::vector<int16_t>& pcm) {
    size_t total = (size_t)n * samples;
    std::vector<uint8_t> planes(3 * total);
    if (!backend::lzfseDecode(data, len, planes.data(), planes.size())) return false;
    std::vector<int32_t> x((size_t)n * samples);
    for (int o = 0; o < n; o++) {
        int32_t* c = x.data() + (size_t)o * samples;
        for (int i = 0; i < samples; i++) {
            size_t k = (size_t)o * samples + i;
            uint32_t z = planes[k] | planes[total + k] << 8 | planes[2 * total + k] << 16;
            int32_t e = (int32_t)(z >> 1) ^ -(int32_t)(z & 1);
            c[i] = e + (i > 1 ? 2 * c[i - 1] - c[i - 2] : i ? c[i - 1] : 0);
        }
    }
    pcm.resize(total);
    for (int o = 0; o + 1 < n; o += 2)
        for (int i = 0; i < samples; i++) {
            int32_t mid = x[(size_t)o * samples + i], side = x[(size_t)(o + 1) * samples + i];
            int32_t r = mid - (side >> 1);
            pcm[(size_t)o * samples + i] = (int16_t)(r + side), pcm[(size_t)(o + 1) * samples + i] = (int16_t)r;
        }
    return true;
}

std::string encodeLossless(int samples, const float* const* in, int n) {
    Hdr2 h = {};
    memcpy(h.magic, "RBS2", 4);
    h.outputs = n, h.samples = samples;
    std::vector<int16_t> pcm((size_t)n * samples);
    for (int o = 0; o < n; o++) {
        float peak = 0;
        for (int i = 0; i < samples; i++) peak = std::max(peak, std::fabs(in[o][i]));
        float scale = std::max(peak, 1e-9f) / 32767;
        h.scale[o] = scale;
        int16_t* p = pcm.data() + (size_t)o * samples;
        for (int i = 0; i < samples; i++) p[i] = (int16_t)lrintf(in[o][i] / scale);
    }
    std::vector<uint8_t> packed = pack(pcm, n, samples);
    if (packed.empty()) return "";
    h.packed = (uint32_t)packed.size();
    std::string s((const char*)&h, sizeof h);
    s.append((const char*)packed.data(), packed.size());
    return s;
}

std::string encodeAac(int samples, const float* const* in, int n, int kbps) {
    int stems = n / 2;
    if (n % 2 || stems < 1 || stems > 4) return "";
    Hdr3 h = {};
    memcpy(h.magic, "RBS3", 4);
    h.outputs = n, h.samples = samples, h.codec = 1, h.kbps = (uint16_t)kbps;
    std::vector<backend::AacStream> st(stems);
    std::vector<char> ok(stems, 0);
    // Peaks above 0.99 are scaled down (the encoder's float path isn't guaranteed to be linear above full scale).
    for (int s = 0; s < stems; s++) {
        float peak = 0;
        for (int c = 0; c < 2; c++)
            for (int i = 0; i < samples; i++) peak = std::max(peak, std::fabs(in[2 * s + c][i]));
        h.scale[s] = peak > 0.99f ? peak / 0.99f : 1.f;
    }
    auto work = [&](int s) {
        std::vector<float> a(samples), b(samples);
        for (int i = 0; i < samples; i++) a[i] = in[2 * s][i] / h.scale[s], b[i] = in[2 * s + 1][i] / h.scale[s];
        ok[s] = backend::aacEncode(a.data(), b.data(), samples, kbps, st[s]);
    };
    for (int s = 0; s < stems; s++) work(s);
    for (int s = 0; s < stems; s++) {
        if (!ok[s] || st[s].cookieLen > sizeof h.cookie || st[s].prime != st[0].prime) return "";
        h.packets[s] = (uint32_t)st[s].sizes.size();
    }
    h.prime = st[0].prime, h.cookieLen = st[0].cookieLen;
    memcpy(h.cookie, st[0].cookie, st[0].cookieLen);
    std::string out((const char*)&h, sizeof h);
    for (int s = 0; s < stems; s++) {
        out.append((const char*)st[s].sizes.data(), st[s].sizes.size() * 2);
        out.append((const char*)st[s].bytes.data(), st[s].bytes.size());
    }
    return out;
}

// The CRC of a checked chunk whose header is hdr bytes: header and payload, not the CRC's own 4 bytes.
uint32_t chunkCrc(const std::string& s, size_t hdr) {
    return crc32(crc32(0, s.data(), hdr), s.data() + hdr + 4, s.size() - hdr - 4);
}
// An RBS2/RBS3 chunk made a checked one (RBC2/RBC3).
std::string seal(std::string s, size_t hdr) {
    if (s.size() < hdr) return "";
    s[2] = 'C';
    s.insert(hdr, 4, '\0');
    uint32_t c = chunkCrc(s, hdr);
    memcpy(&s[hdr], &c, 4);
    return s;
}

}  // namespace

// kbps 0 = lossless.
inline std::string encode(int samples, const float* const* in, int n, int kbps) {
    if (kbps > 0) {  // (no AAC encoder, as on Windows for now: lossless instead)
        std::string s = encodeAac(samples, in, n, kbps);
        if (!s.empty()) return seal(std::move(s), sizeof(Hdr3));
    }
    std::string s = encodeLossless(samples, in, n);
    return s.empty() ? s : seal(std::move(s), sizeof(Hdr2));
}

// Fills out[0..n) (each `samples` floats) from a stored chunk; false if it isn't one, or doesn't fit n/samples.
// *damaged (if given) is set when a checked chunk's CRC doesn't match: its file is bad, not just another layout.
inline bool decode(const std::string& data, int samples, float* const* out, int n, bool* damaged = nullptr) {
    if (damaged) *damaged = false;
    if (data.size() < 4 || memcmp(data.data(), "RB", 2) || (data[3] != '2' && data[3] != '3')) return false;
    bool checked = data[2] == 'C';
    if (!checked && data[2] != 'S') return false;
    size_t hdr = data[3] == '2' ? sizeof(Hdr2) : sizeof(Hdr3);
    if (data.size() < hdr + (checked ? 4 : 0)) {
        if (damaged) *damaged = checked;
        return false;
    }
    if (checked) {
        uint32_t c;
        memcpy(&c, data.data() + hdr, 4);
        if (c != chunkCrc(data, hdr)) {
            if (damaged) *damaged = true;
            return false;
        }
    }
    size_t body = hdr + (checked ? 4 : 0);  // where the payload starts (a multiple of 4)
    if (data[3] == '2') {
        Hdr2 h;
        memcpy(&h, data.data(), sizeof h);
        if ((int)h.outputs != n || (int)h.samples != samples || n > 8) return false;
        std::vector<int16_t> pcm;
        if (!unpack((const uint8_t*)data.data() + body, data.size() - body, n, samples, pcm)) return false;
        for (int o = 0; o < n; o++) {
            const int16_t* p = pcm.data() + (size_t)o * samples;
            for (int i = 0; i < samples; i++) out[o][i] = p[i] * h.scale[o];
        }
        return true;
    }
    {
        Hdr3 h;
        memcpy(&h, data.data(), sizeof h);
        int stems = n / 2;
        if ((int)h.outputs != n || (int)h.samples != samples || n % 2 || stems < 1 || stems > 4 || h.codec != 1 ||
            h.cookieLen > sizeof h.cookie)
            return false;
        // (a damaged file: the decoder sizes its buffers by these, and an allocation that fails ends rekordbox)
        if (h.prime > 65536) return false;
        for (int s = 0; s < stems; s++)
            if (h.packets[s] > ((uint64_t)h.prime + h.samples) / 1024 + 64) return false;
        // Each stem's section: packet sizes, then the bytes (their sum).
        struct Sec { const uint16_t* sizes; const uint8_t* bytes; size_t len; };
        std::vector<Sec> secv(4);
        Sec* sec = secv.data();
        size_t off = body;
        for (int s = 0; s < stems; s++) {
            size_t sb = (size_t)h.packets[s] * 2;
            if (off + sb > data.size()) return false;
            sec[s].sizes = (const uint16_t*)(data.data() + off);  // 2-aligned: body is a multiple of 4
            off += sb;
            size_t len = 0;
            for (uint32_t p = 0; p < h.packets[s]; p++) len += sec[s].sizes[p];
            if (off + len > data.size()) return false;
            sec[s].bytes = (const uint8_t*)data.data() + off, sec[s].len = len;
            off += len;
        }
        char okv[4] = {0, 0, 0, 0};
        char* ok = okv;
        rbm::plat::parallelFor(stems, [&](size_t s) {
            ok[s] = backend::aacDecode(h.cookie, h.cookieLen, h.prime, (int)h.samples, sec[s].sizes, sec[s].bytes, sec[s].len,
                                       (int)h.packets[s], h.scale[s], out[2 * s], out[2 * s + 1]);
        });
        for (int s = 0; s < stems; s++)
            if (!ok[s]) return false;
        return true;
    }
}

// "lossless" -> 0, "128".."320" -> that, anything else -> 256.
inline int qualityKbps(const std::string& q) {
    if (q == "lossless") return 0;
    int k = atoi(q.c_str());
    return k >= 64 && k <= 320 ? k : 256;
}

}  // namespace stemcodec
