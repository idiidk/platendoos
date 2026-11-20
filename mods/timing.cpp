// timing: how long each stage of rekordbox's analysis of a track takes.
// Stages are marked on the analysis thread by core's hooks on calls inside CBeatAnalyzeThread::analyze (core/analysis.cpp)
// and reach this mod as the "timing.mark" service; "timing.report" (analysisTiming(uri, since) in core/rbmod.h) formats them for the analyzer's "row … ok"
// line, e.g.  load→analysis 4.1s; #1 audio+beats+waveform 5.6s, phrase 1.2s, key 0.8s, similarity 1.9s+
// Every analysis of the track counts (rekordbox sometimes analyses a loaded track twice): #1, #2.
//   config: timing=0 to disable
#include "../core/rbmod.h"
#include <string.h>
#include <algorithm>
#include <iterator>
#include <map>
#include <mutex>

using namespace rbm;

static volatile bool g_on = true;

// One analysis: stage -> when it was first reached.
namespace {  // file-local
struct Analysis {
    double start = 0, downloadFrom = 0, downloadTo = 0;
    int streamWaitMs = 0;  // slept waiting for streamed audio (in the audio pass and after)
    std::vector<std::pair<std::string, double>> stages;
};
static std::mutex g_lock;
static std::map<std::string, std::vector<Analysis>> g_analyses;  // by track URI

}  // namespace

// Service "timing.mark" (called by core's analysis hooks on the analysis thread): `fresh` is the first mark of an
// analysis; "start" only opens it, "poll" is the wait for the stream download, any other stage is noted once.
static void mark(const char* uri, const char* stage, bool fresh) {
    double t = now();
    bool start = !strcmp(stage, "start");
    std::lock_guard<std::mutex> l(g_lock);
    if (fresh) {
        // (reports are asked for within minutes: what a long run analysed an hour ago is dropped, or this grew all session)
        static double prunedAt;
        if (g_analyses.size() > 500 && t - prunedAt > 300) {
            prunedAt = t;
            for (auto it = g_analyses.begin(); it != g_analyses.end();) {
                double last = it->second.empty() ? 0 : it->second.back().start;
                for (const Analysis& a : it->second)
                    if (!a.stages.empty()) last = std::max(last, a.stages.back().second);
                it = t - last > 3600 ? g_analyses.erase(it) : std::next(it);
            }
        }
        g_analyses[uri].push_back(Analysis{start ? t : 0});
    }
    if (start) return;
    auto it = g_analyses.find(uri);
    if (it == g_analyses.end() || it->second.empty()) return;
    Analysis& a = it->second.back();
    if (!strcmp(stage, "poll")) {
        if (!a.downloadFrom) a.downloadFrom = t;
        a.downloadTo = t;
        return;
    }
    a.stages.push_back({stage, t});
}

// Service "timing.wait": juce::Thread::sleep(ms) in the analysis' "stream not ready yet" polls.
static void streamWait(const char* uri, int ms) {
    std::lock_guard<std::mutex> l(g_lock);
    auto it = g_analyses.find(uri);
    if (it != g_analyses.end() && !it->second.empty()) it->second.back().streamWaitMs += ms;
}

static std::string report(const std::string& uri, double since, double loadedAt) {
    if (loadedAt < 0) loadedAt = since;
    std::lock_guard<std::mutex> l(g_lock);
    auto it = g_analyses.find(uri);
    if (it == g_analyses.end()) return "";
    std::string out;
    char buf[96];
    int n = 0;
    double t = now();
    for (const Analysis& a : it->second) {
        if (a.start && a.start < since) continue;
        if (!n++ && a.start) {
            snprintf(buf, sizeof buf, "load→analysis %.1fs; ", std::max(0.0, a.start - loadedAt));
            out += buf;
        }
        if (n > 1 && a.start) snprintf(buf, sizeof buf, " | #%d (+%.1fs)", n, a.start - loadedAt);
        else snprintf(buf, sizeof buf, "%s#%d", n > 1 ? " | " : "", n);
        out += buf;
        double at = a.start;
        bool beatJob = false;
        for (auto& s : a.stages) beatJob |= s.first == "beat job";
        std::string cur = beatJob ? "audio+waveform" : "audio+beats+waveform";
        if (a.streamWaitMs) {
            snprintf(buf, sizeof buf, " (stream wait %.1fs)", a.streamWaitMs / 1000.0);
            out += buf;
        }
        if (a.downloadFrom) {
            snprintf(buf, sizeof buf, " opening %.1fs, download wait %.1fs,", a.downloadFrom - at, a.downloadTo - a.downloadFrom);
            out += buf;
            at = a.downloadTo;
        }
        for (auto& s : a.stages) {
            snprintf(buf, sizeof buf, " %s %.1fs,", cur.c_str(), s.second - at);
            out += buf;
            cur = s.first;
            at = s.second;
        }
        snprintf(buf, sizeof buf, " %s %.1fs+", cur.c_str(), t - at);  // up to now
        out += buf;
    }
    return out;
}

RBM_PROVIDE("timing", "timing.mark", mark, &g_on);
RBM_PROVIDE("timing", "timing.wait", streamWait, &g_on);
RBM_PROVIDE("timing", "timing.report", report, &g_on);
RBM_MOD({.name = "timing", .enabled = &g_on});
