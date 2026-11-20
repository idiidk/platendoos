// Tests the audio codecs behind stemcodec.h and audiofile.h on either platform, without rekordbox:
//   codectest decode <chunk> <out.f32>                       a stem chunk -> raw planar floats
//   codectest encode <in.f32> <outputs> <samples> <kbps> <chunk>   raw planar floats -> a chunk
//   codectest compare <a.f32> <b.f32> <outputs> <samples>    per channel: best lag (should be 0) and SNR
//   codectest check [old chunk...]                            checked chunks (RBC2/RBC3): round trip, the same without
//                                                            the CRC (RBS2/RBS3) still decodes, a flipped byte is rejected;
//                                                            each old chunk given (any version) must still decode
// macOS: clang++ -std=c++20 -O2 -fobjc-arc tools/dev/codectest.cpp mods/stemcodec_mac.mm mods/audiofile_mac.mm
//        -framework AudioToolbox -framework Foundation -lcompression   (-DRBM_PUBLIC=1 without private/)
// Windows: built by tools/win/build.sh (codectest.exe, not in the package).
#include "../../mods/stemcodec.h"
#include "../../mods/audiofile.h"
#include <cstdarg>
#include <chrono>
#include <cstdio>
#include <thread>

namespace rbm {
void logf(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}
namespace plat {
void parallelFor(size_t n, const std::function<void(size_t)>& fn) {
    std::vector<std::thread> t;
    for (size_t i = 0; i < n; i++) t.emplace_back(fn, i);
    for (auto& x : t) x.join();
}
std::filesystem::path fsPath(const std::string& s) { return std::filesystem::u8path(s); }
FILE* openFile(const std::string& s, const char* mode) {
#ifdef _WIN32
    std::wstring m(mode, mode + strlen(mode));
    return _wfopen(fsPath(s).c_str(), m.c_str());
#else
    return fopen(s.c_str(), mode);
#endif
}
}  // namespace plat
}  // namespace rbm

static std::string slurp(const char* p) {
    std::string s;
    FILE* f = fopen(p, "rb");
    if (!f) return s;
    char b[1 << 16];
    size_t n;
    while ((n = fread(b, 1, sizeof b, f)) > 0) s.append(b, n);
    fclose(f);
    return s;
}

// Decodes a chunk with the layout its header gives; *damaged as decode sets it.
static bool decodeAny(const std::string& d, bool* damaged, std::vector<float>* pcm = nullptr) {
    if (d.size() < 12) return false;
    uint32_t outputs, samples;
    memcpy(&outputs, d.data() + 4, 4), memcpy(&samples, d.data() + 8, 4);
    if (outputs > 8 || samples > 2000000) return false;
    std::vector<float> buf((size_t)outputs * samples);
    std::vector<float*> ch(outputs);
    for (uint32_t o = 0; o < outputs; o++) ch[o] = buf.data() + (size_t)o * samples;
    bool ok = stemcodec::decode(d, (int)samples, ch.data(), (int)outputs, damaged);
    if (pcm) *pcm = std::move(buf);
    return ok;
}

static int check(int argc, char** argv) {
    int fails = 0;
    auto expect = [&](bool c, const char* what) {
        printf("%s: %s\n", c ? "ok  " : "FAIL", what);
        fails += !c;
    };
    const int n = 8, samples = 519168;
    std::vector<float> in((size_t)n * samples);
    uint32_t r = 1;
    for (int o = 0; o < n; o++)
        for (int i = 0; i < samples; i++) {
            r = r * 1664525 + 1013904223;
            in[(size_t)o * samples + i] = 0.3f * sinf(i * 0.01f * (o + 1)) + 0.05f * ((r >> 8) / 16777216.f - 0.5f);
        }
    std::vector<const float*> ch(n);
    for (int o = 0; o < n; o++) ch[o] = in.data() + (size_t)o * samples;
    for (int kbps : {256, 0}) {
        std::string c = stemcodec::encode(samples, ch.data(), n, kbps);
        char label[64];
        snprintf(label, sizeof label, "%s (%zu bytes)", c.substr(0, 4).c_str(), c.size());
        printf("---- %s\n", label);
        expect(c.size() > 4 && c[2] == 'C', "new chunks are checked");
        bool damaged = true;
        std::vector<float> out;
        expect(decodeAny(c, &damaged, &out) && !damaged, "round trip decodes");
        double sig = 0, err = 0;
        for (size_t i = 0; i < in.size(); i++) sig += (double)in[i] * in[i], err += (double)(in[i] - out[i]) * (in[i] - out[i]);
        double snr = 10 * log10(sig / std::max(err, 1e-20));
        printf("      SNR %.1f dB\n", snr);
        expect(snr > (kbps ? 20 : 60), "round trip sounds like the input");
        // The same without the CRC: what a chunk stored before it looks like.
        size_t hdr = c[3] == '2' ? sizeof(stemcodec::Hdr2) : sizeof(stemcodec::Hdr3);
        std::string old = c.substr(0, hdr) + c.substr(hdr + 4);
        old[2] = 'S';
        std::vector<float> outOld;
        expect(decodeAny(old, &damaged, &outOld) && !damaged && outOld == out, "old version (no CRC) decodes the same");
        for (size_t at : {(size_t)0x30, hdr + 2, hdr + 4, c.size() / 2, c.size() - 1}) {
            std::string bad = c;
            bad[at] ^= 0x10;
            char what[80];
            snprintf(what, sizeof what, "flipped byte at %zu rejected as damaged", at);
            expect(!decodeAny(bad, &damaged) && damaged, what);
        }
        expect(!decodeAny(c.substr(0, c.size() - 100), &damaged) && damaged, "cut-short chunk rejected as damaged");
        auto t0 = std::chrono::steady_clock::now();
        volatile uint32_t x = 0;
        for (int k = 0; k < 20; k++) x = stemcodec::chunkCrc(c, hdr);
        double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count() / 20;
        printf("      CRC of the whole chunk: %.2f ms (%08x)\n", ms, (uint32_t)x);
        expect(stemcodec::crc32(0, "123456789", 9) == 0xCBF43926, "CRC-32 check value");
        t0 = std::chrono::steady_clock::now();
        for (int k = 0; k < 5; k++) decodeAny(c, &damaged);
        printf("      whole decode: %.1f ms\n", std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count() / 5);
    }
    for (int i = 2; i < argc; i++) {
        std::string d = slurp(argv[i]);
        bool damaged = true;
        char what[1100];
        snprintf(what, sizeof what, "%s chunk %s decodes", d.substr(0, 4).c_str(), argv[i]);
        expect(decodeAny(d, &damaged) && !damaged, what);
    }
    printf("%s\n", fails ? "FAILED" : "all passed");
    return fails != 0;
}

int main(int argc, char** argv) {
    std::string cmd = argc > 1 ? argv[1] : "";
    if (cmd == "decode" && argc == 4) {
        std::string d = slurp(argv[2]);
        uint32_t outputs, samples;
        memcpy(&outputs, d.data() + 4, 4), memcpy(&samples, d.data() + 8, 4);
        std::vector<float> pcm((size_t)outputs * samples);
        std::vector<float*> ch(outputs);
        for (uint32_t o = 0; o < outputs; o++) ch[o] = pcm.data() + (size_t)o * samples;
        bool ok = stemcodec::decode(d, (int)samples, ch.data(), (int)outputs);
        printf("decode %s: %u outputs, %u samples\n", ok ? "ok" : "FAILED", outputs, samples);
        FILE* f = fopen(argv[3], "wb");
        fwrite(pcm.data(), 4, pcm.size(), f);
        fclose(f);
        return !ok;
    }
    if (cmd == "encode" && argc == 7) {
        int outputs = atoi(argv[3]), samples = atoi(argv[4]), kbps = atoi(argv[5]);
        std::string d = slurp(argv[2]);
        if (d.size() != (size_t)outputs * samples * 4) return printf("size mismatch\n"), 1;
        std::vector<const float*> ch(outputs);
        for (int o = 0; o < outputs; o++) ch[o] = (const float*)d.data() + (size_t)o * samples;
        std::string c = stemcodec::encode(samples, ch.data(), outputs, kbps);
        printf("encode: %s, %zu bytes\n", c.substr(0, 4).c_str(), c.size());
        FILE* f = fopen(argv[6], "wb");
        fwrite(c.data(), 1, c.size(), f);
        fclose(f);
        return c.empty();
    }
    if (cmd == "compare" && argc == 6) {
        std::string a = slurp(argv[2]), b = slurp(argv[3]);
        int outputs = atoi(argv[4]), samples = atoi(argv[5]);
        if (a.size() != b.size() || a.size() != (size_t)outputs * samples * 4) return printf("size mismatch\n"), 1;
        for (int o = 0; o < outputs; o++) {
            const float* x = (const float*)a.data() + (size_t)o * samples;
            const float* y = (const float*)b.data() + (size_t)o * samples;
            int bestLag = 0;
            double bestErr = 1e300, sig = 0;
            for (int i = 0; i < samples; i++) sig += (double)x[i] * x[i];
            for (int lag = -3000; lag <= 3000; lag++) {
                double e = 0;
                for (int i = 4000; i < samples - 4000; i += 3) e += (double)(x[i] - y[i + lag]) * (x[i] - y[i + lag]);
                if (e < bestErr) bestErr = e, bestLag = lag;
            }
            double err = 0;
            for (int i = 0; i < samples; i++) err += (double)(x[i] - y[i]) * (x[i] - y[i]);
            printf("channel %d: best lag %d, SNR at lag 0 %.1f dB\n", o, bestLag, 10 * log10(sig / std::max(err, 1e-20)));
        }
        return 0;
    }
    if (cmd == "check") return check(argc, argv);
    printf("usage: see the top of tools/dev/codectest.cpp\n");
    return 2;
}
