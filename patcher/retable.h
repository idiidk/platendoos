// A Windows address table for a rekordbox build platendoos has no table for, made from signatures of a build it
// has one for (tools/win/makesigs.py writes them; its header documents the line kinds). The patcher runs this when
// rbmods/ has no rekordbox-<build>.tbl for the installed rekordbox: whatever the new build's code still has the
// same way is found again, the rest is left out (and the mods that need it are off, as with any table).
//   retable::Result r = retable::make(exeBytes, exeSize, sigText, progress);
//   r.table: core/image_win.cpp's table text; r.found / r.total parts; r.missing what wasn't found.
// Portable C++17 (tested on the Mac with tools/dev/retable.cpp).
#pragma once
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace retable {

struct Result {
    std::string error;  // "" or why no table could be made
    std::string build;  // the exe's build (<TimeDateStamp>-<SizeOfImage>)
    std::string from;   // the build the signatures were made from
    std::string table;  // the table text
    int found = 0, total = 0;          // parts (addresses, fields, hooks) found, of those the signatures name
    std::vector<std::string> missing;  // "addr <name>", "off <key>", "hook <name>", "guard <hooks>"
    int conflicts = 0;                 // names left out because two ways of finding them disagreed
};

namespace detail {

inline uint32_t u32(const uint8_t* p) { uint32_t v; memcpy(&v, p, 4); return v; }
inline uint64_t u64(const uint8_t* p) { uint64_t v; memcpy(&v, p, 8); return v; }
inline int32_t i32(const uint8_t* p) { int32_t v; memcpy(&v, p, 4); return v; }

inline std::string hexs(int64_t v) {  // as python's '%x' (a minus sign for negative values)
    char b[24];
    snprintf(b, sizeof b, v < 0 ? "-%llx" : "%llx", (unsigned long long)(v < 0 ? -v : v));
    return b;
}
inline int64_t parseHex(const std::string& s) { return strtoll(s.c_str(), nullptr, 16); }
inline int64_t parseDec(const std::string& s) { return strtoll(s.c_str(), nullptr, 10); }
inline std::string unescape(std::string s) { std::replace(s.begin(), s.end(), '~', ' '); return s; }

// The image by rva, the headers' facts, .text and the .pdata extents.
struct Image {
    std::vector<uint8_t> m;
    uint64_t base = 0;
    uint32_t stamp = 0, size = 0, t0 = 0, tn = 0;
    struct Sec { std::string name; uint32_t va, vsize; };
    std::vector<Sec> secs;
    std::vector<std::pair<uint32_t, uint32_t>> pdata;  // begin, end; sorted

    bool load(const uint8_t* f, size_t n, std::string* why) {
        auto bad = [&](const char* w) { *why = w; return false; };
        if (n < 0x40 || f[0] != 'M' || f[1] != 'Z') return bad("not a Windows program");
        uint32_t nt = u32(f + 0x3c);
        if ((uint64_t)nt + 24 + 240 > n || memcmp(f + nt, "PE\0\0", 4)) return bad("not a Windows program");
        const uint8_t* fh = f + nt + 4;
        int nsec = fh[2] | fh[3] << 8, optSize = fh[16] | fh[17] << 8;
        stamp = u32(fh + 4);
        const uint8_t* opt = fh + 20;
        if ((opt[0] | opt[1] << 8) != 0x20b) return bad("not a 64-bit program");
        base = u64(opt + 24);
        size = u32(opt + 56);
        uint32_t ndirs = u32(opt + 108), excRva = 0, excSize = 0;
        if (ndirs > 3) excRva = u32(opt + 112 + 3 * 8), excSize = u32(opt + 112 + 3 * 8 + 4);
        if (size > (1u << 30)) return bad("image too big");
        m.assign(size, 0);
        const uint8_t* sh = opt + optSize;
        if ((uint64_t)(sh - f) + 40ull * nsec > n) return bad("truncated section headers");
        for (int i = 0; i < nsec; i++, sh += 40) {
            Sec s{std::string((const char*)sh, strnlen((const char*)sh, 8)), u32(sh + 12), u32(sh + 8)};
            uint32_t raw = u32(sh + 16), ptr = u32(sh + 20);
            uint64_t len = std::min<uint64_t>(raw, s.vsize ? s.vsize : raw);
            if ((uint64_t)ptr + len > n) len = ptr < n ? n - ptr : 0;
            if ((uint64_t)s.va + len > size) len = s.va < size ? size - s.va : 0;
            if (len) memcpy(&m[s.va], f + ptr, len);
            if (s.name == ".text") t0 = s.va, tn = std::min(s.vsize, size - s.va);
            secs.push_back(s);
        }
        if (!tn) return bad("no .text section");
        for (uint32_t p = excRva; excSize && p + 12 <= excRva + excSize && p + 12 <= size; p += 12) {
            uint32_t b = u32(&m[p]), e = u32(&m[p + 4]);
            if (b && e > b) pdata.push_back({b, e});
        }
        std::sort(pdata.begin(), pdata.end());
        return true;
    }
    bool has(int64_t rva, size_t n) const { return rva >= 0 && (uint64_t)rva + n <= size; }
    const uint8_t* at(uint32_t rva) const { return &m[rva]; }
    bool inText(int64_t rva) const { return rva >= t0 && rva < (int64_t)t0 + tn; }
    std::string build() const {
        char b[40];
        snprintf(b, sizeof b, "%08X-%X", stamp, size);
        return b;
    }
    // Inside a function (after its .pdata start): no function starts there.
    bool insideFn(uint32_t rva) const {
        auto it = std::upper_bound(pdata.begin(), pdata.end(), std::make_pair(rva, ~0u));
        if (it == pdata.begin()) return false;
        --it;
        return it->first < rva && rva < it->second;
    }
    // A scoped line's range: the anchor's .pdata extent, or (a leaf) its old extent with room to grow.
    std::pair<uint32_t, uint32_t> scope(uint32_t anchor, uint32_t range) const {
        auto it = std::lower_bound(pdata.begin(), pdata.end(), std::make_pair(anchor, 0u));
        if (it != pdata.end() && it->first == anchor) return {anchor, it->second};
        return {anchor, anchor + range + range / 4 + 16};
    }
};

// hex with ?? for masked bytes and a * before loose ones (field and stack displacements, immediates, short
// jumps: compared unless the exact pattern isn't found), then |dispPos,insEnd,hex: the rip-relative target must
// start with those bytes. A % before a byte marks a ModRM or SIB byte and a ^ a REX prefix: they carry the
// registers, compared exactly except in the third, register-insensitive pass (only when the exact and the loose
// pass found nothing): ModRM/SIB under mask 0xC0 (mod, scale), REX under 0xF8 (W stays).
// Levels: 0 exact (loose bytes compared), 1 loose bytes ignored, 2 also the register bits ignored.
struct Pattern {
    std::vector<uint8_t> b;
    std::vector<uint8_t> mask;  // 0 exact, 1 any byte, 2 loose, 3 ModRM/SIB (0xC0), 4 REX (0xF8)
    struct Con { int pos, end; std::vector<uint8_t> want; };
    std::vector<Con> cons;
    struct Run { size_t start = 0, len = 0; } run[3];  // the longest run compared (what's searched for) per level: exact, loose, register
    bool ok = false;

    static int nib(char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; }
    static bool hexBytes(const std::string& s, std::vector<uint8_t>* out) {
        if (s.size() % 2) return false;
        for (size_t i = 0; i < s.size(); i += 2) {
            int h = nib(s[i]), l = nib(s[i + 1]);
            if (h < 0 || l < 0) return false;
            out->push_back((uint8_t)(h << 4 | l));
        }
        return true;
    }
    explicit Pattern(const std::string& s) {
        size_t bar = s.find('|');
        std::string body = s.substr(0, bar);
        if (body.empty()) return;
        for (size_t i = 0; i < body.size(); i += 2) {
            char pre = body[i];
            bool loose = pre == '*', reg = pre == '%' || pre == '^';
            if (loose || reg) i++;
            if (i + 1 >= body.size()) return;
            if (!loose && !reg && body[i] == '?' && body[i + 1] == '?') { b.push_back(0); mask.push_back(1); continue; }
            int h = nib(body[i]), l = nib(body[i + 1]);
            if (h < 0 || l < 0) return;
            b.push_back((uint8_t)(h << 4 | l));
            mask.push_back(loose ? 2 : pre == '%' ? 3 : pre == '^' ? 4 : 0);
        }
        while (bar != std::string::npos) {
            size_t next = s.find('|', bar + 1);
            std::string c = s.substr(bar + 1, next == std::string::npos ? std::string::npos : next - bar - 1);
            bar = next;
            size_t c1 = c.find(','), c2 = c.find(',', c1 + 1);
            if (c1 == std::string::npos || c2 == std::string::npos) return;
            Con k{atoi(c.substr(0, c1).c_str()), atoi(c.substr(c1 + 1, c2 - c1 - 1).c_str()), {}};
            if (!hexBytes(c.substr(c2 + 1), &k.want) || k.pos < 0 || k.pos + 4 > (int)b.size()) return;
            cons.push_back(k);
        }
        for (int lv = 0; lv < 3; lv++) {
            size_t rs = 0;
            for (size_t i = 0; i <= b.size(); i++)
                if (i == b.size() || skip(mask[i], lv)) {
                    if (i - rs > run[lv].len) run[lv] = {rs, i - rs};
                    rs = i + 1;
                }
        }
        ok = true;
    }
    // is a byte of this mask not compared exactly at the level (so not part of the searched run)?
    static bool skip(uint8_t m, int lv) { return m == 1 || (lv >= 1 && m == 2) || (lv >= 2 && m >= 3); }
    // does the byte match at the level?
    static bool byteOk(uint8_t m, uint8_t got, uint8_t want, int lv) {
        if (m == 1 || (lv >= 1 && m == 2)) return true;
        if (lv >= 2 && m >= 3) return !((got ^ want) & (m == 3 ? 0xC0 : 0xF8));
        return got == want;
    }
    bool matches(const Image& im, int64_t q, int loose = 0) const {  // at rva q (within .text)
        if (q < im.t0 || q + (int64_t)b.size() > (int64_t)im.t0 + im.tn) return false;
        const uint8_t* p = im.at((uint32_t)q);
        for (size_t i = 0; i < b.size(); i++)
            if (!byteOk(mask[i], p[i], b[i], loose)) return false;
        for (auto& c : cons) {
            int64_t t = q + c.end + i32(p + c.pos);
            if (!im.has(t, c.want.size()) || memcmp(im.at((uint32_t)t), c.want.data(), c.want.size())) return false;
        }
        return true;
    }
    // Matches in .text, up to limit (a pattern with less than 4 compared bytes in a row counts as not unique).
    std::vector<uint32_t> find(const Image& im, size_t limit, int loose) const {
        std::vector<uint32_t> hits;
        if (!ok) return hits;
        const Run& r = run[loose];
        if (r.len < 4) { hits.assign(limit, 0); return hits; }
        const uint8_t *text = im.at(im.t0), *end = text + im.tn, *needle = b.data() + r.start;
        std::boyer_moore_horspool_searcher<const uint8_t*> searcher(needle, needle + r.len);
        for (const uint8_t* p = text;; p++) {
            p = std::search(p, end, searcher);
            if (p == end) return hits;
            int64_t q = (int64_t)im.t0 + (p - text) - (int64_t)r.start;
            if (matches(im, q, loose)) {
                hits.push_back((uint32_t)q);
                if (hits.size() >= limit) return hits;
            }
        }
    }
    // Matches starting in [lo, hi), up to limit.
    std::vector<uint32_t> findIn(const Image& im, uint32_t lo, uint32_t hi, size_t limit, int loose) const {
        std::vector<uint32_t> hits;
        if (!ok) return hits;
        int64_t stop = std::min<int64_t>(hi, (int64_t)im.t0 + im.tn - (int64_t)b.size() + 1);
        for (int64_t q = std::max<int64_t>(lo, im.t0); q < stop; q++)
            if (byteOk(mask[0], im.m[q], b[0], loose) && matches(im, q, loose)) {
                hits.push_back((uint32_t)q);
                if (hits.size() >= limit) break;
            }
        return hits;
    }
    // The exact matches, or if there are none the loose ones, or then the register-insensitive ones.
    std::vector<uint32_t> find(const Image& im, size_t limit) const {
        std::vector<uint32_t> h;
        for (int lv = 0; lv < 3 && h.empty(); lv++) h = find(im, limit, lv);
        return h;
    }
    std::vector<uint32_t> findIn(const Image& im, uint32_t lo, uint32_t hi, size_t limit) const {
        std::vector<uint32_t> h;
        for (int lv = 0; lv < 3 && h.empty(); lv++) h = findIn(im, lo, hi, limit, lv);
        return h;
    }
};

// RTTI: complete object locators {1, offset, cdOffset, typeDescriptor rva, classDescriptor rva, self rva}; a vtable
// is the 8 bytes after a qword holding ImageBase + its locator's rva; a type descriptor {vftable, spare, name}.
struct Rtti {
    std::unordered_map<std::string, std::vector<uint32_t>> vtables;  // "<name>:<offset>" -> vtables
    std::unordered_map<std::string, std::vector<uint32_t>> types;    // name -> type descriptors

    static std::string cstr(const Image& im, uint32_t rva) {
        std::string s;
        while (im.has(rva, 1) && im.m[rva] && s.size() < 4096) s += (char)im.m[rva++];
        return s;
    }
    void scan(const Image& im) {
        std::vector<std::pair<uint32_t, uint32_t>> data;
        for (auto& s : im.secs)
            if (s.name == ".rdata" || s.name == ".data") data.push_back({s.va, std::min(s.va + s.vsize, im.size)});
        std::vector<uint64_t> cols;
        for (auto [lo, hi] : data)
            for (uint32_t p = lo; p + 24 <= hi; p += 4)
                if (u32(im.at(p)) == 1 && u32(im.at(p + 20)) == p) cols.push_back(im.base + p);
        std::sort(cols.begin(), cols.end());
        std::set<uint64_t> tiVft;
        for (auto [lo, hi] : data)
            for (uint32_t p = lo; p + 8 <= hi; p += 8) {
                uint64_t v = u64(im.at(p));
                if (!std::binary_search(cols.begin(), cols.end(), v)) continue;
                uint32_t col = (uint32_t)(v - im.base), off = u32(im.at(col + 4)), td = u32(im.at(col + 12));
                if (!im.has(td, 24)) continue;
                std::string name = cstr(im, td + 16);
                vtables[name + ":" + std::to_string(off)].push_back(p + 8);
                tiVft.insert(u64(im.at(td)));
            }
        // type descriptors, with or without a vtable: the name ".?A…" after the type_info vftable and a spare
        for (auto [lo, hi] : data)
            for (uint32_t p = lo; p + 20 <= hi; p += 8)
                if (im.m[p + 16] == '.' && im.m[p + 17] == '?' && im.m[p + 18] == 'A' && tiVft.count(u64(im.at(p))))
                    types[cstr(im, p + 16)].push_back(p);
    }
};

inline std::vector<std::string> split(const std::string& line) {
    std::vector<std::string> w;
    for (size_t a = 0; a < line.size();) {
        size_t b = line.find(' ', a);
        if (b == std::string::npos) b = line.size();
        if (b > a) w.push_back(line.substr(a, b - a));
        a = b + 1;
    }
    return w;
}

}  // namespace detail

// progress(0..1) is called from the calling thread.
inline Result make(const uint8_t* exe, size_t size, const std::string& sig, const std::function<void(double)>& progress = {}) {
    using namespace detail;
    Result r;
    Image im;
    if (!im.load(exe, size, &r.error)) return r;
    r.build = im.build();
    auto prog = [&](double p) { if (progress) progress(p); };

    // ---- the signature lines
    std::vector<std::vector<std::string>> lines;
    std::vector<std::string> uncovered;
    for (size_t p = 0; p < sig.size();) {
        size_t e = sig.find('\n', p);
        if (e == std::string::npos) e = sig.size();
        std::string line = sig.substr(p, e - p);
        p = e + 1;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        static const char kNot[] = "# not covered: ";
        if (line.rfind(kNot, 0) == 0) uncovered.push_back(line.substr(sizeof kNot - 1));
        if (line.empty() || line[0] == '#') continue;
        auto w = split(line);
        if (w[0] == "build" && w.size() >= 2) r.from = w[1];
        else lines.push_back(w);
    }
    if (lines.empty()) return r.error = "no signatures", r;

    // ---- the patterns searched in all of .text, in parallel (most of the time is here)
    // (the index of the line's pattern word, or -1 for the other lines)
    auto unscopedPat = [](const std::vector<std::string>& w) -> int {
        const std::string& k = w[0];
        if ((k == "fn" && w.size() == 3) || (k == "ref" && w.size() == 5)) return 2;
        if (k == "hook" && w.size() == 6) return 3;
        if ((k == "off" || k == "offany" || k == "guard") && w.size() == 6) return 3;
        if ((k == "offadj" || k == "offadjany") && w.size() == 7) return 4;
        return -1;
    };
    std::vector<std::vector<uint32_t>> hits(lines.size());
    std::vector<size_t> todo;
    for (size_t i = 0; i < lines.size(); i++)
        if (unscopedPat(lines[i]) >= 0) todo.push_back(i);
    prog(0.02);
    Rtti rtti;
    {
        std::atomic<size_t> next{0}, done{0};
        auto work = [&] {
            for (size_t k; (k = next++) < todo.size(); done++) {
                auto& w = lines[todo[k]];
                Pattern pt(w[unscopedPat(w)]);
                hits[todo[k]] = pt.find(im, w[0] == "offany" || w[0] == "offadjany" ? 9 : 2);
            }
        };
        unsigned n = std::max(2u, std::min(16u, std::thread::hardware_concurrency()));
        std::vector<std::thread> threads;
        for (unsigned t = 0; t < n; t++) threads.emplace_back(work);
        rtti.scan(im);
        while (done < todo.size()) {
            prog(0.05 + 0.85 * done / todo.size());
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        for (auto& t : threads) t.join();
    }
    prog(0.9);

    // ---- names: several passes, a scoped line's anchor may be found by another line
    std::map<std::string, uint32_t> names;
    std::unordered_map<std::string, uint32_t> vtStart;  // vt line names -> the vtable they're in
    std::set<std::string> named;                        // the table's names (not @<rva> anchors) the lines are for
    auto isName = [](const std::string& k) { return k == "fn" || k == "ref" || k == "refin" || k == "vt" || k == "ti" || k == "fnvt"; };
    for (auto& w : lines)
        if (isName(w[0]) && w.size() >= 2 && w[1][0] != '@') named.insert(w[1]);
    auto slots = [&](uint32_t vt) {
        std::vector<uint32_t> s;
        for (uint32_t k = 0; k < 1024 && im.has(vt + 8 * k, 8); k++) {
            int64_t f = (int64_t)(u64(im.at(vt + 8 * k)) - im.base);
            if (!im.inText(f)) break;
            s.push_back((uint32_t)f);
        }
        return s;
    };
    auto vtableOf = [&](const std::string& n, uint32_t* out) {  // a vt line's name, or rtti:<class>:<offset>
        if (n.rfind("rtti:", 0) == 0) {
            auto it = rtti.vtables.find(n.substr(5));
            if (it == rtti.vtables.end() || it->second.size() != 1) return false;
            return *out = it->second[0], true;
        }
        auto it = vtStart.find(n);
        return it != vtStart.end() && (*out = it->second, true);
    };
    // A function's name (mangled, not a vtable or typeinfo; or an @<rva> anchor) can't be inside another function.
    auto plausible = [&](const std::string& n, uint32_t v) {
        bool fn = n[0] == '@' || (n.rfind("__Z", 0) == 0 && n.rfind("__ZT", 0) != 0) || n.rfind("__ZThn", 0) == 0;
        return !(fn && im.inText(v) && im.insideFn(v));
    };
    // What a name's fn / ref / refin / fnvt line finds (0: nothing, or not unique).
    auto scopedFind = [&](const std::string& anchor, const std::string& range, const std::string& pat, uint32_t* out) {
        auto a = names.find(anchor);
        if (a == names.end()) return false;
        auto [lo, hi] = im.scope(a->second, (uint32_t)parseHex(range));
        auto h = Pattern(pat).findIn(im, lo, hi, 2);
        return h.size() == 1 && (*out = h[0], true);
    };
    auto lineValue = [&](size_t i) -> uint32_t {
        auto& w = lines[i];
        uint32_t m, vt, v = 0;
        if (w[0] == "fn" && w.size() == 3 && hits[i].size() == 1) v = hits[i][0];
        else if (w[0] == "ref" && w.size() == 5 && hits[i].size() == 1)
            v = (uint32_t)(hits[i][0] + parseDec(w[4]) + i32(im.at(hits[i][0] + (uint32_t)parseDec(w[3]))));
        else if (w[0] == "refin" && w.size() == 7 && scopedFind(w[2], w[3], w[4], &m))
            v = (uint32_t)(m + parseDec(w[6]) + i32(im.at(m + (uint32_t)parseDec(w[5]))));
        else if (w[0] == "fnvt" && (w.size() == 4 || w.size() == 6)) {
            // fnvt <name> <vtable> <pattern> [<k> <n>]: the one function among the vtable's slots that starts so,
            // or the k-th (in slot order) of the n that do (look-alikes that differ only in what they call). A
            // subobject's vtable (rtti:<class>:<offset>) may have moved: then the class's vtables that have it.
            Pattern pt(w[3]);
            std::vector<uint32_t> vts, fs;
            if (vtableOf(w[2], &vt)) vts.push_back(vt);
            auto scan = [&] {
                for (int loose = 0; loose < 3 && fs.empty(); loose++)
                    for (uint32_t t : vts)
                        for (uint32_t f : slots(t))
                            if (pt.matches(im, f, loose) && std::find(fs.begin(), fs.end(), f) == fs.end()) fs.push_back(f);
            };
            scan();
            if (fs.empty() && w[2].rfind("rtti:", 0) == 0 && w[2].rfind(':') > 5) {
                std::string cls = w[2].substr(5, w[2].rfind(':') - 5);
                vts.clear();
                for (auto& [k, v] : rtti.vtables)
                    if (k.rfind(cls + ":", 0) == 0 && k.find(':', cls.size() + 1) == std::string::npos && v.size() == 1) vts.push_back(v[0]);
                std::sort(vts.begin(), vts.end());
                scan();
            }
            if (w.size() == 4 && fs.size() == 1) v = fs[0];
            if (w.size() == 6 && fs.size() == (size_t)parseDec(w[5]) && (size_t)parseDec(w[4]) < fs.size()) v = fs[(size_t)parseDec(w[4])];
        }
        return v && plausible(w[1], v) ? v : 0;
    };
    for (size_t i = 0; i < lines.size(); i++) {
        auto& w = lines[i];
        if (w[0] == "vt" && w.size() == 5) {
            auto it = rtti.vtables.find(w[3] + ":" + w[4]);
            if (it == rtti.vtables.end() || it->second.size() != 1) continue;
            names[w[1]] = (uint32_t)(it->second[0] + parseDec(w[2]));
            vtStart[w[1]] = it->second[0];
        } else if (w[0] == "ti" && w.size() == 3) {
            auto it = rtti.types.find(w[2]);
            if (it != rtti.types.end() && it->second.size() == 1) names[w[1]] = it->second[0];
        } else if ((w[0] == "fn" || w[0] == "ref") && !names.count(w[1])) {
            if (uint32_t v = lineValue(i)) names[w[1]] = v;
        }
    }
    // scoped to a found function: the pattern is unique in the anchor's extent
    auto scoped = [&](const std::string& anchor, const std::string& range, const std::string& pat, uint32_t* out, bool exact = false) {
        auto a = names.find(anchor);
        if (a == names.end()) return false;
        auto [lo, hi] = im.scope(a->second, (uint32_t)parseHex(range));
        Pattern pt(pat);
        auto h = exact ? pt.findIn(im, lo, hi, 2, false) : pt.findIn(im, lo, hi, 2);
        return h.size() == 1 && (*out = h[0], true);
    };
    for (bool more = true; more;) {
        more = false;
        for (size_t i = 0; i < lines.size(); i++) {
            auto& w = lines[i];
            if (w.size() < 2 || names.count(w[1]) || (w[0] != "refin" && w[0] != "fnvt")) continue;
            if (uint32_t v = lineValue(i)) names[w[1]] = v, more = true;
        }
    }

    // ---- every line of a name votes (the fn / ref one and the refin / fnvt fallbacks through other functions):
    // the address most of them find wins; a tie leaves the name out (one of them matched the wrong code).
    {
        std::map<std::string, std::map<uint32_t, int>> votes;
        for (size_t i = 0; i < lines.size(); i++) {
            auto& w = lines[i];
            if (w.size() < 2 || !names.count(w[1])) continue;
            if (w[0] != "fn" && w[0] != "ref" && w[0] != "refin" && w[0] != "fnvt") continue;
            if (uint32_t v = lineValue(i)) votes[w[1]][v]++;
        }
        for (auto& [n, vs] : votes) {
            if (vs.size() < 2) continue;
            std::vector<std::pair<int, uint32_t>> by;
            for (auto& [v, c] : vs) by.push_back({c, v});
            std::sort(by.rbegin(), by.rend());
            if (getenv("RETABLE_DEBUG"))
                for (auto& [c, v] : by) fprintf(stderr, "vote %s: %x x%d\n", n.c_str(), v, c);
            if (by[0].first == by[1].first) names.erase(n), r.conflicts++;
            else names[n] = by[0].second;
        }
    }

    // ---- this adjustments: a function that only sits in vtables at one non-zero subobject offset takes that
    // subobject as `this` (as tools/win/maketable.py works them out). They're worked out again here: a subobject
    // can move (PcmReaderTemplate's from +0x250 to +0x270 between 7.2.18 and 7.2.19), and a stale one would hand
    // rekordbox a wrong pointer. A function whose adjustment can't be had is left out.
    std::map<std::string, int64_t> adj;
    {
        std::unordered_map<uint32_t, std::set<int64_t>> where;  // function -> subobject offsets of vtables holding it
        for (auto& w : lines) {
            if (w[0] != "adj" || w.size() != 3 || !names.count(w[1])) continue;
            if (where.empty())
                for (auto& [key, vts] : rtti.vtables) {
                    int64_t off = parseDec(key.substr(key.rfind(':') + 1));
                    for (uint32_t vt : vts)
                        for (uint32_t f : slots(vt)) where[f].insert(off);
                }
            auto it = where.find(names[w[1]]);
            int64_t thunk = 0;  // __ZThn<N>_: mac passes object + N already
            if (w[1].rfind("__ZThn", 0) == 0) thunk = parseDec(w[1].substr(6));
            if (it == where.end() || it->second.size() != 1 || *it->second.begin() == 0 || *it->second.begin() == thunk) {
                names.erase(w[1]);
                continue;
            }
            adj[w[1]] = *it->second.begin() - thunk;
        }
    }

    // ---- guards: a constant a mod has in its source must be unchanged, else its hooks are left out
    std::set<std::string> dropped;
    auto readDisp = [&](uint32_t m, const std::string& pos, const std::string& sz) -> int64_t {
        uint32_t p = m + (uint32_t)parseDec(pos);
        return parseDec(sz) == 1 ? (int64_t)(int8_t)im.m[p] : (int64_t)i32(im.at(p));
    };
    for (size_t i = 0; i < lines.size(); i++) {
        auto& w = lines[i];
        bool ok;
        if (w[0] == "guard" && w.size() == 6) ok = hits[i].size() == 1 && readDisp(hits[i][0], w[4], w[5]) == parseHex(w[2]);
        else if (w[0] == "guardin" && w.size() == 8) {
            uint32_t m;
            ok = scoped(w[3], w[4], w[5], &m) && readDisp(m, w[6], w[7]) == parseHex(w[2]);
        } else continue;
        if (ok) continue;
        r.missing.push_back("guard " + w[1] + " " + w[2]);
        for (size_t a = 0, b; a <= w[1].size(); a = b + 1) {  // the hooks, comma-separated
            b = w[1].find(',', a);
            if (b == std::string::npos) b = w[1].size();
            dropped.insert(w[1].substr(a, b - a));
        }
    }

    // ---- hooks: every site found, each the call/jmp of the same target
    struct Site { std::string kind; uint32_t site; int64_t target; };
    std::map<std::string, std::vector<Site>> sites;
    std::map<std::string, std::vector<const std::vector<std::string>*>> hookCalls;  // the hookcall lines per hook
    std::map<std::string, int> wantSites;
    std::set<std::string> badHook, regHooks;
    // a hookreg hook found by hookcall: its own bytes (registers exact, displacements loose) must be at the site
    std::map<std::string, std::vector<std::pair<Pattern, int64_t>>> regPats;
    for (auto& w : lines)  // these read a register of the caller: their sites must be unchanged code (no loose match)
        if (w[0] == "hookreg" && w.size() >= 2) regHooks.insert(w[1]);
    for (size_t i = 0; i < lines.size(); i++) {
        auto& w = lines[i];
        if (w[0] == "hooksites" && w.size() == 3) { wantSites[w[1]] = (int)parseDec(w[2]); continue; }
        if (w[0] == "hookcall" && w.size() == 8) { hookCalls[w[1]].push_back(&w); continue; }
        if (regHooks.count(w[1]) && w[0] == "hook" && w.size() == 6) regPats[w[1]].push_back({Pattern(w[3]), parseDec(w[4])});
        if (regHooks.count(w[1]) && w[0] == "hookin" && w.size() == 8) regPats[w[1]].push_back({Pattern(w[5]), parseDec(w[6])});
        uint32_t m;
        std::string kind, pos, tname;
        if (w[0] == "hook" && w.size() == 6) {
            std::vector<uint32_t> h = regHooks.count(w[1]) ? Pattern(w[3]).find(im, 2, false) : hits[i];
            if (h.size() != 1) continue;
            m = h[0], kind = w[2], pos = w[4], tname = w[5];
        } else if (w[0] == "hookin" && w.size() == 8) {
            if (!scoped(w[3], w[4], w[5], &m, regHooks.count(w[1]) > 0)) continue;
            kind = w[2], pos = w[6], tname = w[7];
        } else continue;
        uint32_t s = m + (uint32_t)parseDec(pos);
        if (!im.has(s, 6)) continue;
        const uint8_t* c = im.at(s);
        int64_t target;
        if (kind == "call" && c[0] == 0xE8) target = (int64_t)s + 5 + i32(c + 1);
        else if (kind == "jmp" && c[0] == 0xE9) target = (int64_t)s + 5 + i32(c + 1);
        else if (kind == "icall" && c[0] == 0xFF && c[1] == 0x15) {  // the import slot's (unbound) value, as the table has it
            int64_t slot = (int64_t)s + 6 + i32(c + 2);
            if (!im.has(slot, 8)) continue;
            target = (int64_t)(u64(im.at((uint32_t)slot)) - im.base);
        } else if (kind == "lea" && im.has(s, 7) && (c[0] & 0xFB) == 0x48 && c[1] == 0x8D && (c[2] & 0xC7) == 0x05) {
            // lea reg,[rip+fn]: a function's address taken for a callback table (core/image_win.cpp changes the
            // displacement); it must be code
            target = (int64_t)s + 7 + i32(c + 3);
            if (!im.inText(target)) { badHook.insert(w[1]); continue; }
        } else { badHook.insert(w[1]); continue; }
        auto t = names.find(tname);
        if (tname != "-" && t != names.end() && t->second != target) { badHook.insert(w[1]); continue; }
        sites[w[1]].push_back({kind, s, target});
    }
    std::vector<std::string> hookLines;
    std::set<std::string> kept;
    // complete: every site once, all the same kind and target
    auto complete = [](const std::vector<Site>& v, int want) {
        std::set<uint32_t> distinct;
        bool same = true;
        for (auto& s : v) {
            distinct.insert(s.site);
            same = same && s.kind == v[0].kind && s.target == v[0].target;
        }
        return same && (int)distinct.size() == want && (int)v.size() == want;
    };
    for (auto& [name, want] : wantSites) {
        // The sites by their own bytes; else by their place among the calls of the target in the found caller.
        bool ok = !badHook.count(name) && complete(sites[name], want);
        if (!ok && hookCalls.count(name)) {
            // hookcall <name> <kind> <anchor|*> <range> <target> <k> <n>: the k-th of the n call/jmp rel32 to the
            // target in the anchor's extent (* all of .text)
            std::vector<Site> v;
            for (auto* l : hookCalls[name]) {
                auto& w = *l;
                auto t = names.find(w[5]);
                if (t == names.end()) continue;
                uint32_t lo = im.t0, hi = (uint32_t)(im.t0 + im.tn);
                if (w[3] != "*") {
                    auto a = names.find(w[3]);
                    if (a == names.end()) continue;
                    std::tie(lo, hi) = im.scope(a->second, (uint32_t)parseHex(w[4]));
                }
                uint8_t op = w[2] == "jmp" ? 0xE9 : 0xE8;
                std::vector<uint32_t> at;
                for (uint32_t q = lo; q + 5 <= hi && im.has(q, 5); q++)
                    if (im.m[q] == op && (int64_t)q + 5 + i32(im.at(q + 1)) == (int64_t)t->second) at.push_back(q);
                size_t k = (size_t)parseDec(w[6]);
                if (at.size() != (size_t)parseDec(w[7]) || k >= at.size()) continue;
                if (regHooks.count(name)) {
                    bool same = false;
                    for (auto& [pt, pos] : regPats[name]) same = same || (at[k] >= pos && pt.matches(im, at[k] - (uint32_t)pos, true));
                    if (!same) continue;
                }
                v.push_back({w[2], at[k], t->second});
            }
            // a site may come from more than one line (its caller's and all of .text): once each
            std::sort(v.begin(), v.end(), [](const Site& a, const Site& b) { return a.site < b.site; });
            v.erase(std::unique(v.begin(), v.end(), [](const Site& a, const Site& b) { return a.site == b.site && a.target == b.target; }), v.end());
            if (complete(v, want)) sites[name] = v, ok = true;
        }
        auto& v = sites[name];
        if (dropped.count(name) || !ok) {
            r.missing.push_back("hook " + name);
            continue;
        }
        kept.insert(name);
        std::sort(v.begin(), v.end(), [](const Site& a, const Site& b) { return a.site < b.site; });
        for (auto& s : v) hookLines.push_back("hook " + name + " " + s.kind + " " + hexs(s.site) + " " + hexs(s.target));
    }

    // ---- fields: every line that finds one must give the same value
    struct Field { std::set<int64_t> seen, vt, vtfn, seq; bool same = false; int64_t old = 0; };
    std::map<std::string, Field> fields;
    std::map<std::string, size_t> vtSlots;  // vtable name -> its old slot count
    for (auto& w : lines)
        if (w[0] == "vtslots" && w.size() == 3) vtSlots[w[1]] = (size_t)parseDec(w[2]);
    // The slot a vtable method is in: the one slot that holds it, or (identical functions folded into several
    // slots) the old one if it's among them and the vtable has as many slots as before.
    auto pickSlot = [&](const std::vector<size_t>& at, const std::string& vtName, size_t count, int64_t old, std::set<int64_t>* to) {
        if (at.size() == 1) to->insert((int64_t)at[0] * 8);
        else if (at.size() > 1 && vtSlots.count(vtName) && vtSlots[vtName] == count && old % 8 == 0 &&
                 std::find(at.begin(), at.end(), (size_t)(old / 8)) != at.end())
            to->insert(old);
    };
    for (size_t i = 0; i < lines.size(); i++) {
        auto& w = lines[i];
        const std::string& k = w[0];
        if (k.rfind("off", 0) != 0 || w.size() < 3) continue;
        Field& f = fields[w[1]];
        f.old = parseHex(w[2]);
        if ((k == "off" && w.size() == 6 && hits[i].size() == 1) || (k == "offany" && w.size() == 6 && !hits[i].empty() && hits[i].size() <= 8)) {
            for (uint32_t m : hits[i]) f.seen.insert(readDisp(m, w[4], w[5]));
        } else if (k == "offin" && w.size() == 8) {
            uint32_t m;
            if (scoped(w[3], w[4], w[5], &m)) f.seen.insert(readDisp(m, w[6], w[7]));
        } else if (k.rfind("offadj", 0) == 0 && w.size() >= 4) {
            // a field of the whole object read through the subobject the function gets: its adjustment + the
            // displacement (offadj <key> <value> <fn> then an off / offin / offany line's tail)
            auto a = adj.find(w[3]);
            if (a == adj.end()) continue;
            std::string in = k.substr(6);
            if ((in.empty() && w.size() == 7 && hits[i].size() == 1) || (in == "any" && w.size() == 7 && !hits[i].empty() && hits[i].size() <= 8)) {
                for (uint32_t m : hits[i]) f.seen.insert(a->second + readDisp(m, w[5], w[6]));
            } else if (in == "in" && w.size() == 9) {
                uint32_t m;
                if (scoped(w[4], w[5], w[6], &m)) f.seen.insert(a->second + readDisp(m, w[7], w[8]));
            }
        } else if (k == "offsame" && w.size() == 5) {
            auto fn = names.find(w[3]);
            if (fn != names.end() && Pattern(w[4]).matches(im, fn->second)) f.same = true;
        } else if (k == "offseq" && w.size() == 7) {
            // offseq <key> <value> <fn> <range> <gap> <d0,d1,...>: the v for which v+d0, v+d1, ... are dwords in
            // the function in that order, each within gap bytes of the one before (the reads through one register
            // around the field: its struct moved, or the compiler picked another register)
            auto fn = names.find(w[3]);
            if (fn == names.end()) continue;
            auto [lo, hi] = im.scope(fn->second, (uint32_t)parseHex(w[4]));
            uint32_t gap = (uint32_t)parseDec(w[5]);
            std::vector<int64_t> d;
            for (size_t a = 0, b; a < w[6].size(); a = b + 1) {
                b = w[6].find(',', a);
                if (b == std::string::npos) b = w[6].size();
                std::string x = w[6].substr(a, b - a);
                d.push_back(x[0] == '-' ? -parseHex(x.substr(1)) : parseHex(x));
            }
            std::set<int64_t> vals;
            for (uint32_t q = lo; q + 4 <= hi && im.has(q, 4) && vals.size() < 2; q++) {
                int64_t v = (int64_t)i32(im.at(q)) - d[0];
                uint32_t p = q;
                bool ok = true;
                for (size_t j = 1; j < d.size() && ok; j++) {
                    int64_t want = v + d[j];
                    ok = false;
                    for (uint32_t p2 = p + 4; p2 < p + 4 + gap && p2 + 4 <= hi && im.has(p2, 4); p2++)
                        if ((int64_t)i32(im.at(p2)) == want) { p = p2; ok = true; break; }
                }
                if (ok) vals.insert(v);
            }
            if (vals.size() == 1) f.seq.insert(*vals.begin());
        } else if (k == "offvt" && w.size() == 5) {
            uint32_t vt;
            auto fn = names.find(w[4]);
            if (fn == names.end() || !vtableOf(w[3], &vt)) continue;
            auto s = slots(vt);
            std::vector<size_t> at;
            for (size_t j = 0; j < s.size(); j++)
                if (s[j] == fn->second) at.push_back(j);
            pickSlot(at, w[3], s.size(), f.old, &f.vt);
        } else if (k == "offvtfn" && w.size() == 5) {
            uint32_t vt;
            if (!vtableOf(w[3], &vt)) continue;
            Pattern pt(w[4]);
            auto s = slots(vt);
            std::vector<size_t> at;
            for (int loose = 0; loose < 3 && at.empty(); loose++)
                for (size_t j = 0; j < s.size(); j++)
                    if (pt.matches(im, s[j], loose)) at.push_back(j);
            pickSlot(at, w[3], s.size(), f.old, &f.vtfn);
        }
    }
    std::vector<std::string> offLines;
    std::map<std::string, int64_t> value;
    std::map<std::string, std::set<int64_t>> rel;
    int fieldsFound = 0;
    for (auto& [key, f] : fields) {
        std::set<int64_t> v;
        if (!f.vt.empty() || !f.vtfn.empty()) {
            v = f.vt;
            v.insert(f.vtfn.begin(), f.vtfn.end());
        } else if (!f.seen.empty()) v = f.seen;
        else if (f.same) v = {f.old};
        else v = f.seq;  // (only when nothing else finds the field: its own bytes say more)
        if (v.size() != 1) continue;
        value[key] = *v.begin();
    }
    // offrel <key> <value> <other key> <other value>: still missing, the other field's new value + the old difference
    for (auto& w : lines)
        if (w[0] == "offrel" && w.size() == 5 && !value.count(w[1]) && value.count(w[3]))
            rel[w[1]].insert(value[w[3]] + parseHex(w[2]) - parseHex(w[4]));
    for (auto& [k, v] : rel)
        if (v.size() == 1) value[k] = *v.begin();
    for (auto& [key, f] : fields) {
        if (!value.count(key)) { r.missing.push_back("off " + unescape(key)); continue; }
        offLines.push_back("off " + unescape(key) + " " + hexs(value[key]));
        fieldsFound++;
    }

    // ---- the table
    std::vector<std::string> out{"build " + r.build,
                                 "# platendoos address table for rekordbox.exe " + r.build + ", made by the patcher (patcher/retable.h) from signatures of " + r.from};
    int namesFound = 0;
    for (auto& [n, a] : names)
        if (n[0] != '@') out.push_back("addr " + unescape(n) + " " + hexs(a)), namesFound++;
    for (auto& n : named)
        if (!names.count(n)) r.missing.push_back("addr " + unescape(n));
    out.insert(out.end(), offLines.begin(), offLines.end());
    out.insert(out.end(), hookLines.begin(), hookLines.end());
    for (auto& w : lines) {
        if (w[0] == "hookreg" && w.size() == 4 && kept.count(w[1])) out.push_back("hookreg " + w[1] + " " + w[2] + " " + w[3]);
        if (w[0] == "adj" && w.size() == 3 && adj.count(w[1])) out.push_back("adj " + unescape(w[1]) + " " + std::to_string(adj[w[1]]));
    }
    int uncoveredParts = 0;  // a hook with only some sites uncovered is already counted (and listed) as a hook
    for (auto& u : uncovered) {
        auto w = split(u);
        if (w.size() >= 2 && w[0] == "hook" && wantSites.count(w[1])) {
            for (auto& m : r.missing)
                if (m == "hook " + w[1]) m += " (a site isn't in the signatures)";
            continue;
        }
        r.missing.push_back(u + " (not in the signatures)"), uncoveredParts++;
    }
    r.found = namesFound + fieldsFound + (int)kept.size();
    r.total = (int)named.size() + (int)fields.size() + (int)wantSites.size() + uncoveredParts;
    out.push_back("# " + std::to_string(r.found) + " of " + std::to_string(r.total) + " parts found");
    for (auto& m : r.missing) out.push_back("# not found: " + m);
    for (auto& l : out) r.table += l + "\n";
    prog(1);
    return r;
}

}  // namespace retable
