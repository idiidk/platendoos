// Signatures for finding a Windows address table's entries in another rekordbox build: a C++ port of
// tools/win/makesigs.py (whose header documents the line format), byte-identical in output and several hundred
// times faster (a 4-gram index of .text for the uniqueness searches, call/rip-reference indexes, the per-name
// work spread over all cores where it doesn't depend on what earlier names found).
//   clang++ -std=c++17 -O2 -I/opt/homebrew/include -L/opt/homebrew/lib -lcapstone -o build/makesigs tools/win/makesigs.cpp
//   build/makesigs <rekordbox.exe> <its .tbl> [workdir] > tools/win/sigs/rekordbox-<build>.sig
// workdir (default ~/rbwin) holds verify/*.out.json and win_chunks.txt ("va size root" in hex per line: the .pdata-
// independent function chunks the Python reads from win.pkl). tools/win/{sigfields,sigguards}.txt and
// tools/win/verify/*.out.json are found next to this source (the repo root: $MAKESIGS_REPO, or the parent of the
// binary's directory, or the current directory). The full edition (RBM_EDITION, full by default where the repo has
// private/; docs/editions.md) adds private/tools/win/verify/*.out.json and private/tools/win/sigfields.txt, as
// tools/win/verifydata.py does for the Python.
// Needs capstone (5.0.x) with its detail mode: operand/encoding info like the Python's.
#include <capstone/capstone.h>

#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <regex>
#include <set>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

using namespace std;
typedef int64_t i64;
typedef uint32_t u32;
typedef uint64_t u64;

static const i64 NONE = -1;  // python's None for an rva

// ------------------------------------------------------------------------------------------------ helpers
static string strf(const char* f, ...) __attribute__((format(printf, 1, 2)));
static string strf(const char* f, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, f);
    int n = vsnprintf(buf, sizeof buf, f, ap);
    va_end(ap);
    if (n < (int)sizeof buf) return string(buf, n > 0 ? n : 0);
    string s(n + 1, 0);
    va_start(ap, f);
    vsnprintf(&s[0], n + 1, f, ap);
    va_end(ap);
    s.resize(n);
    return s;
}
static string hexs(i64 v) { return v < 0 ? strf("-%llx", (unsigned long long)-v) : strf("%llx", (unsigned long long)v); }
static string E(string n) { for (auto& c : n) if (c == ' ') c = '~'; return n; }
static bool startsWith(const string& s, const char* p) { return s.compare(0, strlen(p), p) == 0; }
static bool endsWith(const string& s, const char* p) { size_t n = strlen(p); return s.size() >= n && s.compare(s.size() - n, n, p) == 0; }
static vector<string> splitWs(const string& s) {
    vector<string> w;
    size_t i = 0;
    while (i < s.size()) {
        while (i < s.size() && isspace((unsigned char)s[i])) i++;
        size_t j = i;
        while (j < s.size() && !isspace((unsigned char)s[j])) j++;
        if (j > i) w.push_back(s.substr(i, j - i));
        i = j;
    }
    return w;
}
static string readFile(const string& p, bool* ok = nullptr) {
    FILE* f = fopen(p.c_str(), "rb");
    if (!f) { if (ok) *ok = false; return ""; }
    string s;
    char buf[1 << 16];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) s.append(buf, n);
    fclose(f);
    if (ok) *ok = true;
    return s;
}
static vector<string> lines(const string& s) {  // python's iteration over a file (split at \n)
    vector<string> r;
    size_t i = 0;
    while (i < s.size()) {
        size_t j = s.find('\n', i);
        if (j == string::npos) { r.push_back(s.substr(i)); break; }
        r.push_back(s.substr(i, j - i));
        i = j + 1;
    }
    return r;
}
static string latin1ToUtf8(const string& s) {
    string r;
    for (unsigned char c : s) {
        if (c < 0x80) r += (char)c;
        else { r += (char)(0xC0 | (c >> 6)); r += (char)(0x80 | (c & 0x3F)); }
    }
    return r;
}

static int NTHREADS = 1;
template <class F> static void parallel_for(size_t n, F f, size_t grain = 1) {
    atomic<size_t> next{0};
    auto work = [&] {
        for (;;) {
            size_t i = next.fetch_add(grain);
            if (i >= n) break;
            size_t e = min(n, i + grain);
            for (; i < e; i++) f(i);
        }
    };
    vector<thread> ts;
    for (int t = 1; t < NTHREADS; t++) ts.emplace_back(work);
    work();
    for (auto& t : ts) t.join();
}

// ------------------------------------------------------------------------------------------------ the PE
static string FILEDATA;
static const uint8_t* Fp;
static i64 Fn;
static i64 HDRLEN;
struct Sec { string name; i64 va, vsize, ptr, raw, mn, mx; };
static vector<Sec> secs;
static i64 BASE, IMGSIZE;
static u32 STAMP;
static i64 T0;
static const uint8_t* TB;
static i64 TBn;
static vector<pair<i64, i64>> rdata;
static vector<pair<u32, u32>> pdata;  // sorted (begin, end)
static vector<u32> pbeg;

static u32 ld32(const uint8_t* p) { u32 v; memcpy(&v, p, 4); return v; }
static u64 ld64(const uint8_t* p) { u64 v; memcpy(&v, p, 8); return v; }

struct Span { const uint8_t* p; i64 n; };
static Span pyslice(const uint8_t* d, i64 len, i64 s, i64 e) {
    if (s < 0) { s += len; if (s < 0) s = 0; } else if (s > len) s = len;
    if (e < 0) { e += len; if (e < 0) e = 0; } else if (e > len) e = len;
    if (e < s) e = s;
    return {d + s, e - s};
}
// pefile's get_data(rva, n); an empty span where it raises
static Span imgSpan(i64 rva, i64 n) {
    for (auto& s : secs)
        if (s.mn <= rva && rva < s.mx) {
            i64 off = rva - s.va + s.ptr, end = off + n;
            if (end > s.ptr + s.raw) end = s.ptr + s.raw;
            return pyslice(Fp, Fn, off, end);
        }
    if (rva < HDRLEN) return pyslice(Fp, HDRLEN, rva, rva + n);
    if (rva < Fn) return pyslice(Fp, Fn, rva, rva + n);
    return {Fp, 0};
}
static string img(i64 rva, i64 n) { Span s = imgSpan(rva, n); return string((const char*)s.p, s.n); }
static bool in_text(i64 rva) { return T0 <= rva && rva < T0 + TBn; }

static void loadPE(const string& path) {
    bool ok;
    FILEDATA = readFile(path, &ok);
    if (!ok) { fprintf(stderr, "cannot read %s\n", path.c_str()); exit(1); }
    Fp = (const uint8_t*)FILEDATA.data();
    Fn = FILEDATA.size();
    u32 nt = ld32(Fp + 0x3c);
    const uint8_t* fh = Fp + nt + 4;
    int nsec = fh[2] | fh[3] << 8, optSize = fh[16] | fh[17] << 8;
    STAMP = ld32(fh + 4);
    const uint8_t* opt = fh + 20;
    BASE = (i64)ld64(opt + 24);
    IMGSIZE = ld32(opt + 56);
    HDRLEN = ld32(opt + 60);
    u32 excRva = ld32(opt + 112 + 3 * 8), excSize = ld32(opt + 112 + 3 * 8 + 4);
    const uint8_t* sh = fh + 20 + optSize;
    for (int i = 0; i < nsec; i++, sh += 40) {
        Sec s;
        s.name = string((const char*)sh, strnlen((const char*)sh, 8));
        s.vsize = ld32(sh + 8);
        s.va = ld32(sh + 12);
        s.raw = ld32(sh + 16);
        s.ptr = ld32(sh + 20);
        secs.push_back(s);
    }
    // contains_rva as pefile works it out (section sizes cut at the next section by virtual address)
    vector<i64> vas;
    for (auto& s : secs) vas.push_back(s.va);
    sort(vas.begin(), vas.end());
    for (auto& s : secs) {
        i64 size = (Fn - s.ptr < s.raw) ? s.vsize : max(s.raw, s.vsize);
        auto it = upper_bound(vas.begin(), vas.end(), s.va);
        if (it != vas.end() && s.va + size > *it) size = *it - s.va;
        s.mn = s.va;
        s.mx = s.va + size;
    }
    const Sec* text = nullptr;
    for (auto& s : secs) if (s.name == ".text") { text = &s; break; }
    if (!text) { fprintf(stderr, "no .text\n"); exit(1); }
    T0 = text->va;
    Span tb = pyslice(Fp, Fn, text->ptr, text->ptr + text->vsize);
    TB = tb.p;
    TBn = tb.n;
    for (auto& s : secs) if (s.name == ".rdata" || s.name == ".data") rdata.push_back({s.va, s.va + s.vsize});
    Span ex = imgSpan(excRva, excSize);
    for (i64 i = 0; i < (i64)excSize / 12 && (i + 1) * 12 <= ex.n; i++) pdata.push_back({ld32(ex.p + i * 12), ld32(ex.p + i * 12 + 4)});
    sort(pdata.begin(), pdata.end());
    for (auto& p : pdata) pbeg.push_back(p.first);
}

// ------------------------------------------------------------------------------------------------ decoding
struct Ins {
    u32 a;
    uint8_t sz, dispOff, dispSize, immOff, immSize, hasTgt, mn;  // mn: 1 ret, 2 jmp, 3 int3
    uint16_t m, lz, r1, r2;  // byte-index masks: moves between builds / loose / ModRM-SIB / REX
    int32_t memBase;         // first memory operand (not rip, no index) base register, or 0
    i64 disp, tgt;
};
struct Dec { vector<Ins> v; i64 next = 0; bool done = false; };

struct CsThread {
    csh h = 0;
    cs_insn* in = nullptr;
    CsThread() {
        cs_open(CS_ARCH_X86, CS_MODE_64, &h);
        cs_option(h, CS_OPT_DETAIL, CS_OPT_ON);
        in = cs_malloc(h);
    }
    ~CsThread() { cs_free(in, 1); cs_close(&h); }
};
static CsThread& cst() { static thread_local CsThread t; return t; }

static void decodeMore(Dec& d, i64 start, i64 limit) {
    CsThread& c = cst();
    while (!d.done && d.next < limit) {
        i64 off = d.next - T0;
        if (off < 0 || off >= TBn) { d.done = true; break; }
        const uint8_t* code = TB + off;
        size_t size = TBn - off;
        u64 addr = d.next;
        if (!cs_disasm_iter(c.h, &code, &size, &addr, c.in)) { d.done = true; break; }
        cs_insn* in = c.in;
        cs_x86& x = in->detail->x86;
        Ins r;
        memset(&r, 0, sizeof r);
        r.a = (u32)in->address;
        r.sz = (uint8_t)in->size;
        r.dispOff = x.encoding.disp_offset;
        r.dispSize = x.encoding.disp_size;
        r.immOff = x.encoding.imm_offset;
        r.immSize = x.encoding.imm_size;
        r.disp = x.disp;
        bool rel = false;
        for (int i = 0; i < in->detail->groups_count; i++)
            if (in->detail->groups[i] == CS_GRP_JUMP || in->detail->groups[i] == CS_GRP_CALL) rel = true;
        bool ripmem = false;
        r.memBase = 0;
        bool haveBase = false;
        for (int i = 0; i < x.op_count; i++) {
            const cs_x86_op& o = x.operands[i];
            if (o.type != X86_OP_MEM) continue;
            if (o.mem.base == X86_REG_RIP) ripmem = true;
            if (!haveBase && o.mem.base != X86_REG_INVALID && o.mem.base != X86_REG_RIP && o.mem.index == X86_REG_INVALID) {
                r.memBase = o.mem.base;
                haveBase = true;
            }
        }
        uint16_t m = 0;
        if (r.dispSize && ripmem) {
            for (int k = r.dispOff; k < r.dispOff + r.dispSize; k++) m |= 1 << k;
            if (r.dispSize == 4) { r.hasTgt = 1; r.tgt = (i64)in->address + in->size + x.disp; }
        }
        if (rel && r.immSize == 4) for (int k = r.immOff; k < r.immOff + 4; k++) m |= 1 << k;
        r.m = m;
        uint16_t lz = 0;  // loose_bytes
        if ((r.dispSize == 1 || r.dispSize == 4) && r.dispOff) for (int k = r.dispOff; k < r.dispOff + r.dispSize; k++) lz |= 1 << k;
        if ((r.immSize == 1 || r.immSize == 2 || r.immSize == 4) && r.immOff && (!rel || r.immSize == 1))
            for (int k = r.immOff; k < r.immOff + r.immSize; k++) lz |= 1 << k;
        r.lz = lz & ~m;
        // reg_bytes
        const uint8_t* raw = in->bytes;
        int len = in->size;
        if (!(raw[0] == 0xc4 || raw[0] == 0xc5 || raw[0] == 0x62)) {
            int i = 0;
            while (i < len) {
                uint8_t b = raw[i];
                if (b == 0x66 || b == 0x67 || b == 0xf0 || b == 0xf2 || b == 0xf3 || b == 0x2e || b == 0x36 || b == 0x3e || b == 0x26 || b == 0x64 || b == 0x65) i++;
                else break;
            }
            if (x.rex && i < len && raw[i] >= 0x40 && raw[i] <= 0x4f && raw[i] == x.rex) r.r2 |= 1 << i;
            if (x.encoding.modrm_offset) {
                int mo = x.encoding.modrm_offset;
                r.r1 |= 1 << mo;
                if ((raw[mo] >> 6) != 3 && (raw[mo] & 7) == 4 && mo + 1 < len) r.r1 |= 1 << (mo + 1);
                r.r2 &= ~r.r1;
            }
        }
        const char* mn = in->mnemonic;
        r.mn = !strcmp(mn, "ret") ? 1 : !strcmp(mn, "jmp") ? 2 : !strcmp(mn, "int3") ? 3 : 0;
        d.v.push_back(r);
        d.next = (i64)in->address + in->size;
    }
    if (d.next >= T0 + TBn) d.done = true;
}

struct View {
    shared_ptr<const Dec> d;
    size_t n = 0;
    const Ins& operator[](size_t i) const { return d->v[i]; }
    size_t size() const { return n; }
    const Ins* begin() const { return d->v.data(); }
    const Ins* end() const { return d->v.data() + n; }
};
struct DecShard { mutex mu; unordered_map<i64, shared_ptr<const Dec>> m; };
static DecShard decShards[64];
static View insns(i64 start, i64 end) {
    View vw;
    if (end <= start) { vw.d = make_shared<Dec>(); return vw; }
    DecShard& sh = decShards[(start >> 1) & 63];
    shared_ptr<const Dec> cur;
    {
        lock_guard<mutex> g(sh.mu);
        auto it = sh.m.find(start);
        if (it != sh.m.end()) cur = it->second;
    }
    if (!cur || (!cur->done && cur->next < end)) {
        auto nd = make_shared<Dec>();
        i64 limit = end;
        if (cur) { *nd = *cur; limit = max(end, start + 2 * (cur->next - start)); }
        else { nd->next = start; limit = max(end, start + 256); }
        decodeMore(*nd, start, limit);
        if (!nd->done && nd->next < end) nd->done = true;  // (cannot happen: decodeMore runs to the limit)
        cur = nd;
        lock_guard<mutex> g(sh.mu);
        auto it = sh.m.find(start);
        if (it == sh.m.end() || it->second->next < cur->next || (cur->done && !it->second->done)) sh.m[start] = cur;
    }
    vw.d = cur;
    auto b = cur->v.begin();
    auto pp = partition_point(b, cur->v.end(), [&](const Ins& x) { return (i64)x.a + x.sz <= end; });
    vw.n = pp - b;
    return vw;
}

// fn_end of a leaf (not a .pdata begin): the first ret/jmp/int3 followed by padding, within 4096 bytes
static mutex leafMu;
static unordered_map<i64, i64> leafEnd;
static i64 fn_end(i64 rva) {
    auto it = upper_bound(pbeg.begin(), pbeg.end(), rva);
    i64 i = (it - pbeg.begin()) - 1;
    if (i >= 0 && pdata[i].first == rva) return pdata[i].second;
    {
        lock_guard<mutex> g(leafMu);
        auto f = leafEnd.find(rva);
        if (f != leafEnd.end()) return f->second;
    }
    i64 end = rva;
    View L = insns(rva, rva + 4096);
    for (const Ins& x : L) {
        end = (i64)x.a + x.sz;
        if (x.mn && end - T0 >= 0 && end - T0 < TBn && (TB[end - T0] == 0xcc || TB[end - T0] == 0x90)) break;
    }
    lock_guard<mutex> g(leafMu);
    leafEnd[rva] = end;
    return end;
}

static vector<i64> leafs;
static i64 chunk_start(i64 rva) {
    auto it = upper_bound(pbeg.begin(), pbeg.end(), rva);
    i64 i = (it - pbeg.begin()) - 1;
    if (i >= 0 && (i64)pdata[i].first <= rva && rva < (i64)pdata[i].second) return pdata[i].first;
    auto jt = upper_bound(leafs.begin(), leafs.end(), rva);
    i64 j = (jt - leafs.begin()) - 1;
    return (j >= 0 && rva < fn_end(leafs[j])) ? leafs[j] : NONE;
}

// ------------------------------------------------------------------------------------------------ strings, patterns
static optional<string> string_at(i64 rva) {
    bool ok = false;
    for (auto& r : rdata) if (r.first <= rva && rva < r.second) ok = true;
    if (!ok) return nullopt;
    Span s = imgSpan(rva, 48);
    const uint8_t* d = s.p;
    i64 len = s.n, n = 0;
    while (n < len && d[n] >= 0x20 && d[n] < 0x7f) n++;
    if (n >= 4 && (n == len || d[n] == 0)) return string((const char*)d, min<i64>(min<i64>(n + 1, 32), len));
    n = 0;
    while (n + 1 < len && d[n] >= 0x20 && d[n] < 0x7f && d[n + 1] == 0) n += 2;
    if (n >= 8 && (n + 1 >= len || (d[n] == 0 && d[n + 1] == 0))) return string((const char*)d, min<i64>(min<i64>(n + 2, 32), len));
    return nullopt;
}

enum { MK = 1, LZ = 2, R1 = 4, R2 = 8 };
struct Con { int p, e; string d; };
struct Pat {
    i64 start = 0;
    vector<uint8_t> b, fl;
    vector<Con> cons;
    string text() const {
        string s;
        s.reserve(b.size() * 3 + 16);
        char t[8];
        for (size_t i = 0; i < b.size(); i++) {
            uint8_t f = fl[i];
            if (f & MK) s += "??";
            else if (f & LZ) { snprintf(t, sizeof t, "*%02x", b[i]); s += t; }
            else if (f & R1) { snprintf(t, sizeof t, "%%%02x", b[i]); s += t; }
            else if (f & R2) { snprintf(t, sizeof t, "^%02x", b[i]); s += t; }
            else { snprintf(t, sizeof t, "%02x", b[i]); s += t; }
        }
        for (auto& c : cons) {
            s += strf("|%d,%d,", c.p, c.e);
            for (unsigned char ch : c.d) { snprintf(t, sizeof t, "%02x", ch); s += t; }
        }
        return s;
    }
};
struct Rng { i64 lo = 0, hi = 0; };  // extra_mask: absolute addresses [lo, hi)

static Pat mkPat(i64 start, i64 end, Rng ex = Rng()) {
    Pat p;
    p.start = start;
    i64 cs0 = chunk_start(start);
    i64 cs = cs0 == NONE ? start : cs0;
    i64 n = end - start;
    if (n < 0) n = 0;
    if (start - T0 < 0 || start - T0 + n > TBn) { fprintf(stderr, "pattern outside .text at %llx\n", (unsigned long long)start); exit(2); }
    p.b.assign(TB + (start - T0), TB + (start - T0) + n);
    p.fl.assign(n, 0);
    View V = insns(cs, end);
    auto it = partition_point(V.begin(), V.end(), [&](const Ins& x) { return (i64)x.a + x.sz <= start; });
    for (; it != V.end(); ++it) {
        const Ins& x = *it;
        if (x.a >= end) break;
        for (int k = 0; k < 16; k++) {
            uint16_t bit = 1 << k;
            if (!((x.m | x.lz | x.r1 | x.r2) & bit)) continue;
            i64 pos = (i64)x.a + k;
            if (pos < start || pos >= end) continue;
            uint8_t& f = p.fl[pos - start];
            if (x.m & bit) f |= MK;
            if (x.lz & bit) f |= LZ;
            if (x.r1 & bit) f = (f & ~R2) | R1;
            else if (x.r2 & bit) f = (f & ~R1) | R2;
        }
        if (x.hasTgt && start <= x.a && (i64)x.a + x.sz <= end) {
            auto st = string_at(x.tgt);
            if (st) p.cons.push_back({(int)(x.a + x.dispOff - start), (int)(x.a + x.sz - start), *st});
        }
    }
    for (i64 x = ex.lo; x < ex.hi; x++) if (start <= x && x < end) p.fl[x - start] |= MK;
    return p;
}

static bool pat_at(const Pat& p, i64 q, int loose) {
    i64 n = p.b.size();
    if (q < 0 || q + n > TBn) return false;
    const uint8_t* t = TB + q;
    const uint8_t* b = p.b.data();
    const uint8_t* fl = p.fl.data();
    for (i64 i = 0; i < n; i++) {
        uint8_t f = fl[i];
        if (f & MK) continue;
        if (t[i] == b[i]) continue;
        if (loose && (f & LZ)) continue;
        if (loose >= 2 && (f & (R1 | R2)) && !((t[i] ^ b[i]) & ((f & R1) ? 0xC0 : 0xF8))) continue;
        return false;
    }
    for (auto& c : p.cons) {
        int32_t disp;
        memcpy(&disp, t + c.p, 4);
        Span s = imgSpan(q + T0 + c.e + disp, c.d.size());
        if (s.n != (i64)c.d.size() || memcmp(s.p, c.d.data(), s.n)) return false;
    }
    return true;
}

// 4-gram index of .text: positions by hash of the 4 bytes there
static vector<u32> gOff, gPos;
static const int GBITS = 22;
static inline u32 gh(u32 g) { return (g * 2654435761u) >> (32 - GBITS); }
static void buildGramIndex() {
    i64 n = TBn - 3;
    const int NB = NTHREADS;
    i64 blk = (n + NB - 1) / NB;
    vector<vector<u32>> cnt(NB);
    parallel_for(NB, [&](size_t b) {
        cnt[b].assign((size_t(1) << GBITS), 0);
        i64 lo = b * blk, hi = min(n, lo + blk);
        for (i64 i = lo; i < hi; i++) cnt[b][gh(ld32(TB + i))]++;
    });
    gOff.assign((size_t(1) << GBITS) + 1, 0);
    // offsets per (bucket, block): start of block b's entries within bucket h
    // (kept as one running pass: gOff[h] = total before h; block offsets are rebuilt below)
    for (size_t h = 0; h < (size_t(1) << GBITS); h++) {
        u32 t = 0;
        for (int b = 0; b < NB; b++) t += cnt[b][h];
        gOff[h + 1] = gOff[h] + t;
    }
    gPos.assign(n > 0 ? n : 0, 0);
    // fill in position order per bucket: block b starts at gOff[h] + sum of cnt of blocks < b
    parallel_for(NB, [&](size_t b) {
        // thread b fills its block; compute its starting cursor per bucket lazily
        // (sum over earlier blocks): done with a per-bucket prefix computed on the fly
        vector<u32> cur((size_t(1) << GBITS));
        for (size_t h = 0; h < (size_t(1) << GBITS); h++) {
            u32 s = gOff[h];
            for (size_t c = 0; c < b; c++) s += cnt[c][h];
            cur[h] = s;
        }
        i64 lo = b * blk, hi = min(n, lo + blk);
        for (i64 i = lo; i < hi; i++) gPos[cur[gh(ld32(TB + i))]++] = (u32)i;
    });
}

static int pat_count(const Pat& p, int limit = 2, int loose = 0) {
    i64 n = p.b.size(), bs = 0, be = 0, rs = 0;
    for (i64 i = 0; i <= n; i++) {
        bool m = true;
        if (i < n) {
            uint8_t f = p.fl[i];
            m = (f & MK) || (loose && (f & LZ)) || (loose >= 2 && (f & (R1 | R2)));
        }
        if (m) {
            if (i - rs > be - bs) { bs = rs; be = i; }
            rs = i + 1;
        }
    }
    i64 L = be - bs;
    if (L < (loose ? 6 : 4)) return limit;
    const uint8_t* nd = p.b.data() + bs;
    i64 bestJ = 0;
    u32 bestC = 0xffffffffu;
    for (i64 j = 0; j + 4 <= L; j++) {
        u32 h = gh(ld32(nd + j));
        u32 c = gOff[h + 1] - gOff[h];
        if (c < bestC) { bestC = c; bestJ = j; }
        if (c == 0) break;
    }
    int hits = 0;
    u32 h = gh(ld32(nd + bestJ));
    for (u32 k = gOff[h]; k < gOff[h + 1]; k++) {
        i64 q = (i64)gPos[k] - bestJ;  // the needle's place
        if (q < 0 || q + L > TBn) continue;
        if (memcmp(TB + q, nd, L)) continue;
        if (pat_at(p, q - bs, loose)) {
            hits++;
            if (hits >= limit) return hits;
        }
    }
    return hits;
}
static int pat_count_in(const Pat& p, i64 lo, i64 hi, int limit = 2, int loose = 0) {
    int hits = 0;
    i64 stop = min(hi - T0, TBn - (i64)p.b.size() + 1);
    for (i64 q = lo - T0; q < stop; q++) {
        if (loose < 2 && !((p.fl[0] & MK) || (loose && (p.fl[0] & LZ)) || TB[q] == p.b[0])) continue;
        if (pat_at(p, q, loose)) {
            hits++;
            if (hits >= limit) return hits;
        }
    }
    return hits;
}

static i64 whole_end(i64 start, i64 n) {
    i64 end = start, lim = min(fn_end(start), start + n);
    View V = insns(start, lim);
    for (const Ins& x : V) if ((i64)x.a + x.sz <= lim) end = (i64)x.a + x.sz;
    return end > start ? end : lim;
}

static optional<Pat> unique_from(i64 start, i64 maxlen, i64 minlen = 16) {
    i64 end = min(fn_end(start), start + maxlen);
    View V = insns(start, end);
    vector<i64> ins;
    for (const Ins& x : V) ins.push_back((i64)x.a + x.sz);
    for (int loose = 1; loose >= 0; loose--)
        for (i64 stop : ins) {
            if (stop - start < minlen && stop != ins.back()) continue;
            Pat pt = mkPat(start, stop);
            if (pat_count(pt, 2, loose) == 1) return pt;
        }
    return nullopt;
}

struct Win { i64 lo, hi; };
static vector<Win> windows(i64 site, i64 size, i64 after = 96, i64 before = 160, bool back = false) {
    vector<Win> res;
    i64 cs = chunk_start(site);
    if (cs == NONE) return res;
    View L = insns(cs, min(fn_end(cs), site + size + after));
    auto it = partition_point(L.begin(), L.end(), [&](const Ins& x) { return (i64)x.a < site; });
    if (it == L.end() || (i64)it->a != site) return res;
    i64 idx = it - L.begin(), n = L.size();
    i64 lo = idx, hi = idx + 1;
    for (int step = 0; step < 80; step++) {
        res.push_back({(i64)L[lo].a, (i64)L[hi - 1].a + L[hi - 1].sz});
        if (step % 2 == 0 && hi < n && !back) hi++;
        else if (lo > 0 && site - (i64)L[lo - 1].a <= before) lo--;
        else if (hi < n) hi++;
        else break;
    }
    return res;
}

static optional<Pat> unique_around(i64 site, i64 size, Rng ex = Rng()) {
    vector<Win> W = windows(site, size);
    for (int loose = 1; loose >= 0; loose--)
        for (auto& w : W)
            if (w.hi - w.lo >= 12) {
                Pat pt = mkPat(w.lo, w.hi, ex);
                if (pat_count(pt, 2, loose) == 1) return pt;
            }
    return nullopt;
}

// ------------------------------------------------------------------------------------------------ call and rip indexes
static unordered_map<i64, vector<u32>> callIdx[2];  // e8, e9: target -> site offsets in .text
static vector<u64> ripIdx;                           // (target << 32) | (p << 2 | kidx), sorted
static const int RIPK[4] = {0, 1, 2, 4};
static void buildCallIndexes() {
    for (i64 i = 0; i + 5 < TBn; i++) {
        uint8_t c = TB[i];
        if (c != 0xe8 && c != 0xe9) continue;
        int32_t rel;
        memcpy(&rel, TB + i + 1, 4);
        callIdx[c - 0xe8][i + T0 + 5 + (i64)rel].push_back((u32)i);
    }
    i64 n = TBn - 8;
    const int NB = 256;
    i64 blk = (n + NB - 1) / NB;
    vector<vector<u64>> parts(NB);
    parallel_for(NB, [&](size_t b) {
        i64 lo = b * blk, hi = min(n, lo + blk);
        auto& v = parts[b];
        for (i64 p = lo; p < hi; p++) {
            int32_t d;
            memcpy(&d, TB + p, 4);
            for (int ki = 0; ki < 4; ki++) {
                i64 t = T0 + p + 4 + RIPK[ki] + (i64)d;
                if (t >= 0 && t < IMGSIZE) v.push_back(((u64)t << 32) | ((u64)p << 2) | ki);
            }
        }
        sort(v.begin(), v.end());
    });
    size_t tot = 0;
    for (auto& v : parts) tot += v.size();
    ripIdx.reserve(tot);
    // k-way merge by successive pairwise merges
    vector<vector<u64>> cur = std::move(parts);
    while (cur.size() > 1) {
        vector<vector<u64>> nxt((cur.size() + 1) / 2);
        parallel_for(nxt.size(), [&](size_t i) {
            if (2 * i + 1 < cur.size()) {
                nxt[i].resize(cur[2 * i].size() + cur[2 * i + 1].size());
                merge(cur[2 * i].begin(), cur[2 * i].end(), cur[2 * i + 1].begin(), cur[2 * i + 1].end(), nxt[i].begin());
            } else nxt[i] = std::move(cur[2 * i]);
        });
        cur = std::move(nxt);
    }
    ripIdx = std::move(cur[0]);
}

static vector<i64> callers(i64 target) {  // call/jmp rel32 sites to target: e8 ones then e9 ones
    vector<i64> res;
    for (int op = 0; op < 2; op++) {
        auto it = callIdx[op].find(target);
        if (it != callIdx[op].end()) for (u32 p : it->second) res.push_back(p + T0);
    }
    return res;
}
struct Cand { i64 site, sz, doff; };
static vector<Cand> riprefs(i64 target) {
    vector<Cand> res;
    vector<i64> pos[4];
    if (target >= 0 && target < IMGSIZE) {
        auto it = lower_bound(ripIdx.begin(), ripIdx.end(), (u64)target << 32);
        for (; it != ripIdx.end() && (i64)(*it >> 32) == target; ++it) {
            u32 low = (u32)*it;
            pos[low & 3].push_back(low >> 2);
        }
    } else {
        i64 n = TBn - 8;
        for (i64 p = 0; p < n; p++) {
            int32_t d;
            memcpy(&d, TB + p, 4);
            for (int ki = 0; ki < 4; ki++) if (T0 + p + 4 + RIPK[ki] + (i64)d == target) pos[ki].push_back(p);
        }
    }
    for (int ki = 0; ki < 4; ki++) {
        sort(pos[ki].begin(), pos[ki].end());
        int taken = 0;
        for (i64 pp : pos[ki]) {
            if (taken++ >= 50) break;
            i64 p = pp + T0;
            i64 cs = chunk_start(p);
            if (cs == NONE) continue;
            View V = insns(cs, min(fn_end(cs), p + 16));
            for (const Ins& x : V)
                if ((i64)x.a <= p && p < (i64)x.a + x.sz && x.dispOff == p - (i64)x.a && (i64)x.a + x.sz == p + 4 + RIPK[ki])
                    res.push_back({x.a, x.sz, x.dispOff});
        }
    }
    return res;
}

// ------------------------------------------------------------------------------------------------ the table
struct HookSite { string kind; i64 site, target; };
static vector<pair<string, i64>> tblAddr;  // file order
static unordered_map<string, size_t> tblAddrIdx;
static vector<pair<string, i64>> tblOff;
static map<string, vector<HookSite>> tblHook;
static vector<pair<string, i64>> tblAdj;
static vector<vector<string>> tblHookreg;
static unordered_map<i64, vector<string>> byaddr;
static i64 addrOf(const string& n) { return tblAddr[tblAddrIdx.at(n)].second; }
static bool hasAddr(const string& n) { return tblAddrIdx.count(n) > 0; }

static string BUILD;
static vector<string> out;
static unordered_map<string, set<string>> refinUsed;  // name -> anchors of its refin lines
static void emit(const string& line) {
    out.push_back(line);
    if (startsWith(line, "refin ")) {
        size_t a = line.find(' ', 6);
        size_t b = line.find(' ', a + 1);
        if (a != string::npos && b != string::npos) refinUsed[line.substr(6, a - 6)].insert(line.substr(a + 1, b - a - 1));
    }
}
static vector<string> miss;

// found: insertion-ordered
static vector<pair<string, i64>> foundList;
static unordered_map<string, i64> foundMap;
static unordered_map<i64, vector<string>> foundBy;
static unordered_set<i64> foundVals;
static bool isFound(const string& n) { return foundMap.count(n) > 0; }
static void setFound(const string& n, i64 a) {
    if (foundMap.count(n)) return;  // (every name is set once)
    foundMap[n] = a;
    foundList.push_back({n, a});
    foundBy[a].push_back(n);
    foundVals.insert(a);
}
static unordered_set<string> rtti;
static vector<string> rttiList;

static unordered_map<i64, string> anchors;  // chunk start -> name ("" = none)
static int depthG = 0;
static bool emit_fnvt(const string& name, i64 a);
static bool emit_ref(const string& name, i64 a, bool scoped);

struct Anch { bool ok = false; string name; i64 cs = NONE; };
static Anch anchor_of(i64 site) {
    i64 cs = chunk_start(site);
    if (cs == NONE) return Anch();
    if (!anchors.count(cs)) {
        string named;
        auto f = foundBy.find(cs);
        if (f != foundBy.end()) for (auto& n : f->second) if (!startsWith(n, "__ZT")) { named = n; break; }
        if (!named.empty()) anchors[cs] = named;
        else {
            string name = strf("@%llx", (unsigned long long)cs);
            auto u = unique_from(cs, 512);
            bool ok = false;
            if (u) { emit("fn " + E(name) + " " + u->text()); ok = true; }
            if (!ok) ok = emit_fnvt(name, cs);
            if (!ok && depthG == 0) {
                depthG++;
                ok = emit_ref(name, cs, false) || emit_ref(name, cs, true);
                depthG--;
            }
            anchors[cs] = ok ? name : "";
        }
    }
    const string& nm = anchors[cs];
    if (nm.empty()) return Anch();
    Anch r;
    r.ok = true;
    r.name = nm;
    r.cs = cs;
    return r;
}

struct Scoped { string name; i64 rng; Pat pt; };
static optional<Scoped> scoped_window(i64 site, i64 size, Rng ex = Rng(), bool back = false) {
    Anch an = anchor_of(site);
    if (!an.ok) return nullopt;
    i64 cs = an.cs, rng = fn_end(cs) - cs;
    vector<Win> W = windows(site, size, 64, 1 << 20, back);
    for (int loose = 1; loose >= 0; loose--)
        for (auto& w : W)
            if (w.hi - w.lo >= 8) {
                Pat pt = mkPat(w.lo, w.hi, ex);
                if (pat_count_in(pt, cs, cs + rng, 2, loose) == 1) return Scoped{an.name, rng, std::move(pt)};
            }
    return nullopt;
}
static optional<Scoped> scoped_after(i64 site, i64 size, Rng ex = Rng(), int maxins = 40) {
    Anch an = anchor_of(site);
    if (!an.ok) return nullopt;
    i64 cs = an.cs, rng = fn_end(cs) - cs;
    View L = insns(chunk_start(site), min(fn_end(site), site + 24 * maxins));
    auto it = partition_point(L.begin(), L.end(), [&](const Ins& x) { return (i64)x.a < site; });
    if (it == L.end() || (i64)it->a != site) return nullopt;
    i64 i0 = it - L.begin();
    for (i64 hi = i0 + 1; hi <= min<i64>(L.size(), i0 + maxins); hi++) {
        i64 stop = (i64)L[hi - 1].a + L[hi - 1].sz;
        if (stop - site < 8) continue;
        Pat pt = mkPat(site, stop, ex);
        if (pat_count_in(pt, cs, cs + rng, 2, 2) == 1) return Scoped{an.name, rng, std::move(pt)};
    }
    return nullopt;
}

// ------------------------------------------------------------------------------------------------ vtables, RTTI
static vector<i64> vt_slots(i64 vt) {
    vector<i64> res;
    for (int k = 0; k < 1024; k++) {
        Span w = imgSpan(vt + 8 * k, 8);
        if (w.n < 8) break;
        i64 f = (i64)(ld64(w.p) - (u64)BASE);
        if (!in_text(f)) break;
        res.push_back(f);
    }
    return res;
}
static string td_name(i64 td) {
    string d = img(td + 16, 256);
    size_t z = d.find('\0');
    if (z != string::npos) d.resize(z);
    return latin1ToUtf8(d);
}
static bool rd32(i64 rva, u32* v, int off = 0) {
    Span s = imgSpan(rva + off, 4);
    if (s.n < 4) return false;
    *v = ld32(s.p);
    return true;
}
static vector<pair<i64, string>> all_vtables() {
    vector<pair<i64, string>> res;
    for (auto& r : rdata) {
        i64 lo = r.first, hi = r.second;
        const Sec* sec = nullptr;
        for (auto& s : secs) if (s.va == lo) { sec = &s; break; }
        Span raw = pyslice(Fp, Fn, sec->ptr, sec->ptr + min(sec->raw, hi - lo));
        i64 nu = raw.n / 4, nq = raw.n / 8;
        unordered_set<u64> colset;
        for (i64 i = 0; i + 6 < nu; i++)
            if (ld32(raw.p + i * 4) == 1 && ld32(raw.p + (i + 5) * 4) == (u32)(lo + 4 * i)) colset.insert((u64)(lo + 4 * i) + (u64)BASE);
        for (i64 i = 0; i < nq; i++) {
            u64 q = ld64(raw.p + i * 8);
            if (!colset.count(q)) continue;
            i64 vt = lo + i * 8 + 8;
            i64 col = (i64)(q - (u64)BASE);
            u32 off, td;
            if (!rd32(col, &off, 4) || !rd32(col, &td, 12)) { fprintf(stderr, "bad locator at %llx\n", (unsigned long long)col); exit(3); }
            res.push_back({vt, strf("rtti:%s:%u", td_name(td).c_str(), off)});
        }
    }
    return res;
}
static bool inslotInit = false;
static unordered_map<i64, i64> inslot;
static vector<pair<i64, string>> vtnameList;
static unordered_map<i64, size_t> vtnameIdx;
static unordered_map<string, int> vtnameCount;
static void vtnameSet(i64 vt, const string& n, bool onlyIfNew) {
    auto it = vtnameIdx.find(vt);
    if (it == vtnameIdx.end()) { vtnameIdx[vt] = vtnameList.size(); vtnameList.push_back({vt, n}); }
    else if (!onlyIfNew) vtnameList[it->second].second = n;
}
static bool emit_fnvt(const string& name, i64 a) {
    if (!inslotInit) {
        inslotInit = true;
        vector<string> rs = rttiList;  // sorted, as the Python's sorted(rtti)
        sort(rs.begin(), rs.end());
        for (auto& vn : rs) if (endsWith(vn, "+0x10")) vtnameSet(addrOf(vn), vn, false);
        for (auto& p : all_vtables()) vtnameSet(p.first, p.second, true);
        for (auto& p : vtnameList) {
            vtnameCount[p.second]++;
            for (i64 f : vt_slots(p.first)) inslot.emplace(f, p.first);
        }
    }
    auto it = inslot.find(a);
    if (it == inslot.end()) return false;
    i64 vt = it->second;
    const string& vn = vtnameList[vtnameIdx[vt]].second;
    if (startsWith(vn, "rtti:") && vtnameCount[vn] > 1) return false;
    vector<i64> slots = vt_slots(vt);
    Pat pts[4];
    const i64 Ls[4] = {16, 32, 64, 128};
    for (int i = 0; i < 4; i++) pts[i] = mkPat(a, whole_end(a, Ls[i]));
    for (int loose = 1; loose >= 0; loose--)
        for (int i = 0; i < 4; i++) {
            bool any = false;
            for (i64 g : slots) if (g != a && pat_at(pts[i], g - T0, loose)) { any = true; break; }
            if (!any) { emit("fnvt " + E(name) + " " + E(vn) + " " + pts[i].text()); return true; }
        }
    Pat pt = mkPat(a, whole_end(a, 64));
    for (int loose = 0; loose <= 1; loose++) {
        vector<i64> same;
        for (i64 g : slots) if (pat_at(pt, g - T0, loose) && find(same.begin(), same.end(), g) == same.end()) same.push_back(g);
        auto f = find(same.begin(), same.end(), a);
        if (f != same.end() && same.size() <= 4) {
            emit("fnvt " + E(name) + " " + E(vn) + " " + pt.text() + strf(" %d %d", (int)(f - same.begin()), (int)same.size()));
            return true;
        }
    }
    return false;
}

// ------------------------------------------------------------------------------------------------ refs
static bool emit_ref(const string& name, i64 a, bool scoped) {
    vector<Cand> cands;
    if (in_text(a)) for (i64 s : callers(a)) cands.push_back({s, 5, 1});
    for (auto& c : riprefs(a)) cands.push_back(c);
    if (!scoped) {
        for (size_t i = 0; i < cands.size() && i < 40; i++) {
            auto pt = unique_around(cands[i].site, cands[i].sz);
            if (pt) {
                emit("ref " + E(name) + " " + pt->text() + strf(" %d %d", (int)(cands[i].site + cands[i].doff - pt->start), (int)(cands[i].site + cands[i].sz - pt->start)));
                return true;
            }
        }
        return false;
    }
    vector<pair<char, size_t>> key(cands.size());
    for (size_t i = 0; i < cands.size(); i++) key[i] = {!foundVals.count(chunk_start(cands[i].site)), i};
    vector<size_t> ord(cands.size());
    for (size_t i = 0; i < ord.size(); i++) ord[i] = i;
    stable_sort(ord.begin(), ord.end(), [&](size_t x, size_t y) {
        if (key[x].first != key[y].first) return key[x].first < key[y].first;
        return cands[x].site < cands[y].site;
    });
    for (size_t k = 0; k < ord.size() && k < 500; k++) {
        const Cand& c = cands[ord[k]];
        auto u = scoped_window(c.site, c.sz);
        if (u) {
            emit("refin " + E(name) + " " + E(u->name) + " " + hexs(u->rng) + " " + u->pt.text() +
                 strf(" %d %d", (int)(c.site + c.doff - u->pt.start), (int)(c.site + c.sz - u->pt.start)));
            return true;
        }
    }
    return false;
}

// ------------------------------------------------------------------------------------------------ chunks of the Windows analysis
static unordered_map<i64, vector<pair<i64, i64>>> chunksByRoot;  // root va -> (rva, rva+size) in file order
static vector<pair<i64, i64>> chunks(i64 fn_rva) {
    auto it = chunksByRoot.find(fn_rva + BASE);
    if (it != chunksByRoot.end() && !it->second.empty()) return it->second;
    return {{fn_rva, fn_end(fn_rva)}};
}

// ------------------------------------------------------------------------------------------------ offsets
static unordered_map<string, regex> rxCache;
static string insText(const Ins& x) {
    // the instruction's "mnemonic operands" text (decoded again: not kept per instruction)
    CsThread& c = cst();
    const uint8_t* code = TB + (x.a - T0);
    size_t size = TBn - (x.a - T0);
    u64 addr = x.a;
    if (!cs_disasm_iter(c.h, &code, &size, &addr, c.in)) return "";
    return string(c.in->mnemonic) + " " + c.in->op_str;
}
static bool likeMatch(const string& like, const Ins& x) {
    auto it = rxCache.find(like);
    if (it == rxCache.end()) it = rxCache.emplace(like, regex(like)).first;
    return regex_search(insText(x), it->second);
}

struct WideRes { string k; Pat pt; string pre; };
static optional<WideRes> wide_around(i64 site, i64 size, Rng mask, i64 span = 48) {
    i64 cs = chunk_start(site);
    if (cs == NONE) return nullopt;
    View L = insns(cs, min(fn_end(cs), site + size + span));
    i64 lo = site;
    for (const Ins& x : L) if ((i64)x.a >= site - span) { lo = x.a; break; }
    i64 hi = L.size() ? (i64)L[L.size() - 1].a + L[L.size() - 1].sz : site + size;
    Pat pt = mkPat(lo, hi, mask);
    if (pat_count(pt, 2, 1) == 1) return WideRes{"", std::move(pt), ""};
    Anch an = anchor_of(site);
    if (an.ok && pat_count_in(pt, an.cs, fn_end(an.cs), 2, 1) == 1)
        return WideRes{"in", std::move(pt), E(an.name) + " " + hexs(fn_end(an.cs) - an.cs) + " "};
    return nullopt;
}

typedef vector<pair<string, string>> KR;
static KR disp_anchors(i64 fn_rva, i64 value, int limit = 3, const string* like = nullptr) {
    KR res;
    auto chs = chunks(fn_rva);
    bool stop = false;
    for (auto& ch : chs) {
        View L = insns(ch.first, ch.second);
        for (size_t xi = 0; xi < L.size() && !stop; xi++) {
            Ins x = L[xi];
            i64 a = x.a;
            bool likeOk = true, likeDone = false;
            for (int o = 0; o < 2; o++) {
                int off = o ? x.immOff : x.dispOff, size = o ? x.immSize : x.dispSize;
                if (!size || (size != 1 && size != 4) || (x.m >> off & 1)) continue;
                i64 raw = 0;
                if (size == 1) raw = (int8_t)TB[a - T0 + off];
                else { int32_t v; memcpy(&v, TB + a - T0 + off, 4); raw = v; }
                if (raw != value) continue;
                if (like && !likeDone) { likeOk = likeMatch(*like, x); likeDone = true; }
                if (like && !likeOk) break;
                Rng mask{a + off, a + off + size};
                auto wide = wide_around(a, x.sz, mask);
                if (wide) res.push_back({wide->k, wide->pre + wide->pt.text() + strf(" %d %d", (int)(a + off - wide->pt.start), size)});
                auto pt = unique_around(a, x.sz, mask);
                if (pt) res.push_back({"", pt->text() + strf(" %d %d", (int)(a + off - pt->start), size)});
                else {
                    auto u = scoped_window(a, x.sz, mask);
                    if (u) res.push_back({"in", E(u->name) + " " + hexs(u->rng) + " " + u->pt.text() + strf(" %d %d", (int)(a + off - u->pt.start), size)});
                    else {
                        for (auto& w : windows(a, x.sz, 32, 32)) {
                            if (w.hi - w.lo < 8) continue;
                            Pat p2 = mkPat(w.lo, w.hi, mask);
                            int n = pat_count(p2, 9, 0);
                            if (1 <= n && n <= 8) { res.push_back({"any", p2.text() + strf(" %d %d", (int)(a + off - p2.start), size)}); break; }
                        }
                    }
                }
                if ((int)res.size() >= limit) stop = true;
                break;
            }
            if (stop) break;
        }
        if (stop) break;
    }
    for (auto& ch : chs) {
        View L = insns(ch.first, ch.second);
        for (size_t xi = 0; xi < L.size(); xi++) {
            Ins x = L[xi];
            i64 a = x.a;
            bool likeOk = true, likeDone = false;
            for (int o = 0; o < 2; o++) {
                int off = o ? x.immOff : x.dispOff, size = o ? x.immSize : x.dispSize;
                if (!size || (size != 1 && size != 4) || (x.m >> off & 1)) continue;
                i64 raw = 0;
                if (size == 1) raw = (int8_t)TB[a - T0 + off];
                else { int32_t v; memcpy(&v, TB + a - T0 + off, 4); raw = v; }
                if (raw != value) continue;
                if (like && !likeDone) { likeOk = likeMatch(*like, x); likeDone = true; }
                if (like && !likeOk) break;
                auto u = scoped_after(a, x.sz, Rng{a + off, a + off + size});
                if (u) {
                    pair<string, string> line{"in", E(u->name) + " " + hexs(u->rng) + " " + u->pt.text() + strf(" %d %d", (int)(a + off - u->pt.start), size)};
                    if (find(res.begin(), res.end(), line) == res.end() && (int)res.size() < limit + 3) res.push_back(line);
                }
                break;
            }
        }
    }
    return res;
}

static vector<i64> seq_values(i64 lo, i64 hi, const vector<i64>& deltas, i64 gap) {
    set<i64> vals;
    for (i64 q = lo - T0; q < hi - T0 - 3; q++) {
        int32_t qv;
        memcpy(&qv, TB + q, 4);
        i64 v = (i64)qv - deltas[0];
        i64 p = q;
        for (size_t di = 1; di < deltas.size(); di++) {
            i64 want = v + deltas[di];
            if (!(-0x80000000LL <= want && want < 0x80000000LL)) { p = -1; break; }
            int32_t w = (int32_t)want;
            i64 s = p + 4, e = min(p + 7 + gap, hi - T0);
            i64 found = -1;
            if (s < 0) s = 0;
            for (i64 k = s; k + 4 <= e && k + 4 <= TBn; k++) if (!memcmp(TB + k, &w, 4)) { found = k; break; }
            p = found;
            if (p < 0) break;
        }
        if (p >= 0) vals.insert(v);
        if (vals.size() > 1) break;
    }
    return vector<i64>(vals.begin(), vals.end());
}

static optional<string> emit_offseq(const string& key, i64 value, const string& fn, i64 a, const string* like) {
    for (auto& ch : chunks(a)) {
        i64 cs = ch.first;
        View L = insns(cs, ch.second);
        struct M { i64 i, da, d; int base; };
        vector<M> mem;
        for (size_t i = 0; i < L.size(); i++) {
            const Ins& x = L[i];
            if (x.dispSize != 4) continue;
            if (x.memBase) mem.push_back({(i64)i, (i64)x.a + x.dispOff, x.disp, x.memBase});
        }
        for (auto& cur : mem) {
            if (cur.d != value) continue;
            if (like && !likeMatch(*like, L[cur.i])) continue;
            vector<M> near;
            for (auto& x : mem) if (x.base == cur.base && llabs(x.i - cur.i) <= 16) near.push_back(x);
            stable_sort(near.begin(), near.end(), [&](const M& p, const M& q) { return llabs(p.i - cur.i) < llabs(q.i - cur.i); });
            if (near.size() > 8) near.resize(8);
            stable_sort(near.begin(), near.end(), [](const M& p, const M& q) { return p.da < q.da; });
            set<i64> dd;
            for (auto& x : near) dd.insert(x.d);
            if (near.size() < 4 || dd.size() < 3) continue;
            vector<i64> deltas;
            for (auto& x : near) deltas.push_back(x.d - value);
            i64 gap = 0;
            for (size_t j = 0; j + 1 < near.size(); j++) gap = max(gap, near[j + 1].da - near[j].da);
            gap += 32;
            i64 rng = fn_end(cs) - cs;
            auto sv = seq_values(cs, cs + rng, deltas, gap);
            if (!(sv.size() == 1 && sv[0] == value)) continue;
            string ds;
            for (size_t j = 0; j < deltas.size(); j++) ds += (j ? "," : "") + hexs(deltas[j]);
            return "offseq " + E(key) + " " + hexs(value) + " " + E(fn) + " " + hexs(rng) + strf(" %d ", (int)gap) + ds;
        }
    }
    return nullopt;
}
static int field_base(i64 a, i64 value, const string* like) {
    for (auto& ch : chunks(a)) {
        View L = insns(ch.first, ch.second);
        for (const Ins& x : L) {
            if (x.dispSize != 4 || x.disp != value) continue;
            if (like && !likeMatch(*like, x)) continue;
            if (x.memBase) return x.memBase;
        }
    }
    return 0;
}

// ------------------------------------------------------------------------------------------------ a tiny JSON reader
struct JV {
    enum T { Null, Bool, Num, Str, Arr, Obj } t = Null;
    string s;  // string value, or a number's text
    bool b = false;
    vector<JV> a;
    vector<pair<string, JV>> o;
    const JV* get(const char* k) const { for (auto& p : o) if (p.first == k) return &p.second; return nullptr; }
};
struct JP {
    const string& s;
    size_t i = 0;
    bool bad = false;
    explicit JP(const string& str) : s(str) {}
    void ws() { while (i < s.size() && isspace((unsigned char)s[i])) i++; }
    static void utf8(string& r, u32 c) {
        if (c < 0x80) r += (char)c;
        else if (c < 0x800) { r += (char)(0xC0 | c >> 6); r += (char)(0x80 | (c & 63)); }
        else if (c < 0x10000) { r += (char)(0xE0 | c >> 12); r += (char)(0x80 | ((c >> 6) & 63)); r += (char)(0x80 | (c & 63)); }
        else { r += (char)(0xF0 | c >> 18); r += (char)(0x80 | ((c >> 12) & 63)); r += (char)(0x80 | ((c >> 6) & 63)); r += (char)(0x80 | (c & 63)); }
    }
    string str() {
        string r;
        i++;
        while (i < s.size() && s[i] != '"') {
            if (s[i] == '\\' && i + 1 < s.size()) {
                char c = s[++i];
                switch (c) {
                    case 'n': r += '\n'; break;
                    case 't': r += '\t'; break;
                    case 'r': r += '\r'; break;
                    case 'b': r += '\b'; break;
                    case 'f': r += '\f'; break;
                    case 'u': {
                        u32 cp = strtoul(s.substr(i + 1, 4).c_str(), nullptr, 16);
                        i += 4;
                        if (cp >= 0xD800 && cp < 0xDC00 && s.compare(i + 1, 2, "\\u") == 0) {
                            u32 lo = strtoul(s.substr(i + 3, 4).c_str(), nullptr, 16);
                            cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                            i += 6;
                        }
                        utf8(r, cp);
                        break;
                    }
                    default: r += c;
                }
                i++;
            } else r += s[i++];
        }
        i++;
        return r;
    }
    JV val() {
        JV v;
        ws();
        if (i >= s.size()) { bad = true; return v; }
        char c = s[i];
        if (c == '{') {
            v.t = JV::Obj;
            i++;
            ws();
            if (s[i] == '}') { i++; return v; }
            for (;;) {
                ws();
                string k = str();
                ws();
                if (s[i] != ':') { bad = true; return v; }
                i++;
                v.o.push_back({k, val()});
                ws();
                if (s[i] == ',') { i++; continue; }
                if (s[i] == '}') { i++; break; }
                bad = true;
                return v;
            }
        } else if (c == '[') {
            v.t = JV::Arr;
            i++;
            ws();
            if (s[i] == ']') { i++; return v; }
            for (;;) {
                v.a.push_back(val());
                ws();
                if (s[i] == ',') { i++; continue; }
                if (s[i] == ']') { i++; break; }
                bad = true;
                return v;
            }
        } else if (c == '"') { v.t = JV::Str; v.s = str(); }
        else if (!s.compare(i, 4, "true")) { v.t = JV::Bool; v.b = true; i += 4; }
        else if (!s.compare(i, 5, "false")) { v.t = JV::Bool; i += 5; }
        else if (!s.compare(i, 4, "null")) { i += 4; }
        else {
            size_t j = i;
            while (j < s.size() && (isdigit((unsigned char)s[j]) || strchr("+-.eE", s[j]))) j++;
            if (j == i) { bad = true; return v; }
            v.t = JV::Num;
            v.s = s.substr(i, j - i);
            i = j;
        }
        return v;
    }
};
// python's int(text, 0)
static bool pyInt0(string t, i64* out_) {
    size_t b = 0, e = t.size();
    while (b < e && isspace((unsigned char)t[b])) b++;
    while (e > b && isspace((unsigned char)t[e - 1])) e--;
    t = t.substr(b, e - b);
    bool neg = false;
    size_t i = 0;
    if (i < t.size() && (t[i] == '+' || t[i] == '-')) { neg = t[i] == '-'; i++; }
    int base = 10;
    if (i + 1 < t.size() && t[i] == '0') {
        char c = tolower(t[i + 1]);
        if (c == 'x') { base = 16; i += 2; }
        else if (c == 'o') { base = 8; i += 2; }
        else if (c == 'b') { base = 2; i += 2; }
    }
    if (i >= t.size()) return false;
    string digits;
    for (; i < t.size(); i++) if (t[i] != '_') digits += t[i];
    char* endp;
    errno = 0;
    long long v = strtoll(digits.c_str(), &endp, base);
    if (*endp || digits.empty()) return false;
    *out_ = neg ? -v : v;
    return true;
}

static vector<string> listJson(const string& dir) {  // *.out.json
    vector<string> r;
    DIR* d = opendir(dir.c_str());
    if (!d) return r;
    while (dirent* e = readdir(d)) {
        string n = e->d_name;
        if (n.size() > 9 && n.compare(n.size() - 9, 9, ".out.json") == 0 && n[0] != '.') r.push_back(dir + "/" + n);
    }
    closedir(d);
    sort(r.begin(), r.end());
    return r;
}

// ------------------------------------------------------------------------------------------------ main
static string expandHome(const string& p) {
    if (!p.empty() && p[0] == '~') { const char* h = getenv("HOME"); return string(h ? h : "") + p.substr(1); }
    return p;
}
static bool isDir(const string& p) { struct stat st; return stat(p.c_str(), &st) == 0 && S_ISDIR(st.st_mode); }

static bool emit_fallbacks_one(const string& name, i64 a, int k);

int main(int argc, char** argv) {
    if (argc < 3) { fprintf(stderr, "usage: makesigs <rekordbox.exe> <table.tbl> [workdir]\n"); return 1; }
    string exe = argv[1], tblfile = argv[2];
    string work = expandHome(argc > 3 ? argv[3] : "~/rbwin");
    NTHREADS = max(1u, thread::hardware_concurrency());
    if (getenv("MAKESIGS_THREADS")) NTHREADS = max(1, atoi(getenv("MAKESIGS_THREADS")));
    string repo;
    {
        vector<string> tries;
        if (getenv("MAKESIGS_REPO")) tries.push_back(getenv("MAKESIGS_REPO"));
        char self[4096];
        if (realpath(argv[0], self)) { string s = self; tries.push_back(s.substr(0, s.rfind('/')) + "/.."); }
        tries.push_back(".");
        for (auto& t : tries) if (isDir(t + "/tools/win")) { repo = t; break; }
        if (repo.empty()) { fprintf(stderr, "repo root not found (set MAKESIGS_REPO)\n"); return 1; }
    }
    const char* edEnv = getenv("RBM_EDITION");
    bool fullEdition = isDir(repo + "/private") && (!edEnv || !*edEnv || string(edEnv) == "full");
    loadPE(exe);
    {
        char b[64];
        snprintf(b, sizeof b, "%08X-%X", STAMP, (u32)IMGSIZE);
        BUILD = b;
    }
    cst();

    // ---- the table
    {
        bool ok;
        string t = readFile(tblfile, &ok);
        if (!ok) { fprintf(stderr, "cannot read %s\n", tblfile.c_str()); return 1; }
        for (auto& line : lines(t)) {
            auto w = splitWs(line);
            if (w.empty() || w[0][0] == '#') continue;
            auto joinMid = [&] { string s; for (size_t i = 1; i + 1 < w.size(); i++) s += (i > 1 ? " " : "") + w[i]; return s; };
            if (w[0] == "addr") {
                string n = joinMid();
                i64 v = strtoll(w.back().c_str(), nullptr, 16);
                auto it = tblAddrIdx.find(n);
                if (it == tblAddrIdx.end()) { tblAddrIdx[n] = tblAddr.size(); tblAddr.push_back({n, v}); }
                else tblAddr[it->second].second = v;
            } else if (w[0] == "off") {
                string key = joinMid();
                i64 v = strtoll(w.back().c_str(), nullptr, 16);
                if (find(tblOff.begin(), tblOff.end(), make_pair(key, v)) == tblOff.end()) tblOff.push_back({key, v});
            } else if (w[0] == "hook") {
                tblHook[w[1]].push_back({w[2], strtoll(w[3].c_str(), nullptr, 16), strtoll(w[4].c_str(), nullptr, 16)});
            } else if (w[0] == "adj") tblAdj.push_back({w[1], strtoll(w[2].c_str(), nullptr, 10)});
            else if (w[0] == "hookreg") tblHookreg.push_back(vector<string>(w.begin() + 1, w.end()));
        }
    }
    for (auto& p : tblAddr) byaddr[p.second].push_back(p.first);
    {
        set<i64> pset(pbeg.begin(), pbeg.end());
        set<i64> lf;
        for (auto& kv : byaddr) if (in_text(kv.first) && !pset.count(kv.first)) lf.insert(kv.first);
        leafs.assign(lf.begin(), lf.end());
    }
    out.push_back("# platendoos signatures from rekordbox.exe " + BUILD + " (tools/win/makesigs.py)");
    out.push_back("build " + BUILD);

    // ---- the chunks of the Windows analysis (win_chunks.txt: "va size root", hex)
    {
        bool ok;
        string t = readFile(work + "/win_chunks.txt", &ok);
        if (!ok) { fprintf(stderr, "cannot read %s/win_chunks.txt\n", work.c_str()); return 1; }
        for (auto& line : lines(t)) {
            if (line.empty() || line[0] == '#') continue;
            auto w = splitWs(line);
            if (w.size() < 3) continue;
            i64 va = strtoll(w[0].c_str(), nullptr, 16), size = strtoll(w[1].c_str(), nullptr, 16), root = strtoll(w[2].c_str(), nullptr, 16);
            chunksByRoot[root].push_back({va - BASE, va - BASE + size});
        }
    }

    // ---- RTTI
    for (auto& kv : tblAddr) {
        const string& n = kv.first;
        i64 a = kv.second;
        if (startsWith(n, "__ZTV")) {
            i64 vt = a + (endsWith(n, "+0x10") ? 0 : 0x10);
            bool good = false;
            u32 off = 0, td = 0;
            string nm;
            Span cp = imgSpan(vt - 8, 8);
            if (cp.n == 8) {
                i64 colva = (i64)ld64(cp.p);
                Span c16 = imgSpan(colva - BASE, 16);
                if (c16.n == 16) {
                    off = ld32(c16.p + 4);
                    td = ld32(c16.p + 12);
                    nm = td_name(td);
                    good = nm.compare(0, 3, ".?A") == 0;
                }
            }
            if (!good) { miss.push_back("vt " + n); continue; }
            emit(strf("vt %s %lld %s %u", E(n).c_str(), (long long)(a - vt), nm.c_str(), off));
            rtti.insert(n);
            rttiList.push_back(n);
        } else if (startsWith(n, "__ZTI")) {
            string nm = td_name(a);
            if (nm.compare(0, 3, ".?A") != 0) { miss.push_back("ti " + n); continue; }
            emit("ti " + E(n) + " " + nm);
            rtti.insert(n);
            rttiList.push_back(n);
        }
    }

    fprintf(stderr, "[%s] indexes\n", "t");
    buildGramIndex();
    buildCallIndexes();
    fprintf(stderr, "[t] indexes done\n");
    if (getenv("MS_DBG")) {
        i64 a = strtoll(getenv("MS_DBG"), nullptr, 16);
        View V = insns(a, min(fn_end(a), a + 512));
        fprintf(stderr, "fn_end %llx, %zu insns\n", (unsigned long long)fn_end(a), V.size());
        for (const Ins& x : V) {
            Pat pt = mkPat(a, (i64)x.a + x.sz);
            fprintf(stderr, "%llx cnt1=%d cnt0=%d %s\n", (unsigned long long)x.a, pat_count(pt, 2, 1), pat_count(pt, 2, 0), pt.text().c_str());
        }
    }

    // ---- the addresses: unique by their own bytes, else through a call or reference
    vector<pair<string, i64>> sortedAddr = tblAddr;
    sort(sortedAddr.begin(), sortedAddr.end());
    vector<pair<string, i64>> todo;
    for (auto& p : sortedAddr) if (!rtti.count(p.first)) todo.push_back(p);
    vector<optional<Pat>> uniq(todo.size());
    parallel_for(todo.size(), [&](size_t i) { if (in_text(todo[i].second)) uniq[i] = unique_from(todo[i].second, 512); }, 8);
    vector<pair<string, i64>> left;
    for (size_t i = 0; i < todo.size(); i++) {
        if (uniq[i]) { emit("fn " + E(todo[i].first) + " " + uniq[i]->text()); setFound(todo[i].first, todo[i].second); }
        else left.push_back(todo[i]);
    }
    fprintf(stderr, "[t] unique_from done, %zu left\n", left.size());
    for (int pass = 0; pass < 3; pass++) {
        bool scoped = pass > 0;
        vector<pair<string, i64>> still;
        if (!scoped) {
            // not scoped: independent of everything found so far: worked out in parallel, written in order
            struct R { bool ok = false; string line; };
            vector<R> rs(left.size());
            parallel_for(left.size(), [&](size_t i) {
                i64 a = left[i].second;
                vector<Cand> cands;
                if (in_text(a)) for (i64 s : callers(a)) cands.push_back({s, 5, 1});
                for (auto& c : riprefs(a)) cands.push_back(c);
                for (size_t k = 0; k < cands.size() && k < 40; k++) {
                    auto pt = unique_around(cands[k].site, cands[k].sz);
                    if (pt) {
                        rs[i].ok = true;
                        rs[i].line = "ref " + E(left[i].first) + " " + pt->text() + strf(" %d %d", (int)(cands[k].site + cands[k].doff - pt->start), (int)(cands[k].site + cands[k].sz - pt->start));
                        return;
                    }
                }
            });
            for (size_t i = 0; i < left.size(); i++) {
                if (rs[i].ok) { emit(rs[i].line); setFound(left[i].first, left[i].second); }
                else still.push_back(left[i]);
            }
        } else {
            for (auto& p : left) {
                if (emit_ref(p.first, p.second, true)) setFound(p.first, p.second);
                else still.push_back(p);
            }
        }
        left = still;
        for (auto it = anchors.begin(); it != anchors.end();) { if (it->second.empty()) it = anchors.erase(it); else ++it; }
        fprintf(stderr, "[t] ref pass %d done, %zu left\n", pass, left.size());
    }
    {
        vector<pair<string, i64>> keep;
        for (auto& p : left) {
            if (emit_fnvt(p.first, p.second)) setFound(p.first, p.second);  // (python: found.setdefault)
            else keep.push_back(p);
        }
        left = keep;
        for (auto& p : left) miss.push_back("addr " + p.first);
    }
    fprintf(stderr, "[t] addresses done\n");

    // ---- hooks
    for (auto& hk : tblHook) {
        const string& name = hk.first;
        auto& sites = hk.second;
        emit(strf("hooksites %s %d", name.c_str(), (int)sites.size()));
        string tname = "-";
        auto bn = byaddr.find(sites[0].target);
        if (bn != byaddr.end()) for (auto& t : bn->second) if (isFound(t)) { tname = t; break; }
        // a lea site takes a function's address (a callback): the function is named too (an @<rva> anchor when the
        // table has no name for it), so a new build's site must take the address of the same function
        if (tname == "-" && sites[0].kind == "lea") {
            Anch an = anchor_of(sites[0].target);
            if (an.ok && an.cs == sites[0].target) tname = an.name;
        }
        for (auto& st : sites) {
            const string& kind = st.kind;
            i64 site = st.site, target = st.target;
            i64 size = kind == "icall" ? 6 : 5;
            if (kind == "call" || kind == "jmp") {
                Anch ca = anchor_of(site);
                string ta;
                bool taOk = false;
                if (tname != "-") { ta = tname; taOk = true; }
                else {
                    Anch t1 = anchor_of(target);
                    if (t1.ok) { ta = t1.name; taOk = !ta.empty(); }
                    else taOk = false;  // (None)
                }
                bool cond1 = false;
                if (ca.ok && taOk) {
                    Anch t2 = anchor_of(target);
                    if (t2.ok) {
                        Anch t3 = anchor_of(target);
                        cond1 = t3.cs == target;
                    }
                }
                if (cond1) {
                    i64 cs = ca.cs;
                    uint8_t op = kind == "jmp" ? 0xe9 : 0xe8;
                    vector<i64> at;
                    for (i64 q : callers(target)) if (cs <= q && q < fn_end(cs) && TB[q - T0] == op) at.push_back(q);
                    auto f = find(at.begin(), at.end(), site);
                    if (f != at.end())
                        emit(strf("hookcall %s %s %s %llx %s %d %d", name.c_str(), kind.c_str(), E(ca.name).c_str(), (unsigned long long)(fn_end(cs) - cs), E(ta).c_str(), (int)(f - at.begin()), (int)at.size()));
                }
                bool cond2 = false;
                if (taOk) {
                    Anch t2 = anchor_of(target);
                    if (t2.ok) {
                        Anch t3 = anchor_of(target);
                        cond2 = t3.cs == target;
                    }
                }
                if (cond2) {
                    uint8_t op = kind == "jmp" ? 0xe9 : 0xe8;
                    vector<i64> at;
                    for (i64 q : callers(target)) if (TB[q - T0] == op) at.push_back(q);
                    sort(at.begin(), at.end());
                    auto f = find(at.begin(), at.end(), site);
                    if (f != at.end() && at.size() <= 4)
                        emit(strf("hookcall %s %s * 0 %s %d %d", name.c_str(), kind.c_str(), E(ta).c_str(), (int)(f - at.begin()), (int)at.size()));
                }
            }
            auto pt = unique_around(site, size);
            if (pt) {
                emit(strf("hook %s %s ", name.c_str(), kind.c_str()) + pt->text() + strf(" %d ", (int)(site - pt->start)) + E(tname));
                continue;
            }
            auto u = scoped_window(site, size);
            if (!u) { miss.push_back(strf("hook %s %llx", name.c_str(), (unsigned long long)site)); continue; }
            emit(strf("hookin %s %s ", name.c_str(), kind.c_str()) + E(u->name) + " " + hexs(u->rng) + " " + u->pt.text() + strf(" %d ", (int)(site - u->pt.start)) + E(tname));
        }
    }
    fprintf(stderr, "[t] hooks done\n");

    // ---- offsets
    map<pair<string, i64>, set<i64>> vnotes, vfns;  // vfns: only the function(s) "win_fn" names (for pinned fields)
    {
        vector<string> files = listJson(work + "/verify");
        for (auto& f : listJson(repo + "/tools/win/verify")) files.push_back(f);
        if (fullEdition) for (auto& f : listJson(repo + "/private/tools/win/verify")) files.push_back(f);
        for (auto& f : files) {
            string t = readFile(f);
            JP jp(t);
            JV root = jp.val();
            if (jp.bad || root.t != JV::Arr) { fprintf(stderr, "cannot read %s\n", f.c_str()); return 1; }
            for (auto& r : root.a) {
                if (r.t != JV::Obj) continue;
                const JV* kind = r.get("kind");
                const JV* val = r.get("value");
                if (!kind || kind->t != JV::Str || kind->s != "offset" || !val || val->t == JV::Null) continue;
                const JV* fn = r.get("fn");
                string vs = val->t == JV::Str ? val->s : val->t == JV::Num ? val->s : val->b ? "True" : "False";
                i64 v;
                if (!pyInt0(vs, &v)) { fprintf(stderr, "bad value %s in %s\n", vs.c_str(), f.c_str()); return 1; }
                string text;
                const JV* wf = r.get("win_fn");
                const JV* why = r.get("why");
                text = ((wf && wf->t == JV::Str) ? wf->s : "") + " " + ((why && why->t == JV::Str) ? why->s : "");
                if (!fn || fn->t != JV::Str) continue;  // (the key (None, v) is never looked up)
                // r'0x14[0-9a-f]{7}'
                auto scan = [&](const string& text, map<pair<string, i64>, set<i64>>& to) {
                    for (size_t i = 0; i + 11 <= text.size(); i++) {
                        if (text.compare(i, 4, "0x14") != 0) continue;
                        bool ok = true;
                        for (int k = 4; k < 11; k++) { char c = text[i + k]; if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) { ok = false; break; } }
                        if (ok) {
                            to[{fn->s, v}].insert(strtoll(text.substr(i + 2, 9).c_str(), nullptr, 16) - BASE);
                            i += 10;
                        }
                    }
                };
                scan(text, vnotes);
                if (wf && wf->t == JV::Str) scan(wf->s, vfns);
            }
        }
    }
    auto anchors_in = [&](const set<i64>& rvas, i64 value, const string* like = nullptr) {
        KR res;
        for (i64 a : rvas) {
            i64 cs = byaddr.count(a) ? a : chunk_start(a);
            if (cs == NONE) continue;
            for (auto& x : disp_anchors(cs, value, 3 - (int)res.size(), like)) res.push_back(x);
            if (res.size() >= 3) break;
        }
        return res;
    };
    unordered_map<string, string> likes;
    {
        bool ok;
        string t = readFile(repo + "/tools/win/sigfields.txt", &ok);
        if (!ok) t.clear();
        if (fullEdition) {
            bool ok2;
            string t2 = readFile(repo + "/private/tools/win/sigfields.txt", &ok2);
            if (ok2) { if (!t.empty() && t.back() != '\n') t += '\n'; t += t2; ok = true; }
        }
        if (ok) for (auto& line : lines(t)) {
            if (line.empty() || line[0] == '#') continue;
            if (splitWs(line).empty()) continue;
            size_t sp = line.find(' ');
            string k = line.substr(0, sp), rx = sp == string::npos ? "" : line.substr(sp + 1);
            for (auto& c : k) if (c == '~') c = ' ';
            size_t b = 0, e = rx.size();
            while (b < e && isspace((unsigned char)rx[b])) b++;
            while (e > b && isspace((unsigned char)rx[e - 1])) e--;
            likes[k] = rx.substr(b, e - b);
        }
    }
    unordered_map<string, i64> adjs;
    for (auto& p : tblAdj) adjs[p.first] = p.second;
    set<string> vtused;
    auto likeOf = [&](const string& key) -> const string* { auto it = likes.find(key); return it == likes.end() ? nullptr : &it->second; };
    for (auto& ko : tblOff) {
        const string& key = ko.first;
        i64 value = ko.second;
        size_t p1 = key.find('|'), p2 = key.find('|', p1 + 1);
        string fn = key.substr(0, p1), kind = key.substr(p1 + 1, p2 - p1 - 1);
        if (startsWith(kind, "vt:")) {
            bool done = false;
            if (isFound(fn) || rtti.count(fn)) {
                if (isFound(kind.substr(3))) {
                    emit("offvt " + E(key) + " " + hexs(value) + " " + E(fn) + " " + E(kind.substr(3)));
                    done = true;
                    vtused.insert(fn);
                }
            }
            string vtn = fn + "+0x10";
            if (rtti.count(vtn)) {
                vtused.insert(vtn);
                Span w = imgSpan(addrOf(vtn) + value, 8);
                i64 m = w.n >= 8 ? (i64)(ld64(w.p) - (u64)BASE) : -1;
                if (in_text(m)) {
                    emit("offvtfn " + E(key) + " " + hexs(value) + " " + E(vtn) + " " + mkPat(m, whole_end(m, 48)).text());
                    done = true;
                }
            }
            if (!done) miss.push_back("off " + key);
            continue;
        }
        bool hasA = hasAddr(fn);
        i64 a = hasA ? addrOf(fn) : 0;
        const string* like = likeOf(key);
        KR an;
        if (hasA) an = disp_anchors(a, value, 3, like);
        if (an.empty() && !like) {
            auto vn = vnotes.find({fn, value});
            an = anchors_in(vn == vnotes.end() ? set<i64>() : vn->second, value);
        }
        // a pinned field read in a function the table has no address for (ListViewSorter::compareElements): only in
        // the function its verification names as "win_fn", and only through the pinned instruction
        if (an.empty() && like && !hasA) {
            auto vn = vfns.find({fn, value});
            an = anchors_in(vn == vfns.end() ? set<i64>() : vn->second, value, like);
        }
        for (auto& kr : an) emit("off" + kr.first + " " + E(key) + " " + hexs(value) + " " + kr.second);
        bool anFlag = !an.empty();
        if (!anFlag && hasA && adjs.count(fn)) {
            for (auto& kr : disp_anchors(a, value - adjs[fn])) {
                emit("offadj" + kr.first + " " + E(key) + " " + hexs(value) + " " + E(fn) + " " + kr.second);
                anFlag = true;
            }
        }
        if (isFound(fn)) {
            emit("offsame " + E(key) + " " + hexs(value) + " " + E(fn) + " " + mkPat(a, whole_end(a, 2048)).text());
            auto sq = emit_offseq(key, value, fn, a, like);
            if (sq) emit(*sq);
        } else if (!anFlag) miss.push_back("off " + key);
    }
    fprintf(stderr, "[t] offsets done\n");

    // offrel
    {
        vector<pair<pair<string, int>, vector<pair<string, i64>>>> byfn;
        map<pair<string, int>, size_t> byfnIdx;
        for (auto& ko : tblOff) {
            size_t p1 = ko.first.find('|'), p2 = ko.first.find('|', p1 + 1);
            string fn = ko.first.substr(0, p1), kind = ko.first.substr(p1 + 1, p2 - p1 - 1);
            if (startsWith(kind, "vt:") || !isFound(fn)) continue;
            int b = field_base(addrOf(fn), ko.second, likeOf(ko.first));
            if (b) {
                auto k = make_pair(fn, b);
                auto it = byfnIdx.find(k);
                if (it == byfnIdx.end()) { byfnIdx[k] = byfn.size(); byfn.push_back({k, {}}); it = byfnIdx.find(k); }
                byfn[it->second].second.push_back(ko);
            }
        }
        for (auto& g : byfn)
            for (auto& kv : g.second) {
                bool have = false;
                i64 bd = 0, bv = 0;
                string bk;
                for (auto& o : g.second) {
                    i64 d = llabs(o.second - kv.second);
                    if (o.first == kv.first || d == 0 || d > 0x100) continue;
                    // min of (distance, key, value) tuples
                    if (!have || make_tuple(d, o.first, o.second) < make_tuple(bd, bk, bv)) { have = true; bd = d; bk = o.first; bv = o.second; }
                }
                if (have) emit("offrel " + E(kv.first) + " " + hexs(kv.second) + " " + E(bk) + " " + hexs(bv));
            }
    }
    for (auto& n : vtused)
        if (rtti.count(n)) emit(strf("vtslots %s %d", E(n).c_str(), (int)vt_slots(addrOf(n) + (endsWith(n, "+0x10") ? 0 : 0x10)).size()));
    for (auto& p : tblAdj) emit(strf("adj %s %d", E(p.first).c_str(), (int)p.second));
    for (auto& r : tblHookreg) {
        string s = "hookreg";
        for (auto& x : r) s += " " + x;
        emit(s);
    }

    // ---- guards
    {
        bool ok;
        string t = readFile(repo + "/tools/win/sigguards.txt", &ok);
        if (ok) for (auto& line0 : lines(t)) {
            string line = line0.substr(0, line0.find('#'));
            // split(None, 3)
            vector<string> w;
            size_t i = 0;
            while (w.size() < 3) {
                while (i < line.size() && isspace((unsigned char)line[i])) i++;
                if (i >= line.size()) break;
                size_t j = i;
                while (j < line.size() && !isspace((unsigned char)line[j])) j++;
                w.push_back(line.substr(i, j - i));
                i = j;
            }
            while (i < line.size() && isspace((unsigned char)line[i])) i++;
            if (w.size() == 3 && i < line.size()) w.push_back(line.substr(i));
            if (w.size() != 4) continue;
            string fn = w[0], hooks = w[2];
            i64 value = strtoll(w[1].c_str(), nullptr, 16);
            string lk = w[3];
            while (!lk.empty() && isspace((unsigned char)lk.back())) lk.pop_back();
            KR an;
            if (hasAddr(fn)) an = disp_anchors(addrOf(fn), value, 1, &lk);
            if (an.empty()) { miss.push_back("guard " + fn + " " + w[1]); continue; }
            emit("guard" + an[0].first + " " + hooks + " " + hexs(value) + " " + an[0].second);
        }
    }
    fprintf(stderr, "[t] guards done\n");

    // ---- fallbacks
    {
        map<string, i64> fb_names;
        for (auto& kv : foundList)
            if (!startsWith(kv.first, "__ZTV") && !startsWith(kv.first, "__ZTI") && !startsWith(kv.first, "__ZTS") && kv.first[0] != '@') fb_names[kv.first] = kv.second;
        for (auto& line : out) {
            auto w = splitWs(line);
            string an;
            if (w.size() > 2 && w[0] == "refin") an = w[2];
            else if (w.size() > 3 && (w[0] == "offin" || w[0] == "hookin" || w[0] == "guardin")) an = w[3];
            if (!an.empty() && an[0] == '@') fb_names[an] = strtoll(an.c_str() + 1, nullptr, 16);
        }
        int fb = 0;
        for (auto& kv : fb_names) if (emit_fallbacks_one(kv.first, kv.second, 3)) fb++;
        unordered_set<string> have_fnvt;
        for (auto& line : out) if (startsWith(line, "fnvt ")) {
            size_t e = line.find(' ', 5);
            if (e != string::npos) have_fnvt.insert(line.substr(5, e - 5));
        }
        for (auto& kv : fb_names) if (!have_fnvt.count(E(kv.first)) && in_text(kv.second) && emit_fnvt(kv.first, kv.second)) fb++;
        fprintf(stderr, "%d names have fallback lines\n", fb);
    }
    for (auto& m : miss) out.push_back("# not covered: " + m);
    string all;
    for (auto& l : out) { all += l; all += '\n'; }
    fwrite(all.data(), 1, all.size(), stdout);
    fprintf(stderr, "%s: %d lines, %d not covered\n", BUILD.c_str(), (int)out.size(), (int)miss.size());
    return 0;
}

// anchor_name: the anchor of a chunk without making one
static string anchor_name(i64 cs) {
    auto it = anchors.find(cs);
    if (it != anchors.end() && !it->second.empty()) return it->second;
    auto f = foundBy.find(cs);
    if (f != foundBy.end()) for (auto& n : f->second) if (!startsWith(n, "__ZT")) return n;
    return "";
}
static bool emit_fallbacks_one(const string& name, i64 a, int k) {
    set<string> used;
    {
        auto it = refinUsed.find(E(name));
        if (it != refinUsed.end()) used = it->second;
    }
    vector<Cand> cands;
    if (in_text(a)) for (i64 s : callers(a)) cands.push_back({s, 5, 1});
    for (auto& c : riprefs(a)) cands.push_back(c);
    vector<Cand> cf;
    vector<i64> csOf;
    for (auto& c : cands) {
        i64 cs = chunk_start(c.site);
        if (cs != NONE && cs != a) cf.push_back(c);
    }
    // what's found before this name's candidates are tried (anchor_of adds anchors as it goes; they don't count)
    unordered_set<i64> haveSet(foundVals.begin(), foundVals.end());
    for (auto& kv : anchors) if (!kv.second.empty()) haveSet.insert(kv.first);
    auto have = [&](i64 cs) { return haveSet.count(cs) > 0; };
    {
        vector<pair<char, i64>> key(cf.size());
        for (size_t i = 0; i < cf.size(); i++) key[i] = {!have(chunk_start(cf[i].site)), cf[i].site};
        vector<size_t> ord(cf.size());
        for (size_t i = 0; i < ord.size(); i++) ord[i] = i;
        stable_sort(ord.begin(), ord.end(), [&](size_t x, size_t y) { return key[x] < key[y]; });
        vector<Cand> sorted;
        for (size_t i : ord) sorted.push_back(cf[i]);
        cf = sorted;
    }
    int n = 0, tries = 0;
    for (size_t ci = 0; ci < cf.size() && ci < 300; ci++) {
        const Cand& c = cf[ci];
        i64 cs = chunk_start(c.site);
        if (!have(cs)) {
            tries++;
            if (tries > 12) break;
        }
        string an = anchor_name(cs);
        if (!an.empty() && (an == name || used.count(E(an)))) continue;
        auto u = scoped_window(c.site, c.sz);
        if (!u || used.count(E(u->name))) continue;
        used.insert(E(u->name));
        emit("refin " + E(name) + " " + E(u->name) + " " + hexs(u->rng) + " " + u->pt.text() +
             strf(" %d %d", (int)(c.site + c.doff - u->pt.start), (int)(c.site + c.sz - u->pt.start)));
        auto b = scoped_window(c.site, c.sz, Rng(), true);
        if (b && b->pt.text() != u->pt.text())
            emit("refin " + E(name) + " " + E(b->name) + " " + hexs(b->rng) + " " + b->pt.text() +
                 strf(" %d %d", (int)(c.site + c.doff - b->pt.start), (int)(c.site + c.sz - b->pt.start)));
        n++;
        if (n >= k) break;
    }
    return n > 0;
}
