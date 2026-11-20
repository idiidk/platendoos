// patcher/retable.h on the Mac: makes a Windows address table for a rekordbox.exe from a .sig file.
//   clang++ -std=c++17 -O2 -o build/retable tools/dev/retable.cpp
//   build/retable <rekordbox.exe> tools/win/sigs/rekordbox-<build>.sig > rekordbox-<newbuild>.tbl
// The table goes to stdout, the count and what wasn't found to stderr.
#include "../../patcher/retable.h"
#include <chrono>
#include <cstdio>
#include <fstream>
#include <sstream>

int main(int argc, char** argv) {
    if (argc != 3) return fprintf(stderr, "usage: retable <rekordbox.exe> <signatures.sig>\n"), 2;
    std::ifstream e(argv[1], std::ios::binary), s(argv[2], std::ios::binary);
    if (!e || !s) return fprintf(stderr, "can't read the input files\n"), 1;
    std::string exe((std::istreambuf_iterator<char>(e)), {}), sig((std::istreambuf_iterator<char>(s)), {});
    auto t = std::chrono::steady_clock::now();
    int last = -1;
    auto r = retable::make((const uint8_t*)exe.data(), exe.size(), sig, [&](double p) {
        if ((int)(p * 10) != last) last = (int)(p * 10), fprintf(stderr, "%d%% ", (int)(p * 100));
    });
    double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t).count();
    if (!r.error.empty()) return fprintf(stderr, "\n%s\n", r.error.c_str()), 1;
    fputs(r.table.c_str(), stdout);
    fprintf(stderr, "\n%s from %s: %d of %d parts found in %.1f s\n", r.build.c_str(), r.from.c_str(), r.found, r.total, secs);
    if (r.conflicts) fprintf(stderr, "%d names left out: their lines disagreed\n", r.conflicts);
    for (auto& m : r.missing) fprintf(stderr, "  not found: %s\n", m.c_str());
    return 0;
}
