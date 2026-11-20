// See genres_model.h. The input is Essentia's TensorflowInputMusiCNN, as the model was trained with: 16 kHz, frames of
// 512 every 256 samples, periodic Hann window, power spectrum, 96 Slaney mel bands (0-8 kHz, each band's triangle of
// area 1), log10(1 + 10000 x); patches of 128 frames every 62 frames, scores averaged over the track. (Checked against
// Essentia on DnB and techno tracks: the scores agree to 0.001.)
#include "genres_model.h"
#include "../core/rbmod.h"
#include "../engines/beatthis.h"
#include "../engines/dsp.h"
#include <algorithm>
#include <atomic>
#include <cctype>
#include <initializer_list>
#include <utility>
#include <cmath>
#include <mutex>
#include <string.h>

using namespace rbm;

namespace genres {
namespace {

const char* const kStyles[400] = {
#include "genres_styles.inc"
};
constexpr int kFrame = 512, kHop = 256, kBins = kFrame / 2 + 1, kMels = 96, kPatch = 128, kPatchHop = 62, kBatch = 64;

double hzToMel(double f) { return f < 1000 ? f * 3 / 200 : 15 + 27 * std::log(f / 1000) / std::log(6.4); }
double melToHz(double m) { return m < 15 ? m * 200 / 3 : 1000 * std::exp((m - 15) * std::log(6.4) / 27); }

const std::vector<float>& melBank() {  // [mel][bin]
    static std::vector<float> fb = [] {
        std::vector<float> v((size_t)kMels * kBins, 0.f);
        double top = hzToMel(8000);
        std::vector<double> edge(kMels + 2);
        for (int i = 0; i < kMels + 2; i++) edge[i] = melToHz(top * i / (kMels + 1));
        for (int b = 0; b < kMels; b++) {
            double l = edge[b], c = edge[b + 1], h = edge[b + 2];
            for (int k = 0; k < kBins; k++) {
                double f = k * 16000.0 / kFrame, w = std::max(0.0, std::min((f - l) / (c - l), (h - f) / (h - c)));
                v[(size_t)b * kBins + k] = (float)(w * 2 / (h - l));
            }
        }
        return v;
    }();
    return fb;
}

// A Discogs label ("Electronic---Liquid Funk") -> a broad genre as DJs file them ("Drum & Bass"). Electronic styles
// go by keyword (the order matters: "Tech House" is house, "Hard Trance" trance); other music by its Discogs genre.
std::string broad(const char* full) {
    const char* sep = strstr(full, "---");
    std::string parent(full, sep ? (size_t)(sep - full) : strlen(full)), style = sep ? sep + 3 : full, s = style;
    for (char& c : s) c = (char)tolower((unsigned char)c);
    auto has = [&](std::initializer_list<const char*> words) {
        for (const char* w : words)
            if (s.find(w) != std::string::npos) return true;
        return false;
    };
    if (parent == "Electronic") {
        struct Rule { const char* words[8]; const char* name; };  // plain arrays (initializer_lists here would dangle)
        static const Rule kMap[] = {
            {{"jazz"}, "Jazz"},  // acid jazz, future jazz: not acid techno
            {{"drum n bass", "jungle", "liquid", "neurofunk", "halftime", "darkstep", "techstep"}, "Drum & Bass"},
            {{"dubstep", "bass music", "riddim"}, "Dubstep"},
            {{"house"}, "House"},
            {{"garage", "2-step", "bassline"}, "UK Garage"},
            {{"trance", "psy", "goa"}, "Trance"},
            {{"hardstyle", "jumpstyle", "hard beat"}, "Hardstyle"},
            {{"hardcore", "gabber", "speedcore", "breakcore", "terrorcore"}, "Hardcore"},
            {{"techno", "minimal", "acid", "schranz"}, "Techno"},
            {{"breakbeat", "breaks", "big beat", "nu skool"}, "Breaks"},
            {{"electro"}, "Electro"},
            {{"disco", "italo"}, "Disco"},
            {{"ambient", "drone", "new age"}, "Ambient"},
            {{"downtempo", "trip hop", "chillwave", "lo-fi", "abstract"}, "Downtempo"},
            {{"synth-pop", "synthwave", "new wave", "darkwave", "electropop"}, "Synth-pop"},
            {{"grime"}, "Grime"},
            {{"footwork", "juke"}, "Footwork"},
            {{"idm", "glitch", "experimental", "leftfield"}, "Experimental"},
        };
        for (const Rule& m : kMap)
            for (const char* w : m.words)
                if (w && s.find(w) != std::string::npos) return m.name;
        return style;
    }
    if (parent == "Hip Hop") return has({"trap"}) ? "Trap" : "Hip Hop";
    if (parent == "Funk / Soul") return has({"disco"}) ? "Disco" : has({"funk"}) ? "Funk" : "Soul";
    if (parent == "Folk, World, & Country") return has({"country"}) ? "Country" : "World";
    if (parent == "Stage & Screen") return "Soundtrack";
    if (parent == "Non-Music") return "Other";
    return parent;  // Rock, Pop, Jazz, Reggae, Latin, Blues, Classical, Brass & Military, Children's
}

std::once_flag g_once;
bool g_loaded;
std::mutex g_run;
}  // namespace

bool modelAvailable() {
    std::call_once(g_once, [] { g_loaded = genres_backend::load(); });
    return g_loaded;
}

bool detect(const std::vector<float>& mono, int rate, Style* out, int maxPatches) {
    if (!modelAvailable() || rate <= 0) return false;
    std::vector<float> x = beatthis::resample(mono.data(), mono.size(), rate, 16000);
    int frames = x.size() >= kFrame ? (int)((x.size() - kFrame) / kHop + 1) : 0;
    if (frames <= kPatch + 32) return false;
    // The patches: one every kPatchHop frames, or a bounded number spread over the middle of the track.
    std::vector<int> starts;
    for (int s = 0; s < frames - kPatch; s += kPatchHop) starts.push_back(s);
    if (maxPatches > 0 && (int)starts.size() > maxPatches) {
        size_t lo = starts.size() / 10, span = starts.size() - 2 * lo;
        std::vector<int> pick;
        for (int k = 0; k < maxPatches; k++) pick.push_back(starts[lo + (size_t)(span * (k + 0.5) / maxPatches)]);
        starts = pick;
    }
    std::vector<char> need((size_t)(frames + 255) / 256, 0);  // the 256-frame blocks the patches cover
    for (int s : starts)
        for (int b = s / 256; b <= (s + kPatch - 1) / 256 && b < (int)need.size(); b++) need[b] = 1;
    // The mel frames.
    static dsp::RealFFT fft(9);  // 512
    std::vector<float> window(kFrame);
    for (int i = 0; i < kFrame; i++) window[i] = 0.5f - 0.5f * cosf(2 * (float)M_PI * i / kFrame);
    const std::vector<float>& bank = melBank();
    std::vector<float> mel((size_t)frames * kMels);
    plat::parallelFor(need.size(), [&](size_t blk) {
        if (!need[blk]) return;
        std::vector<float> buf(kFrame), re(kFrame / 2), im(kFrame / 2), power(kBins);
        for (int f = (int)blk * 256; f < std::min(frames, (int)blk * 256 + 256); f++) {
            const float* s = x.data() + (size_t)f * kHop;
            for (int i = 0; i < kFrame; i++) buf[i] = s[i] * window[i];
            fft.forward(buf.data(), re.data(), im.data());  // scaled by 2
            power[0] = re[0] * re[0] / 4, power[kBins - 1] = im[0] * im[0] / 4;
            for (int k = 1; k < kBins - 1; k++) power[k] = (re[k] * re[k] + im[k] * im[k]) / 4;
            for (int b = 0; b < kMels; b++) {
                const float* w = bank.data() + (size_t)b * kBins;
                double e = 0;
                for (int k = 0; k < kBins; k++) e += w[k] * power[k];
                mel[(size_t)f * kMels + b] = (float)std::log10(1 + 10000 * e);
            }
        }
    });
    // Patches through the model, a batch at a time; the scores averaged.
    std::vector<double> sum(400, 0);
    std::vector<float> batch, act;
    for (size_t i = 0; i < starts.size(); i += kBatch) {
        int n = (int)std::min<size_t>(kBatch, starts.size() - i);
        batch.resize((size_t)n * kPatch * kMels);
        for (int j = 0; j < n; j++) std::copy_n(mel.data() + (size_t)starts[i + j] * kMels, (size_t)kPatch * kMels, batch.data() + (size_t)j * kPatch * kMels);
        std::string err;
        std::lock_guard<std::mutex> l(g_run);
        if (!genres_backend::predict(batch.data(), n, act, &err) || act.size() != (size_t)n * 400) {
            logf("genres: style model failed: %s", err.c_str());
            return false;
        }
        for (int j = 0; j < n; j++)
            for (int c = 0; c < 400; c++) sum[c] += act[(size_t)j * 400 + c];
    }
    for (double& v : sum) v /= starts.size();
    std::vector<int> order(400);
    for (int c = 0; c < 400; c++) order[c] = c;
    std::partial_sort(order.begin(), order.begin() + 3, order.end(), [&](int a, int b) { return sum[a] > sum[b]; });
    Style st;
    for (int k = 0; k < 3; k++) {
        const char* full = kStyles[order[k]];
        const char* s = strstr(full, "---");
        st.top[k] = {s ? s + 3 : full, broad(full), (float)sum[order[k]]};
    }
    // Halftime is a kind of drum & bass; when the model can't choose between them, the broader name is the useful one.
    if (st.top[0].style == "Halftime" && st.top[1].style == "Drum n Bass" && st.top[1].score >= 0.8f * st.top[0].score) std::swap(st.top[0], st.top[1]);
    // The family of the best-scoring style that has one (by its best style, not the sum: techno has many more styles).
    double best = 0.1;  // under this, no family fits clearly
    for (int c = 0; c < 400; c++) {
        std::string f = family(kStyles[c]);
        if (!f.empty() && sum[c] > best) best = sum[c], st.profile = f;
    }
    *out = st;
    return true;
}

}  // namespace genres
