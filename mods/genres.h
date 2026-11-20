// The genres mod's public side: what a track's style detection gives, the services other mods call (through
// rbm::service, so the mods stay independent), and the genre-name families (autocue's old drop-detection profiles; unused since its rewrite).
#pragma once
#include <cctype>
#include <initializer_list>
#include <string>
#include <vector>

namespace genres {

struct Guess {
    std::string style;  // the Discogs style ("Drum n Bass", "Minimal Techno", "Synth-pop")
    std::string broad;  // its broad genre ("Drum & Bass", "Techno", "Electronic"... "Rock")
    float score = 0;    // 0..1 (the model's activation averaged over the track)
};

struct Style {
    Guess top[3];         // the three best styles, best first
    std::string profile;  // autocue's detection family that fits: dnb, bass, house, techno, or "" (none clearly)
};

// Services (provided while the genres mod is on):
//   "genres.detect"     bool (*)(const std::vector<float>* mono, int rate, Style* out)   any thread; false: no model
//   "genres.analysing"  void (*)(const char* path)   rekordbox's analysis of this track reached a stage (core's
//       analysis hooks call it, on the analysis thread): with genres_on_analyse its empty Genre field gets filled
//       once the analysis has gone quiet
//   "genres.known"      bool (*)(const char* uri, std::string* genre, bool* written)   main thread; never listens:
//       the Genre field (*written true), else the style the model is sure of (*written false: only heard so far)
//   "genres.forTrack"   bool (*)(const char* uri, const std::vector<float>* mono, int rate, std::string* genre)
//       The best genre name known for a track (path or spotify: URI): rekordbox's Genre field as it is (typed by the
//       user or written by this mod), else the style the model found before (kept in the library, "Drum n Bass",
//       "Halftime", "Deep House"), else, when mono is given (mono audio at rate, may be the whole track), the model's best
//       style for it, run on the calling thread over at most ~2 minutes of it and kept for next time. A model guess it
//       isn't sure of (under 20%) counts as nothing. False when nothing is known and no audio was given, or the model
//       isn't available. Any thread, several at once (a second ask for a track being
//       detected waits for that result). From a thread other than the main one, reading the Genre field waits for the
//       main thread (2 s at most; then it counts as empty).
using DetectFn = bool (*)(const std::vector<float>* mono, int rate, Style* out);
using AnalysingFn = void (*)(const char* path);
using KnownFn = bool (*)(const char* uri, std::string* genre, bool* written);
using ForTrackFn = bool (*)(const char* uri, const std::vector<float>* mono, int rate, std::string* genre);

// A genre or style name ("Tech House", "Drum & Bass", "Electronic---Jungle") -> autocue's detection family:
// dnb, bass, house, techno, or "". The order matters ("Electro House" is house, not electro).
inline std::string family(std::string s) {
    for (char& c : s) c = (char)tolower((unsigned char)c);
    auto has = [&](std::initializer_list<const char*> words) {
        for (const char* w : words)
            if (s.find(w) != std::string::npos) return true;
        return false;
    };
    if (has({"drum n bass", "drum & bass", "drum and bass", "drum'n'bass", "dnb", "d&b", "jungle", "halftime", "neurofunk", "liquid funk", "hardcore", "breakcore"})) return "dnb";
    if (has({"dubstep", "bass music", "riddim", "grime", "trap", "breakbeat", "breaks", "juke", "footwork", "drumstep"})) return "bass";
    if (has({"house", "garage", "disco", "trance", "progressive", "balearic"})) return "house";
    if (has({"techno", "minimal", "electro", "acid", "industrial", "ebm", "hard beat"})) return "techno";
    return "";
}

}  // namespace genres
