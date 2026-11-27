// usbphrases: phrase blocks (PSSI: intro, up, chorus, down, outro, or the high/mid/low mood's names) under the
// waveform for a track loaded on a deck straight from a USB export, in Performance mode. rekordbox shows them only
// for collection tracks: a deck's phrases come from SongStructInformation::loadSongStructInfoFromDb(song id), which
// asks DatabaseMediator::getSongStructInfo, which reads the main (collection) controller's row of that id and only
// uses it for a local track (row type 1 or 6, flag 0x402 bit 0). A device track has none of that, so its deck
// stays empty, though the export's .EXT file holds the phrases (masked, as an export writes them).
// With this, when a deck holds a device track (MovableItemBasicInfo browse type 2) and rekordbox shows phrases
// (shouldLoadSongStruct: phrase display on, enlarged or full, or lighting):
//  - the device row's analysis path (ANLZ0000.DAT on the stick) gives its .EXT; it is read on a worker thread, its
//    PSSI unmasked (the export's XOR mask, rekordbox's analyzer::ConvUSB_ScrambleSongStruct, undone) and written as a
//    copy to <configDir>/usbphrases/deckN.EXT;
//  - then, on the main thread, loadSongStructInfoFromDb is called with a made-up song id (0xFFFFFF00 + deck) while
//    the main controller's getRowDataTrack (vtable slot) answers that id with a row that points at the copy: rekordbox
//    parses and shows the phrases itself;
//  - outside that call the made-up ids are refused (getRowDataTrack returns false, as for an unknown id). Every phrase
//    write goes writeSongStructInfoToDb → DatabaseIF::setSongStructInfo → DatabaseMediator::setSongStructInfo, which
//    first asks the same slot for the row and writes nothing without one: an edit of these phrases on the deck is
//    never saved, and nothing is written to the collection (master.db or its analysis files). Every other id goes
//    to rekordbox's own function unchanged.
// Visibility switched off clears the deck's phrases (rekordbox's own changeListenerCallback); switched on again they
// are fed again. A deck that loads another track gets that track's phrases (rekordbox's own load, or this again).
//   config: usbphrases=0 for rekordbox's own behaviour (at the next start)
#include "../core/rbmod.h"
#include "../core/plat.h"
#include <atomic>
#include <string.h>
#include <string>
#include <vector>

using namespace rbm;

namespace {

volatile bool g_on = true;

// The decks' items (what is loaded, and from where): PlayerAccessor::getMovableItemBasicInfo(ePlayerID), the UiPlayer's
// db::MovableItemBasicInfo. +0 browse type (2: a device), +4 the device's handle (read together by an ldp in
// DjplayDB::createObject, which no recipe kind decodes, so plain constants; the same in every build looked at), +0x20
// the track's id on that device.
const Addr A_deckItem{"__ZN6browse14PlayerAccessor23getMovableItemBasicInfoEN6djplay9ePlayerIDE"};
const size_t O_itemType = 0x0, O_itemDevice = 0x4;
const Offset O_itemId{0x20, "__ZN6djplay8DjplayDB12createObjectEPN2db20MovableItemBasicInfoE", "ldr", 2};
const int kBrowseDevice = 2;

// The decks' phrases: SongStructInfoManager::getInformation(deck) → djplay::SongStructInformation.
// (the holder as a "SingletonHolder<...>" Addr: on Windows get() takes no holder, and the table names the global its
// get() uses, tools/win/maketable.py)
const Addr A_infoHolder{"SingletonHolder<SongStructInfoManager>", [] { return symbol("__ZN6djplay21SongStructInfoManager15singletonHolderE"); }};
const Addr A_infoMgrGet{"__ZN4juce15SingletonHolderIN6djplay21SongStructInfoManagerENS_15CriticalSectionELb0EE3getEv"};
const Addr A_getInfo{"__ZN6djplay21SongStructInfoManager14getInformationEi"};
const Addr A_loadFromDb{"__ZN6djplay21SongStructInformation24loadSongStructInfoFromDbEj"};
const Addr A_shouldLoad{"__ZNK6djplay21SongStructInformation20shouldLoadSongStructEv"};
const Addr A_clear{"__ZN6djplay21SongStructInformation19clearSongStructInfoEv"};
// Only for the "edit" test command: its file name keeps a build that lacks them from switching the mod off
// (core/symbols.cpp modOf).
const Addr A_setPhraseId{"__ZN6djplay21SongStructInformation11setPhraseIdEii", intptr_t(0), "usbphrases_testcommand"};
const Addr A_saveIfNeeded{"__ZN6djplay21SongStructInformation26saveSongStructInfoIfNeededEv", intptr_t(0), "usbphrases_testcommand"};
const Offset O_infoSong{0x48, "__ZNK6djplay21SongStructInformation9getSongIdEv", "ldr", 0};
const Offset O_infoCount{0x64, "__ZNK6djplay21SongStructInformation22getTotalNumberOfPhraseEv", "ldr", 0};

// The database: DatabaseIF (its first field the DatabaseMediator), a device's row, the main controller and the row
// fields DatabaseMediator::getSongStructInfo reads (all from that function: its controller call, then the checks).
const Addr A_dbHolder{"SingletonHolder<DatabaseIF>", [] { return singleton("__ZN11DetailColorC2Ev", "__ZN4juce15SingletonHolderIN2db10DatabaseIFENS_15CriticalSectionELb0EE3getEv"); }};
const Addr A_dbGet{"__ZN4juce15SingletonHolderIN2db10DatabaseIFENS_15CriticalSectionELb0EE3getEv"};
const Addr A_getRow{"__ZN2db10DatabaseIF15getRowDataTrackEjRNS_12RowDataTrackENS_12_browse_typeEl"};
const Addr A_mainCtl{"__ZN2db16DatabaseMediator16mainDBControllerEv"};
const Addr A_rowCtor{"__ZN2db12RowDataTrackC2Ev"};
const Addr A_rowDtor{"__ZN2db12RowDataTrackD2Ev"};
const Addr A_strAssign{"__ZN4juce6StringaSERKS0_"};  // juce::String::operator=(const String&)
const size_t S_rowDataTrack = 0x4e0;
const char* const kStruct = "__ZN2db16DatabaseMediator17getSongStructInfoEjRN8analyzer14SongStructInfoE";
const Offset V_ctlGetRow{0x48, kStruct, "ldr", 1};      // the controller's getRowDataTrack(unsigned, RowDataTrack&) slot
const Offset O_rowType{0x50, kStruct, "ldr", 2};        // 1 or 6: a local track
#ifndef _WIN32
const Offset O_rowFlags{0x402, kStruct, "ldr", 3};      // bit 0: analysed here
#else
// MSVC reads the same flag as bit 16 of the dword at 0x400 ("test dword [row+0x400], 0x10000"), never as a byte, so
// the table can only find the dword's offset (tools/win/verify/usbphrases.out.json)
const Offset O_rowFlagsWord{0x400, kStruct, "win:row flags dword", 0};
#endif
const Offset O_rowAnalysis{0x3f0, kStruct, "add", 5};   // juce::String: the analysis file (ANLZ0000.DAT)

const unsigned kMagic = 0xFFFFFF00u;  // + deck: the made-up song ids (a collection's ids are far below)
const int kDecks = 4;

void* dbif() { return at<void* (*)(uintptr_t)>(A_dbGet)(A_dbHolder + g_slide); }
char* songInfo(int deck) {
    void* mgr = at<void* (*)(uintptr_t)>(A_infoMgrGet)(A_infoHolder + g_slide);
    return mgr ? at<char* (*)(void*, int)>(A_getInfo)(mgr, deck) : nullptr;
}

// ---- the controller's getRowDataTrack --------------------------------------------------------------------------

using GetRowFn = bool (*)(void*, unsigned, char*);
GetRowFn g_origGetRow;
std::atomic<int> g_feedDeck{-1};  // the deck being fed (main thread, during loadSongStructInfoFromDb)
std::string g_feedPath;           // its copy, as ".DAT" (rekordbox reads the ".EXT" beside it)

bool getRow(void* ctl, unsigned id, char* row) {
    if (id - kMagic < 0x100) {
        int deck = g_feedDeck.load();
        if (g_on && deck >= 0 && id == kMagic + (unsigned)deck) {
            *(int*)(row + O_rowType) = 1;
#ifndef _WIN32
            *(unsigned char*)(row + O_rowFlags) |= 1;
#else
            *(uint32_t*)(row + O_rowFlagsWord) |= 0x10000;
#endif
            JStr p(g_feedPath.c_str());
            at<void (*)(void*, const JStr*)>(A_strAssign)(row + O_rowAnalysis, &p);
            return true;
        }
        static std::atomic<int> logged{0};
        if (logged++ < 20) logf("usbphrases: no row for deck %u's phrases outside a load (a save of an edit: not written)", id - kMagic + 1);
        return false;
    }
    return g_origGetRow(ctl, id, row);
}

bool patchController() {
    void** db = (void**)dbif();
    if (!db || !*db) return false;
    void* ctl = at<void* (*)(void*)>(A_mainCtl)(*db);
    if (!ctl) return false;
    void** slot = *(void***)ctl + V_ctlGetRow / sizeof(void*);
    if (*slot == (void*)&getRow) return true;
    void* old = patchSlot(slot, (void*)&getRow);
    if (!old) {
        logf("usbphrases: could not patch the controller's getRowDataTrack: no phrases for device tracks");
        g_on = false;
        return false;
    }
    g_origGetRow = (GetRowFn)old;
    return true;
}

// ---- the export's .EXT ----------------------------------------------------------------------------------------

uint32_t be32(const unsigned char* p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }

// Unmasks the file's PSSI tag in place (an export masks it from byte 18 on: XOR with "Kanzen-niRikaishita" ^ 0x80
// plus the entry count's low byte; a mood above 20 says it is masked, as crate-digger and rekordbox's own
// ConvUSB_ScrambleSongStruct have it). The number of phrases, -1 if the file has no PSSI.
int unmaskPssi(std::string& f) {
    static const unsigned char kKey[19] = {'K', 'a', 'n', 'z', 'e', 'n', '-', 'n', 'i', 'R', 'i', 'k', 'a', 'i', 's', 'h', 'i', 't', 'a'};
    auto* b = (unsigned char*)f.data();
    if (f.size() < 28 || memcmp(b, "PMAI", 4)) return -1;
    size_t o = be32(b + 4);
    while (o + 12 <= f.size()) {
        uint32_t len = be32(b + o + 8);
        if (len < 12 || o + len > f.size()) break;
        if (!memcmp(b + o, "PSSI", 4) && len >= 0x20) {
            unsigned n = b[o + 0x11];
            unsigned mood = b[o + 0x12] << 8 | b[o + 0x13];
            if (mood > 20)
                for (size_t i = 18; i < len; i++) b[o + i] ^= (unsigned char)((kKey[(i - 18) % 19] ^ 0x80) + n);
            return b[o + 0x10] << 8 | b[o + 0x11];
        }
        o += len;
    }
    return -1;
}

// ---- the decks ------------------------------------------------------------------------------------------------

struct Deck {
    int dev = -1;           // the device track fed or being read (dev, id)
    unsigned id = 0;
    bool reading = false;   // a worker reads its .EXT
    bool ready = false;     // its copy is written
    bool none = false;      // it has no phrases (no .EXT, no PSSI): not tried again
    bool fed = false;
};
Deck g_decks[kDecks];

std::string copyBase(int deck) { return plat::configDir() + "/usbphrases/deck" + std::to_string(deck + 1); }

// The device row's analysis file (main thread), "" if none.
std::string analysisPath(int dev, unsigned id) {
    alignas(16) char row[S_rowDataTrack + 0x100];
    at<void (*)(void*)>(A_rowCtor)(row);
    bool ok = at<bool (*)(void*, unsigned, void*, int, long)>(A_getRow)(dbif(), id, row, kBrowseDevice, dev);
    const char* p = *(const char**)(row + O_rowAnalysis);
    std::string path = ok && p ? p : "";
    at<void (*)(void*)>(A_rowDtor)(row);
    return path;
}

void feed(int deck, char* info) {
    Deck& d = g_decks[deck];
    g_feedPath = copyBase(deck) + ".DAT";
#ifdef _WIN32
    for (char& c : g_feedPath)
        if (c == '/') c = '\\';  // (rekordbox's own paths there use backslashes)
#endif
    g_feedDeck = deck;
    at<void (*)(void*, unsigned)>(A_loadFromDb)(info, kMagic + deck);
    g_feedDeck = -1;
    d.fed = true;
    int n = *(int*)(info + O_infoCount);
    logf("usbphrases: deck %d: %d phrases of device %d's track %u", deck + 1, n, d.dev, d.id);
    if (n <= 0) d.none = true;
}

void start(int deck, int dev, unsigned id) {
    Deck& d = g_decks[deck];
    d = Deck{};
    d.dev = dev;
    d.id = id;
    std::string dat = analysisPath(dev, id);
    size_t dot = dat.rfind('.');
    if (dot == std::string::npos || dat.find('/', dot) != std::string::npos) {
        d.none = true;
        logf("usbphrases: deck %d: device %d's track %u has no analysis file", deck + 1, dev, id);
        return;
    }
    d.reading = true;
    std::string ext = dat.substr(0, dot) + ".EXT", copy = copyBase(deck) + ".EXT";
    plat::background([deck, dev, id, ext, copy] {
        std::string f;
        int n = plat::readFile(ext, &f) ? unmaskPssi(f) : -2;
        bool ok = n > 0;
        if (ok) {
            std::error_code ec;
            std::filesystem::create_directories(plat::fsPath(plat::configDir() + "/usbphrases"), ec);
            ok = plat::writeFileAtomic(copy, f.data(), f.size());
        }
        plat::onMain([deck, dev, id, n, ok, ext] {
            Deck& d = g_decks[deck];
            if (d.dev != dev || d.id != id || !d.reading) return;  // the deck moved on meanwhile
            d.reading = false;
            if (ok) {
                d.ready = true;
            } else {
                d.none = true;
                logf("usbphrases: deck %d: no phrases in %s (%s)", deck + 1, ext.c_str(),
                     n == -2 ? "can't read it" : n == -1 ? "no PSSI" : n == 0 ? "none" : "can't write the copy");
            }
        });
    });
}

// Test commands (command-usbphrases in the config folder): "state" | "edit N" (deck N's second phrase to another kind
// and a save, as an edit on the deck does: the save must find no row).
void testCommand() {
    std::string c = devCommand("usbphrases");
    if (c.empty()) return;
    if (c == "state") {
        for (int i = 0; i < kDecks; i++) {
            char* item = at<char* (*)(int)>(A_deckItem)(i);
            char* info = songInfo(i);
            Deck& d = g_decks[i];
            logf("usbphrases: deck %d item type %d dev %d id %u; song %x phrases %d shown %d; fed dev %d id %u reading %d ready %d none %d fed %d", i + 1,
                 item ? *(int*)(item + O_itemType) : -1, item ? *(int*)(item + O_itemDevice) : -1, item ? *(unsigned*)(item + O_itemId) : 0,
                 info ? *(unsigned*)(info + O_infoSong) : 0, info ? *(int*)(info + O_infoCount) : -1,
                 info ? at<bool (*)(void*)>(A_shouldLoad)(info) : -1, d.dev, d.id, d.reading, d.ready, d.none, d.fed);
        }
    } else if (!c.compare(0, 5, "edit ")) {
        int deck = atoi(c.c_str() + 5) - 1;
        char* info = deck >= 0 && deck < kDecks ? songInfo(deck) : nullptr;
        if (!info || !A_setPhraseId || !A_saveIfNeeded) return;
        logf("usbphrases: edit: deck %d (song %x) phrase 2 → kind 2, then a save", deck + 1, *(unsigned*)(info + O_infoSong));
        at<void (*)(void*, int, int)>(A_setPhraseId)(info, 1, 2);
        at<void (*)(void*)>(A_saveIfNeeded)(info);
    }
}

void tick() {
    static double last;
    double t = now();
    if (!g_on || t - last < 0.25) return;
    last = t;
    static bool patched;
    if (!patched && !(patched = patchController())) return;
    testCommand();
    for (int i = 0; i < kDecks && i < deckCount(); i++) {
        char* item = at<char* (*)(int)>(A_deckItem)(i);
        char* info = songInfo(i);
        if (!info) continue;
        unsigned song = *(unsigned*)(info + O_infoSong);
        int count = *(int*)(info + O_infoCount);
        Deck& d = g_decks[i];
        bool device = item && *(int*)(item + O_itemType) == kBrowseDevice && *(unsigned*)(item + O_itemId);
        if (!device) {
            if (song == kMagic + (unsigned)i && count > 0) {  // ours, left from a device track
                at<void (*)(void*)>(A_clear)(info);
                logf("usbphrases: deck %d: cleared the device track's phrases", i + 1);
            }
            if (d.dev >= 0) d = Deck{};
            continue;
        }
        int dev = *(int*)(item + O_itemDevice);
        unsigned id = *(unsigned*)(item + O_itemId);
        if (dev != d.dev || id != d.id) {
            if (song == kMagic + (unsigned)i && count > 0) {  // the previous device track's: gone while this one is read
                at<void (*)(void*)>(A_clear)(info);
                count = 0;
            }
            d = Deck{};
            d.dev = dev;
            d.id = id;
        }
        if (d.none || d.reading) continue;
        if (!at<bool (*)(void*)>(A_shouldLoad)(info)) continue;  // not shown: nothing to load (rekordbox's own rule)
        if (count > 0 && song != kMagic + (unsigned)i) continue;  // rekordbox has phrases of its own for it
        if (count > 0 && d.fed) continue;                         // ours, shown
        if (!d.ready) {
            start(i, dev, id);
            continue;
        }
        feed(i, info);
    }
}

void init() {
    if (g_on) logf("usbphrases: phrases for device tracks on the decks");
}

const rbm::settings::Area kArea{"decks", "Decks & sound",
    "What you hear and mix: inputs, outputs, the mixer's effects.",
    "<circle cx=\"8\" cy=\"8\" r=\"6\"/><circle cx=\"8\" cy=\"8\" r=\"1.6\"/>", 10};

}  // namespace

RBM_MOD({.name = "usbphrases", .init = init, .tick = tick, .enabled = &g_on,
         .title = "Phrases from USB sticks",
         .about = "A track loaded straight from a USB stick shows its phrases under the waveform, as a collection track does. "
                  "They come from the stick's export; nothing is copied into your collection, and phrase edits on such a deck are not saved.",
         .area = "decks", .order = 60});
