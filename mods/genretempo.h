// A genre's tempo range: the BPMs a DJ counts that genre in, so bettergrid can pick the tempo's octave (a drum & bass
// track at 172, not 86; hip hop at 90, not 180). Only the octave: a genre never picks a tempo the analysis didn't find.
// A header so tools/dev/genretempo_test.cpp can test it offline.
#pragma once
#include <cmath>
#include <string>

namespace genretempo {

struct Range {
    double lo = 0, hi = 0;        // BPM; hi 0: the genre doesn't fix the octave
    const char* rule = nullptr;   // the rule that matched (for the test)
    bool constrains() const { return hi > 0; }
    // 1% leeway: a 186 BPM jungle track still counts.
    bool has(double bpm) const { return constrains() && bpm >= lo * 0.99 && bpm <= hi * 1.01; }
};

namespace detail {
struct Rule { const char* word; double lo, hi; };

// Matched against the name squashed (lower case, letters, digits and & only: "Drum 'n' Bass" -> "drumnbass",
// "Psy-Trance" -> "psytrance"), first match wins, so the order matters: a rule that would wrongly catch a later
// genre comes after it ("liquidfunk" before "funk", "hardcorehiphop" before "hardcore", "trap" before "rap",
// "tropicalhouse" before "house"). Rules with no range are there to stop a later rule. Every range is less than an
// octave wide (hi < 2 lo), so at most one octave of a tempo lands in it. A plain array: a static initializer_list
// table once crashed rekordbox.
inline constexpr Rule kRules[] = {
    // Not counted in one octave, or not dance music: no range.
    {"rock", 0, 0},          // also "Garage Rock", "Rock---Hardcore" (hardcore punk)
    {"metal", 0, 0},
    {"punk", 0, 0},
    {"classical", 0, 0},     // "Classical---Minimal"
    {"ambient", 0, 0},       // "Ambient House" is often slower than house
    {"downtempo", 0, 0},
    {"chillout", 0, 0},
    {"triphop", 0, 0},
    {"breakcore", 0, 0},
    {"trap", 0, 0},          // counted at 130-170 by EDM DJs and at 65-85 in hip hop
    {"cloudrap", 0, 0},
    {"phonk", 0, 0},
    {"drill", 0, 0},         // UK drill 140 or 70, Chicago 60-70: either
    {"bailefunk", 0, 0},     // Brazilian funk: 130 or 150, not funk's range
    {"funkcarioca", 0, 0},
    {"funkbrasileiro", 0, 0},
    {"brazilianfunk", 0, 0},
    // Drum & bass. Halftime is made at drum & bass tempo (170-175) with the snare on every other beat, released on
    // drum & bass labels and mixed with drum & bass: DJs count it at 170 so it beatmatches the rest of their set.
    {"drumnbass", 160, 185},
    {"drum&bass", 160, 185},
    {"drumandbass", 160, 185},
    {"drumbass", 160, 185},
    {"dnb", 160, 185},
    {"d&b", 160, 185},
    {"halftime", 160, 185},
    {"liquidfunk", 160, 185},
    {"neurofunk", 160, 185},
    {"techstep", 160, 185},
    {"darkstep", 160, 185},
    {"jumpup", 160, 185},
    {"drumfunk", 160, 185},
    {"drumstep", 160, 185},
    {"crossbreed", 160, 185},
    {"jungle", 150, 180},
    {"footwork", 150, 170},
    {"juke", 150, 170},
    // Bass music at 140.
    {"dubstep", 135, 150},
    {"brostep", 135, 150},
    {"riddim", 135, 150},
    {"grime", 130, 150},
    // Hard dance.
    {"hardcorehiphop", 80, 115},  // (hip hop, not hardcore)
    {"hardcorerap", 80, 115},
    {"hardstyle", 140, 165},
    {"rawstyle", 140, 165},
    {"jumpstyle", 140, 165},
    {"speedcore", 200, 380},      // (faster ones are found an octave down anyway)
    {"terrorcore", 180, 300},
    {"uptempo", 185, 250},        // "Uptempo Hardcore"
    {"frenchcore", 175, 230},
    {"gabber", 160, 230},
    {"hardcore", 160, 230},       // happy, UK, uptempo and hardcore techno too: 105 is 210, 115 is 230
    {"hardhouse", 135, 155},
    {"hardtechno", 135, 170},
    {"schranz", 135, 170},
    // House and its relatives.
    {"tropicalhouse", 95, 120},
    {"ghettohouse", 120, 140},
    {"amapiano", 105, 120},
    {"gqom", 118, 132},
    {"ukfunky", 120, 135},
    {"bigroom", 124, 132},
    {"house", 115, 130},          // deep, tech, electro, progressive, afro, garage house...
    {"bassline", 130, 145},
    {"garage", 125, 140},         // UK garage, speed garage, future garage
    {"ukg", 125, 140},
    {"2step", 125, 140},
    {"jerseyclub", 130, 145},
    {"baltimoreclub", 125, 135},
    // Techno and trance.
    {"techno", 120, 150},
    {"minimal", 115, 135},
    {"psytrance", 135, 150},
    {"goatrance", 135, 150},
    {"psychedelictrance", 135, 150},
    {"trance", 125, 150},
    {"eurodance", 125, 150},
    {"hinrg", 120, 140},
    {"breakbeat", 120, 150},
    {"breaks", 120, 150},
    // Disco, funk, soul.
    {"disco", 105, 130},
    {"soul", 0, 0},               // "Funk / Soul": anything from ballads up
    {"r&b", 0, 0},
    {"rnb", 0, 0},
    // Hip hop and its neighbours.
    {"lofihiphop", 60, 95},
    {"lofibeats", 60, 95},
    {"chillhop", 60, 95},
    {"hiphop", 80, 115},
    {"boombap", 80, 115},
    {"gfunk", 80, 115},
    {"gangsta", 80, 115},
    {"rap", 80, 115},
    {"funk", 80, 130},
    {"reggaeton", 85, 105},
    {"dembow", 85, 105},
    {"moombahton", 100, 120},
    {"dancehall", 85, 110},
    {"afrobeat", 95, 125},        // afrobeat and afrobeats
};

inline std::string squash(const std::string& s) {
    std::string out;
    for (char ch : s) {
        unsigned char c = (unsigned char)ch;
        if (c >= 'A' && c <= 'Z') out += (char)(c - 'A' + 'a');
        else if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '&') out += (char)c;
    }
    return out;
}

// Discogs' broad genres whose styles share names with dance styles ("Rock---Hardcore", "Classical---Minimal").
inline constexpr const char* kNotDance[] = {"rock", "classical", "jazz", "blues", "folk", "stage", "nonmusic", "children", "brass"};
}  // namespace detail

// A genre or style name ("Drum n Bass", "Electronic---Deep House", "DnB", "hip-hop") -> its tempo range, or none.
// A "broad---style" label goes by its style ("Funk / Soul---Afrobeat" is afrobeat, not soul).
inline Range rangeOf(const std::string& genre) {
    std::string s = detail::squash(genre);
    size_t dash = genre.find("---");
    if (dash != std::string::npos) {
        std::string broad = detail::squash(genre.substr(0, dash));
        for (const char* b : detail::kNotDance)
            if (broad.find(b) != std::string::npos) return {};
        s = detail::squash(genre.substr(dash + 3));
    }
    if (s.empty()) return {};
    for (const detail::Rule& r : detail::kRules)
        if (s.find(r.word) != std::string::npos) return Range{r.lo, r.hi, r.word};
    return {};
}

// How rekordbox's tempo relates to Beat This!'s: an octave or 3:2 apart (as bettergrid's log names them).
inline bool related(double bt, double rb) {
    if (!(bt > 0) || !(rb > 0)) return false;
    double r = bt / rb;
    return std::fabs(r - 2) < 0.02 || std::fabs(r - 0.5) < 0.01 || std::fabs(r - 1.5) < 0.02 || std::fabs(r - 2.0 / 3) < 0.01;
}

enum class Source {
    Today,      // no change: bettergrid's own choice (Beat This!'s tempo, rekordbox's beats when they agree)
    BeatThis,   // Beat This!'s grid at factor times its tempo (2 or 0.5)
    Rekordbox,  // rekordbox's grid as it is
};
struct Choice {
    Source source = Source::Today;
    double factor = 1;  // BeatThis: 2 or 0.5
    double bpm = 0;     // the chosen tempo (BeatThis, Rekordbox)
};

// The tempo octave for a track in range r, given Beat This!'s tempo bt and rekordbox's rb (0: unknown). bettergrid
// uses Beat This!'s tempo (sure fits only), so: when that is in the range, nothing changes. Else Beat This!'s tempo
// doubled or halved when that lands in it; else rekordbox's tempo when that is in it and an octave or 3:2 from Beat
// This!'s (a tempo the analyses found, never a third one). Else, a near miss: the octave of Beat This!'s tempo that
// rekordbox found too, when it is within 15% of the range and nearer to it than Beat This!'s own (a 190 BPM jungle
// track read as 95: the range ends at 180, and nobody plays it at 95). Else nothing changes.
inline Choice choose(double bt, double rb, const Range& r) {
    Choice c;
    if (!r.constrains() || !(bt > 0) || !std::isfinite(bt) || r.has(bt)) return c;
    const double factors[] = {2.0, 0.5};
    for (double f : factors)
        if (r.has(bt * f)) return Choice{Source::BeatThis, f, bt * f};
    if (r.has(rb) && related(bt, rb)) return Choice{Source::Rekordbox, 1, rb};
    auto off = [&](double bpm) { return bpm < r.lo ? r.lo / bpm : bpm > r.hi ? bpm / r.hi : 1.0; };  // 1: in the range
    for (double f : factors)
        if (rb > 0 && std::fabs(bt * f / rb - 1) < 0.01 && off(bt * f) <= 1.15 && off(bt * f) < off(bt))
            return Choice{Source::BeatThis, f, bt * f};
    return c;
}

}  // namespace genretempo
