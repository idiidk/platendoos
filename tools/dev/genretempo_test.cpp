// Offline checks of mods/genretempo.h: genre names -> tempo ranges (order pitfalls, free-text spellings, Discogs
// "broad---style" labels) and the octave bettergrid picks with them.
//   clang++ -std=c++17 -Wall -o $TMPDIR/genretempo_test tools/dev/genretempo_test.cpp && $TMPDIR/genretempo_test
#include "../../mods/genretempo.h"
#include <stdio.h>
#include <string>

using namespace genretempo;

static int failures = 0;
static void check(bool ok, const std::string& what) {
    printf("%s %s\n", ok ? "  ok  " : "  FAIL", what.c_str());
    if (!ok) failures++;
}

// name -> lo-hi (0, 0: no range)
static void range(const char* name, double lo, double hi) {
    Range r = rangeOf(name);
    char b[200];
    snprintf(b, sizeof b, "%-28s -> %3.0f-%-3.0f (want %3.0f-%-3.0f, rule %s)", name, r.lo, r.hi, lo, hi, r.rule ? r.rule : "-");
    check(r.lo == lo && r.hi == hi, b);
}

static const char* name(Source s) { return s == Source::BeatThis ? "Beat This!" : s == Source::Rekordbox ? "rekordbox" : "today"; }

// Beat This! bt, rekordbox rb, genre -> the source and tempo wanted (want 0: today's choice).
static void octave(const char* genre, double bt, double rb, Source source, double want) {
    Choice c = choose(bt, rb, rangeOf(genre));
    char b[200];
    snprintf(b, sizeof b, "%-14s BT %6.2f rb %6.2f -> %s %.2f (want %s %.2f)", genre, bt, rb, name(c.source), c.bpm, name(source), want);
    check(c.source == source && (source == Source::Today || fabs(c.bpm - want) < 0.01), b);
}

int main() {
    printf("ranges:\n");
    range("Drum n Bass", 160, 185);
    range("Drum & Bass", 160, 185);
    range("drum and bass", 160, 185);
    range("Drum'n'Bass", 160, 185);
    range("DnB", 160, 185);
    range("D&B", 160, 185);
    range("DRUM N BASS", 160, 185);
    range("Electronic---Drum n Bass", 160, 185);
    range("Liquid Funk", 160, 185);      // before funk
    range("Neurofunk", 160, 185);
    range("Halftime", 160, 185);
    range("Jungle", 150, 180);
    range("Ragga Jungle", 150, 180);
    range("Footwork", 150, 170);
    range("Juke", 150, 170);
    range("Dubstep", 135, 150);
    range("Riddim", 135, 150);
    range("Grime", 130, 150);
    range("Trap", 0, 0);                 // either octave
    range("Hip Hop---Trap", 0, 0);       // trap before hip hop
    range("Drill", 0, 0);
    range("Hip Hop", 80, 115);
    range("Hip-Hop", 80, 115);
    range("hiphop", 80, 115);
    range("Boom Bap", 80, 115);
    range("Hardcore Hip-Hop", 80, 115);  // before hardcore
    range("G-Funk", 80, 115);            // before funk
    range("Pop Rap", 80, 115);
    range("Trip Hop", 0, 0);
    range("House", 115, 130);
    range("Deep House", 115, 130);
    range("Tech House", 115, 130);       // not techno
    range("Electro House", 115, 130);    // not electro
    range("Bass House", 115, 130);       // not bass
    range("Garage House", 115, 130);     // house, not garage
    range("Tropical House", 95, 120);
    range("Hard House", 135, 155);
    range("Ambient House", 0, 0);
    range("Techno", 120, 150);
    range("Minimal Techno", 120, 150);
    range("Hard Techno", 135, 170);      // before techno
    range("Hardcore Techno", 160, 230);  // hardcore, not hard techno
    range("Trance", 125, 150);
    range("Psy-Trance", 135, 150);
    range("Psytrance", 135, 150);
    range("Hardstyle", 140, 165);
    range("Gabber", 160, 230);
    range("Hardcore", 160, 230);
    range("Happy Hardcore", 160, 230);
    range("Rock---Hardcore", 0, 0);      // hardcore punk
    range("Speedcore", 200, 380);
    range("Uptempo Hardcore", 185, 250);
    range("Garage", 125, 140);
    range("UK Garage", 125, 140);
    range("UKG", 125, 140);
    range("2-Step", 125, 140);
    range("Garage Rock", 0, 0);
    range("Speed Garage", 125, 140);
    range("Bassline", 130, 145);
    range("Breakbeat", 120, 150);
    range("Breaks", 120, 150);
    range("Breakcore", 0, 0);
    range("Disco", 105, 130);
    range("Nu-Disco", 105, 130);
    range("Funk / Soul---Disco", 105, 130);
    range("Funk / Soul---Funk", 80, 130); // by its style
    range("Funk / Soul---Afrobeat", 95, 125);
    range("Funk / Soul", 0, 0);
    range("Funk", 80, 130);
    range("Baile Funk", 0, 0);
    range("UK Funky", 120, 135);
    range("Reggaeton", 85, 105);
    range("Dancehall", 85, 110);
    range("Afrobeat", 95, 125);
    range("Afrobeats", 95, 125);
    range("Afro House", 115, 130);
    range("Amapiano", 105, 120);
    range("Moombahton", 100, 120);
    range("Classical---Minimal", 0, 0);
    range("Minimal", 115, 135);
    range("Downtempo", 0, 0);
    range("Ambient", 0, 0);
    range("Pop", 0, 0);
    range("Rock", 0, 0);
    range("Synth-pop", 0, 0);
    range("Electronic", 0, 0);
    range("", 0, 0);

    printf("every range less than an octave wide (with the leeway), so one octave at most lands in it:\n");
    bool narrow = true;
    for (const detail::Rule& r : detail::kRules)
        if (r.hi > 0 && !(r.hi * 1.01 < 2 * r.lo * 0.99)) {
            printf("  too wide: %s %.0f-%.0f\n", r.word, r.lo, r.hi);
            narrow = false;
        }
    check(narrow, "all ranges");

    printf("octaves:\n");
    octave("Drum n Bass", 83, 166, Source::BeatThis, 166);     // Liked Songs: rekordbox 166, Beat This! 83 (half)
    octave("Drum n Bass", 89, 178, Source::BeatThis, 178);
    octave("Jungle", 75, 150, Source::BeatThis, 150);
    octave("Drum n Bass", 83, 0, Source::BeatThis, 166);       // rekordbox's tempo unknown (the early grid)
    octave("Drum n Bass", 86, 86, Source::BeatThis, 172);      // both at half: doubled all the same
    octave("Drum n Bass", 174, 87, Source::Today, 0);          // Beat This! already right
    octave("House", 63.5, 127, Source::BeatThis, 127);
    octave("House", 63.5, 0, Source::BeatThis, 127);
    octave("Hip Hop", 180, 90, Source::BeatThis, 90);
    octave("Hip Hop", 180, 180, Source::BeatThis, 90);
    octave("Dubstep", 140, 70, Source::Today, 0);              // stays
    octave("Dubstep", 70, 140, Source::BeatThis, 140);
    octave("Techno", 128, 128, Source::Today, 0);
    octave("Ambient", 60, 120, Source::Today, 0);              // no range
    octave("Polka", 83, 166, Source::Today, 0);                // unknown genre
    octave("", 83, 166, Source::Today, 0);
    octave("Trap", 70, 140, Source::Today, 0);
    octave("Drum n Bass", 164, 109.33, Source::Today, 0);      // Liked Songs 3:2: Beat This! 164 already in range
    octave("Drum n Bass", 109.33, 164, Source::Rekordbox, 164); // the other way round: rekordbox's grid
    octave("Drum n Bass", 128, 170, Source::Today, 0);         // "other": the genre never brings in rekordbox's tempo
    octave("House", 100, 150, Source::Today, 0);               // 3:2, neither in range
    octave("Drum n Bass", 120, 120, Source::Today, 0);         // no octave lands in it
    octave("Hip Hop", 92, 184, Source::Today, 0);              // in range: rekordbox's octave doesn't matter
    octave("Drum n Bass", 0, 166, Source::Today, 0);           // no Beat This! tempo
    octave("Hardcore", 104.99, 210, Source::BeatThis, 209.98); // uptempo hardcore (the 11-18 set)
    octave("Hardcore", 100, 200, Source::BeatThis, 200);
    octave("Speedcore", 104.99, 210, Source::BeatThis, 209.98);
    octave("Jungle", 95, 190, Source::BeatThis, 190);          // near miss (150-180), rekordbox found it too
    octave("Jungle", 95, 95, Source::Today, 0);                // near miss, but only Beat This!'s octave says so
    octave("Jungle", 105, 210, Source::Today, 0);              // 17% over: too far
    octave("House", 135, 67.5, Source::Today, 0);              // the near one is Beat This!'s own: stays
    octave("Hip Hop", 122, 61, Source::Today, 0);              // 61 is no nearer hip hop than 122

    printf(failures ? "%d FAILED\n" : "all ok\n", failures);
    return failures ? 1 : 0;
}
