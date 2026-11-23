// autocue: cues (intro, build, drop, breakdown, build 2, drop 2, outro, fake drop) on a playlist's tracks, as hot cues
// A-H, as memory cues, or both: the layout says per cue which pad it gets and whether it is a memory cue (older CDJs
// show three or four hot cues only; every one has ten memory cues)
// (Spotify ones read from the stream into memory by the spotifyaudio mod, or from a local file of the track, rbmod.h
// "trackfiles"). Playlist menu: "Open auto cues window".
//
// One batch run, then fine-tuning: Analyse reads the ticked tracks, autocue_sheet.h works out its arrangement (which bars
// are copies of which, per stem: the drums, bass, melody and vocals from betterstems' HTDemucs, or three bands of the
// mix without it) and writes it out as a sheet, and a language model reads the sheet (it never hears the track) and
// places the cues, following the DJ's own instructions. Without a key, or if the model fails, the rules' own cues are
// used. Every track's analysis, the model's answer and your edits are kept in <configDir>/autocue/, so the window
// opens where you left it. In the window you check each track (waveform, the stems' clips, the sections), listen from
// any bar, and move (by the bar, beat or half beat), add or remove cues, ⌘Z / Ctrl+Z undoing edits; nothing goes into
// rekordbox until Write cues (Undo write takes it back). Stems come from the stemcache mod's store where a deck (or
// the stems window) separated the track before, and what autocue separates is stored there for the decks too (a Spotify
// track's stems in memory only, in the public edition: docs/editions.md).
// The model goes through OpenRouter (any model; the key is in the OS's keychain, not in the config) or through the
// Claude Code CLI installed on this computer (its own login, no key: `claude -p`, with the models it offers).
//   config: autocue=0 to disable
//           autocue_provider=openrouter   or claude: the local Claude Code CLI; none: no model, the built-in rules place the cues
//           autocue_model=anthropic/claude-haiku-5.5   any model id from openrouter.ai/models
//           autocue_claude_model=haiku   Claude Code's model (an alias or a full name)
//           autocue_claude_path=         the claude program, if it isn't found by itself
//           autocue_stems=1   0 analyses three bands of the mix instead of stems (faster: no separation)
//           autocue_parallel=3   tracks analysed at once (one of them separates stems at a time)
//           autocue_pads=ABCDEFGH   the hot cue pad of Intro, Build, Drop, Breakdown, Build 2, Drop 2, Outro and Fake drop, in that order (- for none); set in the window under Layout
//           autocue_memory=00000000   per cue, in autocue_pads' order: 1 writes it as a memory cue too (or only, without a pad)
//           autocue_build_bars=0   4, 8, 16 or 32 puts build cues that many bars before their drop (0: where the build starts)
//           autocue_intro=sound   where the intro cue goes: sound (the first bar with sound), drums (the first bar with drums) or start (the track's first bar)
//           autocue_outro_bars=0   16, 32 or 64 puts the outro cue that many bars before the track ends (0: where the last loud part ends)
#include "../core/rbmod.h"
#include "../core/library.h"
#include "../core/webui.h"
#include "audiofile.h"
#include "autocue_sheet.h"
#include "jsonmini.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <deque>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <new>
#include <thread>

using namespace rbm;
namespace web = rbm::webui;
namespace sheet = autocue_sheet;

namespace {  // file-local: another mod has types of the same names

const Addr A_dbHolder{"SingletonHolder<DatabaseIF>", [] { return singleton("__ZN11DetailColorC2Ev", "__ZN4juce15SingletonHolderIN2db10DatabaseIFENS_15CriticalSectionELb0EE3getEv"); }};
const Addr A_dbGet{"__ZN4juce15SingletonHolderIN2db10DatabaseIFENS_15CriticalSectionELb0EE3getEv"};
const Addr A_isCollectionSong{"__ZN2db10DatabaseIF16isCollectionSongERKN4juce6StringERNS_12RowDataTrackE"};
const Addr A_rowCtor{"__ZN2db12RowDataTrackC2Ev"};
const Addr A_rowDtor{"__ZN2db12RowDataTrackD2Ev"};
const Offset O_rowAnalysisPath{0x3f0, "__ZNK2db12RowDataTrack22getExtAnalysisFilePathEv", "ldr x0", 0};
const size_t S_rowDataTrack = 0x4e0;
const Addr A_getCues{"__ZN2db10DatabaseIF10getCueInfoEjRN4juce10OwnedArrayIN8analyzer3CueENS1_20DummyCriticalSectionEEENS_12_browse_typeEl"};
const Addr A_setCue{"__ZN2db10DatabaseIF6setCueEjRN8analyzer3CueENS_12_browse_typeElPN6rbxfrm21IPreparedDataListenerE"};
const Addr A_deleteCue{"__ZN2db10DatabaseIF9deleteCueEjjNS_12_browse_typeElPN6rbxfrm21IPreparedDataListenerE"};
const Addr A_cueVtable{"__ZTVN8analyzer3CueE"};

// analyzer::Cue: id +8, kind +0xc (0 memory; 1 2 3 5 6 7 8 9 = hot cue A-H), in ms +0x10, in in 1/150 s +0x14,
// comment (juce::String) +0x38, colour table index +0x4c. Out points stay 0; +0x31 is "no RGB colour" (0xff).
const int kBrowseCollection = 1;
const int kKinds[8] = {1, 2, 3, 5, 6, 7, 8, 9};
const int kColors[8] = {43, 8, 0, 0, 19, 38, 1, 32};  // rekordbox's default pad colours
int kindOf(char slot) { return kKinds[slot - 'A']; }
char slotOfKind(int kind) {
    for (int i = 0; i < 8; i++)
        if (kKinds[i] == kind) return (char)('A' + i);
    return 'M';
}
const char* slotName(char slot) { return sheet::slotNames()[slot - 'A']; }
const int kMemoryKind = 0, kMemoryMax = 10;  // (a track holds ten memory cues)

// The layout: where each cue goes. A cue is known by its letter in the sheet and to the model (A Intro ... H Fake
// drop); its hot cue pad is the layout's, and so is whether it is written as a memory cue as well (or only).
struct Layout {
    char pad[8];    // per cue: 'A'-'H', or '-' for no pad
    bool mem[8];    // per cue: a memory cue too
    // The rules for placing them (the page's rulesets are sets of these); they hold for the model's cues too.
    int buildBars;  // > 0: build cues go this many bars before their drop
    int intro;      // the intro cue: 0 the first bar with sound, 1 the first bar with drums, 2 the track's first bar
    int outroBars;  // > 0: the outro cue goes this many bars before the track ends
};
const char* const kIntro[] = {"sound", "drums", "start"};
Layout g_layout{{'A', 'B', 'C', 'D', 'E', 'F', 'G', 'H'}, {}, 0, 0, 0};  // g_lock
Layout layoutFrom(const std::string& pads, const std::string& mem, int build, const std::string& intro, int outro) {
    Layout l{};
    bool taken[8] = {};
    for (int i = 0; i < 8; i++) {
        char p = i < (int)pads.size() ? (char)toupper((unsigned char)pads[i]) : '-';
        l.pad[i] = p >= 'A' && p <= 'H' && !taken[p - 'A'] ? p : '-';  // (a pad holds one cue)
        if (l.pad[i] != '-') taken[l.pad[i] - 'A'] = true;
        l.mem[i] = i < (int)mem.size() && mem[i] == '1';
    }
    l.buildBars = build == 4 || build == 8 || build == 16 || build == 32 ? build : 0;
    l.intro = intro == kIntro[1] ? 1 : intro == kIntro[2] ? 2 : 0;
    l.outroBars = outro == 16 || outro == 32 || outro == 64 ? outro : 0;
    return l;
}
std::string padsStr(const Layout& l) { return std::string(l.pad, 8); }
std::string memStr(const Layout& l) {
    std::string s;
    for (bool m : l.mem) s += m ? '1' : '0';
    return s;
}
char padOf(char slot) { return g_layout.pad[slot - 'A']; }   // g_lock held
bool memOf(char slot) { return g_layout.mem[slot - 'A']; }

volatile bool g_on;
void* database() { return at<void* (*)(uintptr_t)>(A_dbGet)(A_dbHolder + g_slide); }

struct OwnedArr { char** data; int allocated, used; };

struct CueCopy { int kind, inMs; std::string comment; int color; unsigned id; int outMs, outFrame; unsigned char rgb[2]; };

std::vector<CueCopy> readCues(unsigned track) {
    OwnedArr a{nullptr, 0, 0};
    at<void (*)(void*, unsigned, OwnedArr*, int, long)>(A_getCues)(database(), track, &a, kBrowseCollection, 0);
    std::vector<CueCopy> out;
    for (int i = 0; i < a.used; i++) {
        const char* c = a.data[i];
        const char* text = *(const char* const*)(c + 0x38);
        out.push_back({*(const int*)(c + 0xc), *(const int*)(c + 0x10), text ? text : "", *(const int*)(c + 0x4c), *(const unsigned*)(c + 8), *(const int*)(c + 0x18), *(const int*)(c + 0x1c), {(unsigned char)c[0x30], (unsigned char)c[0x31]}});
    }
    free(a.data);  // (the objects stay: a few bytes per reading)
    return out;
}

unsigned writeCue(unsigned track, int kind, int ms, const std::string& comment, int color, const CueCopy* old = nullptr) {
    alignas(16) char c[0x100] = {};
    *(void**)c = (char*)(A_cueVtable + g_slide) + 0x10;
    c[0x31] = (char)0xff;
    JStr* text = new (c + 0x38) JStr(comment.c_str());
    *(int*)(c + 0xc) = kind;
    *(int*)(c + 0x10) = ms;
    *(int*)(c + 0x14) = (int)(ms * 150LL / 1000);
    *(int*)(c + 0x4c) = color;
    if (old) *(int*)(c + 0x18) = old->outMs, *(int*)(c + 0x1c) = old->outFrame, c[0x30] = (char)old->rgb[0], c[0x31] = (char)old->rgb[1];  // a loop stays a loop
    at<void (*)(void*, unsigned, void*, int, long, void*)>(A_setCue)(database(), track, c, kBrowseCollection, 0, nullptr);
    unsigned id = *(const unsigned*)(c + 8);
    text->~JStr();
    return id;
}

void deleteCue(unsigned track, unsigned id) {
    at<void (*)(void*, unsigned, unsigned, int, long, void*)>(A_deleteCue)(database(), track, id, kBrowseCollection, 0, nullptr);
}

// The beat grid in rekordbox's ANLZ .DAT file (PQTZ section, big-endian): times in ms and bar positions 1-4.
bool readGrid(const std::vector<uint8_t>& f, std::vector<int>* ms, std::vector<int>* num) {
    auto u32 = [&](size_t p) { return (uint32_t)f[p] << 24 | f[p + 1] << 16 | f[p + 2] << 8 | f[p + 3]; };
    auto u16 = [&](size_t p) { return (uint32_t)f[p] << 8 | f[p + 1]; };
    if (f.size() < 12 || memcmp(f.data(), "PMAI", 4)) return false;
    size_t p = u32(4);
    while (p + 12 <= f.size()) {
        uint32_t total = u32(p + 8);
        if (total < 12 || p + total > f.size()) return false;
        if (!memcmp(f.data() + p, "PQTZ", 4)) {
            if (total < 0x18) return false;
            uint32_t n = u32(p + 0x14);
            if (0x18 + (uint64_t)n * 8 > total) return false;
            ms->clear(), num->clear();
            for (uint32_t i = 0; i < n; i++) {
                size_t e = p + 0x18 + i * 8;
                num->push_back((int)u16(e)), ms->push_back((int)u32(e + 4));
            }
            return !ms->empty();
        }
        p += total;
    }
    return false;
}

// ---- the tracks -----------------------------------------------------------------------------------------------

struct Cue { char slot; int bar; std::string why; };  // bar: 1-based

struct Answer {  // the model's
    std::string status;  // "" none, queued, asking, done, failed, nokey
    std::string model, notes, error, key;
    std::vector<Cue> cues;
    double cost = 0;
};

struct Row {
    std::string uri, title, artist, dat;
    bool usable = false;  // in the collection with audio to read (sorted to the top)
    std::string audio;    // the file to read: the track's own, or a Spotify track's local file; its URI: the stream
    unsigned id = 0;
    std::string state = "Not analysed";  // Waiting, Reading…, or why it can't be
    bool force = false;  // Analyse again asked for it, ticked or not
    bool done = false, busy = false, tick = true;  // done: analysed (or can't be); busy: being read
    std::vector<CueCopy> existing;
    std::shared_ptr<const sheet::Analysis> an;  // null until analysed
    Answer ai;
    std::map<char, double> draft;  // slot -> position in bars (1-based; 33.25 is bar 33's second beat): what Write cues writes
    bool edited = false;        // the draft is the user's (a new answer doesn't replace it)
    int version = 0;            // bumped on every change the detail view shows
    int cueAck = 0;             // the page's number of the last setCue applied: the detail echoes it so the page can drop an older draft
};

struct Undo {
    unsigned track;
    std::vector<unsigned> added;
    std::vector<CueCopy> removed;
};

web::Window* g_win;
std::mutex g_lock;  // everything below
std::vector<Row> g_rows;
std::string g_status, g_playlistName, g_selected;
bool g_replace, g_hasKey;
std::string g_models;  // OpenRouter's models for the dropdown, as JSON for the page
std::atomic<int> g_generation{0};
std::atomic<bool> g_dirty{false};
std::vector<Undo> g_undo;
bool g_useStems = true;
std::string g_instructions;
int g_sentVersion = -1;
std::string g_sentSelected;
bool g_dumpNext;    // a test run asked for the next state as a file
std::mutex g_keyLock;
std::string g_key;  // the OpenRouter key, read from the keychain once (the OS may ask the user to allow it)
double g_cost = 0;  // OpenRouter's cost of this session's answers

bool useClaude() { return cfgStr("autocue_provider", "openrouter") == "claude"; }
bool noModel() { return cfgStr("autocue_provider", "openrouter") == "none"; }  // the rules' cues, and no model asked
std::string model() { return useClaude() ? cfgStr("autocue_claude_model", "haiku") : cfgStr("autocue_model", "anthropic/claude-haiku-5.5"); }

// The Claude Code CLI on this computer (looked for when the window opens).
struct Claude {
    bool looked = false, ok = false;  // ok: found and logged in
    std::string path, version, account, error;
    std::string models;  // for the dropdown, as JSON for the page
} g_claude;  // g_lock
bool canAsk() { return noModel() ? false : useClaude() ? g_claude.ok : g_hasKey; }  // g_lock held
std::string dataDir() { return plat::configDir() + "/autocue"; }

// betterstems' HTDemucs, by service; false when that mod is off or has no model.
bool stemsAvailable() {
    auto active = service<bool (*)()>("betterstems.active");
    return active && service<void*>("betterstems.separate") && active();
}

uint64_t fnv(const std::string& s) {
    uint64_t h = 1469598103934665603ull;
    for (unsigned char c : s) h = (h ^ c) * 1099511628211ull;
    return h;
}
std::string hex(uint64_t v) {
    char b[24];
    snprintf(b, sizeof b, "%016llx", (unsigned long long)v);
    return b;
}

Row* rowOf(const std::string& uri) {  // (rows move: new ones come in, and they're sorted); g_lock held
    for (Row& r : g_rows)
        if (r.uri == uri) return &r;
    return nullptr;
}

std::string num(double v, int decimals) {
    char b[32];
    snprintf(b, sizeof b, "%.*f", decimals, v);
    return b;
}

// The last bar with sound (1-based), from the waveform: where "bars before the track ends" counts from.
int lastSoundBar(const sheet::Analysis& a) {
    int cols = (int)a.amp.size(), c = cols - 1;
    while (c > 0 && a.amp[c] < 0.02f) c--;
    if (cols < 2 || a.duration <= 0) return a.bars;
    double t = (c + 1.0) / cols * a.duration;
    int bar = a.bars;
    while (bar > 1 && a.barTimes[bar - 1] >= t) bar--;
    return bar;
}

// A source's cues as the layout's rules place them: the intro cue on the first drums or the first bar, build cues a
// fixed number of bars before their drop, the outro cue a fixed number of bars before the end. Bars are 1-based.
std::vector<Cue> placed(std::vector<Cue> cues, const sheet::Analysis* an) {  // g_lock held
    const Layout& l = g_layout;
    if (!l.buildBars && !l.intro && !l.outroBars) return cues;
    auto barOf = [&](char slot) {
        for (const Cue& c : cues)
            if (c.slot == slot) return c.bar;
        return 0;
    };
    auto drop = [&](char slot) { cues.erase(std::remove_if(cues.begin(), cues.end(), [&](const Cue& c) { return c.slot == slot; }), cues.end()); };
    if (l.buildBars) {
        const std::pair<char, char> buildDrop[] = {{'B', 'C'}, {'E', 'F'}};
        for (auto [build, at] : buildDrop) {
            int d = barOf(at), from = barOf(build == 'B' ? 'A' : 'D');  // not before the intro, or inside the drop before
            if (d <= 0 || d - l.buildBars < std::max(from, 1)) continue;         // (no room for it: the build stays where it starts)
            drop(build);
            cues.push_back({build, d - l.buildBars, std::to_string(l.buildBars) + " bars before the drop (the rules for build cues)"});
        }
    }
    if (l.intro && an) {
        int next = an->bars + 1, bar = 0;  // the intro cue stays before every other cue
        for (const Cue& c : cues)
            if (c.slot != 'A') next = std::min(next, c.bar);
        std::string why;
        if (l.intro == 2) bar = 1, why = "the track's first bar";
        else {
            // the drums' first clip of four bars or more (a fill before it isn't the beat); without stems, the low band's
            const std::vector<sheet::Lane>& lane = an->lanes[an->stems ? sheet::DRUMS : 1];
            for (const sheet::Lane& x : lane)
                if (x.id != -2 && x.end - x.start >= 4) { bar = x.start + 1; break; }
            why = an->stems ? "first bar with drums" : "first bar with low end";
        }
        if (bar >= 1 && bar < next) drop('A'), cues.push_back({'A', bar, why + " (the rules for the intro cue)"});
    }
    if (l.outroBars && an) {
        int after = 0, bar = lastSoundBar(*an) - l.outroBars + 1;  // the outro cue stays after every other cue
        for (const Cue& c : cues)
            if (c.slot != 'G') after = std::max(after, c.bar);
        if (bar > after) drop('G'), cues.push_back({'G', bar, std::to_string(l.outroBars) + " bars before the track ends (the rules for the outro cue)"});
    }
    std::sort(cues.begin(), cues.end(), [](const Cue& a, const Cue& b) { return a.slot < b.slot; });
    return cues;
}
std::vector<Cue> modelCues(const Row& r) { return placed(r.ai.cues, r.an.get()); }
std::vector<Cue> rulesCues(const Row& r) {  // (1-based, like the model's)
    std::vector<Cue> out;
    if (r.an)
        for (const sheet::Cue& c : r.an->cues) out.push_back({c.slot, c.bar + 1, c.why});
    return placed(out, r.an.get());
}

// The cues the draft starts from: the model's, else the rules'.
std::map<char, double> defaultDraft(const Row& r) {
    std::map<char, double> d;
    for (const Cue& c : !noModel() && r.ai.status == "done" && !r.ai.cues.empty() ? modelCues(r) : rulesCues(r)) d[c.slot] = c.bar;
    return d;
}

// A draft position in half beats of 4/4 (eighths of a bar), within the track.
double snapPos(const sheet::Analysis& a, double pos) {
    pos = std::round(pos * 8) / 8;
    return pos >= 1 && pos < a.bars + 1 ? pos : 0;
}
std::string posStr(double pos) { return num(pos, 3); }

int barMs(const sheet::Analysis& a, double pos) {  // 1-based; a part of a bar between its bar lines (the grid's tempo is even within a bar)
    int bar = (int)std::floor(pos);
    if (bar < 1 || bar > (int)a.barTimes.size()) return -1;
    double t = a.barTimes[bar - 1];
    if (pos > bar && bar < (int)a.barTimes.size()) t += (pos - bar) * (a.barTimes[bar] - a.barTimes[bar - 1]);
    return std::max(0, (int)std::lround(t * 1000));
}

// Per draft cue, its hot cue: 0 to write, 1 already in rekordbox, 2 its pad holds another cue, -1 the layout gives it no pad.
int padState(const Row& r, char slot, double bar) {
    if (padOf(slot) == '-') return -1;
    int ms = r.an ? barMs(*r.an, bar) : -1;
    for (const CueCopy& c : r.existing)
        if (c.kind == kindOf(padOf(slot))) return abs(c.inMs - ms) < 100 ? 1 : 2;
    return 0;
}
// And its memory cue: 0 to write, 1 already in rekordbox, -1 the layout makes it none.
int memState(const Row& r, char slot, double bar) {
    if (!memOf(slot)) return -1;
    int ms = r.an ? barMs(*r.an, bar) : -1;
    for (const CueCopy& c : r.existing)
        if (c.kind == kMemoryKind && abs(c.inMs - ms) < 100) return 1;
    return 0;
}

// ---- the cache: <configDir>/autocue/<hash of the uri>.json --------------------------------------------------------


std::string cuesJson(const std::vector<Cue>& cues) {
    std::string j = "[";
    for (const Cue& c : cues) j += std::string(j.size() > 1 ? "," : "") + "{\"s\":\"" + c.slot + "\",\"b\":" + std::to_string(c.bar) + ",\"w\":" + web::jsonStr(c.why) + "}";
    return j + "]";
}

std::string analysisJson(const sheet::Analysis& a) {
    std::string j = "{\"bars\":" + std::to_string(a.bars) + ",\"bpm\":" + num(a.bpm, 2) + ",\"stems\":" + (a.stems ? "true" : "false") +
                    ",\"moved\":" + std::to_string(a.downMoved) + ",\"copied\":" + num(a.copiedShare, 3) + ",\"dur\":" + num(a.duration, 3) + ",\"times\":[";
    for (size_t i = 0; i < a.barTimes.size(); i++) j += (i ? "," : "") + num(a.barTimes[i], 4);
    j += "],\"cues\":[";
    for (size_t i = 0; i < a.cues.size(); i++) j += std::string(i ? "," : "") + "{\"s\":\"" + a.cues[i].slot + "\",\"b\":" + std::to_string(a.cues[i].bar + 1) + ",\"w\":" + web::jsonStr(a.cues[i].why) + "}";
    j += "],\"sections\":[";
    for (size_t i = 0; i < a.sections.size(); i++) j += std::string(i ? "," : "") + "[" + std::to_string(a.sections[i].start) + "," + std::to_string(a.sections[i].end) + "," + (a.sections[i].high ? "1" : "0") + "]";
    j += "],\"lanes\":[";
    for (int s = 0; s < sheet::NSTEMS; s++) {
        j += std::string(s ? "," : "") + "[";
        for (size_t i = 0; i < a.lanes[s].size(); i++) {
            const sheet::Lane& l = a.lanes[s][i];
            j += std::string(i ? "," : "") + "[" + std::to_string(l.start) + "," + std::to_string(l.end) + "," + std::to_string(l.id) + "," + web::jsonStr(l.label) + "]";
        }
        j += "]";
    }
    j += "],\"amp\":[";
    for (size_t i = 0; i < a.amp.size(); i++) j += (i ? "," : "") + num(a.amp[i], 3);
    j += "],\"hi\":[";
    for (size_t i = 0; i < a.hi.size(); i++) j += (i ? "," : "") + num(a.hi[i], 3);
    return j + "],\"sheet\":" + web::jsonStr(a.sheet) + "}";
}

std::shared_ptr<sheet::Analysis> analysisFrom(const jsonmini::Value& v) {
    auto a = std::make_shared<sheet::Analysis>();
    a->bars = (int)v["bars"].n, a->bpm = v["bpm"].n, a->stems = v["stems"].b, a->downMoved = (int)v["moved"].n, a->copiedShare = v["copied"].n, a->duration = v["dur"].n;
    for (auto& t : v["times"].a) a->barTimes.push_back(t.n);
    for (auto& c : v["cues"].a)
        if (c["s"].s.size() == 1) a->cues.push_back({c["s"].s[0], (int)c["b"].n - 1, c["w"].s});
    for (auto& s : v["sections"].a) a->sections.push_back({(int)s[(size_t)0].n, (int)s[1].n, s[2].n != 0});
    for (int s = 0; s < sheet::NSTEMS; s++)
        for (auto& l : v["lanes"][(size_t)s].a) a->lanes[s].push_back({(int)l[(size_t)0].n, (int)l[1].n, (int)l[2].n, l[3].s});
    for (auto& x : v["amp"].a) a->amp.push_back((float)x.n);
    for (auto& x : v["hi"].a) a->hi.push_back((float)x.n);
    a->sheet = v["sheet"].s;
    if (a->bars < 2 || (int)a->barTimes.size() != a->bars + 1 || a->sheet.empty()) return nullptr;
    return a;
}

std::vector<Cue> cuesFrom(const jsonmini::Value& v) {
    std::vector<Cue> out;
    for (auto& c : v.a) {  // (from a cache file: checked like a model's answer, a slot outside A-H would index past the pad tables)
        char slot = c["s"].s.size() == 1 ? c["s"].s[0] : 0;
        if (slot < 'A' || slot > 'H' || c["b"].n < 1 || c["b"].n > 100000) continue;
        out.push_back({slot, (int)c["b"].n, c["w"].s});
    }
    return out;
}

// What the analysis was made from: the audio file, the grid and the stems setting. A Spotify track's audio is its
// stream, or a local file that may be replaced by another later (the same samples): its key is the track, not that file, or the analysis, the model's answer and the user's edits would all be dropped the
// next time the window opens. byFile: the key as it was made before (for analyses saved with it).
std::string sourceKey(const std::string& uri, const std::string& audio, const std::vector<int>& beatMs, bool stems, bool byFile = false) {
    std::string grid;
    for (int ms : beatMs) grid += std::to_string(ms) + ",";
    std::string tail = "|" + hex(fnv(grid)) + (stems ? "|stems" : "|mix");
    if (!byFile && !uri.compare(0, 8, "spotify:")) return "v1|" + uri + tail;
    plat::FileInfo fi;
    plat::fileInfo(audio, &fi);
    return "v1|" + audio + "|" + std::to_string(fi.size) + "|" + num(fi.mtime, 0) + tail;
}

void save(const Row& r, const std::string& source) {  // g_lock held (small files)
    if (!r.an) return;
    std::string j = "{\"uri\":" + web::jsonStr(r.uri) + ",\"source\":" + web::jsonStr(source) + ",\"an\":" + analysisJson(*r.an);
    if (r.ai.status == "done" || r.ai.status == "failed")
        j += ",\"ai\":{\"status\":" + web::jsonStr(r.ai.status) + ",\"model\":" + web::jsonStr(r.ai.model) + ",\"key\":" + web::jsonStr(r.ai.key) + ",\"notes\":" + web::jsonStr(r.ai.notes) +
             ",\"error\":" + web::jsonStr(r.ai.error) + ",\"cues\":" + cuesJson(r.ai.cues) + "}";
    j += std::string(",\"edited\":") + (r.edited ? "true" : "false") + ",\"draft\":{";
    bool first = true;
    for (auto& [s, b] : r.draft) j += std::string(first ? "" : ",") + "\"" + s + "\":" + posStr(b), first = false;
    j += "}}";
    plat::writeFileAtomic(dataDir() + "/" + hex(fnv(r.uri)) + ".json", j.data(), j.size());
}

std::map<std::string, std::string> g_sourceOf;  // per uri: what its analysis was made from (saved with it)

void saveRow(const Row& r) {
    auto it = g_sourceOf.find(r.uri);
    if (it != g_sourceOf.end()) save(r, it->second);
}

// The cached analysis of a track, if it was made from the same audio, grid and stems setting.
thread_local bool t_keyBefore;  // loadCached's last match was by the key as made before: saved again under the new one
bool loadCached(const std::string& uri, const std::string& source, const std::string& sourceBefore, std::shared_ptr<const sheet::Analysis>* an, Answer* ai, std::map<char, double>* draft, bool* edited) {
    std::string text;
    jsonmini::Value v;
    if (!plat::readFile(dataDir() + "/" + hex(fnv(uri)) + ".json", &text) || !jsonmini::parse(text, &v) || v["uri"].s != uri ||
        (v["source"].s != source && v["source"].s != sourceBefore))
        return false;
    auto a = analysisFrom(v["an"]);
    if (!a) return false;
    t_keyBefore = v["source"].s != source;
    *an = a;
    if (v["ai"].type == jsonmini::Value::Obj) {
        ai->status = v["ai"]["status"].s, ai->model = v["ai"]["model"].s, ai->key = v["ai"]["key"].s, ai->notes = v["ai"]["notes"].s, ai->error = v["ai"]["error"].s;
        ai->cues = cuesFrom(v["ai"]["cues"]);
    }
    draft->clear();
    for (auto& [k, b] : v["draft"].o)
        if (k.size() == 1 && k[0] >= 'A' && k[0] <= 'H' && snapPos(*a, b.n)) (*draft)[k[0]] = snapPos(*a, b.n);
    *edited = v["edited"].b;
    return true;
}

// ---- audio ------------------------------------------------------------------------------------------------------

const double kPi = 3.14159265358979323846;

// Windowed-sinc resampling (Blackman, 16 zero crossings): plenty for band levels, and clean enough to listen to.
std::vector<float> resample(const float* x, size_t n, int from, int to) {
    if (from == to) return std::vector<float>(x, x + n);
    const int W = 16, RES = 512;
    double ratio = (double)from / to, fc = std::min(1.0, 1 / ratio) * 0.97, half = W / fc;
    static std::mutex tm;
    static std::map<std::pair<int, int>, std::vector<float>> tables;
    const std::vector<float>* tab;
    {
        std::lock_guard<std::mutex> l(tm);
        auto& t = tables[{from, to}];
        if (t.empty()) {
            int len = (int)std::ceil(half * RES) + 2;
            t.resize(len);
            for (int i = 0; i < len; i++) {
                double xx = (double)i / RES, s = xx == 0 ? 1 : std::sin(kPi * fc * xx) / (kPi * fc * xx), w = xx >= half ? 0 : 0.42 + 0.5 * std::cos(kPi * xx / half) + 0.08 * std::cos(2 * kPi * xx / half);
                t[i] = (float)(fc * s * w);
            }
        }
        tab = &t;
    }
    size_t m = (size_t)((double)n / ratio);
    std::vector<float> out(m);
    const std::vector<float>& T = *tab;
    for (size_t j = 0; j < m; j++) {
        double t = j * ratio;
        long lo = (long)std::ceil(t - half), hi = (long)std::floor(t + half);
        double acc = 0;
        for (long k = std::max(lo, 0L); k <= std::min(hi, (long)n - 1); k++) {
            double d = std::fabs(t - k) * RES;
            size_t i = (size_t)d;
            double f = d - i;
            if (i + 1 >= T.size()) continue;
            acc += x[k] * (T[i] + (T[i + 1] - T[i]) * f);
        }
        out[j] = (float)acc;
    }
    return out;
}

// The four stems of the whole track (44.1 kHz in), each kept at 22050 Hz mono: drums, bass, other, vocals. Asked for
// chunk by chunk exactly as rekordbox asks its separator (chunks of kStemOut samples with kStemCtx of context on both
// sides, zeros outside the track), so a chunk the stemcache mod has on disk (the track was on a deck, or separated
// from the stems window) is read from there, and only the others go through betterstems. needModel is called before
// the first chunk that has to be separated (one track separates at a time) and progress after every chunk; either
// returning false stops. Chunks it separates from the file's own samples (stereo) are stored for the decks too (a
// Spotify track's through "stemcache.storeSpotify": in memory in the public edition).
const int kStemOut = 519168, kStemCtx = 4096, kStemIn = kStemOut + 2 * kStemCtx;  // as stemcache's first engine

bool trackStems(const std::vector<float>& L, const std::vector<float>& R, bool stereo, bool spotify, std::vector<float> stems[4], int* fromDisk,
                const std::function<bool()>& needModel, const std::function<bool(double, bool)>& progress) {
    auto sep = service<bool (*)(const float*, const float*, int, float* const*)>("betterstems.separate");
    auto cached = service<bool (*)(const float*, const float*, int, float* const*)>("stemcache.cached");
    auto store = stereo ? service<void (*)(const float*, const float*, int, float* const*)>(spotify ? "stemcache.storeSpotify" : "stemcache.store") : nullptr;
    if (!sep || !stemsAvailable()) return false;
    const long n = (long)L.size();
    for (int s = 0; s < 4; s++) stems[s].clear(), stems[s].reserve(n / 2 + 1);
    std::vector<float> inL(kStemIn), inR(kStemIn), out((size_t)8 * kStemIn), m(kStemOut);
    bool model = false;
    *fromDisk = 0;
    for (long at = 0; at < n; at += kStemOut) {
        long from = at - kStemCtx;
        for (long i = 0; i < kStemIn; i++) {
            long k = from + i;
            inL[i] = k >= 0 && k < n ? L[k] : 0, inR[i] = k >= 0 && k < n ? R[k] : 0;
        }
        int len = (int)std::min<long>(kStemOut, n - at);
        float* o[8];
        const float* st[4][2];  // drums, bass, other, vocals (L, R)
        for (int k = 0; k < 8; k++) o[k] = out.data() + (size_t)k * kStemOut;
        if (cached && cached(inL.data(), inR.data(), kStemIn, o)) {  // rekordbox's slots: other, vocals, drums, bass
            static const int slot[4] = {2, 3, 0, 1};
            for (int s = 0; s < 4; s++) st[s][0] = o[2 * slot[s]], st[s][1] = o[2 * slot[s] + 1];
            ++*fromDisk;
        } else {
            if (!model && !needModel()) return false;
            model = true;
            for (int k = 0; k < 8; k++) o[k] = out.data() + (size_t)k * kStemIn;
            if (!sep(inL.data(), inR.data(), kStemIn, o)) return false;
            for (int s = 0; s < 4; s++) st[s][0] = o[2 * s] + kStemCtx, st[s][1] = o[2 * s + 1] + kStemCtx;
            if (store && len == kStemOut) {  // for the decks too (rekordbox's slots: other, vocals, drums, bass)
                float* rb[8] = {(float*)st[2][0], (float*)st[2][1], (float*)st[3][0], (float*)st[3][1], (float*)st[0][0], (float*)st[0][1], (float*)st[1][0], (float*)st[1][1]};
                store(inL.data(), inR.data(), kStemIn, rb);
            }
        }
        for (int s = 0; s < 4; s++) {
            for (int i = 0; i < len; i++) m[i] = (st[s][0][i] + st[s][1][i]) / 2;
            std::vector<float> r = resample(m.data(), len, 44100, sheet::SR);
            stems[s].insert(stems[s].end(), r.begin(), r.end());
        }
        if (!progress((double)(at + len) / n, model)) return false;
    }
    return true;
}

// ---- reading the tracks -----------------------------------------------------------------------------------------

std::atomic<int> g_readRun{0};
int g_workers;   // workers of the current run still going (g_lock)
bool g_started;  // Analyse was clicked for this playlist

bool live(int generation, int run) { return generation == g_generation && run == g_readRun; }

// The file to read a row's audio from, now: a Spotify track's local file may have been replaced since the row was
// made. Any thread.
std::string audioNow(const std::string& uri, const std::string& audio) {
    if (uri.compare(0, 8, "spotify:")) return audio;
    std::string now;
    return trackFile(uri.c_str(), &now) ? now : audio;
}

// A row's audio, 16-bit stereo: its file (a local track, or a Spotify track's local file), or
// else a Spotify track's stream, decoded into memory by the spotifyaudio mod. Not on the main thread.
typedef bool (*SpotifyReadFn)(const char* uri, std::shared_ptr<const std::vector<int16_t>>* stereo, int* rate, std::string* why);
bool readAudio(const std::string& uri, const std::string& path, std::vector<int16_t>* pcm, int* rate, std::string* why) {
    bool spotify = !uri.compare(0, 8, "spotify:");
    std::string file = audioNow(uri, path);
    if (!spotify || file != uri) {
        if (audiofile::readStereo16(file, pcm, rate) && !pcm->empty()) return true;
        file = audioNow(uri, file);  // (replaced just then: once more)
        if ((!spotify || file != uri) && audiofile::readStereo16(file, pcm, rate) && !pcm->empty()) return true;
        if (!spotify) return *why = "Couldn't read the audio", false;
    }
    auto read = service<SpotifyReadFn>("spotifyaudio.read");
    if (!read) return *why = "Spotify: needs the spotifyaudio mod", false;
    std::shared_ptr<const std::vector<int16_t>> stereo;
    if (!read(uri.c_str(), &stereo, rate, why)) return false;
    *pcm = *stereo;
    return !pcm->empty();
}

void unclaim(const std::string& uri) {  // a track given back unread (its run was stopped); g_lock held
    if (Row* r = rowOf(uri))
        if (r->busy) r->busy = false, r->state = "Not analysed", r->version++;
    g_dirty = true;
}

void queueAsk(const std::string& uri, bool force);

// The grid and source key of a row (reads the .DAT; any thread).
bool gridOf(const std::string& dat, std::vector<int>* ms, std::vector<int>* num) {
    std::string f;
    if (!plat::readFile(dat, &f)) return false;
    return readGrid(std::vector<uint8_t>(f.begin(), f.end()), ms, num);
}

// After an analysis (or a cached one) came in: the draft and the model's turn. g_lock held.
void analysed(Row& r, const std::string& source) {
    r.done = true, r.busy = false, r.force = false, r.state = "";
    // (a draft nobody moved follows the layout and the model setting of now, not those it was saved under)
    if (!r.edited) r.draft = defaultDraft(r);
    else r.edited = r.draft != defaultDraft(r);
    g_sourceOf[r.uri] = source;
    r.version++;
    g_dirty = true;
}

void readTrack(int generation, int run, const std::string& uri) {
    std::string path, dat;
    bool stems;
    {
        std::lock_guard<std::mutex> l(g_lock);
        Row* r = rowOf(uri);
        if (!live(generation, run) || !r) return unclaim(uri);
        path = r->audio, dat = r->dat, stems = g_useStems && stemsAvailable();
        r->state = "Reading", r->version++;
        g_dirty = true;
    }
    auto fail = [&](const std::string& why) {
        std::lock_guard<std::mutex> l(g_lock);
        Row* r = rowOf(uri);
        if (!live(generation, run) || !r) return unclaim(uri);
        r->state = why, r->done = true, r->busy = false, r->tick = false, r->version++;
        g_dirty = true;
    };
    std::vector<int> beatMs, beatNum;
    if (!gridOf(dat, &beatMs, &beatNum) || beatMs.size() < 8) return fail("No beat grid: analyse it in rekordbox first");
    std::string source = sourceKey(uri, path, beatMs, stems);
    {
        std::shared_ptr<const sheet::Analysis> an;
        Answer ai;
        std::map<char, double> draft;
        bool edited = false;
        if (loadCached(uri, source, sourceKey(uri, path, beatMs, stems, true), &an, &ai, &draft, &edited)) {
            std::lock_guard<std::mutex> l(g_lock);
            Row* r = rowOf(uri);
            if (!live(generation, run) || !r) return unclaim(uri);
            r->an = an, r->ai = ai, r->draft = draft, r->edited = edited;
            analysed(*r, source);
            if (t_keyBefore) save(*r, source);
            if (r->ai.status != "done") queueAsk(uri, false);
            return;
        }
    }
    std::vector<int16_t> pcm;
    int rate = 0;
    std::string whyNot;
    double t0 = now();
    if (!readAudio(uri, path, &pcm, &rate, &whyNot)) return fail(whyNot);
    double tAudio = now() - t0, tStems = 0, tWait = 0;
    size_t frames = pcm.size() / 2;
    std::vector<float> mono(frames);
    for (size_t i = 0; i < frames; i++) mono[i] = (pcm[2 * i] + pcm[2 * i + 1]) / 65536.f;
    std::vector<float> mono44 = resample(mono.data(), mono.size(), rate, 44100);
    std::vector<float>().swap(mono);
    double duration = mono44.size() / 44100.0;
    std::vector<float> st[4];
    bool haveStems = false;
    if (stems) {
        // The file's own samples when it is at 44.1 kHz (what rekordbox separated, if it did: stored chunks match);
        // any other rate has nothing stored, and its mono mix is separated.
        std::vector<float> L, R;
        if (rate == 44100) {
            L.resize(frames), R.resize(frames);
            for (size_t i = 0; i < frames; i++) L[i] = pcm[2 * i] / 32768.f, R[i] = pcm[2 * i + 1] / 32768.f;
        }
        std::vector<int16_t>().swap(pcm);
        // One track separates at a time (the model takes the whole GPU); the other workers wait here, stoppable.
        static std::mutex sepLock;
        std::unique_lock<std::mutex> sep(sepLock, std::defer_lock);
        bool stopped = false;
        int fromDisk = 0;
        auto setState = [&](const std::string& state) {
            std::lock_guard<std::mutex> l(g_lock);
            Row* r = rowOf(uri);
            if (!live(generation, run) || !r) return !(stopped = true);
            if (r->state != state) r->state = state, r->version++, g_dirty = true;
            return true;
        };
        double s0 = now();
        haveStems = trackStems(L.empty() ? mono44 : L, L.empty() ? mono44 : R, !L.empty(), !uri.compare(0, 8, "spotify:"), st, &fromDisk,
            [&] {
                if (!setState("Waiting for stems")) return false;
                double w0 = now();
                while (!sep.try_lock()) {
                    if (!live(generation, run)) return !(stopped = true);
                    std::this_thread::sleep_for(std::chrono::milliseconds(200));
                }
                tWait = now() - w0;
                return true;
            },
            [&](double done, bool model) { return setState((model ? "Separating " : "Reading stored stems ") + std::to_string((int)(done * 100)) + "%"); });
        if (stopped || !live(generation, run)) {
            std::lock_guard<std::mutex> l(g_lock);
            return unclaim(uri);
        }
        tStems = now() - s0 - tWait;
        if (haveStems) logf("autocue: %s: %d of %d stem chunks from the stem cache", uri.c_str(), fromDisk, (int)((duration * 44100 + kStemOut - 1) / kStemOut));
        if (!haveStems) source = sourceKey(uri, path, beatMs, false);  // (separation failed: the mix it is)
    }
    std::vector<int16_t>().swap(pcm);
    {
        std::lock_guard<std::mutex> l(g_lock);
        if (Row* r = rowOf(uri)) r->state = "Analysing", g_dirty = true;
    }
    std::vector<float> mix = resample(mono44.data(), mono44.size(), 44100, sheet::SR);
    std::vector<float>().swap(mono44);
    std::vector<double> bt;
    for (int ms : beatMs) bt.push_back(ms / 1000.0);
    double a0 = now();
    auto an = std::make_shared<sheet::Analysis>(sheet::analyse(mix, haveStems ? st : nullptr, bt, beatNum, duration));
    // where an analysis' time went (a slow one: the audio is a Spotify stream, or the stems weren't stored yet)
    logf("autocue: %s (%.0f s of audio): reading %.1f s, stems %.1f s%s, arrangement %.1f s", uri.c_str(), duration, tAudio, tStems,
         tWait >= 0.5 ? (" after waiting " + num(tWait, 1) + " s for another track's").c_str() : "", now() - a0);
    std::lock_guard<std::mutex> l(g_lock);
    Row* rp = rowOf(uri);
    if (!live(generation, run) || !rp) return unclaim(uri);
    Row& r = *rp;
    if (an->bars < 16) {
        r.state = "Too short", r.done = true, r.busy = false, r.tick = false, r.version++;
        g_dirty = true;
        return;
    }
    r.an = an, r.ai = Answer(), r.draft.clear(), r.edited = false;
    analysed(r, source);
    save(r, source);
    queueAsk(uri, false);
}

// Reads the ticked rows not done yet, several at once (autocue_parallel; stem separation itself one track at a time), in
// the list's order, until the playlist changes or Stop.
void startReading(int generation) {
    int run, n = std::clamp(cfgInt("autocue_parallel", 3), 1, 8);
    {
        std::lock_guard<std::mutex> l(g_lock);
        run = ++g_readRun;
        g_workers = n;
        for (Row& r : g_rows)
            if (!r.done && !r.busy && (r.tick || r.force)) r.state = "Waiting", r.version++;
        g_dirty = true;
    }
    for (int w = 0; w < n; w++)
        plat::background([generation, run] {
            plat::lowPriorityThread();
            for (;;) {
                std::string next;
                bool waiting = false;
                {
                    std::lock_guard<std::mutex> l(g_lock);
                    if (live(generation, run))
                        for (Row& r : g_rows) {
                            if (r.done || !(r.tick || r.force)) continue;
                            if (r.busy) { waiting = true; continue; }
                            r.busy = true, next = r.uri;
                            break;
                        }
                    if (next.empty() && (!waiting || !live(generation, run))) {
                        if (run == g_readRun) g_workers--;
                        g_dirty = true;
                        return;
                    }
                }
                if (next.empty()) { std::this_thread::sleep_for(std::chrono::milliseconds(200)); continue; }
                readTrack(generation, run, next);
            }
        });
}

void stopReading() {
    std::lock_guard<std::mutex> l(g_lock);
    ++g_readRun;
    g_workers = 0, g_started = false;
    for (Row& r : g_rows)
        if (!r.done && !r.busy) r.state = "Not analysed", r.version++;
    g_status = "Stopped.";
    g_dirty = true;
}

// A playlist track as a row: its collection entry, cues, and the audio to read (main thread).
Row makeRow(const std::string& uri, void* db) {
    Row r;
    r.uri = uri, r.audio = uri;
    bool spotify = !uri.compare(0, 8, "spotify:");
    alignas(16) char row[S_rowDataTrack + 0x100];
    at<void (*)(void*)>(A_rowCtor)(row);
    JStr path(uri.c_str());
    if (at<bool (*)(void*, const void*, void*)>(A_isCollectionSong)(db, &path, row)) {
        CollectionTrack t;
        collectionTrack(uri, &t);
        const char* dat = *(const char* const*)(row + O_rowAnalysisPath);
        r.id = t.id, r.title = t.title, r.artist = t.artist, r.dat = dat ? dat : "";
        r.existing = readCues(r.id);
        // (a Spotify track: its local file, else its stream, read into memory by spotifyaudio)
        if (spotify && !trackFile(uri.c_str(), &r.audio) && !provided("spotifyaudio.read"))
            r.state = trackFilesOn() ? trackFilesText("autocue.state") : "Spotify: needs the spotifyaudio mod";
        else r.usable = true;
    } else {
        r.state = spotify ? "Not in the collection: analyse the playlist first" : "Not in the collection";
    }
    if (!r.usable) r.done = true, r.tick = false;
    if (r.title.empty()) {
        library::Track lt;
        if (library::track(uri, &lt)) r.title = lt.title, r.artist = lt.artist;
    }
    at<void (*)(void*)>(A_rowDtor)(row);
    return r;
}

// Rows that were analysed before come back from the cache at once (no Analyse needed).
void loadCachedRows(int generation, std::vector<std::string> uris) {
    plat::background([generation, uris] {
        for (const std::string& uri : uris) {
            std::string dat, audio;
            {
                std::lock_guard<std::mutex> l(g_lock);
                Row* r = rowOf(uri);
                if (generation != g_generation || !r || !r->usable || r->done || r->busy) continue;
                dat = r->dat, audio = r->audio;
            }
            std::vector<int> ms, num;
            if (!gridOf(dat, &ms, &num)) continue;
            std::shared_ptr<const sheet::Analysis> an;
            Answer ai;
            std::map<char, double> draft;
            bool edited = false;
            bool stems = g_useStems && stemsAvailable();
            std::string source = sourceKey(uri, audio, ms, stems);
            bool ok = loadCached(uri, source, sourceKey(uri, audio, ms, stems, true), &an, &ai, &draft, &edited);
            if (!ok && stems)  // (separation had failed)
                ok = loadCached(uri, source = sourceKey(uri, audio, ms, false), sourceKey(uri, audio, ms, false, true), &an, &ai, &draft, &edited);
            if (!ok) continue;
            std::lock_guard<std::mutex> l(g_lock);
            Row* r = rowOf(uri);
            if (generation != g_generation || !r || r->done || r->busy) continue;
            r->an = an, r->ai = ai, r->draft = draft, r->edited = edited;
            analysed(*r, source);
            if (t_keyBefore) save(*r, source);
        }
    });
}

// The playlist's rows again: tracks rekordbox has listed since come in, tracks that couldn't be read are looked at
// again when `recheck` (a local file came meanwhile), and the readable ones go to the top (main thread).
void refreshRows(int generation, const std::string& id, bool recheck) {
    PlaylistTracks pt = playlistTracks(id);
    void* db = database();
    std::vector<std::string> fresh;
    {
        std::lock_guard<std::mutex> l(g_lock);
        if (generation != g_generation) return;
        std::map<std::string, int> have;
        for (const Row& r : g_rows) have[r.uri]++;
        for (const std::string& uri : pt.uris) {
            auto it = have.find(uri);
            if (it != have.end() && it->second > 0) it->second--;
            else fresh.push_back(uri);
        }
        std::string file;
        if (recheck && trackFilesOn())
            for (const Row& r : g_rows)
                if (!r.usable && r.id && trackFile(r.uri.c_str(), &file)) fresh.push_back(r.uri);
    }
    std::vector<Row> made;  // (outside the lock: the database calls take a while on big lists)
    for (const std::string& uri : fresh) made.push_back(makeRow(uri, db));
    std::vector<std::string> toLoad;
    bool read = false;  // new rows for a run that has gone idle: startReading takes g_lock itself, so after it is released
    {
    std::lock_guard<std::mutex> l(g_lock);
    if (generation != g_generation) return;
    size_t before = g_rows.size();
    int nowUsable = 0;
    for (Row& m : made) {
        Row* old = recheck ? rowOf(m.uri) : nullptr;
        if (old && !old->usable) {
            if (m.usable) toLoad.push_back(m.uri), *old = std::move(m), nowUsable++;
        } else if (!old) {
            if (m.usable) toLoad.push_back(m.uri);
            g_rows.push_back(std::move(m));
        }
    }
    std::stable_sort(g_rows.begin(), g_rows.end(), [](const Row& a, const Row& b) { return a.usable > b.usable; });
    std::string status = pt.known ? (g_rows.empty() ? "No tracks in this playlist" : "") : "rekordbox hasn't listed this playlist yet";
    bool listing = g_status.empty() || g_status == "No tracks in this playlist" || g_status == "rekordbox hasn't listed this playlist yet";
    if (listing && status != g_status) g_status = status, g_dirty = true;
    if (g_selected.empty() && !g_rows.empty()) g_selected = g_rows[0].uri;
    if (!toLoad.empty()) loadCachedRows(generation, toLoad);
    if (g_rows.size() != before || nowUsable) {
        g_dirty = true;
        read = g_started && !g_workers;
    }
    }
    if (read) startReading(generation);
}

void pollRows(const std::string& id, int generation, int n) {
    if (generation != g_generation || !g_win || (n > 2 && !web::visible(g_win))) return;
    refreshRows(generation, id, n % 5 == 4);
    plat::onMainAfter(2, [id, generation, n] { pollRows(id, generation, n + 1); });
}

void openPlaylistId(const std::string& id, const std::string& name) {
    int generation = ++g_generation;
    {
        std::lock_guard<std::mutex> l(g_lock);
        g_rows.clear();
        g_playlistName = name;
        g_status = "", g_selected = "", g_started = false, g_workers = 0, g_dirty = true;
        g_sentVersion = -1;
        ++g_readRun;
    }
    plat::onMainAfter(0.5, [id, generation] { pollRows(id, generation, 0); });
}

// ---- the language model -----------------------------------------------------------------------------------------

std::deque<std::pair<std::string, bool>> g_askQueue;  // uri, force (ask even if this sheet was answered)
int g_asking;
const int kAskParallel = 4;

void pumpAsks();

// ---- Claude Code as the model: the CLI's print mode, with nothing of the user's setup loaded and no tools -------------

const char* const kClaudeQuiet[] = {"--safe-mode", "--strict-mcp-config", "--no-session-persistence", "--tools", ""};

// The claude program: the config's, the usual install places, else wherever the shell finds it.
std::string findClaude() {
    std::string set = cfgStr("autocue_claude_path", "");
    if (!set.empty()) return plat::fileInfo(set, nullptr) ? set : "";
    std::string home = plat::homeDir();
    std::vector<std::string> places;
#ifdef _WIN32
    const char* appdata = getenv("APPDATA");
    const char* local = getenv("LOCALAPPDATA");
    places = {home + "/.local/bin/claude.exe", home + "/.claude/local/claude.exe"};
    if (local) places.push_back(std::string(local) + "/Programs/claude/claude.exe");
    if (appdata) places.push_back(std::string(appdata) + "/npm/claude.cmd");
#else
    places = {home + "/.local/bin/claude", home + "/.claude/local/claude", "/opt/homebrew/bin/claude", "/usr/local/bin/claude", home + "/.npm-global/bin/claude"};
#endif
    for (const std::string& p : places)
        if (plat::fileInfo(p, nullptr)) return p;
    std::string out;
#ifdef _WIN32
    const char* root = getenv("SystemRoot");
    plat::run({std::string(root ? root : "C:/Windows") + "/System32/where.exe", "claude"}, "", "", &out, 10);
    std::string best;  // (an .exe before a .cmd; the extensionless npm shim isn't runnable)
    for (size_t a = 0; a < out.size();) {
        size_t b = out.find('\n', a);
        if (b == std::string::npos) b = out.size();
        std::string line = out.substr(a, b - a);
        while (!line.empty() && isspace((unsigned char)line.back())) line.pop_back();
        std::string ext = line.size() > 4 ? line.substr(line.size() - 4) : "";
        for (char& c : ext) c = (char)tolower((unsigned char)c);
        if (ext == ".exe") return line;
        if (ext == ".cmd" && best.empty()) best = line;
        a = b + 1;
    }
    return best;
#else
    // rekordbox wasn't started from a shell: ask a login shell for its PATH's claude
    plat::run({"/bin/zsh", "-lc", "command -v claude"}, "", "", &out, 10);
    while (!out.empty() && isspace((unsigned char)out.back())) out.pop_back();
    size_t nl = out.rfind('\n');
    if (nl != std::string::npos) out = out.substr(nl + 1);
    return !out.empty() && out[0] == '/' && plat::fileInfo(out, nullptr) ? out : "";
#endif
}

std::string claudeDir() {  // an empty folder to run it in: no project of the user's is read
    std::string d = dataDir() + "/claude";
    std::error_code ec;
    std::filesystem::create_directories(plat::fsPath(d), ec);
    return d;
}

// Finds the CLI and asks it (the SDK's initialize request) which models this account can use.
void checkClaude(bool again) {
    {
        std::lock_guard<std::mutex> l(g_lock);
        if (g_claude.looked && !again) return;
        g_claude.looked = true;
    }
    plat::background([] {
        Claude c;
        c.looked = true;
        c.path = findClaude();
        if (c.path.empty()) c.error = "Claude Code wasn't found on this computer";
        else {
            std::vector<std::string> argv = {c.path, "-p", "--input-format", "stream-json", "--output-format", "stream-json", "--verbose"};
            argv.insert(argv.end(), std::begin(kClaudeQuiet), std::end(kClaudeQuiet));
            std::string out;
            int code = plat::run(argv, claudeDir(), "{\"type\":\"control_request\",\"request_id\":\"models\",\"request\":{\"subtype\":\"initialize\"}}\n", &out, 60);
            for (size_t a = 0; a < out.size();) {  // one JSON object per line
                size_t b = out.find('\n', a);
                if (b == std::string::npos) b = out.size();
                jsonmini::Value v;
                if (jsonmini::parse(out.substr(a, b - a), &v) && v["type"].s == "control_response") {
                    const jsonmini::Value& r = v["response"]["response"];
                    c.version = r["claude_code_version"].s;
                    c.account = r["account"]["subscriptionType"].s;
                    if (c.account.empty() && !r["account"]["apiProvider"].s.empty()) c.account = "API";
                    std::string j = "[";
                    for (const jsonmini::Value& m : r["models"].a)
                        if (!m["value"].s.empty())
                            j += std::string(j.size() > 1 ? "," : "") + "{\"id\":" + web::jsonStr(m["value"].s) + ",\"name\":" + web::jsonStr(m["displayName"].s.empty() ? m["value"].s : m["displayName"].s) +
                                 ",\"price\":" + web::jsonStr(m["description"].s) + ",\"free\":false}";
                    if (j.size() > 1) c.models = j + "]";
                }
                a = b + 1;
            }
            if (c.models.empty()) {
                c.error = code == -1 ? "Claude Code didn't answer" : "Claude Code gave no model list: is it up to date and logged in? (run claude in a terminal)";
                logf("autocue: claude (%s) initialize: exit %d, %.300s", c.path.c_str(), code, out.c_str());
            } else if (c.account.empty()) c.error = "Claude Code isn't logged in: run claude in a terminal and /login";
            else c.ok = true;
        }
        logf("autocue: Claude Code %s%s%s", c.path.empty() ? "not found" : c.path.c_str(), c.version.empty() ? "" : (" " + c.version).c_str(), c.error.empty() ? "" : (": " + c.error).c_str());
        std::lock_guard<std::mutex> l(g_lock);
        g_claude = c;
        g_dirty = true;
        if (c.ok && useClaude())  // tracks that came in before it was found
            for (Row& r : g_rows)
                if (r.an && r.ai.status == "nokey") queueAsk(r.uri, false);
    });
}

// One answer from `claude -p`: the instructions and the sheet go in as the message (on its standard input, so
// nothing long or odd is on the command line), the answer's text comes back.
bool askClaude(const std::string& m, const std::string& system, const std::string& sheetText, std::string* text, std::string* err, Answer* ans) {
    std::string path;
    {
        std::lock_guard<std::mutex> l(g_lock);
        path = g_claude.path;
        if (!g_claude.ok) { *err = g_claude.error.empty() ? "Claude Code isn't ready" : g_claude.error; return false; }
    }
    std::vector<std::string> argv = {path, "-p", "--model", m, "--output-format", "json", "--system-prompt",
                                     "You place DJ cue points from a written description of a track. Follow the instructions in the message and answer with the JSON object only."};
    argv.insert(argv.end(), std::begin(kClaudeQuiet), std::end(kClaudeQuiet));
    std::string out;
    int code = plat::run(argv, claudeDir(), system + "\n\nThe track's sheet:\n\n" + sheetText, &out, 900);
    jsonmini::Value v;
    size_t a = out.find('{');
    if (a == std::string::npos || !jsonmini::parse(out.substr(a), &v)) {
        *err = code == -1 ? "Claude Code took too long or couldn't be started" : "Claude Code gave no answer (exit " + std::to_string(code) + ")";
        logf("autocue: claude %s: exit %d: %.300s", m.c_str(), code, out.c_str());
        return false;
    }
    if (v["is_error"].b || v["result"].s.empty()) {  // not logged in, a usage limit, an unknown model...
        *err = v["result"].s.empty() ? "Claude Code: " + (v["subtype"].s.empty() ? "no answer" : v["subtype"].s) : web::utf8Prefix(v["result"].s, 200);
        logf("autocue: claude %s: %.300s", m.c_str(), out.c_str());
        return false;
    }
    *text = v["result"].s;
    for (auto& kv : v["modelUsage"].o) ans->model = kv.first;  // the full name of the alias
    return true;
}

// The model's cues for a sheet (autocue.py's llm_cues): the sheet alone is sent, no title or audio.
void askModel(int generation, const std::string& uri, bool force) {
    std::string sheetText, m = model(), system, key;
    bool claude = useClaude();
    int bars = 0;
    std::shared_ptr<const sheet::Analysis> asked;  // the analysis this answer is for
    decltype(Row::ai) before;                       // the answer the track had
    {
        std::lock_guard<std::mutex> l(g_lock);
        Row* r = rowOf(uri);
        if (generation != g_generation || !r || !r->an || noModel()) { g_asking--; return pumpAsks(); }
        before = r->ai;
        asked = r->an;
        sheetText = r->an->sheet, bars = r->an->bars;
        system = sheet::systemPrompt(r->an->stems, g_instructions);
        key = hex(fnv((claude ? "claude|" : "") + m + "|" + system + "|" + sheetText));
        if (!force && r->ai.status == "done" && r->ai.key == key) { g_asking--; return pumpAsks(); }
        r->ai.status = "asking", r->version++, g_dirty = true;
    }
    std::string bearer;
    {
        std::lock_guard<std::mutex> l(g_keyLock);
        bearer = g_key;
    }
    Answer ans;
    ans.model = m, ans.key = key;
    if (!claude && bearer.empty()) ans.status = "nokey";
    else {
        jsonmini::Value out, resp;
        std::string err;
        if (claude) {
            std::string text;
            if (askClaude(m, system, sheetText, &text, &err, &ans)) {
                size_t a = text.find('{'), b = text.rfind('}');
                jsonmini::Value v;
                if (a == std::string::npos || b == std::string::npos || b < a || !jsonmini::parse(text.substr(a, b - a + 1), &v) || v["cues"].a.empty()) {
                    err = "no cues in the answer";
                    logf("autocue: claude %s: %s: %.300s", m.c_str(), err.c_str(), text.c_str());
                } else out = v, ans.notes = v["notes"].s;
            }
        }
        for (int thinking = 1; !claude && thinking >= 0 && out.type != jsonmini::Value::Obj; thinking--) {
            std::string req = "{\"model\":" + web::jsonStr(m) + ",\"temperature\":0,\"max_tokens\":48000,\"messages\":[{\"role\":\"system\",\"content\":" + web::jsonStr(system) +
                              "},{\"role\":\"user\",\"content\":" + web::jsonStr(sheetText) + "}]" + (thinking ? "" : ",\"reasoning\":{\"enabled\":false}") + "}";
            std::string body;
            int http = 0;
            for (int attempt = 0; attempt < 4; attempt++) {  // rate limits and provider hiccups
                body.clear();
                http = plat::httpPostJson("https://openrouter.ai/api/v1/chat/completions", bearer, req, &body, 600);
                if (http == 200 || (http && http != 408 && http != 429 && http < 500)) break;
                if (generation != g_generation) break;
                std::this_thread::sleep_for(std::chrono::seconds(5 * (attempt + 1)));
            }
            if (http != 200) {
                jsonmini::Value e;
                std::string why;
                if (jsonmini::parse(body, &e)) why = !e["error"]["metadata"]["raw"].s.empty() ? e["error"]["metadata"]["raw"].s : e["error"]["message"].s;
                if (why.size() > 200) why = web::utf8Prefix(why, 200) + "…";
                logf("autocue: OpenRouter %d for %s: %.400s", http, m.c_str(), body.c_str());
                err = http == 401 ? "OpenRouter didn't accept the key" : http == 402 ? "no credit left on the OpenRouter account" : http == 429 ? "the model is busy or over its limit" :
                      !http ? "couldn't reach OpenRouter" : why.empty() ? "OpenRouter error " + std::to_string(http) : why;
                break;
            }
            if (!jsonmini::parse(body, &resp) || resp["choices"].a.empty()) { err = "no answer"; continue; }
            const jsonmini::Value& ch = resp["choices"][(size_t)0];
            std::string text = ch["message"]["content"].s;
            size_t a = text.find('{'), b = text.rfind('}');
            jsonmini::Value v;
            if (a == std::string::npos || b == std::string::npos || b < a || !jsonmini::parse(text.substr(a, b - a + 1), &v) || v["cues"].a.empty()) {
                // the model thought until its allowance ran out, or rambled: take a direct answer instead
                err = ch["finish_reason"].s == "length" ? "ran out of tokens while thinking" : "no cues in the answer";
                logf("autocue: %s: %s: %.300s", m.c_str(), err.c_str(), text.c_str());
                continue;
            }
            out = v;
            ans.notes = (thinking ? "" : "(answered without thinking: " + err + ") ") + v["notes"].s;
            if (!resp["model"].s.empty()) ans.model = resp["model"].s;
            ans.cost = resp["usage"]["cost"].n;
        }
        if (out.type == jsonmini::Value::Obj) {
            for (const jsonmini::Value& c : out["cues"].a) {
                std::string s = c["slot"].s;
                int bar = c["bar"].type == jsonmini::Value::Num ? (int)c["bar"].n : atoi(c["bar"].s.c_str());
                if (s.empty()) continue;
                char slot = (char)toupper((unsigned char)s[0]);
                if (slot < 'A' || slot > 'H' || bar < 1 || bar > bars) continue;
                if (std::any_of(ans.cues.begin(), ans.cues.end(), [&](const Cue& x) { return x.slot == slot; })) continue;
                ans.cues.push_back({slot, bar, web::utf8Prefix(c["why"].s, 160)});
            }
            auto barOf = [&](char s) {
                for (const Cue& c : ans.cues)
                    if (c.slot == s) return c.bar;
                return -1;
            };
            const std::pair<char, char> buildDrop[] = {{'B', 'C'}, {'E', 'F'}};
            for (auto [build, drop] : buildDrop)  // a build belongs before its drop
                if (barOf(build) > 0 && barOf(drop) > 0 && barOf(build) >= barOf(drop))
                    ans.cues.erase(std::remove_if(ans.cues.begin(), ans.cues.end(), [&](const Cue& c) { return c.slot == build; }), ans.cues.end());
            std::sort(ans.cues.begin(), ans.cues.end(), [](const Cue& a, const Cue& b) { return a.slot < b.slot; });
            if (ans.notes.size() > 300) ans.notes = web::utf8Prefix(ans.notes, 300);
        }
        ans.status = ans.cues.empty() ? "failed" : "done";
        if (ans.cues.empty()) ans.error = err.empty() ? "no usable cues in the answer" : err;
    }
    std::lock_guard<std::mutex> l(g_lock);
    g_asking--;
    Row* r = rowOf(uri);
    // (analysed again meanwhile: this answer is about the sheet before, and the new one has been queued)
    if (generation == g_generation && r && r->an == asked) {
        if (!force && ans.status != "done" && before.status == "done") {
            // asked again by itself (new instructions) and it failed: the answer there was is better than none
            logf("autocue: asking again about %s failed (%s), keeping the answer it had", uri.c_str(), ans.error.c_str());
            r->ai = before;
        } else {
            r->ai = ans;
        }
        if (!r->edited) r->draft = defaultDraft(*r);
        r->version++;
        saveRow(*r);
        if (ans.status == "nokey") g_hasKey = false;
        if (!claude) g_cost += ans.cost;
        g_dirty = true;
    }
    pumpAsks();
}

void pumpAsks() {  // g_lock held
    while (g_asking < kAskParallel && !g_askQueue.empty()) {
        auto [uri, force] = g_askQueue.front();
        g_askQueue.pop_front();
        g_asking++;
        int generation = g_generation;
        plat::background([generation, uri = uri, force = force] { askModel(generation, uri, force); });
    }
}

void queueAsk(const std::string& uri, bool force) {  // g_lock held
    Row* r = rowOf(uri);
    if (!r || !r->an) return;
    if (!canAsk()) { r->ai.status = r->ai.status == "done" ? r->ai.status : "nokey"; return; }
    for (auto& q : g_askQueue)
        if (q.first == uri) { q.second |= force; return; }
    g_askQueue.push_back({uri, force});
    // (a "done" answer stays as it is until askModel has seen whether it still fits: same sheet, model and
    // instructions are answered from it, and marking it queued here made that never happen)
    if (force || r->ai.status != "done") r->ai.status = "queued", r->version++;
    pumpAsks();
}

// ---- writing --------------------------------------------------------------------------------------------------

// What Write cues does on a row: the cues to set, the ones in rekordbox that go first, and what stays out.
struct Plan {
    struct Item { char slot; int kind, ms; };
    std::vector<Item> write;
    std::vector<CueCopy> remove;
    int kept = 0, noRoom = 0;  // pads that hold another cue, left alone; memory cues past the ten a track holds
};
Plan planOf(const Row& r) {  // g_lock held
    Plan p;
    if (!r.an || r.id == 0) return p;
    std::vector<std::pair<int, char>> memory;  // ms, cue: the memory cues the draft wants, in the track's order
    for (auto& [slot, bar] : r.draft) {
        int ms = barMs(*r.an, bar);
        if (ms < 0) continue;  // a bar the track doesn't have
        int st = padState(r, slot, bar);
        if (st == 2 && !g_replace) p.kept++;
        else if (st == 0 || st == 2) {
            if (st == 2)
                for (const CueCopy& c : r.existing)
                    if (c.kind == kindOf(padOf(slot))) p.remove.push_back(c);
            p.write.push_back({slot, kindOf(padOf(slot)), ms});
        }
        if (memOf(slot)) memory.push_back({ms, slot});
    }
    std::sort(memory.begin(), memory.end());
    // (two cues on one spot, e.g. a build cue moved onto the breakdown: one memory cue)
    memory.erase(std::unique(memory.begin(), memory.end(), [](auto& a, auto& b) { return abs(a.first - b.first) < 100; }), memory.end());
    int held = 0;  // memory cues that stay
    for (const CueCopy& c : r.existing) {
        if (c.kind != kMemoryKind) continue;
        bool wanted = std::any_of(memory.begin(), memory.end(), [&](auto& m) { return abs(m.first - c.inMs) < 100; });
        if (g_replace && !memory.empty() && !wanted) p.remove.push_back(c);  // Replace: the track's memory cues become these
        else held++;
    }
    for (auto& [ms, slot] : memory) {
        bool there = std::any_of(r.existing.begin(), r.existing.end(), [&, ms = ms](const CueCopy& c) { return c.kind == kMemoryKind && abs(c.inMs - ms) < 100; });
        if (there) continue;
        if (held >= kMemoryMax) { p.noRoom++; continue; }
        held++;
        p.write.push_back({slot, kMemoryKind, ms});
    }
    if (p.write.empty()) p.remove.clear();  // (nothing new: nothing goes either)
    return p;
}

int writable(const Row& r) {  // cues Write would set on this row
    return r.tick ? (int)planOf(r).write.size() : 0;
}

void applyCues() {
    std::lock_guard<std::mutex> l(g_lock);
    g_undo.clear();
    int written = 0, memory = 0, tracks = 0, kept = 0, noRoom = 0;
    for (Row& r : g_rows) {
        if (!r.tick) continue;
        Plan p = planOf(r);
        kept += p.kept, noRoom += p.noRoom;
        if (p.write.empty()) continue;
        Undo u{r.id, {}, {}};
        for (const CueCopy& c : p.remove) deleteCue(r.id, c.id), u.removed.push_back(c);
        for (const Plan::Item& w : p.write) {
            // (a memory cue has no pad colour: rekordbox's own for memory cues)
            unsigned id = writeCue(r.id, w.kind, w.ms, slotName(w.slot), w.kind == kMemoryKind ? 0 : kColors[w.slot - 'A']);
            if (id) u.added.push_back(id), written++, memory += w.kind == kMemoryKind;
        }
        if (!u.added.empty()) tracks++;
        if (!u.added.empty() || !u.removed.empty()) g_undo.push_back(std::move(u));
        r.existing = readCues(r.id);
        r.version++;
    }
    g_status = "Wrote " + std::to_string(written) + " cue" + (written == 1 ? "" : "s") + (memory ? " (" + std::to_string(memory) + " as memory cues)" : "") + " to " + std::to_string(tracks) + " track" +
               (tracks == 1 ? "" : "s") + "." +
               (kept ? " " + std::to_string(kept) + " pad" + (kept == 1 ? "" : "s") + " already held another cue (tick Replace existing cues to overwrite)." : "") +
               (noRoom ? " " + std::to_string(noRoom) + " memory cue" + (noRoom == 1 ? "" : "s") + " didn't fit (a track holds " + std::to_string(kMemoryMax) + ")." : "");
    logf("autocue: %s", g_status.c_str());
    g_dirty = true;
}

void removeCues() {  // every hot and memory cue on the ticked tracks; Undo puts them back
    std::lock_guard<std::mutex> l(g_lock);
    g_undo.clear();
    int removed = 0, tracks = 0;
    for (Row& r : g_rows) {
        if (!r.tick || !r.id) continue;
        Undo u{r.id, {}, readCues(r.id)};
        for (const CueCopy& c : u.removed) deleteCue(r.id, c.id), removed++;
        if (!u.removed.empty()) tracks++, g_undo.push_back(std::move(u));
        r.existing = readCues(r.id);
        r.version++;
    }
    g_status = "Removed " + std::to_string(removed) + " cue" + (removed == 1 ? "" : "s") + " from " + std::to_string(tracks) + " track" + (tracks == 1 ? "" : "s") + ".";
    logf("autocue: %s", g_status.c_str());
    g_dirty = true;
}

void undoCues() {
    std::lock_guard<std::mutex> l(g_lock);
    for (const Undo& u : g_undo) {
        for (unsigned id : u.added) deleteCue(u.track, id);
        for (const CueCopy& c : u.removed) writeCue(u.track, c.kind, c.inMs, c.comment, c.color, &c);
    }
    for (Row& r : g_rows)
        if (r.id) r.existing = readCues(r.id), r.version++;
    g_status = "Undone.";
    g_undo.clear();
    g_dirty = true;
}

// ---- listening: a stretch of the track from a bar, as a WAV the page plays -------------------------------------

std::string base64(const std::string& in) {
    static const char* t = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string o;
    o.reserve((in.size() + 2) / 3 * 4);
    for (size_t i = 0; i < in.size(); i += 3) {
        uint32_t v = (uint8_t)in[i] << 16 | (i + 1 < in.size() ? (uint8_t)in[i + 1] << 8 : 0) | (i + 2 < in.size() ? (uint8_t)in[i + 2] : 0);
        o += t[v >> 18 & 63], o += t[v >> 12 & 63];
        o += i + 1 < in.size() ? t[v >> 6 & 63] : '=';
        o += i + 2 < in.size() ? t[v & 63] : '=';
    }
    return o;
}

std::mutex g_audioLock;
std::string g_audioUri;
std::vector<float> g_audio;  // the last listened-to track at 24 kHz mono
const int kListenRate = 24000;

void listen(const std::string& uri, int ms, int seq) {
    std::string path;
    {
        std::lock_guard<std::mutex> l(g_lock);
        Row* r = rowOf(uri);
        if (!r || !r->usable) return;
        path = r->audio;
    }
    plat::background([uri, ms, seq, path] {
        std::lock_guard<std::mutex> al(g_audioLock);
        if (g_audioUri != uri) {
            std::vector<int16_t> pcm;
            int rate = 0;
            std::string why;
            g_audioUri.clear(), g_audio.clear();
            if (!readAudio(uri, path, &pcm, &rate, &why)) return logf("autocue: couldn't read %s to listen to: %s", uri.c_str(), why.c_str());
            std::vector<float> mono(pcm.size() / 2);
            for (size_t i = 0; i < mono.size(); i++) mono[i] = (pcm[2 * i] + pcm[2 * i + 1]) / 65536.f;
            std::vector<int16_t>().swap(pcm);
            g_audio = resample(mono.data(), mono.size(), rate, kListenRate);
            g_audioUri = uri;
        }
        size_t a = (size_t)std::max(0LL, (long long)ms * kListenRate / 1000), n = std::min(g_audio.size() - std::min(a, g_audio.size()), (size_t)kListenRate * 20);
        std::string wav(44 + n * 2, '\0');
        auto put32 = [&](size_t p, uint32_t v) { memcpy(&wav[p], &v, 4); };
        auto put16 = [&](size_t p, uint16_t v) { memcpy(&wav[p], &v, 2); };
        memcpy(&wav[0], "RIFF", 4), put32(4, (uint32_t)(36 + n * 2)), memcpy(&wav[8], "WAVEfmt ", 8), put32(16, 16), put16(20, 1), put16(22, 1);
        put32(24, kListenRate), put32(28, kListenRate * 2), put16(32, 2), put16(34, 16), memcpy(&wav[36], "data", 4), put32(40, (uint32_t)(n * 2));
        for (size_t i = 0; i < n; i++) {
            int v = (int)std::lround(std::clamp(g_audio[a + i], -1.f, 1.f) * 32767);
            put16(44 + i * 2, (uint16_t)(int16_t)v);
        }
        std::string j = "{\"audio\":{\"u\":" + web::jsonStr(uri) + ",\"ms\":" + std::to_string(ms) + ",\"seq\":" + std::to_string(seq) + ",\"data\":\"" + base64(wav) + "\"}}";
        plat::onMain([j] {
            if (g_win) web::post(g_win, j);
        });
    });
}

// ---- the page ---------------------------------------------------------------------------------------------------

const char* dotOf(const Row& r) {
    if (r.busy || r.ai.status == "asking") return "analysing";
    if (!r.usable) return "unusable";
    if (!r.done) return "waiting";
    if (!r.an) return "failed";
    if (r.ai.status == "failed") return "retry";
    return "done";
}

std::string detailJson(const Row& r) {
    using web::jsonStr;
    std::string j = "{\"u\":" + jsonStr(r.uri) + ",\"title\":" + jsonStr(r.title) + ",\"artist\":" + jsonStr(r.artist) + ",\"state\":" + jsonStr(r.state) + ",\"ack\":" + std::to_string(r.cueAck);
    j += ",\"an\":" + (r.an ? analysisJson(*r.an) : std::string("null"));
    j += ",\"ai\":{\"status\":" + jsonStr(r.ai.status) + ",\"model\":" + jsonStr(r.ai.model) + ",\"notes\":" + jsonStr(r.ai.notes) + ",\"error\":" + jsonStr(r.ai.error) + ",\"cues\":" + cuesJson(modelCues(r)) + "}";
    j += ",\"rules\":" + cuesJson(rulesCues(r));  // (both as the layout's options place them)
    j += std::string(",\"edited\":") + (r.edited ? "true" : "false") + ",\"draft\":[";
    bool first = true;
    for (auto& [s, b] : r.draft)
        j += std::string(first ? "" : ",") + "{\"s\":\"" + s + "\",\"b\":" + posStr(b) + ",\"p\":" + std::to_string(padState(r, s, b)) + ",\"m\":" + std::to_string(memState(r, s, b)) + "}", first = false;
    j += "],\"existing\":[";
    first = true;
    for (const CueCopy& c : r.existing) j += std::string(first ? "" : ",") + "{\"s\":\"" + slotOfKind(c.kind) + "\",\"ms\":" + std::to_string(c.inMs) + ",\"c\":" + jsonStr(c.comment) + "}", first = false;
    return j + "]}";
}

void sendState() {
    if (!g_win || !web::visible(g_win)) return;
    using web::jsonStr;
    auto b = [](bool x) { return x ? "true" : "false"; };
    std::lock_guard<std::mutex> l(g_lock);
    int usable = 0, toAnalyse = 0, done = 0, toWrite = 0, tracksToWrite = 0, asking = g_asking + (int)g_askQueue.size();
    bool claude = useClaude();
    const std::string& models = claude ? g_claude.models : g_models;
    std::string rows = "[";
    const Row* sel = nullptr;
    for (const Row& r : g_rows) {
        usable += r.usable, done += r.usable && r.done, toAnalyse += r.usable && !r.done && !r.busy && r.tick;
        int w = writable(r);
        toWrite += w, tracksToWrite += w > 0;
        if (r.uri == g_selected) sel = &r;
        // per row: the draft's cues the layout writes, with their state (lower case: in rekordbox already; '!' after: pad taken)
        std::string cues;
        bool allSet = false, any = false;
        for (auto& [s, bar] : r.draft) {
            int p = padState(r, s, bar), m = memState(r, s, bar);
            if (p < 0 && m < 0) continue;  // off in the layout
            bool set = p != 0 && p != 2 && m != 0;
            allSet = any ? allSet && set : set, any = true;
            cues += set ? (char)tolower(s) : s;
            if (p == 2) cues += '!';
        }
        std::string state = r.state;
        if (r.done && r.an) state = r.ai.status == "asking" ? "Asking the model" : r.ai.status == "queued" ? "Waiting for the model" : r.ai.status == "failed" ? "Model failed: rule cues" : allSet ? "In rekordbox" : r.edited ? "Edited" : "";
        rows += std::string(rows.size() > 1 ? "," : "") + "{\"u\":" + jsonStr(r.uri) + ",\"t\":" + jsonStr(r.title.empty() ? r.uri : r.title) + ",\"a\":" + jsonStr(r.artist) + ",\"st\":" + jsonStr(state) +
                ",\"dot\":\"" + dotOf(r) + "\",\"tick\":" + b(r.tick) + ",\"usable\":" + b(r.usable) + ",\"cues\":" + jsonStr(cues) + ",\"bpm\":" + (r.an ? num(r.an->bpm, 1) : "0") + "}";
    }
    rows += "]";
    std::string j = "{\"name\":" + jsonStr(g_playlistName) + ",\"rows\":" + rows + ",\"status\":" + jsonStr(g_status) + ",\"reading\":" + b(g_workers > 0) +
                    ",\"usable\":" + std::to_string(usable) + ",\"toAnalyse\":" + std::to_string(toAnalyse) + ",\"done\":" + std::to_string(done) + ",\"asking\":" + std::to_string(asking) + ",\"toWrite\":" + std::to_string(toWrite) +
                    ",\"tracksToWrite\":" + std::to_string(tracksToWrite) + ",\"canUndo\":" + b(!g_undo.empty()) + ",\"hasKey\":" + b(g_hasKey) + ",\"canAsk\":" + b(canAsk()) + ",\"provider\":\"" + (noModel() ? "none" : claude ? "claude" : "openrouter") +
                    "\",\"claude\":{\"looked\":" + b(g_claude.looked) + ",\"ok\":" + b(g_claude.ok) + ",\"path\":" + jsonStr(g_claude.path) + ",\"version\":" + jsonStr(g_claude.version) +
                    ",\"account\":" + jsonStr(g_claude.account) + ",\"error\":" + jsonStr(g_claude.error) + "},\"model\":" + jsonStr(model()) +
                    ",\"models\":" + (models.empty() ? std::string("[]") : models) + ",\"useStems\":" + b(g_useStems) + ",\"stemsAvailable\":" + b(stemsAvailable()) +
                    ",\"replace\":" + b(g_replace) + ",\"layout\":{\"pads\":\"" + padsStr(g_layout) + "\",\"mem\":\"" + memStr(g_layout) + "\",\"build\":" + std::to_string(g_layout.buildBars) + ",\"intro\":\"" + kIntro[g_layout.intro] + "\",\"outro\":" + std::to_string(g_layout.outroBars) +
                    "},\"instructions\":" + jsonStr(g_instructions) + ",\"cost\":" + num(g_cost, 4) + ",\"sel\":" + jsonStr(g_selected);
    // the selected track in full, when it changed
    int version = sel ? sel->version : -2;
    if (sel && (version != g_sentVersion || g_selected != g_sentSelected)) j += ",\"detail\":" + detailJson(*sel);
    g_sentVersion = version, g_sentSelected = g_selected;
    j += "}";
    if (g_dumpNext) g_dumpNext = false, plat::writeFileAtomic(dataDir() + "/state.json", j.data(), j.size());
    web::post(g_win, j);
}

// The model list for the dropdown: OpenRouter's public list (text models).
void loadModels() {
    static bool once;
    if (once) return;
    once = true;
    plat::background([] {
        std::string body;
        jsonmini::Value v;
        if (plat::httpGet("https://openrouter.ai/api/v1/models", &body, 30) != 200 || !jsonmini::parse(body, &v)) {
            logf("autocue: couldn't load OpenRouter's model list");
            once = false;
            return;
        }
        struct M { std::string id, name; double in, out; };
        std::vector<M> ms;
        for (const jsonmini::Value& m : v["data"].a) {
            bool text = m["architecture"]["output_modalities"].a.empty();
            for (const jsonmini::Value& o : m["architecture"]["output_modalities"].a) text |= o.s == "text";
            if (text) ms.push_back({m["id"].s, m["name"].s, atof(m["pricing"]["prompt"].s.c_str()) * 1e6, atof(m["pricing"]["completion"].s.c_str()) * 1e6});
        }
        std::sort(ms.begin(), ms.end(), [](const M& a, const M& b) { return a.name < b.name; });
        std::string j = "[";
        for (const M& m : ms) {
            char price[64];
            snprintf(price, sizeof price, "$%.2f / $%.2f", m.in, m.out);
            j += std::string(j.size() > 1 ? "," : "") + "{\"id\":" + web::jsonStr(m.id) + ",\"name\":" + web::jsonStr(m.name) + ",\"price\":" + web::jsonStr(price) + ",\"free\":" + (m.in == 0 && m.out == 0 ? "true" : "false") + "}";
        }
        std::lock_guard<std::mutex> l(g_lock);
        g_models = j + "]";
        g_dirty = true;
    });
}

void checkKey() {
    plat::background([] {
        static std::mutex once;  // (one keychain question at a time)
        std::lock_guard<std::mutex> o(once);
        std::string key;
        {
            std::lock_guard<std::mutex> l(g_keyLock);
            key = g_key;
        }
        if (key.empty()) {
            key = plat::secretGet("openrouter");
            std::lock_guard<std::mutex> l(g_keyLock);
            if (g_key.empty()) g_key = key;
            key = g_key;  // (one may have been typed in meanwhile)
        }
        bool has = !key.empty();
        std::lock_guard<std::mutex> l(g_lock);
        g_hasKey = has, g_dirty = true;
        if (has && canAsk())  // tracks that came in before the key was known
            for (Row& r : g_rows)
                if (r.an && r.ai.status == "nokey") queueAsk(r.uri, false);
    });
}

void saveInstructions() {
    plat::writeFileAtomic(dataDir() + "/instructions.txt", g_instructions.data(), g_instructions.size());
}

void onMessage(const std::string& m) {
    std::string cmd = web::jsonGet(m, "cmd"), u = web::jsonGet(m, "u");
    auto row = [&](auto fn) {
        std::lock_guard<std::mutex> l(g_lock);
        if (Row* r = rowOf(u)) fn(*r);
        g_dirty = true;
    };
    if (cmd == "ready") { checkKey(); loadModels(); checkClaude(false); std::lock_guard<std::mutex> l(g_lock); g_sentVersion = -1, g_dirty = true; }
    else if (cmd == "provider") {  // openrouter, claude or none; tracks waiting for a model get this one's cues
        std::string v = web::jsonGet(m, "value");
        bool claude = v == "claude", none = v == "none";
        setCfg("autocue_provider", claude ? "claude" : none ? "none" : "openrouter");
        if (claude) checkClaude(true);
        std::lock_guard<std::mutex> l(g_lock);
        if (none) g_askQueue.clear();
        for (Row& r : g_rows) {
            if (!r.an) continue;
            if (none && r.ai.status == "queued") r.ai.status = r.ai.cues.empty() ? "nokey" : "done";
            // without a model the cues are the rules'; with one, the answer it gave before (cues moved by hand stay)
            if (!r.edited && r.draft != defaultDraft(r)) r.draft = defaultDraft(r), saveRow(r);
            r.version++;
            if (canAsk() && r.ai.status != "done") queueAsk(r.uri, false);
        }
        g_status = none ? "No model: the built-in rules place the cues." : "";
        g_dirty = true;
    }
    else if (cmd == "key") {
        std::string key = web::jsonGet(m, "value");
        plat::background([key] {
            bool ok = plat::secretSet("openrouter", key);
            {
                std::lock_guard<std::mutex> l(g_keyLock);
                g_key = ok ? key : "";
            }
            std::lock_guard<std::mutex> l(g_lock);
            g_hasKey = ok && !key.empty();
            g_status = !ok ? "The key couldn't be stored." : key.empty() ? "Key removed." : "Key stored.";
            if (g_hasKey && canAsk())  // tracks analysed without a key get the model's cues now
                for (Row& r : g_rows)
                    if (r.an && r.ai.status == "nokey") queueAsk(r.uri, false);
            g_dirty = true;
        });
    } else if (cmd == "model") {
        std::string v = web::jsonGet(m, "value");
        if (!v.empty()) setCfg(useClaude() ? "autocue_claude_model" : "autocue_model", v);
        g_dirty = true;
    } else if (cmd == "instructions") {  // new instructions: every track is asked again (an unchanged sheet + prompt is answered from the cache)
        std::lock_guard<std::mutex> l(g_lock);
        g_instructions = web::jsonGet(m, "text");
        saveInstructions();
        for (Row& r : g_rows)
            if (r.an) queueAsk(r.uri, false);
        g_status = canAsk() ? "Asking the model again with these instructions." : "Instructions saved; set up a model under the gear to use them.";
        g_dirty = true;
    } else if (cmd == "analyse") {
        bool running;
        { std::lock_guard<std::mutex> l(g_lock); running = g_workers > 0; if (g_status == "Stopped.") g_status.clear(); }
        g_started = true;
        if (!running) startReading(g_generation);
    } else if (cmd == "stop") stopReading();
    else if (cmd == "stems") {
        g_useStems = web::jsonGet(m, "on") == "true";
        setCfg("autocue_stems", g_useStems ? "1" : "0");
        std::lock_guard<std::mutex> l(g_lock);
        g_status = "New analyses use " + std::string(g_useStems ? "stems" : "the mix") + "; tracks done before keep theirs until analysed again.";
        g_dirty = true;
    } else if (cmd == "replace") { std::lock_guard<std::mutex> l(g_lock); g_replace = web::jsonGet(m, "on") == "true"; g_dirty = true; }
    else if (cmd == "layout") {  // pads, memory cues and the rules for placing: every track's cues follow at once (the model isn't asked again)
        Layout to = layoutFrom(web::jsonGet(m, "pads"), web::jsonGet(m, "mem"), atoi(web::jsonGet(m, "build").c_str()), web::jsonGet(m, "intro"), atoi(web::jsonGet(m, "outro").c_str()));
        setCfg("autocue_pads", padsStr(to)), setCfg("autocue_memory", memStr(to)), setCfg("autocue_build_bars", std::to_string(to.buildBars));
        setCfg("autocue_intro", kIntro[to.intro]), setCfg("autocue_outro_bars", std::to_string(to.outroBars));
        std::lock_guard<std::mutex> l(g_lock);
        g_layout = to;
        for (Row& r : g_rows) {
            if (!r.an) continue;
            auto before = r.draft;
            if (!r.edited) r.draft = defaultDraft(r);
            else r.edited = r.draft != defaultDraft(r);
            r.version++;
            if (r.draft != before) saveRow(r);
        }
        g_status.clear();
        g_dirty = true;
    }
    else if (cmd == "select") { std::lock_guard<std::mutex> l(g_lock); g_selected = u; g_dirty = true; }
    else if (cmd == "tick") row([&](Row& r) { r.tick = web::jsonGet(m, "on") == "true"; });
    else if (cmd == "tickAll") {
        std::lock_guard<std::mutex> l(g_lock);
        bool on = web::jsonGet(m, "on") == "true";
        for (Row& r : g_rows)
            if (r.usable) r.tick = on;
        g_dirty = true;
    } else if (cmd == "setCue") row([&](Row& r) {  // bar 0 removes it
        std::string s = web::jsonGet(m, "slot");
        r.cueAck = atoi(web::jsonGet(m, "n").c_str());
        if (!r.an || s.size() != 1 || s[0] < 'A' || s[0] > 'H') return;
        double bar = snapPos(*r.an, atof(web::jsonGet(m, "bar").c_str()));
        if (bar) r.draft[s[0]] = bar;
        else r.draft.erase(s[0]);
        r.edited = r.draft != defaultDraft(r), r.version++;  // (undone back to the model's: not edited)
        saveRow(r);
    });
    else if (cmd == "reset") row([&](Row& r) {  // back to the model's (or the rules') cues
        std::string from = web::jsonGet(m, "from");
        if (from == "rules" && r.an) {
            r.draft.clear();
            for (const Cue& c : rulesCues(r)) r.draft[c.slot] = c.bar;
            r.edited = true;
        } else r.draft = defaultDraft(r), r.edited = false;
        r.version++;
        saveRow(r);
    });
    else if (cmd == "askAgain") { std::lock_guard<std::mutex> l(g_lock); queueAsk(u, true); g_dirty = true; }
    else if (cmd == "reanalyse") row([&](Row& r) {
        if (!r.usable) return;
        std::error_code ec;
        std::filesystem::remove(plat::fsPath(dataDir() + "/" + hex(fnv(r.uri)) + ".json"), ec);  // (else the cache answers)
        r.done = false, r.an.reset(), r.ai = Answer(), r.draft.clear(), r.edited = false, r.force = true, r.state = "Waiting", r.version++;
        if (!g_workers) plat::onMain([] { g_started = true; startReading(g_generation); });
    });
    else if (cmd == "listen") listen(u, atoi(web::jsonGet(m, "ms").c_str()), atoi(web::jsonGet(m, "seq").c_str()));
    else if (cmd == "apply") applyCues();
    else if (cmd == "undo") undoCues();
    else if (cmd == "remove") removeCues();
    else if (cmd == "close") { web::close(g_win); return; }
}

void showWindow(const std::string& id, const std::string& name) {
    g_win = web::open("autocue", "autocue", "Auto cues", 1180, 760, onMessage);
    openPlaylistId(id, name);
    g_dirty = true;
}

// Scripted test runs: <configDir>/autocue_command (removed when read) holds "open [playlist id]" (the window, on that playlist, on a
// selection of tracks ("selection:<n>", see selectionPlaylist in rbmod.h) or on the one the browser shows), "testkey <file>" (an OpenRouter key for this session, not stored), "dump" (the page's state with the selected track in full, to <configDir>/autocue/state.json) or
// "clearmemory" (removes the selected track's memory cues, without an undo: for cleaning up after a test) or anything else as a message from the page, e.g. {"cmd":"analyse"}.
void readCommand() {
    static double lastRead;
    double t = now();
    if (t - lastRead < 1) return;
    lastRead = t;
    std::string path = plat::configDir() + "/autocue_command", text;
    if (!plat::readFile(path, &text)) return;
    std::error_code ec;
    std::filesystem::remove(plat::fsPath(path), ec);
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r' || text.back() == ' ')) text.pop_back();
    if (!text.compare(0, 8, "testkey ")) {  // "testkey <file>": a key for this session only, not stored
        std::string key;
        plat::readFile(text.substr(8), &key);
        while (!key.empty() && isspace((unsigned char)key.back())) key.pop_back();
        {
            std::lock_guard<std::mutex> l(g_keyLock);
            g_key = key;
        }
        std::lock_guard<std::mutex> l(g_lock);
        g_hasKey = !key.empty(), g_dirty = true;
        for (Row& r : g_rows)
            if (r.an && r.ai.status != "done") queueAsk(r.uri, false);
        return;
    }
    logf("autocue: command '%.200s'", text.find("\"key\"") == std::string::npos ? text.c_str() : "(a key)");
    if (!text.compare(0, 4, "open")) {  // "open /Root/Playlists/<name>": that one, if the tree has it loaded
        std::string id = text.size() > 5 ? text.substr(5) : shownPlaylist();
        // (a selection's tracks are known whatever the tree has loaded)
        if (text.size() > 5 && !showPlaylistId(id) && !isSelection(id)) return logf("autocue: playlist not in the tree (yet)");
        if (id.empty()) return logf("autocue: no playlist shown");
        showWindow(id, playlistName(id));
    } else if (text == "clearmemory") {  // the selected track's memory cues, gone for good (cleaning up after a test of them)
        std::lock_guard<std::mutex> l(g_lock);
        Row* r = rowOf(g_selected);
        if (!r || !r->id) return;
        for (const CueCopy& c : readCues(r->id))
            if (c.kind == kMemoryKind) deleteCue(r->id, c.id);
        r->existing = readCues(r->id), r->version++, g_dirty = true;
    } else if (text == "dump") {
        g_dumpNext = true, g_dirty = true;
        std::lock_guard<std::mutex> l(g_lock);
        g_sentVersion = -1;
    } else if (g_win) onMessage(text);
}

void playlistMenu(Menu* menu, void* treeItem) {
    menuAddItem(menu, "Open auto cues window", [treeItem] {
        showPlaylist(treeItem);
        showWindow(treeItemId(treeItem), treeItemName(treeItem));
    });
}

// The track list's menu: the same window on the selected tracks (a selection playlist: its rows are just those).
void trackMenu(Menu* menu, const std::vector<std::string>& uris, const std::string& selection) {
    menuAddItem(menu, menuWindowItem("auto cues window", uris.size()).c_str(), [selection] { showWindow(selection, playlistName(selection)); });
}

void tick() {
    readCommand();
    if (g_dirty.exchange(false)) sendState();
}

void init() {
    g_useStems = cfgInt("autocue_stems", 1) != 0;
    g_layout = layoutFrom(cfgStr("autocue_pads", "ABCDEFGH"), cfgStr("autocue_memory", "00000000"), cfgInt("autocue_build_bars", 0), cfgStr("autocue_intro", "sound"), cfgInt("autocue_outro_bars", 0));
    plat::readFile(dataDir() + "/instructions.txt", &g_instructions);
    std::error_code ec;
    std::filesystem::create_directories(plat::fsPath(dataDir()), ec);
}

}  // namespace

RBM_MOD({.name = "autocue", .init = init, .tick = tick, .playlistMenu = playlistMenu, .trackMenu = trackMenu, .enabled = &g_on, .menuOrder = 30});
