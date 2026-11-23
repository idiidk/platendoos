// Checks mods/aiff.h (the AIFF reader Windows uses) on the Mac: a test signal is written as WAV, afconvert makes
// AIFF and AIFF-C files of it in every sample format the reader takes, and each must read back as the signal.
//   clang++ -std=c++20 -O2 -o /tmp/aifftest tools/dev/aiff_test.cpp && /tmp/aifftest
#include "../../mods/aiff.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <string>

static int failures = 0;
static void check(bool ok, const std::string& what) {
    printf("%s %s\n", ok ? "  ok  " : "  FAIL", what.c_str());
    if (!ok) failures++;
}
static void put32(std::string& s, uint32_t v, bool big) {
    for (int i = 0; i < 4; i++) s += (char)(v >> (big ? 24 - 8 * i : 8 * i));
}
static void put16(std::string& s, uint16_t v, bool big) { s += (char)(big ? v >> 8 : v), s += (char)(big ? v : v >> 8); }

int main() {
    std::string dir = "/tmp/aifftest.d";
    system(("rm -rf " + dir + " && mkdir -p " + dir).c_str());
    // The signal: two seconds of stereo, a sweep on the left and noise on the right, full scale at times.
    const int rate = 44100, frames = 2 * rate + 1;  // (an odd count: odd-length chunks in the 8- and 24-bit files)
    std::vector<int16_t> sig((size_t)frames * 2);
    uint32_t seed = 1;
    for (int i = 0; i < frames; i++) {
        double t = (double)i / rate;
        sig[2 * i] = (int16_t)std::lrint(32767 * std::sin(2 * M_PI * (50 + 4000 * t) * t));
        seed = seed * 1664525 + 1013904223;
        sig[2 * i + 1] = (int16_t)(seed >> 16);
    }
    sig[0] = -32768, sig[1] = 32767;
    std::string wav = "RIFF";
    put32(wav, 36 + frames * 4, false), wav += "WAVEfmt ", put32(wav, 16, false), put16(wav, 1, false), put16(wav, 2, false);
    put32(wav, rate, false), put32(wav, rate * 4, false), put16(wav, 4, false), put16(wav, 16, false);
    wav += "data", put32(wav, frames * 4, false);
    wav.append((const char*)sig.data(), sig.size() * 2);
    std::ofstream(dir + "/src.wav", std::ios::binary) << wav;

    struct Case { const char *name, *args; int tolerance; bool mono; };
    const Case cases[] = {
        {"AIFF 16-bit", "-f AIFF -d BEI16", 0, false},     {"AIFF 24-bit", "-f AIFF -d BEI24", 0, false},
        {"AIFF 32-bit", "-f AIFF -d BEI32", 0, false},     {"AIFF 8-bit", "-f AIFF -d BEI8", 256, false},
        {"AIFF-C fl32", "-f AIFC -d BEF32", 0, false},     {"AIFF-C fl64", "-f AIFC -d BEF64", 0, false},
        {"AIFF-C raw (unsigned 8-bit)", "-f AIFC -d UI8", 256, false},
        {"AIFF 16-bit mono", "-f AIFF -d BEI16 -c 1", -1, true},
    };
    for (const Case& c : cases) {
        std::string out = dir + "/t.aif";
        if (system(("afconvert " + std::string(c.args) + " " + dir + "/src.wav " + out + " 2>/dev/null").c_str())) {
            check(false, std::string(c.name) + ": afconvert failed");
            continue;
        }
        std::ifstream f(out, std::ios::binary);
        aiff::Format x;
        std::vector<int16_t> got;
        bool ok = aiff::open(f, &x) && aiff::readStereo16(f, x, &got);
        check(ok && x.rate == rate && x.frames == frames && got.size() == sig.size(), std::string(c.name) + ": opens, 44100 Hz, every frame");
        if (!ok || got.size() != sig.size()) continue;
        int worst = 0;
        if (c.mono) {
            for (int i = 0; i < frames; i++) worst = std::max(worst, std::abs(got[2 * i] - got[2 * i + 1]));
            check(worst == 0, std::string(c.name) + ": the same on both sides");
        } else {
            for (size_t i = 0; i < sig.size(); i++) worst = std::max(worst, std::abs(got[i] - sig[i]));
            check(worst <= c.tolerance, std::string(c.name) + ": the signal's samples (largest difference " + std::to_string(worst) + ")");
        }
    }
    {  // AIFF-C sowt (16-bit little-endian; afconvert reads it but can't write it): made by hand
        std::string name = "AIFF-C sowt", body = "AIFC", comm, ssnd;
        put16(comm, 2, true), put32(comm, frames, true), put16(comm, 16, true);
        comm += std::string("\x40\x0e\xac\x44\0\0\0\0\0\0", 10) + "sowt" + std::string("\0\0", 2);
        put32(ssnd, 0, true), put32(ssnd, 0, true);
        for (int16_t v : sig) put16(ssnd, (uint16_t)v, false);
        body += "FVER", put32(body, 4, true), put32(body, 0xA2805140, true);
        body += "COMM", put32(body, (uint32_t)comm.size(), true), body += comm;
        body += "SSND", put32(body, (uint32_t)ssnd.size(), true), body += ssnd;
        std::string file = "FORM", path = dir + "/sowt.aifc";
        put32(file, (uint32_t)body.size(), true), file += body;
        std::ofstream(path, std::ios::binary) << file;
        // (macOS must read the hand-made file as the signal too, or the test proves nothing)
        bool mac = !system(("afconvert -f WAVE -d LEI16 " + path + " " + dir + "/back.wav 2>/dev/null").c_str());
        std::ifstream b(dir + "/back.wav", std::ios::binary);
        std::string back((std::istreambuf_iterator<char>(b)), std::istreambuf_iterator<char>());
        size_t at = back.find("data");
        check(mac && at != std::string::npos && back.size() >= at + 8 + sig.size() * 2 && !memcmp(back.data() + at + 8, sig.data(), sig.size() * 2),
              name + ": macOS reads the hand-made file as the signal");
        std::ifstream f(path, std::ios::binary);
        aiff::Format x;
        std::vector<int16_t> got;
        bool ok = aiff::open(f, &x) && aiff::readStereo16(f, x, &got);
        check(ok && x.rate == rate && x.frames == frames && got == sig, name + ": the signal's samples exactly");
    }
    {  // By hand: the samples before COMM, an odd-length chunk between them, and a header that counts too many frames.
        std::string body = "AIFF", ssnd, comm;
        put32(ssnd, 0, true), put32(ssnd, 0, true);
        for (int i = 0; i < 6; i++) put16(ssnd, (uint16_t)(1000 * i), true);  // 3 stereo frames
        body += "SSND", put32(body, (uint32_t)ssnd.size(), true), body += ssnd;
        body += "NAME", put32(body, 3, true), body += std::string("abc\0", 4);
        put16(comm, 2, true), put32(comm, 99, true), put16(comm, 16, true);
        comm += std::string("\x40\x0e\xac\x44\0\0\0\0\0\0", 10);  // 44100 as an 80-bit float
        body += "COMM", put32(body, 18, true), body += comm;
        std::string file = "FORM";
        put32(file, (uint32_t)body.size(), true), file += body;
        std::ofstream(dir + "/hand.aif", std::ios::binary) << file;
        std::ifstream f(dir + "/hand.aif", std::ios::binary);
        aiff::Format x;
        std::vector<int16_t> got;
        bool ok = aiff::open(f, &x) && aiff::readStereo16(f, x, &got);
        check(ok && x.rate == 44100 && x.frames == 3 && got.size() == 6 && got[5] == 5000, "hand-made: SSND before COMM, odd chunk, short data");
    }
    {  // Not AIFF, and compressed AIFF-C: refused.
        std::ifstream f(dir + "/src.wav", std::ios::binary);
        aiff::Format x;
        check(!aiff::open(f, &x), "a WAV file is refused");
        bool made = !system(("afconvert -f AIFC -d ima4 " + dir + "/src.wav " + dir + "/ima.aifc 2>/dev/null").c_str());
        std::ifstream g(dir + "/ima.aifc", std::ios::binary);
        check(made && !aiff::open(g, &x), "compressed AIFF-C (ima4) is refused");
    }
    printf(failures ? "%d FAILED\n" : "all passed\n", failures);
    return failures != 0;
}
