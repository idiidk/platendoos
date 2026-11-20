// mods/autocue_sheet.h against autocue.py (the autocue-test project): same audio, same rekordbox grid, the
// sheet and cues compared. Dumps come from a script that saves <name>.f32 (22050 Hz mono, as autocue.py reads it)
// and <name>.json (rekordbox's beats and autocue.py's result with stems off).
//   clang++ -std=c++20 -O2 -framework Accelerate -o build/autocue_sheet_test tools/dev/autocue_sheet_test.cpp engines/dsp_mac.mm
//   build/autocue_sheet_test <dir> <name>...      (-v: print both sheets)
#include "../../mods/autocue_sheet.h"
#include "../../mods/jsonmini.h"
#include <chrono>
#include <fstream>
#include <sstream>

int main(int argc, char** argv) {
    bool verbose = false;
    int same = 0, total = 0;
    std::string dir;
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        if (a == "-v") { verbose = true; continue; }
        if (dir.empty()) { dir = a; continue; }
        std::ifstream jf(dir + "/" + a + ".json");
        std::stringstream ss;
        ss << jf.rdbuf();
        jsonmini::Value ref;
        if (!jsonmini::parse(ss.str(), &ref)) { printf("%s: no reference\n", a.c_str()); continue; }
        std::ifstream af(dir + "/" + a + ".f32", std::ios::binary | std::ios::ate);
        if (!af || af.tellg() < 0) { printf("%s: no audio\n", a.c_str()); continue; }
        std::vector<float> mono((size_t)af.tellg() / 4);
        af.seekg(0);
        af.read((char*)mono.data(), mono.size() * 4);
        std::vector<double> bt;
        std::vector<int> bn;
        for (auto& v : ref["bt"].a) bt.push_back(v.n);
        for (auto& v : ref["bn"].a) bn.push_back((int)v.n);
        auto t0 = std::chrono::steady_clock::now();
        autocue_sheet::Analysis A = autocue_sheet::analyse(mono, nullptr, bt, bn, ref["duration"].n);
        double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        std::string cues, rcues;
        for (auto& c : A.cues) cues += std::string(cues.empty() ? "" : " ") + c.slot + std::to_string(c.bar + 1);
        for (auto& c : ref["cues"].a) rcues += std::string(rcues.empty() ? "" : " ") + c["slot"].s + std::to_string((int)c["bar"].n);
        bool sheetSame = A.sheet == ref["sheet"].s;
        total++, same += sheetSame && cues == rcues;
        printf("%s: %d bars (%d) %.0f ms · cues %s%s · sheet %s\n", a.c_str(), A.bars, (int)ref["bars"].n, ms, cues.c_str(),
               cues == rcues ? " (same)" : (" (python " + rcues + ")").c_str(), sheetSame ? "same" : "DIFFERS");
        if (!sheetSame) {  // the first differing lines
            std::istringstream x(A.sheet), y(ref["sheet"].s);
            std::string l1, l2;
            int shown = 0, line = 0;
            while (shown < 6) {
                bool g1 = (bool)std::getline(x, l1), g2 = (bool)std::getline(y, l2);
                if (!g1 && !g2) break;
                line++;
                if (!g1) l1 = "<end>";
                if (!g2) l2 = "<end>";
                if (l1 != l2) printf("  %3d c++ %s\n      py  %s\n", line, l1.c_str(), l2.c_str()), shown++;
            }
        }
        if (verbose) printf("%s\n", A.sheet.c_str());
    }
    printf("%d of %d the same\n", same, total);
}
