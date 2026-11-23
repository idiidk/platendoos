// AIFF and AIFF-C files, read without the OS: Media Foundation (Windows) has no reader for them, and DJs who play
// on older CDJs keep whole libraries as .aif. Uncompressed audio only, which is what these files hold in practice:
// AIFF's big-endian PCM (8 to 32 bits) and AIFF-C's NONE/twos, sowt (little-endian), raw (unsigned 8-bit), in24,
// in32, fl32 and fl64.
// Anything else (ima4, ulaw...) is refused. macOS reads AIFF itself (audiofile_mac.mm); tools/dev/aiff_test.cpp
// checks this reader against it.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <istream>
#include <vector>

namespace aiff {

struct Format {
    int channels = 0, bits = 0;  // bits per sample as stored (the sample fills whole bytes, left-justified)
    double rate = 0;
    long long frames = 0;
    bool little = false, floating = false, offset8 = false;  // offset8: unsigned 8-bit (AIFF-C 'raw ')
    long long data = 0;  // where the samples start in the file
    int bytes() const { return (bits + 7) / 8; }
};

inline uint32_t be32(const unsigned char* p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }

// True for a file that starts as an AIFF or AIFF-C does (the stream is left at its start).
inline bool is(std::istream& f) {
    unsigned char h[12] = {};
    f.clear();
    f.seekg(0);
    f.read((char*)h, 12);
    bool yes = f.gcount() == 12 && !memcmp(h, "FORM", 4) && (!memcmp(h + 8, "AIFF", 4) || !memcmp(h + 8, "AIFC", 4));
    f.clear();
    f.seekg(0);
    return yes;
}

// The header: false for a file that isn't AIFF, is cut short, or holds compressed audio.
inline bool open(std::istream& f, Format* out) {
    if (!is(f)) return false;
    f.seekg(0, std::ios::end);
    long long size = f.tellg();
    bool comm = false, ssnd = false;
    long long dataBytes = 0;
    Format x;
    // The chunks, in whatever order (COMM may follow the samples); each is padded to an even length.
    for (long long at = 12; at + 8 <= size;) {
        unsigned char h[8];
        f.seekg(at);
        f.read((char*)h, 8);
        if (f.gcount() != 8) break;
        long long len = be32(h + 4), body = at + 8;
        if (!memcmp(h, "COMM", 4) && len >= 18) {
            unsigned char c[22] = {};
            f.read((char*)c, (std::streamsize)std::min<long long>(len, 22));
            if (f.gcount() < 18) return false;
            x.channels = c[0] << 8 | c[1];
            x.frames = be32(c + 2);
            x.bits = c[6] << 8 | c[7];
            // the rate: an 80-bit extended float (sign and exponent, then a 64-bit mantissa with its leading 1)
            int exponent = (c[8] & 0x7f) << 8 | c[9];
            uint64_t mantissa = (uint64_t)be32(c + 10) << 32 | be32(c + 14);
            x.rate = std::ldexp((double)mantissa, exponent - 16383 - 63);
            if (f.gcount() >= 22) {  // AIFF-C: how the samples are stored
                const unsigned char* t = c + 18;
                if (!memcmp(t, "sowt", 4)) x.little = true;
                else if (!memcmp(t, "raw ", 4)) x.offset8 = true, x.bits = 8;
                else if (!memcmp(t, "fl32", 4) || !memcmp(t, "FL32", 4)) x.floating = true, x.bits = 32;
                else if (!memcmp(t, "fl64", 4) || !memcmp(t, "FL64", 4)) x.floating = true, x.bits = 64;
                else if (!memcmp(t, "in24", 4)) x.bits = 24;
                else if (!memcmp(t, "in32", 4)) x.bits = 32;
                else if (memcmp(t, "NONE", 4) && memcmp(t, "twos", 4)) return false;
            }
            comm = true;
        } else if (!memcmp(h, "SSND", 4) && len >= 8) {
            unsigned char s[8];
            f.read((char*)s, 8);
            if (f.gcount() != 8) return false;
            x.data = body + 8 + be32(s);  // (offset: bytes to skip before the first sample)
            dataBytes = std::min(body + len, size) - x.data;
            ssnd = true;
        }
        at = body + len + (len & 1);
    }
    f.clear();
    if (!comm || !ssnd || x.channels < 1 || x.channels > 64 || x.rate < 1000 || x.rate > 768000) return false;
    if (x.bits < 1 || (x.bits > 32 && !x.floating)) return false;
    // (a file cut short, or one whose header counts more frames than it holds: what is there)
    x.frames = std::min(x.frames, dataBytes / ((long long)x.channels * x.bytes()));
    if (x.frames <= 0) return false;
    *out = x;
    return true;
}

// One sample as 16 bits: an integer's top 16 bits (a 16-bit file's samples exactly), a float scaled and clipped.
inline int16_t sample16(const unsigned char* p, const Format& x) {
    int n = x.bytes();
    unsigned char b[8];  // big-endian from here on
    for (int i = 0; i < n; i++) b[i] = x.little ? p[n - 1 - i] : p[i];
    if (x.floating) {
        double v;
        if (n == 4) {
            uint32_t u = be32(b);
            float fl;
            memcpy(&fl, &u, 4);
            v = fl;
        } else {
            uint64_t u = (uint64_t)be32(b) << 32 | be32(b + 4);
            memcpy(&v, &u, 8);
        }
        if (!(v == v)) v = 0;
        return (int16_t)std::lrint(std::clamp(v, -1.0, 32767.0 / 32768) * 32768);
    }
    if (n == 1) return (int16_t)((x.offset8 ? b[0] - 128 : (int8_t)b[0]) * 256);
    return (int16_t)(b[0] << 8 | b[1]);
}

// The whole file as 16-bit stereo, interleaved: a mono file on both sides, of more channels the first two.
inline bool readStereo16(std::istream& f, const Format& x, std::vector<int16_t>* out) {
    const int frame = x.channels * x.bytes(), right = x.channels > 1 ? x.bytes() : 0;
    const long long kBlock = 16384;
    std::vector<unsigned char> buf((size_t)(kBlock * frame));
    out->clear();
    out->reserve((size_t)x.frames * 2);
    f.clear();
    f.seekg(x.data);
    for (long long done = 0; done < x.frames;) {
        long long want = std::min(kBlock, x.frames - done);
        f.read((char*)buf.data(), (std::streamsize)(want * frame));
        long long got = f.gcount() / frame;
        for (long long i = 0; i < got; i++) {
            const unsigned char* p = buf.data() + i * frame;
            out->push_back(sample16(p, x));
            out->push_back(sample16(p + right, x));
        }
        done += got;
        if (got < want) break;
    }
    return !out->empty();
}

}  // namespace aiff
