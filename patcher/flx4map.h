// The DDJ-FLX4 mapping the flx4 mod needs, in C++ for the patcher app: rekordbox's own mapping for four decks and
// with stems. A port of tools/build/flx4_mapping.py (same output, byte for byte; what the rows are is explained
// there); keep the two, and mods/flx4.cpp's notes, in sync.
#pragma once
#include <stdio.h>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace flx4map {

using Row = std::vector<std::string>;

// One line's fields, as Python's csv.reader reads them: a field starting with " is quoted ("" inside is a "); a "
// anywhere else is just a character. false: a quoted field runs on to the next line (not handled).
inline bool split(const std::string& line, std::vector<std::string>* out) {
    out->assign(1, "");
    bool quoted = false, start = true;
    for (size_t i = 0; i < line.size(); i++) {
        char c = line[i];
        if (quoted) {
            if (c != '"') out->back() += c;
            else if (i + 1 < line.size() && line[i + 1] == '"') out->back() += '"', i++;
            else quoted = false;
        } else if (c == ',') {
            out->emplace_back(), start = true;
            continue;
        } else if (c == '"' && start) {
            quoted = true;
        } else {
            out->back() += c;
        }
        start = false;
    }
    return !quoted;
}

inline std::string hex2(int v) {
    char b[8];
    snprintf(b, sizeof b, "%02X", v);
    return b;
}

// rekordbox's DDJ-FLX4.midi.csv (as read from the file) -> the extended mapping. "" if it isn't the mapping we know
// (*why says what).
inline std::string make(std::string src, std::string* why, const std::string& device = "DDJ-FLX4") {
    if (src.compare(0, 3, "\xEF\xBB\xBF") == 0) src.erase(0, 3);  // utf-8-sig
    std::vector<Row> rows;
    size_t p = 0;
    while (p < src.size()) {
        size_t e = src.find('\n', p);
        std::string line = src.substr(p, e == std::string::npos ? std::string::npos : e - p);
        p = e == std::string::npos ? src.size() : e + 1;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        Row r;
        if (!line.empty() && !split(line, &r)) {
            if (why) *why = "a quoted field over several lines in the mapping";
            return "";
        }
        rows.push_back(r);
    }
    if (rows.empty() || rows[0].empty() || rows[0][0].rfind("@file", 0) != 0) {
        if (why) *why = "not a rekordbox MIDI mapping";
        return "";
    }

    static const std::pair<const char*, const char*> kDecks[] = {{"0,1", "0,1,2,3"}, {"7,9", "7,9,11,13"}, {"8,10", "8,10,12,14"}};
    std::set<std::pair<int, int>> used;  // (status, data1) the mapping already takes
    std::vector<Row> out;
    for (Row r : rows) {
        if (r.empty()) { out.push_back(r); continue; }
        if (r[0].rfind("@file", 0) == 0) {
            Row f = {"@file", "1", device};
            f.resize(15);
            out.push_back(f);
            continue;
        }
        if (r.size() < 15) r.resize(15);
        if (r[0].rfind("#", 0) != 0) {
            for (int col : {3, 8}) {
                if (r[col].size() != 4) continue;
                int st = (int)strtol(r[col].substr(0, 2).c_str(), nullptr, 16), n = (int)strtol(r[col].substr(2).c_str(), nullptr, 16);
                std::vector<int> decks;
                for (int i = col + 1; i < col + 5; i++)
                    if (!r[i].empty() && r[i].find_first_not_of("0123456789") == std::string::npos) decks.push_back(atoi(r[i].c_str()));
                if (decks.empty()) decks.push_back(st & 15);
                for (int dk : decks) used.insert({(st & 0xF0) | dk, n});
            }
        }
        for (int col : {4, 9}) {  // input / output deck columns
            std::string k = r[col] + "," + r[col + 1];
            for (auto& [from, to] : kDecks)
                if (k == from && r[col + 2].empty() && r[col + 3].empty()) {
                    Row t;
                    split(to, &t);
                    for (int i = 0; i < 4; i++) r[col + i] = t[i];
                }
        }
        if (r[1] == "Load" && r[4] == "9646" && r[5] == "9647") r[6] = "9648", r[7] = "9649";
        if (r[0] == "LoadedIndicator") r[11] = "9F02", r[12] = "9F03";
        out.push_back(r);
    }

    std::string err;
    auto take = [&](int st, int n) {
        if (!used.insert({st, n}).second && err.empty()) err = hex2(st) + hex2(n) + " is already in the FLX4 mapping";
    };
    auto row = [](std::string name, std::string function, std::string kind, std::string inp, std::vector<std::string> decks,
                  std::string outp = "", std::vector<std::string> outDecks = {"", "", "", ""}, std::string option = "",
                  std::string comment = "") {
        Row r = {name, function, kind, inp};
        r.insert(r.end(), decks.begin(), decks.end());
        r.push_back(outp);
        r.insert(r.end(), outDecks.begin(), outDecks.end());
        r.push_back(option), r.push_back(comment);
        return r;
    };
    // The mod's own messages, on the deck's channel. Keep in sync with mods/flx4.cpp.
    static const std::pair<const char*, int> kStemNotes[] = {{"Vocal", 0x30}, {"Inst", 0x31}, {"Bass", 0x32}, {"Drums", 0x33}};
    static const std::pair<const char*, int> kFxPartNotes[] = {{"Vocal", 0x38}, {"Inst", 0x39}, {"Bass", 0x3A}, {"Drums", 0x3B}};
    static const std::pair<const char*, int> kIsolatorCcs[] = {{"Vocal", 0x08}, {"Inst", 0x0C}, {"Drums", 0x10}};
    const int kIsolatorNote = 0x34, kStemsNote = 0x35, kDeckSelectNote = 0x72, kDeckStateNote = 0x3C, kFxAssignNote = 0x1C;
    const std::vector<std::string> all4 = {"0", "1", "2", "3"}, none = {"", "", "", ""};
    auto upper = [](std::string s) { for (char& c : s) c = (char)toupper((unsigned char)c); return s; };

    Row head = {"# platendoos flx4 (mods/flx4.mm): decks 3/4 as on the DDJ-FLX6, and stems"};
    head.resize(15);
    std::vector<Row> extra = {
        head,
        row("Deck", "DeckSelect", "Button", "90" + hex2(kDeckSelectNote), all4, "", none, "RO", "Select Deck"),
        row("DeckState", "DeckState", "Button", "90" + hex2(kDeckStateNote), all4, "", none, "RO", "LED State of the DECK 1/2/3/4"),
        row("CFXParameterCH3", "CFXParameterCH3", "KnobSliderHiRes", "B619", none, "", none, "Fast", "CH3 CFX Depth"),
        row("CFXParameterCH4", "CFXParameterCH4", "KnobSliderHiRes", "B61A", none, "", none, "Fast", "CH4 CFX Depth"),
        row("Stems", "", "Button", "90" + hex2(kStemsNote), all4, "90" + hex2(kStemsNote), all4, "Fast", "STEMS Mode On/Off"),
        row("PartIsolator", "", "Button", "90" + hex2(kIsolatorNote), all4, "90" + hex2(kIsolatorNote), all4, "", "PART ISOLATOR On/Off"),
    };
    for (int c = 0; c < 4; c++) {
        take(0x90 | c, kDeckSelectNote), take(0x90 | c, kDeckStateNote);
        take(0x90 | c, kStemsNote), take(0x90 | c, kIsolatorNote);
    }
    take(0xB6, 0x19), take(0xB6, 0x1A);
    for (auto& [stem, n] : kStemNotes) {
        extra.push_back(row(std::string("ActivePart") + stem, "", "Button", "90" + hex2(n), all4, "90" + hex2(n), all4, "",
                            "ACTIVE PART " + upper(stem)));
        for (int c = 0; c < 4; c++) take(0x90 | c, n);
    }
    for (auto& [stem, n] : kFxPartNotes) {  // one setting for all decks: a row per deck's channel
        for (int c = 0; c < 4; c++) {
            std::string m = "9" + std::string(1, "0123456789ABCDEF"[c]) + hex2(n);
            extra.push_back(row(std::string("FXPartSelect") + stem + "On", "", "Button", m, none, m, none, "", "FX PART SELECT " + upper(stem)));
            take(0x90 | c, n);
        }
    }
    for (int unit = 1; unit <= 2; unit++) {  // FX<unit>Assign.CH1-4
        for (int c = 0; c < 4; c++) {
            std::string m = "9" + std::string(1, "0123456789ABCDEF"[3 + unit]) + hex2(kFxAssignNote + c);
            extra.push_back(row("FX" + std::to_string(unit) + "Assign.CH" + std::to_string(c + 1), "", "Button", m, none, "", none, "",
                                "FX" + std::to_string(unit) + " on CH" + std::to_string(c + 1)));
            take(0x93 + unit, kFxAssignNote + c);
        }
    }
    for (auto& [stem, cc] : kIsolatorCcs) {
        extra.push_back(row(std::string("PartIsolator") + stem, "", "KnobSliderHiRes", "B0" + hex2(cc), all4, "", none, "Fast",
                            "PART ISOLATOR " + upper(stem)));
        for (int c = 0; c < 4; c++) take(0xB0 | c, cc), take(0xB0 | c, cc + 0x20);
    }
    if (!err.empty()) {
        if (why) *why = err;
        return "";
    }

    // As Python's csv.writer (an empty row, a blank line in the source, is written as an empty line).
    std::string text;
    auto write = [&](const Row& r) {
        for (size_t i = 0; i < r.size(); i++) {
            std::string f = r[i];
            if (f.find_first_of(",\"\r\n") != std::string::npos) {  // quoted, as csv.writer does
                std::string q = "\"";
                for (char c : f) q += c == '"' ? std::string("\"\"") : std::string(1, c);
                f = q + "\"";
            }
            text += (i ? "," : "") + f;
        }
        text += "\r\n";
    };
    for (const Row& r : out) write(r);
    for (const Row& r : extra) write(r);
    return text;
}

}  // namespace flx4map
