// stemtest: a command channel for testing stems on a running platendoos (hot-loaded: tools/dev/hotload.sh
// tools/dev/stemtest.mm). Commands, one per line, go into /tmp/rbm-stemtest/cmd.txt; what they find is appended to
// /tmp/rbm-stemtest/out.txt. Stops when /tmp/rbm-stemtest/gen.txt no longer holds GEN (write another number there
// before loading a changed copy).
//   show <playlist id>           the browser shows it (e.g. /Root/Collection)
//   rows <n>                     the first n rows' paths
//   load <row> <deck>            the browser list's row onto a deck (1-4)
//   play <deck> | pause <deck> | unload <deck>
//   seek <deck> <seconds>        jumps there (DjEngineIF::setPlayingTime)
//   state                        per deck: path, playing, position, the on-screen stem buttons (1 = playing, 0 = muted)
//   ui <deck> <part 0-3> <0|1>   as clicking a stem button: PartMuteSoloComp::setPartActive (0 mutes the part)
//   rec <seconds> [ms]           records what each mixer channel's envelope follower is given (the channel's audio:
//                                EnvelopeGeneratorArray::processBlock, tapped by a vtable swap) and logs its level per
//                                half second, per channel that had signal ("REC ch <n>: -12 -11 ... dB")
//   reads | reads dump [reader]  logs what the decks read of the stems (AudioPartReader::fileRead / isReadyToRead)
//   iso <deck> <part> <0..1>     the PART ISOLATOR knob of a stem (0.5 centre), as IDjPlayerControl::setPartIsoValue
//   fx <deck> <part> <0|1>       whether the FX apply to a stem (IDjPlayerControl::setPartFxState)
//   sleep <seconds>              (between commands of one file)
// Parts: 0 vocal, 1 inst, 2 drums, 3 bass (rbxfrm::audiopart::PART; with 3 stems, 3 counts as 1).
#import <Foundation/Foundation.h>
#include <atomic>
#include <cmath>
#include <map>
#include <thread>
#include "rbmod.h"
using namespace rbm;
#define D "/tmp/rbm-stemtest/"
#ifndef GEN
#define GEN 10
#endif
static double wall() { timespec t; clock_gettime(CLOCK_REALTIME, &t); return t.tv_sec + t.tv_nsec * 1e-9; }
static FILE* g_out;
static void out(const char* f, ...) {
    va_list a; va_start(a, f);
    fprintf(g_out, "%.3f ", wall()); vfprintf(g_out, f, a); va_end(a);
    fputc('\n', g_out); fflush(g_out);
}
static int curGen() { FILE* f = fopen(D "gen.txt", "r"); int g = 0; if (f) { fscanf(f, "%d", &g); fclose(f); } return g; }

static const Offset O_compDeck{0x238, "__ZN6djplay16PartMuteSoloComp13setPartActiveEN6rbxfrm9audiopart4PARTEb", "ldr x20", 0};  // PartMuteSoloComp: its deck (0-3)
// IDjPlayerControl's slots, as its streaming wrapper calls them on the player behind it.
static const Offset V_ctlIso{0x2a0, "__ZN6rbxfrm28DjPlayerStreamingEventSender15setPartIsoValueENS_9audiopart4PARTEd", "ldr x8", 0};
static const Offset V_ctlFx{0x2a8, "__ZN6rbxfrm28DjPlayerStreamingEventSender14setPartFxStateENS_9audiopart4PARTEb", "ldr x8", 0};
static bool isPartComp(void* c) { return !strcmp(*(const char**)((char*)(*(void***)c)[-1] + 8), "N6djplay16PartMuteSoloCompE"); }
// The deck's on-screen stem buttons (there is one set per deck panel; the first found).
static void* partComp(int deck) {
    static std::map<int, ComponentPath> paths;
    void* main = at<void* (*)()>(symbol("__ZN13MainComponent11getInstanceEv"))();
    return main ? findComponent(main, paths[deck], [deck](void* c) { return isPartComp(c) && *(int*)((char*)c + O_compDeck) == deck; }) : nullptr;
}
// ---- the channel audio tap ----
static const Offset V_envProcess{0x10, "__ZTV22EnvelopeGeneratorArray", "vt:__ZN22EnvelopeGeneratorArray12processBlockERN4juce11AudioBufferIfEEii", 0};
static const Offset O_bufChannels{0x10, "__ZN22EnvelopeGeneratorArray12processBlockERN4juce11AudioBufferIfEEii", "ldr x1", 1};
static int (*g_envNext)(void*, void*, int, int);
enum { kTaps = 16, kBlocks = 2400 };  // per channel: sums of squares per block (half a second, or rec's <ms>)
static long long g_tapBlock = 22050;  // samples per block
static void* g_tapArray[kTaps];
static double g_tapSum[kTaps][kBlocks];
static std::atomic<long long> g_tapLeft{0};   // samples still to record (per channel: counted on channel 0's clock)
static long long g_tapAt[kTaps];
static int tapProcess(void* array, void* buffer, int start, int n) {  // audio thread
    if (g_tapLeft.load(std::memory_order_relaxed) > 0 && n > 0 && *(int*)buffer == 2) {
        int t = 0;
        while (t < kTaps && g_tapArray[t] != array && g_tapArray[t]) t++;
        if (t < kTaps) {
            g_tapArray[t] = array;
            float** ch = *(float***)((char*)buffer + O_bufChannels);
            double sum = 0;
            for (int i = 0; i < n; i++) sum += (double)ch[0][start + i] * ch[0][start + i] + (double)ch[1][start + i] * ch[1][start + i];
            long long block = g_tapAt[t] / g_tapBlock;
            if (block < kBlocks) g_tapSum[t][block] += sum;
            g_tapAt[t] += n;
            if (t == 0) g_tapLeft.fetch_sub(n, std::memory_order_relaxed);
        }
    }
    return g_envNext(array, buffer, start, n);
}
static void tapInstall() {
    if (g_envNext) return;
    void** slot = (void**)(at<char*>(symbol("__ZTV22EnvelopeGeneratorArray")) + 0x10 + V_envProcess);
    g_envNext = (int (*)(void*, void*, int, int))*slot;
    patchSlot(slot, (void*)&tapProcess);
}
static void tapReport() {
    for (int t = 0; t < kTaps && g_tapArray[t]; t++) {
        std::string line; bool any = false;
        for (long long b = 0; b < kBlocks && b * g_tapBlock < g_tapAt[t]; b++) {
            double rms = sqrt(g_tapSum[t][b] / (2.0 * g_tapBlock));
            char v[16]; snprintf(v, sizeof v, rms > 1e-6 ? "%.0f " : "-- ", 20 * log10(rms > 1e-6 ? rms : 1));
            line += v; any |= rms > 1e-6;
        }
        if (any) out("REC ch %d: %s", t, line.c_str());
    }
    out("REC done");
}

// ---- what the decks read of a track's stems ----
// rbxfrm::audiopart::AudioPartReader (a FileReader over one stem; a windowed PcmReader's thread reads through it):
// fileRead(pos, Float2*, n) and isReadyToRead(pos, n), in samples. Logged with the time, from its vtable's slots.
enum { kReads = 16384 };
struct ReadCall { double at, took; void* reader; int pos, n, result; char kind; };
static ReadCall g_reads[kReads];
static std::atomic<int> g_readCount{0};
static int (*g_fileReadNext)(void*, int, void*, int);
static bool (*g_isReadyNext)(void*, int, int);
static int fileReadLogged(void* reader, int pos, void* to, int n) {
    double t = wall();
    int got = g_fileReadNext(reader, pos, to, n);
    int i = g_readCount.fetch_add(1, std::memory_order_relaxed);
    if (i < kReads) g_reads[i] = {t, wall() - t, reader, pos, n, got, 'r'};
    return got;
}
static bool isReadyLogged(void* reader, int pos, int n) {
    bool ok = g_isReadyNext(reader, pos, n);
    int i = g_readCount.fetch_add(1, std::memory_order_relaxed);
    if (i < kReads) g_reads[i] = {wall(), 0, reader, pos, n, ok, '?'};
    return ok;
}
static void readsInstall() {
    if (g_fileReadNext) return;
    void** vt = (void**)at<char*>(symbol("__ZTVN6rbxfrm9audiopart15AudioPartReaderE"));
    void* fileRead = at<void*>(symbol("__ZN6rbxfrm9audiopart15AudioPartReader8fileReadEiPNS_6Float2Ei"));
    void* isReady = at<void*>(symbol("__ZN6rbxfrm9audiopart15AudioPartReader13isReadyToReadEii"));
    for (int i = 0; i < 25; i++) {
        if (vt[i] == fileRead && !g_fileReadNext) g_fileReadNext = (int (*)(void*, int, void*, int))vt[i], patchSlot(&vt[i], (void*)&fileReadLogged);
        else if (vt[i] == isReady && !g_isReadyNext) g_isReadyNext = (bool (*)(void*, int, int))vt[i], patchSlot(&vt[i], (void*)&isReadyLogged);
    }
    out("READS: fileRead %s, isReadyToRead %s", g_fileReadNext ? "logged" : "not found", g_isReadyNext ? "logged" : "not found");
}
static void run(const std::string& l) {
    std::string c = l.substr(0, l.find(' ')), r = l.find(' ') == std::string::npos ? "" : l.substr(l.find(' ') + 1);
    void* e = engine();
    if (c == "show") { bool ok = showPlaylistId(r); out("SHOW %s -> %d (shown %s)", r.c_str(), ok, shownPlaylist().c_str()); }
    else if (c == "rows") {
        void* lv = browserListViewer(); int n = lv ? listRows(lv) : -1; out("ROWS %d", n);
        for (int i = 0; i < n && i < atoi(r.c_str()); i++) { const char* row = listRow(lv, i); const char* p = row ? trackPath(row) : nullptr; out("ROW %d %s", i, p ? p : "-"); }
    }
    else if (c == "load") {
        int row = 0, deck = 1; sscanf(r.c_str(), "%d %d", &row, &deck);
        void* lv = browserListViewer(); const char* rw = lv && row < listRows(lv) ? listRow(lv, row) : nullptr; const char* p = rw ? trackPath(rw) : nullptr;
        out("LOAD row %d deck %d %s", row, deck, p ? p : "-");
        if (p) listLoadTrack(lv, deck - 1, row);
    }
    else if (c == "play" || c == "pause" || c == "unload") {
        int ch = atoi(r.c_str()) - 1; out("%s deck %d", c.c_str(), ch + 1);
        if (c == "play") at<void (*)(void*, int, bool)>(symbol("__ZN8djengine10DjEngineIF4playENS_15EnPlayerChannelEb"))(e, ch, false);
        else if (c == "pause") at<void (*)(void*, int)>(symbol("__ZN8djengine10DjEngineIF5pauseENS_15EnPlayerChannelE"))(e, ch);
        else deckUnload(ch);
    }
    else if (c == "seek") {
        int deck = 1; double sec = 0; sscanf(r.c_str(), "%d %lf", &deck, &sec);
        out("seek deck %d to %.1f s", deck, sec);
        at<void (*)(void*, int, int)>(symbol("__ZN8djengine10DjEngineIF14setPlayingTimeENS_15EnPlayerChannelEi"))(e, deck - 1, (int)(sec * 1000));
    }
    else if (c == "state") {
        for (int d = 0; d < deckCount(); d++) {
            if (deckPath(d).empty()) continue;
            int ms = at<int (*)(void*, int)>(symbol("__ZNK8djengine10DjEngineIF14getPlayingTimeENS_15EnPlayerChannelE"))(e, d);
            void* comp = partComp(d);
            char parts[8] = "----";
            for (int p = 0; comp && p < 4; p++) parts[p] = at<bool (*)(void*, int)>(symbol("__ZNK6djplay16PartMuteSoloComp12isPartActiveEN6rbxfrm9audiopart4PARTE"))(comp, p) ? '1' : '0';
            out("DECK %d playing %d at %.2f s, stem buttons %s  %s", d + 1, deckPlaying(d), ms / 1000.0, parts, deckPath(d).c_str());
        }
    }
    else if (c == "ui") {
        int deck = 1, part = 0, active = 0; sscanf(r.c_str(), "%d %d %d", &deck, &part, &active);
        void* comp = partComp(deck - 1);
        out("UI deck %d part %d active %d (comp %p)", deck, part, active, comp);
        if (comp) at<void (*)(void*, int, bool)>(symbol("__ZN6djplay16PartMuteSoloComp13setPartActiveEN6rbxfrm9audiopart4PARTEb"))(comp, part, active != 0);
    }
    else if (c == "iso" || c == "fx") {
        int deck = 1, part = 0; double v = 0; sscanf(r.c_str(), "%d %d %lf", &deck, &part, &v);
        void* sys = at<void* (*)(void*)>(symbol("__ZNK8djengine10DjEngineIF11getDjSystemEv"))(e);
        void* ctl = sys ? at<void* (*)(void*, int)>(symbol("__ZNK8DjSystem16getPlayerControlEi"))(sys, deck - 1) : nullptr;
        out("%s deck %d part %d = %g (control %p)", c.c_str(), deck, part, v, ctl);
        if (!ctl) return;
        void** vt = *(void***)ctl;
        if (c == "iso") ((void (*)(void*, int, double))vt[V_ctlIso / 8])(ctl, part, v);
        else ((void (*)(void*, int, bool))vt[V_ctlFx / 8])(ctl, part, v != 0);
    }
    else if (c == "reads") {  // "reads": from now on; "reads dump [reader index]": what was read since, runs of calls joined
        if (r.rfind("dump", 0) != 0) { readsInstall(); g_readCount = 0; return; }
        int n = std::min((int)g_readCount, (int)kReads), only = r.size() > 5 ? atoi(r.c_str() + 5) : -1;
        std::vector<void*> readers;
        for (int i = 0; i < n; i++)
            if (std::find(readers.begin(), readers.end(), g_reads[i].reader) == readers.end()) readers.push_back(g_reads[i].reader);
        for (size_t k = 0; k < readers.size(); k++) {
            if (only >= 0 && (int)k != only) continue;
            // a run: calls of one kind and result that follow each other in position
            int first = -1, last = -1;
            auto flush = [&] {
                if (first < 0) return;
                const ReadCall &a = g_reads[first], &b = g_reads[last];
                out("READ %zu %c %.3f-%.3f: %.3f-%.3f s -> %s%s", k, a.kind, fmod(a.at, 1000), fmod(b.at + b.took, 1000), a.pos / 44100.0, (b.pos + b.n) / 44100.0,
                    a.kind == '?' ? (a.result ? "ready" : "not ready") : (a.result == a.n ? "read" : "short"), a.took > 0.05 ? " (waited)" : "");
                first = -1;
            };
            for (int i = 0; i < n; i++) {
                const ReadCall& q = g_reads[i];
                if (q.reader != readers[k]) continue;
                bool joins = first >= 0 && q.kind == g_reads[last].kind && (q.result != 0) == (g_reads[last].result != 0) && q.took < 0.05 &&
                             (q.pos == g_reads[last].pos + g_reads[last].n || (q.kind == '?' && abs(q.pos - g_reads[last].pos) < 44100));
                if (!joins) flush(), first = i;
                last = i;
            }
            flush();
        }
        out("READS %d calls, %zu readers", n, readers.size());
    }
    else if (c == "rec") {
        tapInstall();
        memset(g_tapSum, 0, sizeof g_tapSum); memset(g_tapAt, 0, sizeof g_tapAt); memset(g_tapArray, 0, sizeof g_tapArray);
        double secs = atof(r.c_str()), ms = r.find(' ') == std::string::npos ? 500 : atof(r.c_str() + r.find(' ') + 1);
        g_tapBlock = std::max(441LL, (long long)(ms * 44.1));
        g_tapLeft = (long long)(secs * 44100);
        out("REC %.1f s, a level per %.0f ms", secs, g_tapBlock / 44.1);
        std::thread([secs] {
            usleep((useconds_t)((secs + 0.3) * 1e6));
            g_tapLeft = 0;
            usleep(50000);
            plat::onMain(tapReport);
        }).detach();
    }
    else out("unknown: %s", l.c_str());
}
extern "C" void rbm_experiment() {
    g_out = fopen(D "out.txt", "a");
    if (!g_out) return;
    std::thread([] {
        while (curGen() == GEN) {
            FILE* f = fopen(D "cmd.txt", "r");
            if (f) {
                std::string s; char b[4096];
                while (fgets(b, sizeof b, f)) s += b;
                fclose(f); remove(D "cmd.txt");
                for (size_t i = 0; i < s.size();) {
                    size_t j = s.find('\n', i); if (j == std::string::npos) j = s.size();
                    std::string l = s.substr(i, j - i); i = j + 1;
                    if (l.rfind("sleep ", 0) == 0) { usleep((useconds_t)(atof(l.c_str() + 6) * 1e6)); continue; }
                    if (l.empty()) continue;
                    auto* L = new std::string(l);
                    plat::onMain([L] { run(*L); delete L; });
                    usleep(20000);
                }
            }
            usleep(30000);
        }
    }).detach();
    logf("stemtest: started (%s)", D "cmd.txt");
}
