// flx4: a Pioneer DDJ-FLX4 for four decks and stems, on SHIFT combinations the FLX4 leaves unused:
//   SHIFT + PLAY          that side switches deck: left 1 <-> 3, right 2 <-> 4 (its mixer channel and beat FX too)
//   SHIFT + LOOP IN       the pads become STEMS pads: 1-4 vocal, inst, bass, drums on/off (lit: playing),
//                         5-8 which stems the beat FX applies to; any pad mode button goes back
//   SHIFT + LOOP OUT      the EQ knobs control the stems instead (HI vocal, MID inst, LOW drums); per mixer channel,
//                         as rekordbox's PART ISOLATOR: a side switching deck takes the new channel's
//   SHIFT + SMART FADER   rekordbox's 2 or 4 deck layout
// The beat FX CH SELECT lever (1, 2, 1&2) picks sides, not channels: 1 is the deck the left side controls, 2 the
// right side's, so the beat FX follows a side to deck 3 or 4 (the FLX4 drives FX1 at 1, FX2 at 2).
// Each switch shows briefly over rekordbox's window. A side's mixer strip (TRIM, EQ, CFX filter, fader) controls its
// deck's channel; with four decks rekordbox's on-screen mixer opens, showing every channel's settings, and the two
// channels the strips control are outlined there. Absolute controls (tempo, faders, knobs) only take over a deck once
// moved to where that deck has them (as on screen), so nothing jumps; until then the on-screen mixer marks where the
// FLX4's knob or fader is (orange).
// How: the FLX4 talks on channels 1/2 for its decks; its four-deck sibling, the DDJ-FLX6, uses 3/4 for decks 3/4. The
// mod rewrites a switched side's messages to those channels (and back for its LEDs), and build.sh gives
// rekordbox an FLX4 mapping with the FLX6's deck 3/4 columns plus rows for stems (tools/flx4_mapping.py).
//   config: flx4=0 to disable; flx4_mixer=fixed keeps the mixer channels on decks 1/2;
//           flx4_device=<MIDI device> (default DDJ-FLX4; for testing with another device using the same mapping);
//           flx4_log=1 to log every MIDI message in and out (level meters: counted per channel instead)
//           flx4_keep_tempo=1   0: a deck a side switches to takes the tempo rekordbox remembers its fader at
//   development: flx4_inject=93 1E 7F,93 1E 00 sends messages to rekordbox as if from the controller (then cleared);
//           flx4_dump=1 logs the on-screen mixer's components (then cleared)
#include "../core/rbmod.h"
#include "flx4_overlay.h"
#include <stdarg.h>
#include <string.h>
#include <algorithm>
#include <atomic>
#include <climits>
#include <deque>
#include <cmath>
#include <map>
#include <mutex>
#include <new>
#include <vector>

using namespace rbm;

namespace {

// A juce::String passed BY VALUE to a rekordbox function. Itanium (mac): the caller destroys it. MSVC (Windows): the
// callee does (verified in DeviceComponent's constructor: it destroys its parameter), so ours must not be destroyed again.
struct StrArg {
    explicit StrArg(const char* s) : p(new (buf) JStr(s)) {}
    ~StrArg() {
#ifndef _WIN32
        p->~JStr();
#endif
    }
    StrArg(const StrArg&) = delete;
    JStr* get() { return p; }
    alignas(JStr) char buf[sizeof(JStr)];
    JStr* p;
};

// juce::MidiMessage: its bytes inline (up to 8) or behind a pointer, then the timestamp and the size.
struct MidiMessage {
    union {
        uint8_t* data;
        uint8_t inl[8];
    };
    double timestamp;
    int size;
    uint8_t* bytes() { return size > 8 ? data : inl; }
};

// A side of the controller (0 left, 1 right).
struct Side {
    std::atomic<int> deck;               // 0-3
    std::atomic<bool> stems{false};      // pads are STEMS pads
    std::atomic<bool> shift{false};
    std::atomic<int> padMode{0};         // the FLX4's pad mode (the high nibble of its pad notes)
    std::atomic<int> phys[128];          // last physical value of each CC (MSB), -1 unknown (set on the MIDI thread)
    std::atomic<bool> engaged[128];      // that control has picked its deck up and passes through
    bool dropLsb[128];                   // the MSB was held back by pickup, so its LSB is too (MIDI thread)
    // A held note's release goes where its press went, also after a deck switch (MIDI thread). [deck channel, pads,
    // shifted pads][note]: 0 not held, else 1 + the deck, + 4 if it was a STEMS pad.
    int8_t held[3][128];
};

// The mixer controls of a channel, as rekordbox's on-screen mixer shows them.
enum Control { C_TRIM, C_HI, C_MID, C_LOW, C_FADER, C_CFX, C_COUNT };

}  // namespace

// The mapping's rows for the mod's own messages (tools/flx4_mapping.py).
enum : uint8_t {
    N_STEM = 0x30,        // + 0-3: ActivePart Vocal/Inst/Bass/Drums
    N_ISOLATOR = 0x34,    // PartIsolator on/off
    N_STEMS_MODE = 0x35,  // STEMS pad mode on/off
    N_FX_PART = 0x38,     // + 0-3: FXPartSelect Vocal/Inst/Bass/Drums
    N_DECK_SELECT = 0x72,
    N_FX_ASSIGN = 0x1C,   // + channel 0-3, on channel 5/6 (beat FX unit 1/2): FX<unit>Assign.CH<n> (toggles; unused)
    N_FX_CHANNEL = 0x10,  // + channel 0-3, on channel 5/6: the FLX4's own "beat FX unit on channel" (CH SELECT lever)
};
// The FLX4's messages (status nibble + note/CC).
enum : uint8_t {
    N_SHIFT = 0x3F,
    N_SHIFT_PLAY = 0x0E,
    N_SHIFT_IN = 0x4C,
    N_SHIFT_OUT = 0x4E,
    N_SHIFT_SMART_FADER = 0x09,  // channel 7
    N_LOAD = 0x46,               // channel 7, + side
    CC_CFX = 0x17,               // channel 7, + side (decks 3/4: 0x19/0x1A)
};
static bool isPadModeButton(uint8_t n) {
    switch (n) {
        case 0x1B: case 0x1E: case 0x20: case 0x22: case 0x69: case 0x6B: case 0x6D: case 0x6F: return true;
    }
    return false;
}
static int padModeOf(uint8_t n) {  // pad mode button -> the high nibble its pads then send
    switch (n) {
        case 0x1B: return 0; case 0x1E: return 1; case 0x20: return 2; case 0x22: return 3;
        case 0x69: return 4; case 0x6B: return 5; case 0x6D: return 6; case 0x6F: return 7;
    }
    return -1;
}
static bool isMixerCC(uint8_t cc) {  // channel strip: gain, EQ, channel fader (MSB or LSB)
    uint8_t m = cc & 0x1F;
    return cc < 0x40 && (m == 0x04 || m == 0x07 || m == 0x0B || m == 0x0F || m == 0x13);
}
static bool isAbsoluteCC(uint8_t cc) { return isMixerCC(cc & 0x1F) || (cc & 0x1F) == 0x00 || (cc & 0x1F) == 0x05; }  // + tempo (0x05: with SHIFT)
static int controlOf(uint8_t msb) {  // a deck channel's CC (MSB) -> its Control, -1 if not on the mixer
    switch (msb) {
        case 0x04: return C_TRIM; case 0x07: return C_HI; case 0x0B: return C_MID; case 0x0F: return C_LOW; case 0x13: return C_FADER;
    }
    return -1;
}
static uint8_t isolatorCC(uint8_t cc) {  // EQ -> part isolator knob (MSB or LSB), 0 if not an EQ
    switch (cc & 0x1F) {
        case 0x07: return (cc & 0x20) | 0x08;
        case 0x0B: return (cc & 0x20) | 0x0C;
        case 0x0F: return (cc & 0x20) | 0x10;
    }
    return 0;
}

static const Offset O_devName{0x10, "__ZN6djplay10DeviceUnitC2EiN4juce6StringEb", "add x19", 0};  // DeviceObject: name (juce::String)
static const Offset O_outHelper{0x460, "__ZN6djplay10DeviceMidi23handleOutputMidiMessageERKN4juce11MidiMessageERKNS1_6StringEi", "ldr x20", 2};  // DeviceMidi: MidiOutputHelper*
static const Offset O_output{0x1d8, "__ZN6djplay16MidiOutputHelper3runEv", "ldr x20", 8};  // MidiOutputHelper: juce::MidiOutput*
static const Offset O_units{0x140, "__ZN6djplay13DeviceManager17connectMidiDeviceEN4juce6StringE", "add x20", 2};  // DeviceManager: std::map<std::string, DeviceUnit*>
static const Offset O_unitsLock{0x100, "__ZN6djplay13DeviceManager17connectMidiDeviceEN4juce6StringE", "add x20", 1};  // DeviceManager: its CriticalSection
static const Addr A_enter{"__ZNK4juce15CriticalSection5enterEv"};
static const Addr A_exit{"__ZNK4juce15CriticalSection4exitEv"};
static const char* kDeviceManagerGet = "__ZN4juce15SingletonHolderIN6djplay13DeviceManagerENS_15CriticalSectionELb1EE3getEv";
static const Addr A_deviceManagerGet{kDeviceManagerGet};
static const Addr A_deviceManager{"SingletonHolder<DeviceManager>", [] { return singleton("__ZN6djplay10DeviceUnitC2EiN4juce6StringEb", kDeviceManagerGet); }};
static const Addr A_midiInData{"__ZN6djplay10DeviceMidi10midiInDataERN4juce11MidiMessageE"};  // DeviceMidi::midiInData (main thread)
static const char* kPlayerLayoutGet = "__ZN4juce15SingletonHolderIN6djplay12PlayerLayoutENS_15CriticalSectionELb0EE3getEv";
static const Addr A_playerLayoutGet{kPlayerLayoutGet};
static const Addr A_playerLayout{"SingletonHolder<PlayerLayout>", [] { return singleton("__ZN13MainComponent12changeLayoutE10CommandIDs", kPlayerLayoutGet); }};
static const Addr A_getLayout{"__ZNK6djplay12PlayerLayout9getLayoutEv"};
static const Addr A_isDj4DeckLayout{"__ZN6djplay12PlayerLayout15isDj4DeckLayoutENS0_10LayoutModeE"};
static const Addr A_mainComponent{"__ZN13MainComponent11getInstanceEv"};
static const Addr A_browseResize{"__ZN13MainComponent12browseResizeEv"};  // fits the browser (a view of its own) to the layout
static const Offset O_layoutChanger{0xb88, "__ZN13MainComponent7performERKN4juce24ApplicationCommandTarget14InvocationInfoE", "ldr x19", 5};  // MainComponent: LayoutChanger*
static const Addr A_performThunk{"__ZThn656_N13MainComponent7performERKN4juce24ApplicationCommandTarget14InvocationInfoE"};  // its ApplicationCommandTarget's
static const Addr A_invokeDirectly{"__ZN4juce24ApplicationCommandTarget14invokeDirectlyEib"};
static const Addr A_changeLayout{"__ZN13LayoutChanger12changeLayoutE10CommandIDs"};  // as MainComponent::perform (the menus) does
enum { CMD_2DECK = 0xB044, CMD_4DECK = 0xB045 };  // layout commands (2 / 4 decks, horizontal)
static const Addr A_showMixerPanel{"__ZN13MainComponent14showMixerPanelEb"};  // rekordbox's on-screen mixer
static const Addr A_stemsEnabled{"__ZNK6djplay9SettingIF23isTrackSeparationEnableEv"};  // STEMS on (reads only its setting file)
// juce, for reading rekordbox's on-screen mixer
static const Addr A_numChildren{"__ZNK4juce9Component21getNumChildComponentsEv"};
static const Addr A_child{"__ZNK4juce9Component17getChildComponentEi"};
static const Addr A_screenBounds{"__ZNK4juce9Component15getScreenBoundsEv"};
static const Addr A_isShowing{"__ZNK4juce9Component9isShowingEv"};
static const Addr A_sliderValue{"__ZNK4juce6Slider8getValueEv"};
static const Addr A_sliderMin{"__ZNK4juce6Slider10getMinimumEv"};
static const Addr A_sliderMax{"__ZNK4juce6Slider10getMaximumEv"};
static const Addr A_sliderProportion{"__ZN4juce6Slider25valueToProportionOfLengthEd"};  // with the slider's skew (EQ: centre at 0 dB)
static const Addr A_labelText{"__ZNK4juce5Label7getTextEb"};
static const Addr A_setBounds{"__ZN4juce9Component9setBoundsEiiii"};
// The jog wheels: rekordbox's FLX4 support (MidiMapDDJFLX4's constructor) makes the jog processors of decks 1/2
// only; the mapping's jog rows find a deck's processor when the mapping loads, so decks 3/4 got none and ignored
// their jog wheels. The mod makes theirs as the constructor does its own, right after it (before the mapping loads).
static const char* kFlx4Ctor = "__ZN6djplay14MidiMapDDJFLX4C2EiN4juce6StringE";
static const Offset O_jogScratch{0x80200, "__ZN6djplay14MidiMapDDJFLX4C2EiN4juce6StringE", "mov", 8};   // MidiMap: JogProcessor* per deck (4), scratch / pitch bend
static const Offset O_jogSearch{0x80220, "__ZN6djplay14MidiMapDDJFLX4C2EiN4juce6StringE", "mov", 13};   // MidiMap: JogProcessor* per deck (4), search
static const Offset O_jogPerf{0xb8, "__ZN6djplay14MidiMapDDJFLX4C2EiN4juce6StringE", "ldr x8", 2};      // JogProcessor: IControllerPerformanceProcessor*
static const Addr A_timeStampProcessor{"__ZN12DjController31IControllerPerformanceProcessor24createTimeStampProcessorEiddi"};
static const Addr A_scanProcessor{"__ZN12DjController31IControllerPerformanceProcessor19createScanProcessorEiiii"};
static const Addr A_jogProcessor{"__ZN6djplay12JogProcessorC2EiN4juce6StringEPN12DjController31IControllerPerformanceProcessorE"};
static const char* kGetProperty = "__ZN6djplay15DeviceComponent18operateGetPropertyEiRN4juce6StringERKNS1_10IdentifierERNS_13DeviceVariantE";
static const Addr A_getProperty{kGetProperty};
#if defined(__x86_64__) && !defined(_WIN32)
// Intel Macs: the one operateGetProperty call there, the Identifier in rcx (`lea rcx, [rip + x]`; 7.2.19 and 7.2.20)
static const Addr A_vinylId{"Identifier \"Vinyl\"", [] { return leaBefore(callSite(kFlx4Ctor, kGetProperty, 0), 1); }};
#else
static const Addr A_vinylId{"Identifier \"Vinyl\"", [] { return adrpAddBefore(callSite(kFlx4Ctor, kGetProperty, 0), 3); }};
#endif
static const Addr A_deviceComponent{"__ZN6djplay15DeviceComponentC1EiN4juce6StringEb"};
static const Addr A_deviceComponentDtor{"__ZN6djplay15DeviceComponentD1Ev"};
static const Addr A_variantVtable{"__ZTVN6djplay13DeviceVariantE", 0x10};
static const Addr A_memoryBlock{"__ZN4juce11MemoryBlockC1Ev"};
static const Addr A_memoryBlockDtor{"__ZN4juce11MemoryBlockD1Ev"};
enum { kJogResolution = 720 };
// LOOP IN/OUT's LEDs and the jog's pause state reach the FLX4 through units its constructor makes for decks 1/2
// only (found by name; MidiMapDDJFLX4::operateOnChange): the mod adds those of decks 3/4 as the constructor does.
static const Addr A_deviceUnit{"__ZN6djplay10DeviceUnitC1EiN4juce6StringEb"};
static const Offset O_mapChildStart{0x48, "__ZN6djplay14MidiMapDDJFLX418operateValueChangeEiRN4juce6StringEh", "ldr x22", 0};  // MidiMap: its children's std::deque<DeviceObject*>'s start (0x20 in)
static const Offset O_mapLock{0x58, "__ZN6djplay14MidiMapDDJFLX418operateValueChangeEiRN4juce6StringEh", "add x0", 1};  // MidiMap: CriticalSection
static const Offset V_setParent{0x10, "__ZTVN6djplay10DeviceUnitE", "vt:__ZN6djplay12DeviceObject9setParentEPS0_", 0};  // DeviceObject::setParent's slot, from the object's vptr
#ifdef _LIBCPP_VERSION
static_assert(sizeof(std::deque<void*>) == 0x30, "libc++ deque: map (split buffer, 0x20), start, size");
#endif
// A deck's state to the controller as when it connects (all its LEDs; rekordbox sends them only on changes)
static const Addr A_getUiPlayer{"__ZN6browse14PlayerAccessor11getUiPlayerEN6djplay9ePlayerIDE"};
static const Addr A_initializeDevice{"__ZN6djplay8UiPlayer16initializeDeviceERN4juce6StringE"};  // pulses per turn, as the constructor sets for the FLX4
// A deck's tempo where the tempo fader would have it, for pickup: rekordbox sets tempo % = (value - 8192) * range / 8191
// from the fader's 14-bit value (UiPlayer::eventAbsoluteValueChanged, getCurrentHwTempoValue).
static const Addr A_getTempo{"__ZNK8djengine10DjEngineIF8getTempoENS_15EnPlayerChannelE"};  // tempo %, float
// UiPlayer::getTempoSelectStatus's field (inlined on Windows): the ± range %: 6, 10, 16, 50, 100
static const Offset O_tempoRange{0x51c, "__ZNK6djplay8UiPlayer20getTempoSelectStatusEv", "ldr x0", 0};
// The tempo fader position rekordbox remembers for a deck (the fader's 14-bit value - 8192, an atomic int), stored by
// UiPlayer::eventAbsoluteValueChanged.
static const Offset O_tempoFader{0x604, "__ZN6djplay8UiPlayer25eventAbsoluteValueChangedEPNS_12DeviceObjectEi", "add x19", 0};

// After UiPlayer::initializeDevice (switchDeck). To rekordbox the deck is then a track just loaded on a controller that
// has just connected, and a message later (UiPlayer::AsyncTempoUpdater) it sets a deck that isn't following BEAT SYNC
// to the tempo fader position it remembers for it: where the fader last moved that deck, or 0 %. A deck whose tempo
// had changed since (it followed the sync master for a while and is the master now; the screen) jumped to that old
// tempo the moment its side switched to it, and every synced deck with it. The remembered position is made the
// deck's tempo here, so that update sets what the deck already has.
static void keepTempo(void* player, int deck) {
    void* e = engine();
    int range = *(const int*)((const char*)player + O_tempoRange);
    if (!e || range <= 0 || !deckLoaded(deck)) return;
    float tempo = at<float (*)(void*, int)>(A_getTempo)(e, deck);
    // (rekordbox's own scale: % = value * range / 8191 above the middle, / 8192 below)
    int value = (int)std::lround(tempo * (tempo > 0 ? 8191.0 : 8192.0) / range);
    auto& remembered = *(std::atomic<int>*)((char*)player + O_tempoFader);
    int was = remembered.exchange(value);
    if (abs(was - value) > 8)  // (more than 0.1 % of the range: it would have been heard)
        logf("flx4: deck %d keeps its tempo (%+.2f %%): rekordbox remembered its tempo fader at %+.2f %% and would have set that",
             deck + 1, tempo, was * (double)range / (was > 0 ? 8191.0 : 8192.0));
}

// rekordbox's channel level meters (peak, dB; what its level meter on the controller shows)
static const char* kEngineGet = "__ZN4juce15SingletonHolderIN8djengine10DjEngineIFENS_15CriticalSectionELb0EE3getEv";
static const Addr A_engineGet{kEngineGet};
#if defined(__x86_64__) && !defined(_WIN32)
static const Addr A_engine{"__ZN8djengine10DjEngineIF15singletonHolderE"};  // Intel Macs: the holder's own symbol
#else
static const Addr A_engine{"SingletonHolder<DjEngineIF>", [] { return adrpAdd("__ZN22ChannelLevelDeviceUnit13timerCallbackEi", 0); }};  // (via x21)
#endif
static const Addr A_djSystem{"__ZNK8djengine10DjEngineIF11getDjSystemEv"};
static const Addr A_mixerState{"__ZNK8DjSystem13getMixerStateEi"};
static const Addr A_channelLevel{"__ZThn392_NK11DjMixerUnit25getChannelLevelMeterValueEib"};  // on its IDjMixerState: (channel 0-3, left)
// rekordbox's CFX settings, each channel's knob (CFXPanel keeps them; also when its panel isn't shown)
static const Offset O_cfxBehavior{0x168, "__ZN9effectGui8CFXPanelC2EP8DjSystemPNS_14RbxEffectPanelE", "str x19", 1};  // CFXPanel: RbxCfxControlBehavior*
static const Addr A_cfxKnobValue{"__ZNK9effectGui21RbxCfxControlBehavior15getCfxKnobValueEi"};  // (channel 0-3) -> 0-1, centre 0.5

static volatile bool g_on = true;
static volatile bool g_log = false;
static volatile bool g_mixerFollows = true;
static volatile bool g_stemsOn = true;  // rekordbox's STEMS (Preferences > Extensions) is on (tick)
static void* volatile g_dev;      // the controller's djplay::DeviceMidi (found in tick)
static void* volatile g_output;   // its juce::MidiOutput
static Side g_side[2];
static std::atomic<bool> g_deckStems[4];  // each deck's pads are STEMS pads (a side takes its deck's on a switch)
// Each mixer channel's PART ISOLATOR is on: the EQ knobs of the strip on that channel control the stems. Per channel,
// as rekordbox's mapping row and its on-screen mixer strips have it; kept as rekordbox last sent its LED (also when
// changed on screen), so a strip that switches channel takes that channel's.
static std::atomic<bool> g_isolator[4];
static std::mutex g_ledLock;
static std::map<uint16_t, uint8_t> g_led;          // rekordbox's last output per (status, data1), all decks
static std::map<uint16_t, uint8_t> g_lastValue;    // last value sent to rekordbox per (status, CC MSB), for pickup
static std::atomic<uint8_t> g_padModeButton[4] = {0x1B, 0x1B, 0x1B, 0x1B};  // each deck's last pad mode button (HOT CUE)
static std::atomic<int> g_meters[4];
static std::atomic<int> g_meterResend[2];  // a side switched deck: its meter once, also for decks 1/2 (rekordbox sends on change)
static std::atomic<double> g_meterLogUntil;  // development: meter messages logged until then (shortly after a switch)
static double nowSeconds() { return rbm::now(); }
static std::atomic<int> g_rbMeter[4] = {-1, -1, -1, -1};  // rekordbox's last level meter value per channel              // level meter messages per channel since last logged
static std::atomic<int> g_onScreen[4][C_COUNT];    // each channel's mixer controls on screen, 0-127; -1 unknown (tick)
// The tempo fader (CC 0x00; 0x05 with SHIFT). With BEAT SYNC a deck's tempo moves without its fader (it follows the
// master; a new master keeps its own), so pickup compares the fader with the deck's real tempo, not with the value
// last sent: else a fader left far from it set the deck there at the first touch, and on a sync master every synced
// deck went along (190 BPM).
static bool isTempoCC(uint8_t msb) { return msb == 0x00 || msb == 0x05; }
static std::atomic<int> g_tempoAt[4] = {-1, -1, -1, -1};    // each deck's tempo as a fader MSB, -1 unknown (tick)
static std::atomic<int> g_tempoSent[4] = {-1, -1, -1, -1};  // the last tempo MSB passed to each deck
static std::atomic<double> g_tempoSentAt[4];

static const char* deviceName(void* dev) { return *(const char**)((char*)dev + O_devName); }

static void logMessage(const char* what, const uint8_t* b, int n, const char* extra = "") {
    if (!g_log) return;
    char hex[64] = "";
    for (int i = 0; i < n && i < 16; i++) snprintf(hex + 3 * i, 4, "%02X ", b[i]);
    logf("flx4: %s %s%s", what, hex, extra);
}

static MidiMessage message(uint8_t a, uint8_t b, uint8_t c, double timestamp = 0) {
    MidiMessage m{};
    m.inl[0] = a, m.inl[1] = b, m.inl[2] = c;
    m.timestamp = timestamp;
    m.size = 3;
    return m;
}

// ---- to rekordbox ---------------------------------------------------------------------------

using MidiInFn = bool (*)(void*, MidiMessage&);

// A message as if from the controller (MIDI thread): handled there if rekordbox handles it on that thread, else on the main thread.
static void inject(void* dev, uint8_t a, uint8_t b, uint8_t c) {
    MidiMessage m = message(a, b, c);
    logMessage("in+", m.inl, 3);
    if (RBM_ORIG(midi_in, MidiInFn)(dev, m)) return;
    plat::onMain([=] {
        MidiMessage copy = m;
        if (dev == g_dev) at<void (*)(void*, MidiMessage&)>(A_midiInData)(dev, copy);
    });
}
static void press(void* dev, int ch, uint8_t note) {
    inject(dev, 0x90 | ch, note, 0x7F);
    inject(dev, 0x90 | ch, note, 0x00);
}

// ---- to the controller ----------------------------------------------------------------------

using SendFn = void (*)(void*, const MidiMessage&);  // juce::MidiOutput::sendMessageNow

// Straight to the controller, not routed (any thread).
static void sendOut(uint8_t a, uint8_t b, uint8_t c) {
    void* out = g_output;
    if (!out) return;
    MidiMessage m = message(a, b, c);
    if (!((a & 0xF0) == 0xB0 && b == 0x02)) logMessage("out+", m.inl, 3);  // (not the level meters)
    else if (g_log && nowSeconds() < g_meterLogUntil) logf("flx4: meter mod %02X %02X", a, c);
#ifdef _WIN32
    RBM_ORIG(midi_send2, SendFn)(out, m);  // (MidiOutput::sendMessageNow; send1's site is another call to it)
#else
    RBM_ORIG(midi_send1, SendFn)(out, m);
#endif
}

// A mixer channel's level (0-3), in dB (peak, held and falling as rekordbox's meters); -inf if silent or unknown.
static float channelDb(int ch) {
    void* engine = at<void* (*)(uintptr_t)>(A_engineGet)(A_engine + g_slide);
    void* sys = engine ? at<void* (*)(void*)>(A_djSystem)(engine) : nullptr;
    void* mixer = sys ? at<void* (*)(void*, int)>(A_mixerState)(sys, 0) : nullptr;
    if (!mixer) return -INFINITY;
    // (no thisDelta: getMixerState already returns the IDjMixerState subobject this function takes, on Windows too)
    float l = at<float (*)(void*, int, bool)>(A_channelLevel)(mixer, ch, true), r = at<float (*)(void*, int, bool)>(A_channelLevel)(mixer, ch, false);
    return std::max(l, r);
}

static uint8_t ledValue(uint16_t key, int def = 0) {
    std::lock_guard<std::mutex> l(g_ledLock);
    auto it = g_led.find(key);
    return it == g_led.end() ? def : it->second;
}

// Where rekordbox's output for (status, data1) goes on the controller now: false if nowhere (another deck's, or a
// STEMS pad's while that side shows stems); else its status and data1 there.
static int mixerDeck(int side);
static bool route(uint8_t& st, uint8_t& n) {
    int kind = st & 0xF0, ch = st & 0x0F;
    if (st == 0x9F && n <= 3) {  // decks' loaded indicators
        int side = n & 1;
        if (g_side[side].deck != n) return false;
        n = side;
        return true;
    }
    if ((kind == 0x90 || kind == 0xB0) && ch <= 3) {  // a deck's controls
        int side = ch & 1;
        Side& s = g_side[side];
        // headphone CUE: the strip's channel's (flx4_mixer=fixed). The level meter is always the side's deck's: it
        // shows what the deck selected there plays (the user's choice, 11-18).
        bool mixer = kind == 0x90 && (n == 0x54 || n == 0x68);
        if ((mixer ? mixerDeck(side) : (int)s.deck) != ch) return false;
        if (kind == 0x90 && n >= N_STEM && n <= N_FX_PART + 3) {
            int pad = n < N_ISOLATOR ? n - N_STEM : n >= N_FX_PART ? 4 + n - N_FX_PART : -1;
            if (pad < 0 || !s.stems) return false;
            st = 0x97 + 2 * side;
            n = (uint8_t)(s.padMode << 4 | pad);
            return true;
        }
        st = kind | side;
        return true;
    }
    if (kind == 0x90 && (ch == 4 || ch == 5) && n >= N_FX_CHANNEL && n < N_FX_CHANNEL + 4) {  // beat FX unit on channel
        int deck = n - N_FX_CHANNEL, side = deck & 1;
        if (mixerDeck(side) != deck) return false;
        n = (uint8_t)(N_FX_CHANNEL + side);
        return true;
    }
    if (kind == 0x90 && ch >= 7 && ch <= 14) {  // pads (decks 1-4, unshifted and shifted)
        int deck = (ch - 7) >> 1, shifted = (ch - 7) & 1, side = deck & 1;
        if (g_side[side].deck != deck || g_side[side].stems) return false;
        st = kind | (7 + 2 * side + shifted);
        return true;
    }
    return true;
}

// Repaints the controller's LEDs for a side from what rekordbox last sent (after the side switched deck or pad mode):
// each LED the side's new deck has an output for gets it, the rest of that side's go dark.
static void repaint(int side) {
    std::map<uint16_t, uint8_t> leds;
    {
        std::lock_guard<std::mutex> l(g_ledLock);
        leds = g_led;
    }
    std::map<uint16_t, uint8_t> shown;  // controller LED -> value
    for (auto& [key, v] : leds) {
        uint8_t st = key >> 8, n = key & 0xFF;
        int ch = st & 0x0F, kind = st & 0xF0;
        bool deckLed = (st == 0x9F && n <= 3 && (n & 1) == side) ||
                       ((kind == 0x90 || kind == 0xB0) && ch <= 3 && (ch & 1) == side) ||
                       (kind == 0x90 && ch >= 7 && ch <= 14 && (((ch - 7) >> 1) & 1) == side);
        if (!deckLed || kind == 0xB0) continue;  // (level meters are refreshed constantly anyway)
        uint8_t rst = st, rn = n;
        bool visible = route(rst, rn);
        // the physical LED of this output if its deck were shown
        uint8_t pst = st, pn = n;
        if (st == 0x9F) pn = n & 1;
        else if (ch <= 3) pst = kind | (ch & 1);
        else pst = kind | (7 + 2 * side + ((ch - 7) & 1));
        uint16_t phys = pst << 8 | pn;
        bool stemRow = kind == 0x90 && ch <= 3 && n >= N_STEM && n <= N_FX_PART + 3;  // the mapping's own rows: no LEDs there
        if (visible) shown[rst << 8 | rn] = v;
        else if (!stemRow && !shown.count(phys)) shown[phys] = 0;
    }
    if (g_side[side].stems) {  // the STEMS pads: stems lit while playing (unknown: playing), FX parts as rekordbox says
        int d = g_side[side].deck, mode = g_side[side].padMode;
        for (int pad = 0; pad < 8; pad++) {
            uint16_t key = (0x90 | d) << 8 | (pad < 4 ? N_STEM + pad : N_FX_PART + pad - 4);
            shown[(0x97 + 2 * side) << 8 | (mode << 4 | pad)] = ledValue(key, pad < 4 ? 0x7F : 0);
        }
    }
    if (g_log) {  // what rekordbox last sent for the side's two decks, and what the controller gets
        std::string known, sent;
        char one[16];
        for (auto& [key, v] : leds)
            if ((key >> 8) == (0x90 | side) || (key >> 8) == (0x92 | side)) snprintf(one, sizeof one, " %04X=%02X", key, v), known += one;
        for (auto& [key, v] : shown)
            if ((key >> 8) < 0x97) snprintf(one, sizeof one, " %04X=%02X", key, v), sent += one;
        logf("flx4: repaint %s side: known%s", side ? "right" : "left", known.c_str());
        logf("flx4: repaint %s side: sent%s", side ? "right" : "left", sent.c_str());
    }
    for (auto& [key, v] : shown) sendOut(key >> 8, key & 0xFF, v);
}

// ---- on screen ------------------------------------------------------------------------------

static const char* sideName(int side) { return side ? "right" : "left"; }

static std::string fmt(const char* f, ...) __attribute__((format(printf, 1, 2)));
static std::string fmt(const char* f, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, f);
    vsnprintf(buf, sizeof buf, f, ap);
    va_end(ap);
    return buf;
}

// ---- rekordbox's on-screen mixer --------------------------------------------------------------
// With four decks the FLX4's two mixer strips each control one of two channels. rekordbox's on-screen mixer shows
// every channel's settings; the mod outlines the channels the strips control there, and marks each control that
// hasn't picked its channel up yet (pickup) where the FLX4's knob or fader is.

namespace {
using JRect = flx4ui::Rect;  // juce::Rectangle<int>
// The on-screen mixer (juce screen coordinates), as last read (main thread).
struct MixerView {
    bool shown = false;
    JRect column[4] = {};            // a channel's strip and fader
    JRect control[4][C_COUNT] = {};  // zero if not found
    JRect cfxLabel[4] = {};          // under the CFX knob the mod draws
    JRect deck[4] = {};              // the decks on screen (four-deck layout)
    void* numberLabel[4] = {};       // the juce::Label that writes it (a child of the player panel)
    int value[4][C_COUNT];           // 0-127, -1 unknown
};
static_assert(C_FADER == flx4ui::kFader && C_COUNT == flx4ui::kControls, "flx4_overlay.h's control indices");
}  // namespace

#ifdef _WIN32
// MSVC RTTI: vptr[-1] is the complete object locator; its type descriptor (an image-relative offset) names the class
// ".?AVSlider@juce@@". The classes below are named by their Itanium names (as on the mac): translated, a nested name
// N4juce6SliderE is "Slider@juce@@" and must match whole, a plain 12CustomSlider matches "CustomSlider@...".
static const char* className(void* c) {
    const char* col = *(const char**)((*(void***)c) - 1);
    const char* base = col - *(const uint32_t*)(col + 20);  // pSelf: the locator's own offset
    return base + *(const uint32_t*)(col + 12) + 16;        // pTypeDescriptor: after its vptr and spare pointer
}
static bool isA(void* c, const char* mangled) {
    char want[96];
    std::vector<std::string> names;
    const char* p = mangled + (*mangled == 'N');
    while (*p >= '0' && *p <= '9') {
        int n = atoi(p);
        while (*p >= '0' && *p <= '9') p++;
        names.emplace_back(p, n);
        p += n;
    }
    std::string w;
    for (size_t i = names.size(); i-- > 0;) w += names[i] + "@";
    if (*mangled == 'N') w += "@";
    snprintf(want, sizeof want, "%s", w.c_str());
    const char* name = className(c);  // ".?AV" + the class
    if (strncmp(name, ".?A", 3) || !name[3]) return false;
    return *mangled == 'N' ? strcmp(name + 4, want) == 0 : strncmp(name + 4, want, strlen(want)) == 0;
}
#else
static const char* className(void* c) { return *(const char**)((char*)(*(void***)c)[-1] + 8); }  // RTTI, mangled
static bool isA(void* c, const char* mangled) { return strstr(className(c), mangled) != nullptr; }
#endif
static int numChildren(void* c) { return at<int (*)(void*)>(A_numChildren)(c); }
static void* child(void* c, int i) { return at<void* (*)(void*, int)>(A_child)(c, i); }
static bool showing(void* c) { return at<bool (*)(void*)>(A_isShowing)(c); }
static bool isSlider(void* c) { return isA(c, "12CustomSlider") || isA(c, "N4juce6SliderE") || isA(c, "14CenterThruKnob"); }

static void* findComponent(void* c, const char* mangled, int depth = 0) {
    if (isA(c, mangled)) return c;
    if (depth > 24) return nullptr;
    for (int i = 0, n = numChildren(c); i < n; i++)
        if (void* r = findComponent(child(c, i), mangled, depth + 1)) return r;
    return nullptr;
}
static void showingSliders(void* c, std::vector<void*>& out, int depth = 0) {
    if (!showing(c)) return;
    if (isSlider(c)) return out.push_back(c);
    if (depth < 8)
        for (int i = 0, n = numChildren(c); i < n; i++) showingSliders(child(c, i), out, depth + 1);
}
static JRect screenBounds(void* c) {
#ifdef _WIN32
    JRect r{};  // (16 bytes: MSVC returns it through a hidden pointer after `this`)
    at<void* (*)(void*, JRect*)>(A_screenBounds)(c, &r);
    return r;
#else
    return at<JRect (*)(void*)>(A_screenBounds)(c);
#endif
}
static JRect unionOf(JRect a, JRect b) {  // (an empty one is ignored)
    if (a.w <= 0 || a.h <= 0) return b;
    if (b.w <= 0 || b.h <= 0) return a;
    int x = std::min(a.x, b.x), y = std::min(a.y, b.y);
    return {x, y, std::max(a.x + a.w, b.x + b.w) - x, std::max(a.y + a.h, b.y + b.h) - y};
}
static void setBounds(void* c, int x, int y, int w, int h) { at<void (*)(void*, int, int, int, int)>(A_setBounds)(c, x, y, w, h); }

// A channel strip of rekordbox's on-screen mixer laid out as the FLX4's: a TRIM knob 3/4 of an EQ knob (as on the controller)
// and the EQ knobs closer together, which leaves room under LOW for the CFX knob the mod draws (rekordbox's strip
// has none). rekordbox lays a strip out again when it resizes it; then the next tick does this again.
// The knobs shrink with the strip (a smaller window, four decks); in a strip too short even for the smallest, rekordbox's
// layout stays and there's no CFX knob (it was drawn over LOW and CUE).
enum { H_LABEL = 10, H_CUE = 20 };  // a label under a knob; rekordbox's CUE button at the strip's bottom
struct StripLayout {
    int trim = 0, eq = 0, step = 0, yEq = 0, yCfx = 0;  // knob sizes, EQ knob spacing, offsets from the strip's top
    bool ok = false;
};
static StripLayout stripLayout(JRect sr) {
    StripLayout l;
    if (sr.w < 30) return l;
    for (int eq = 20; eq >= 14; eq--) {
        l.eq = eq, l.trim = std::max(10, eq * 3 / 4), l.step = eq + 11, l.yEq = l.trim + 12, l.yCfx = l.yEq + 3 * l.step + 2;
        if (l.yCfx + eq + H_LABEL + H_CUE <= sr.h) return l.ok = true, l;
    }
    return StripLayout{};
}
static void compactStrip(void* strip) {
    JRect sr = screenBounds(strip);
    StripLayout L = stripLayout(sr);
    if (!L.ok) return;  // not the strip this is made for, or too short
    std::vector<void*> kids;
    for (int i = 0, n = numChildren(strip); i < n; i++) kids.push_back(child(strip, i));
    for (void* c : kids) {  // done already (and not laid out again since)?
        JRect r = screenBounds(c);
        if (isSlider(c) && r.y - sr.y < 10 && r.h == L.trim) return;
    }
    for (void* c : kids) {
        JRect r = screenBounds(c);
        int x = r.x - sr.x, y = r.y - sr.y;
        if (isSlider(c) && y < 10) setBounds(c, (sr.w - L.trim) / 2, 0, L.trim, L.trim);  // TRIM
        else if (isA(c, "16CustomTextButton") && y >= 10 && y < 35) setBounds(c, x, L.trim, r.w, r.h);  // its label
        else if (isA(c, "13EqControlComp") || isA(c, "18PartIsoControlComp")) {
            setBounds(c, x, L.yEq, r.w, 3 * L.step);
            JRect cr = screenBounds(c);
            std::vector<std::pair<int, void*>> knobs, labels;
            for (int i = 0, n = numChildren(c); i < n; i++) {
                void* k = child(c, i);
                (isSlider(k) ? knobs : labels).push_back({screenBounds(k).y, k});
            }
            std::sort(knobs.begin(), knobs.end()), std::sort(labels.begin(), labels.end());
            for (size_t i = 0; i < knobs.size(); i++) setBounds(knobs[i].second, (cr.w - L.eq) / 2, (int)i * L.step, L.eq, L.eq);
            for (size_t i = 0; i < labels.size(); i++) {
                JRect lr = screenBounds(labels[i].second);
                setBounds(labels[i].second, lr.x - cr.x, (int)i * L.step + L.eq, lr.w, lr.h);
            }
        }
    }
}

// Where the knob or fader is turned, 0-127 (as the FLX4's would be): its proportion, not its value, which for the
// EQs is in dB and not centred (0 dB, the knobs' centre, is well past halfway between their minimum and maximum).
static int sliderValue(void* s) {
    double lo = at<double (*)(void*)>(A_sliderMin)(s), hi = at<double (*)(void*)>(A_sliderMax)(s);
    if (!(hi > lo)) return -1;
    double p = at<double (*)(void*, double)>(A_sliderProportion)(s, at<double (*)(void*)>(A_sliderValue)(s));
    return (int)lround(std::clamp(p, 0.0, 1.0) * 127);
}

// The decks on screen in the four-deck layout (rekordbox lays them out 1 2 / 3 4): each one's track info and what's
// under it in its column (jog, pads, cue list), down to the next deck's track info.
static void readDecks(void* main, MixerView& v) {
    void* panel = findComponent(main, "N6djplay15PlayerComponent12PlayerPanelLE");
    if (!panel) return;
    std::vector<JRect> info, parts;
    std::vector<void*> labels;
    for (int i = 0, n = numChildren(panel); i < n; i++) {
        void* c = child(panel, i);
        if (!showing(c)) continue;
        if (isA(c, "N4juce5LabelE")) labels.push_back(c);
        if (isA(c, "N6djplay13TrackInfoViewE")) info.push_back(screenBounds(c));
        else if (!isA(c, "17MixerControlPanel") && !isA(c, "N6djplay14PlayerWaveViewE") && !isA(c, "N6djplay12WaveViewAreaE"))
            parts.push_back(screenBounds(c));
    }
    if (info.size() != 4) return;
    std::sort(info.begin(), info.end(), [](const JRect& a, const JRect& b) { return a.y != b.y ? a.y < b.y : a.x < b.x; });
    for (int d = 0; d < 4; d++) {
        JRect t = info[d], r = t;
        for (void* l : labels) {  // the deck number: a small label over the track info's top left
            JRect lr = screenBounds(l);
            if (lr.x >= t.x && lr.x < t.x + 20 && lr.y >= t.y && lr.y < t.y + 20 && lr.w <= 30) v.numberLabel[d] = l;
        }
        int below = INT_MAX;
        for (const JRect& o : info)
            if (o.y > t.y && abs(o.x - t.x) < 20) below = std::min(below, o.y);
        for (const JRect& k : parts) {
            int cx = k.x + k.w / 2, cy = k.y + k.h / 2;
            if (k.w > 0 && k.h > 0 && cx > t.x && cx < t.x + t.w && cy > t.y && cy < below) r = unionOf(r, k);
        }
        v.deck[d] = r;
    }
    static JRect logged[4];
    if (memcmp(logged, v.deck, sizeof logged)) {
        memcpy(logged, v.deck, sizeof logged);
        logf("flx4: decks on screen: 1 %d,%d %dx%d · 2 %d,%d %dx%d · 3 %d,%d %dx%d · 4 %d,%d %dx%d", v.deck[0].x, v.deck[0].y, v.deck[0].w, v.deck[0].h,
             v.deck[1].x, v.deck[1].y, v.deck[1].w, v.deck[1].h, v.deck[2].x, v.deck[2].y, v.deck[2].w, v.deck[2].h, v.deck[3].x, v.deck[3].y, v.deck[3].w, v.deck[3].h);
    }
}

static MixerView readMixer() {
    MixerView v;
    for (auto& d : v.value) for (int& x : d) x = -1;
    void* main = at<void* (*)()>(A_mainComponent)();
    void* panel = main ? findComponent(main, "17MixerControlPanel") : nullptr;
    if (!panel || !showing(panel)) return v;
    v.shown = true;
    std::vector<void*> strips, sliders;  // the channels' strips, in deck order
    for (int i = 0, n = numChildren(panel); i < n; i++)
        if (isA(child(panel, i), "16MixerControlComp")) strips.push_back(child(panel, i));
    showingSliders(panel, sliders);
    for (int d = 0; d < 4 && d < (int)strips.size(); d++) {
        if (!showing(strips[d])) continue;
        compactStrip(strips[d]);
        JRect strip = screenBounds(strips[d]);
        v.column[d] = strip;
        // the sliders in its column, top to bottom: TRIM, HI, MID, LOW (or the part isolator's), then the fader
        std::vector<std::pair<JRect, void*>> mine;
        for (void* s : sliders) {
            JRect r = screenBounds(s);
            double mid = r.x + r.w / 2.0;
            if (mid > strip.x && mid < strip.x + strip.w) mine.push_back({r, s});
        }
        std::sort(mine.begin(), mine.end(), [](auto& a, auto& b) { return a.first.y < b.first.y; });
        int knob = C_TRIM;
        for (auto& [r, s] : mine) {
            int c = r.h > r.w * 1.5 ? C_FADER : abs(r.w - r.h) < 6 && knob <= C_LOW ? knob++ : -1;
            if (c < 0 || v.value[d][c] >= 0) continue;
            v.control[d][c] = r;
            v.value[d][c] = sliderValue(s);
            v.column[d] = unionOf(v.column[d], r);
        }
    }
    readDecks(main, v);
    // The CFX knobs (the FLX4's filter, under its LOW knob): drawn by the mod where compactStrip made room.
    void* cfx = findComponent(main, "N9effectGui8CFXPanelE");
    void* behavior = cfx ? *(void**)((char*)cfx + O_cfxBehavior) : nullptr;
    for (int d = 0; d < 4 && behavior && d < (int)strips.size(); d++) {
        if (!showing(strips[d]) || v.control[d][C_LOW].w <= 0) continue;
        JRect sr = screenBounds(strips[d]);
        StripLayout L = stripLayout(sr);
        if (L.ok) {  // (else no room: no knob drawn, the FLX4's CFX still picks up by the value)
            v.control[d][C_CFX] = {sr.x + (sr.w - L.eq) / 2, sr.y + L.yCfx, L.eq, L.eq};
            v.cfxLabel[d] = {sr.x, sr.y + L.yCfx + L.eq, sr.w, H_LABEL};
        }
        double f = at<float (*)(void*, int)>(A_cfxKnobValue)(behavior, d);
        v.value[d][C_CFX] = (int)lround(std::clamp(f, 0.0, 1.0) * 127);
    }
    return v;
}

static int mixerDeck(int side) { return g_mixerFollows ? (int)g_side[side].deck : side; }  // the channel a strip controls
static bool isolatorOn(int side) { return g_isolator[mixerDeck(side)]; }  // a strip's EQ knobs control the stems
static uint8_t physicalCC(int side, int control) {  // the FLX4's CC (MSB) for a strip's control
    static const uint8_t cc[C_COUNT] = {0x04, 0x07, 0x0B, 0x0F, 0x13, CC_CFX};
    return control == C_CFX ? CC_CFX + side : cc[control];
}


static int g_prevShown[4][C_COUNT];  // last tick's on-screen values, and the FLX4's
static int g_prevPhys[2][128];

// Reads the on-screen mixer, for pickup and the overlay (tick).
// The deck numbers rekordbox writes over each deck's track info (juce::Labels): the one of a deck a side controls
// in the platendoos blue, bold and a bit bigger, drawn by rekordbox itself. Labels are found again every tick (rekordbox
// makes new ones when it changes the layout), and anything changed is put back once the deck isn't controlled: the
// label's own Font is kept (a copy) and set back, its height read in place (Label::getFont's member; Windows
// inlines getFont).
// Optional (file ""): no mod is switched off when a build lacks them, the numbers just stay as rekordbox draws them.
static const Addr A_setColour{"__ZN4juce9Component9setColourEiNS_6ColourE", intptr_t(0), ""};
static const Addr A_findColour{"__ZNK4juce9Component10findColourEib", intptr_t(0), ""};
static const Addr A_labelSetFont{"__ZN4juce5Label7setFontERKNS_4FontE", intptr_t(0), ""};
static const Offset O_labelFont{0x1b0, "__ZNK4juce5Label7getFontEv", "add x0", 0, X86{0x1e8}, ""};  // juce::Label: its Font
static const Addr A_fontMake{"__ZN4juce4FontC1Efi", intptr_t(0), ""};
static const Addr A_fontCopy{"__ZN4juce4FontC1ERKS0_", intptr_t(0), ""};
static const Addr A_fontFree{"__ZN4juce4FontD1Ev", intptr_t(0), ""};
static const Addr A_fontHeight{"__ZNK4juce4Font9getHeightEv", intptr_t(0), ""};
static bool canStyleNumbers() {
    return O_labelFont.ok && A_setColour && A_findColour && A_labelSetFont && A_fontMake && A_fontCopy && A_fontFree && A_fontHeight;
}
enum : int { kLabelTextColour = 0x1000281 };     // juce::Label::textColourId
static const uint32_t kNumberBlue = 0xFF2978EB;  // ARGB, the overlay's blue
struct FontBuf { alignas(16) unsigned char b[128]; };  // room for a juce::Font (made and destroyed by juce)
// a label's own, to put back. font: a juce::Font copy (refcounted: freed once, when it is put back)
struct NumberStyle { uint32_t colour; float height; FontBuf font; JRect bounds; };
static std::map<void*, NumberStyle> g_numberStyled;

static uint32_t findColour(void* c, int id) {
#ifdef _WIN32  // (MSVC returns the juce::Colour of a member function through a hidden pointer after `this`)
    uint32_t argb = 0;
    at<void* (*)(void*, uint32_t*, int, bool)>(A_findColour)(c, &argb, id, false);
    return argb;
#else
    return at<uint32_t (*)(void*, int, bool)>(A_findColour)(c, id, false);
#endif
}
static float labelFont(void* label) { return at<float (*)(const void*)>(A_fontHeight)((char*)label + O_labelFont); }
static void setLabelFont(void* label, float height, int style) {
    FontBuf f;
    at<void (*)(void*, float, int)>(A_fontMake)(&f, height, style);
    at<void (*)(void*, const void*)>(A_labelSetFont)(label, &f);
    at<void (*)(void*)>(A_fontFree)(&f);
}

static void styleDeckNumbers(const MixerView& v, void* panel) {
    static const bool can = canStyleNumbers();
    if (!can) return;
    std::map<void*, NumberStyle> keep;
    JRect pr = panel ? screenBounds(panel) : JRect{};
    for (auto& [l, st] : g_numberStyled)  // not found this tick (other layout, or gone): kept, but not touched
        if (std::find(std::begin(v.numberLabel), std::end(v.numberLabel), l) == std::end(v.numberLabel)) keep[l] = st;
    for (int d = 0; d < 4; d++) {
        void* l = v.numberLabel[d];
        if (!l || !panel) continue;
        bool want = g_dev && (g_side[0].deck == d || g_side[1].deck == d);
        auto it = g_numberStyled.find(l);
        JRect lr = screenBounds(l);
        if (want) {
            NumberStyle st;
            if (it != g_numberStyled.end()) st = it->second;
            else {
                st.colour = findColour(l, kLabelTextColour);
                st.height = labelFont(l);
                at<void (*)(void*, const void*)>(A_fontCopy)(&st.font, (char*)l + O_labelFont);
                st.bounds = {lr.x - pr.x, lr.y - pr.y, lr.w, lr.h};
            }
            if (findColour(l, kLabelTextColour) != kNumberBlue)
                at<void (*)(void*, int, uint32_t)>(A_setColour)(l, kLabelTextColour, kNumberBlue);
            float h = std::round(st.height * 1.35f);
            if (std::fabs(labelFont(l) - h) > 0.1f) setLabelFont(l, h, 1);  // bold
            int w = st.bounds.w + 6, hh = st.bounds.h + 5;
            if (lr.w != w || lr.h != hh) {  // more room for the bigger number (rekordbox lays it out again on resizes)
                if (lr.w == st.bounds.w && lr.h == st.bounds.h) st.bounds = {lr.x - pr.x, lr.y - pr.y, lr.w, lr.h};
                setBounds(l, st.bounds.x - 2, st.bounds.y - 3, w, hh);
            }
            keep[l] = st;
        } else if (it != g_numberStyled.end()) {  // put back what rekordbox had
            at<void (*)(void*, int, uint32_t)>(A_setColour)(l, kLabelTextColour, it->second.colour);
            at<void (*)(void*, const void*)>(A_labelSetFont)(l, &it->second.font);
            at<void (*)(void*)>(A_fontFree)(&it->second.font);
            setBounds(l, it->second.bounds.x, it->second.bounds.y, it->second.bounds.w, it->second.bounds.h);
        }
    }
    g_numberStyled.swap(keep);
}

static void updateMixer() {
    bool window = flx4ui::windowAvailable();
    MixerView v = window && g_dev ? readMixer() : MixerView{};
    if (!window || !g_dev) for (auto& d : v.value) for (int& x : d) x = -1;
    if (window) {  // (also without the FLX4: to put the numbers back)
        void* main = at<void* (*)()>(A_mainComponent)();
        MixerView decks;
        if (!g_dev && main && !g_numberStyled.empty()) readDecks(main, decks);
        styleDeckNumbers(g_dev ? v : decks, main ? findComponent(main, "N6djplay15PlayerComponent12PlayerPanelLE") : nullptr);
    }
    for (int d = 0; d < 4; d++)
        for (int c = 0; c < C_COUNT; c++) g_onScreen[d][c] = v.value[d][c];
    if (g_log && memcmp(g_prevShown, v.value, sizeof g_prevShown)) {  // trim, hi, mid, low, fader, cfx per channel
        char line[256] = "";
        for (int d = 0; d < 4; d++) {
            size_t n = strlen(line);
            snprintf(line + n, sizeof line - n, " ch%d:%d/%d/%d/%d/%d/%d", d + 1, v.value[d][0], v.value[d][1], v.value[d][2], v.value[d][3], v.value[d][4], v.value[d][5]);
        }
        logf("flx4: on screen%s", line);
    }
    // A control changed on screen (with the mouse, ...) while the FLX4's stayed put: the FLX4's picks it up again.
    for (int side = 0; side < 2; side++) {
        Side& s = g_side[side];
        int d = mixerDeck(side);
        for (int c = 0; c < C_COUNT; c++) {
            uint8_t cc = physicalCC(side, c);
            int now = v.value[d][c], before = g_prevShown[d][c], phys = s.phys[cc];
            if (s.engaged[cc] && now >= 0 && before >= 0 && abs(now - before) > 2 && phys == g_prevPhys[side][cc] && abs(now - phys) > 2)
                s.engaged[cc] = false;
            g_prevPhys[side][cc] = phys;
        }
    }
    memcpy(g_prevShown, v.value, sizeof g_prevShown);

    flx4ui::MixerMarks marks;
    marks.shown = v.shown;
    for (int side = 0; side < 2; side++) {
        int d = mixerDeck(side);
        marks.column[side] = v.column[d];
        for (int c = 0; c < C_COUNT; c++) {
            uint8_t cc = physicalCC(side, c);
            bool marked = !g_side[side].engaged[cc] && g_side[side].phys[cc] >= 0 && v.value[d][c] >= 0 && abs(g_side[side].phys[cc] - v.value[d][c]) > 2;
            marks.control[side][c] = v.control[d][c];
            marks.phys[side][c] = marked ? (int)g_side[side].phys[cc] : -1;
        }
    }
    for (int d = 0; d < 4; d++) {
        marks.deck[d] = v.deck[d];
        marks.cfxKnob[d] = v.control[d][C_CFX];
        marks.cfxLabel[d] = v.cfxLabel[d];
        marks.cfxValue[d] = v.value[d][C_CFX];
    }
    flx4ui::setMixerMarks(marks);
}

static bool g_mixerOpened;  // the mod opened rekordbox's on-screen mixer (main thread)

static bool fourDeckLayout() {
    void* layout = at<void* (*)(uintptr_t)>(A_playerLayoutGet)(A_playerLayout + g_slide);
    int mode = at<int (*)(void*)>(A_getLayout)(layout);
    return at<bool (*)(int)>(A_isDj4DeckLayout)(mode);
}
static void setFourDeckLayout(bool four) {
    if (fourDeckLayout() == four) return;
    char* main = at<char* (*)()>(A_mainComponent)();
    if (!main) return;
    // As the View menu: the command to MainComponent (as an ApplicationCommandTarget, at the offset its perform thunk
    // subtracts: "sub x0, x0, #imm"), which also moves the browser and updates the layout indicator.
#ifdef _WIN32
    // MSVC has no thunk: MainComponent's ApplicationCommandTarget subobject is at +568 (verified in perform, 0x141824840),
    // and perform is slot 4 of its vtable: checked before use, else the layout alone.
    void** actVt = *(void***)(main + 568);
    bool viaTarget = actVt && actVt[4] == (void*)(A_performThunk + g_slide);
    size_t targetOffset = 568;
#elif defined(__x86_64__)
    // Intel Macs: the thunk is "push rbp; mov rbp, rsp; add rdi, -imm" (48 81 C7 imm32, or 48 83 C7 imm8):
    // -0x290 in 7.2.19 and 7.2.20, as the 656 in its name.
    const uint8_t* p = at<const uint8_t*>(A_performThunk);
    int32_t imm = 0;
    bool frame = p[0] == 0x55 && p[1] == 0x48 && p[2] == 0x89 && p[3] == 0xE5 && p[4] == 0x48 && p[6] == 0xC7;
    if (frame && p[5] == 0x81) memcpy(&imm, p + 7, 4);
    else if (frame && p[5] == 0x83) imm = (int8_t)p[7];
    bool viaTarget = imm < 0;
    size_t targetOffset = (size_t)-(int64_t)imm;
#else
    uint32_t ins = *at<const uint32_t*>(A_performThunk);
    bool viaTarget = (ins & 0xFFC003FF) == 0xD1000000;
    size_t targetOffset = (ins >> 10) & 0xFFF;
#endif
    if (viaTarget) {
        at<bool (*)(void*, int, bool)>(A_invokeDirectly)(main + targetOffset, four ? CMD_4DECK : CMD_2DECK, false);
    } else {  // (not as expected: the layout itself only)
        void* changer = *(void**)(main + O_layoutChanger);
        if (!changer) return;
        at<void (*)(void*, int)>(A_changeLayout)(changer, four ? CMD_4DECK : CMD_2DECK);
    }
    // Four decks on two mixer strips: rekordbox's on-screen mixer shows all four channels' settings (and makes room
    // for the four decks: when shown, so if it already was, again); closed again with two decks if the mod opened it.
    void* panel = findComponent(main, "17MixerControlPanel");
    bool shown = panel && showing(panel);
    if (four) {
        if (shown) at<void (*)(void*, bool)>(A_showMixerPanel)(main, false);
        at<void (*)(void*, bool)>(A_showMixerPanel)(main, true);
        g_mixerOpened = !shown;
    }
    if (!four && g_mixerOpened) {
        if (shown) at<void (*)(void*, bool)>(A_showMixerPanel)(main, false);
        g_mixerOpened = false;
    }
    // The browser isn't moved with the decks when the layout changes from here; the layout settles asynchronously.
    for (double t : {0.1, 0.5, 1.2})
        plat::onMainAfter(t, [] {
            if (void* m = at<void* (*)()>(A_mainComponent)()) at<void (*)(void*)>(A_browseResize)(m);
        });
}

// ---- the controller's messages --------------------------------------------------------------

// Beat FX: the FLX4's CH SELECT lever tells rekordbox which channels its two FX units are on, as "unit u on channel c"
// (9<4+u> <0x10+c> on/off; it also sends CH3/4, always off). The mod keeps where the lever puts each unit per side
// (channel 1: left, 2: right) and tells rekordbox the decks those sides control instead, all four channels each time.
static std::atomic<bool> g_fxLever[2][2];  // [unit][side]
static void applyFxAssign(void* dev) {
    for (int u = 0; u < 2; u++)
        for (int d = 0; d < 4; d++) {
            bool on = (mixerDeck(0) == d && g_fxLever[u][0]) || (mixerDeck(1) == d && g_fxLever[u][1]);
            inject(dev, 0x94 + u, N_FX_CHANNEL + d, on ? 0x7F : 0x00);
        }
}

static void switchDeck(void* dev, int side) {
    Side& s = g_side[side];
    int from = s.deck, to = from < 2 ? from + 2 : from - 2;
    if (s.shift) inject(dev, 0x90 | from, N_SHIFT, 0x00), inject(dev, 0x90 | to, N_SHIFT, 0x7F);  // SHIFT is still held
    s.deck = to;
    s.stems = g_deckStems[to].load();  // the new deck's pads as it left them (STEMS or its pad mode)
    g_meterResend[side] = 1, g_meterLogUntil = nowSeconds() + 2;
    press(dev, to, N_DECK_SELECT);
    applyFxAssign(dev);  // the beat FX goes along
    // Absolute controls pick the new deck up where it has them.
    for (int cc = 0; cc < 128; cc++) s.dropLsb[cc] = false, s.engaged[cc] = false;
    if (s.stems) press(dev, to, N_STEMS_MODE);  // STEMS pads go along on screen
    // The controller's pads go to the new deck's pad mode (below): the STEMS pad LEDs are sent as notes of that mode,
    // not of the one the deck before was in (they stayed dark until a pad was pressed).
    if (int mode = padModeOf(g_padModeButton[to]); mode >= 0) s.padMode = mode;
    repaint(side);
    sendOut(0x90 | side, g_padModeButton[to], 0x7F);  // the controller's pads to the new deck's pad mode (after repaint)
    bool stems = s.stems, iso = isolatorOn(side);  // (the new channel's own isolator: nothing pressed for it)
    plat::onMain([=] {
        // rekordbox sends a deck's LEDs only when they change: have it send all of the new deck's now
        void* player = at<void* (*)(int)>(A_getUiPlayer)(to);
        if (g_log) logf("flx4: deck %d's state to the controller (player %p)", to + 1, player);
        if (player) {
            JStr name(cfgStr("flx4_device", "DDJ-FLX4").c_str());
            at<void (*)(void*, JStr*)>(A_initializeDevice)((char*)player + thisDelta(A_initializeDevice), &name);  // (Windows: a base's subobject)
            if (cfgInt("flx4_keep_tempo", 1)) keepTempo(player, to);
            sendOut(0x90 | side, g_padModeButton[to], 0x7F);  // (the pad mode again, in case that reset it)
        }
        if (to >= 2) setFourDeckLayout(true);
        std::string detail = fmt("The %s side %s deck %d%s%s", sideName(side),
                                 g_mixerFollows ? "and its mixer strip control" : "controls", to + 1, stems ? " · pads: STEMS" : "", iso ? " · EQ: stems" : "");
        flx4ui::toast(side, fmt("DECK %d", to + 1), detail, to);
    });
    logf("flx4: %s side -> deck %d", side ? "right" : "left", to + 1);
}

// rekordbox's pads on screen go along: its STEMS row only turns STEMS mode on (as its pad mode buttons do theirs), so
// the deck's last pad mode button leaves it again (unless that button was just pressed: rekordbox has it already).
static void setStems(void* dev, int side, bool on, bool byPadModeButton = false) {
    Side& s = g_side[side];
    if (s.stems == on) return;
    s.stems = on;
    g_deckStems[s.deck] = on;
    int d = s.deck;
    if (on) press(dev, d, N_STEMS_MODE);
    else if (!byPadModeButton) press(dev, d, g_padModeButton[d]);
    repaint(side);
    plat::onMain([=] {
        if (on) flx4ui::toast(side, "STEMS", fmt("Deck %d · pads 1-4: vocal, inst, bass, drums on/off\npads 5-8: beat FX on that stem · pad mode button: back", d + 1), d);
        else flx4ui::toast(side, "PADS", fmt("Deck %d · back to the pad mode", d + 1), d);
    });
}

static void toggleIsolator(void* dev, int side) {
    Side& s = g_side[side];
    int d = mixerDeck(side);  // the channel the EQ knobs send to (with flx4_mixer=fixed not the side's deck)
    bool on = !g_isolator[d];
    g_isolator[d] = on;
    for (int c : {C_HI, C_MID, C_LOW}) s.engaged[physicalCC(side, c)] = false;  // the knobs pick up the stems' (or EQ's) settings
    if ((ledValue((0x90 | d) << 8 | N_ISOLATOR) != 0) != on) press(dev, d, N_ISOLATOR);
    const char* what = g_mixerFollows ? "Deck" : "Channel";
    plat::onMain([=] {
        flx4ui::toast(side, on ? "EQ → STEMS" : "EQ", on ? fmt("%s %d · HI vocal · MID inst · LOW drums", what, d + 1)
                                                          : fmt("%s %d · the knobs are EQ again", what, d + 1), d);
    });
}

static void toggleLayout() {
    plat::onMain([] {
        bool four = !fourDeckLayout();
        setFourDeckLayout(four);
        flx4ui::toast(0, four ? "4 DECKS" : "2 DECKS", "");
    });
}

// Pickup: whether an absolute control's new value may go to rekordbox (MIDI thread). It may once it is where rekordbox
// has that control (as on screen, else as last sent) or has moved across it, and from then on
// until the side switches deck or the value is changed on screen. key: (status, CC) as sent; control: on the mixer, or -1.
static bool pickup(Side& s, int deck, int control, uint16_t key, uint8_t physCC, uint8_t v) {
    int prev = s.phys[physCC].exchange(v);
    if (s.engaged[physCC]) return true;
    bool tempo = control < 0 && isTempoCC(physCC);
    int want = control >= 0 ? g_onScreen[deck][control].load() : tempo ? g_tempoAt[deck].load() : -1;
    if (want < 0) {
        auto it = g_lastValue.find(key);
        if (it == g_lastValue.end()) return s.engaged[physCC] = true;
        want = it->second;
    }
    // Picked up: close to it; or the knob was at it before this move or crossed it; or, if this is the first the mod
    // hears of the knob (e.g. still where it was when rekordbox started), one quick turn's step (the FLX4 skips values) away.
    int tol = tempo ? 1 : 2;  // a tempo step is up to 1.6 % (range ±100 %)
    bool near = abs(v - want) <= tol;
    bool from = prev >= 0 && (abs(prev - want) <= tol || (prev - want) * (v - want) < 0);
    bool first = prev < 0 && abs(v - want) <= 8;
    if (near || from || first) {
        if (tempo) logf("flx4: deck %d's tempo picked up by the tempo fader (%d)", deck + 1, v);
        return s.engaged[physCC] = true;
    }
    if (tempo && (prev < 0 || abs(prev - v) > 0)) {
        static double loggedAt;
        if (nowSeconds() - loggedAt > 2) loggedAt = nowSeconds(), logf("flx4: tempo fader at %d, deck %d's tempo at %d: held until it gets there", v, deck + 1, want);
    }
    return false;
}

// The controller's MIDI thread.
extern "C" bool rbm_hook_midi_in(void* dev, MidiMessage& m) {
    using Orig = MidiInFn;
    if (!g_on || dev != g_dev || m.size != 3) {
        if (g_on && g_log) logMessage(deviceName(dev), m.bytes(), m.size);
        return RBM_ORIG(midi_in, Orig)(dev, m);
    }
    uint8_t* b = m.bytes();
    logMessage("in ", b, 3);
    int kind = b[0] & 0xF0, ch = b[0] & 0x0F;
    bool on = kind == 0x90 && b[2] > 0, off = kind == 0x80 || (kind == 0x90 && b[2] == 0);
    if (kind == 0x80) b[0] = 0x90 | ch, b[2] = 0;  // as the FLX4 sends releases (note on, velocity 0)

    if (kind == 0x90 && (ch == 4 || ch == 5) && b[1] >= N_FX_CHANNEL && b[1] < N_FX_CHANNEL + 4) {  // CH SELECT lever
        int c = b[1] - N_FX_CHANNEL;
        if (c < 2 && g_fxLever[ch - 4][c] != (b[2] > 0)) {
            g_fxLever[ch - 4][c] = b[2] > 0;
            applyFxAssign(dev);
        }
        return true;
    }
    if (kind <= 0x90 && ch <= 1) {  // a deck's buttons
        Side& s = g_side[ch];
        uint8_t n = b[1];
        if (n == N_SHIFT) s.shift = on;
        if (n == N_SHIFT_PLAY || n == N_SHIFT_IN || n == N_SHIFT_OUT) {
            if (on && n == N_SHIFT_PLAY) switchDeck(dev, ch);
            if (on && n != N_SHIFT_PLAY && !g_stemsOn && !s.stems && !isolatorOn(ch))
                plat::onMain([ch, d = (int)s.deck] { flx4ui::toast(ch, "STEMS IS OFF", "Turn it on in Preferences > Extensions > STEMS", d); });
            else if (on && n == N_SHIFT_IN) setStems(dev, ch, !s.stems);
            else if (on && n == N_SHIFT_OUT) toggleIsolator(dev, ch);
            return true;
        }
        if (isPadModeButton(n) && on) {
            // rekordbox's FLX4 support answers a pad mode button with that button's note at 7F, which switches the
            // controller's pads to that mode; only for decks 1/2 (MidiMapDDJFLX4::getMidiInputData), so the mod
            // answers for decks 3/4.
            g_padModeButton[s.deck] = n;
            if (s.deck >= 2) sendOut(0x90 | ch, n, 0x7F);
            s.padMode = padModeOf(n);
            if (s.stems) setStems(dev, ch, false, true);
        }
    }
    if (kind <= 0x90 && ch >= 7 && ch <= 10) {  // pads
        int side = (ch - 7) >> 1;
        Side& s = g_side[side];
        s.padMode = b[1] >> 4;
        int d = s.deck;
        bool stems = s.stems;
        int8_t& held = s.held[1 + ((ch - 7) & 1)][b[1] & 0x7F];
        if (off && held) d = (held - 1) & 3, stems = held > 4, held = 0;  // to the deck (and as the pad) it was pressed on
        else if (on) held = (int8_t)(1 + d + (stems ? 4 : 0));
        if (stems) {
            int pad = b[1] & 7;
            if (on || off) inject(dev, 0x90 | d, pad < 4 ? N_STEM + pad : N_FX_PART + pad - 4, on ? 0x7F : 0);
            return true;
        }
        if (d >= 2) b[0] = 0x90 | (ch + 4);  // decks 3/4: channels 12-15
    }
    if (kind == 0x90 && ch == 6 && b[1] == N_SHIFT_SMART_FADER) {
        if (on) toggleLayout();
        return true;
    }
    if (kind == 0x90 && ch == 6 && (b[1] == N_LOAD || b[1] == N_LOAD + 1)) {
        int side = b[1] - N_LOAD;
        if (g_side[side].deck >= 2) b[1] += 2;  // LOAD 3/4
    }
    if (kind == 0xB0 && ch == 6 && (b[1] & 0x1F) >= CC_CFX && (b[1] & 0x1F) <= CC_CFX + 1) {  // CFX knobs
        int side = (b[1] & 0x1F) - CC_CFX;
        Side& s = g_side[side];
        uint8_t phys = b[1];
        if (s.deck >= 2 && g_mixerFollows) b[1] += 2;
        if (!(phys & 0x20)) {
            s.dropLsb[phys] = !pickup(s, mixerDeck(side), C_CFX, b[0] << 8 | b[1], phys, b[2]);
            if (s.dropLsb[phys]) return true;
            g_lastValue[b[0] << 8 | b[1]] = b[2];
        } else if (s.dropLsb[phys & 0x1F]) return true;
    }
    if (ch <= 1) {  // a deck's controls: to its deck's channel
        Side& s = g_side[ch];
        uint8_t cc = b[1];
        bool mixer = (kind == 0xB0 && isMixerCC(cc)) || (kind == 0x90 && (cc == 0x54 || cc == 0x68));  // + headphone CUE (/ TAP)
        int d = mixer && !g_mixerFollows ? ch : (int)s.deck;
        if ((on || off) && cc != N_SHIFT) {  // (SHIFT: switchDeck hands it over)
            int8_t& held = s.held[0][cc & 0x7F];
            if (off && held) d = held - 1, held = 0;  // released where it was pressed (a hot cue's preview, CUE)
            else if (on) held = (int8_t)(1 + d);
        }
        b[0] = (b[0] & 0xF0) | d;
        if (kind == 0xB0 && isolatorOn(ch) && isolatorCC(cc)) b[1] = isolatorCC(cc);
        if (kind == 0xB0 && isAbsoluteCC(cc) && cc < 0x40) {
            uint8_t msb = cc & 0x1F;
            if (!(cc & 0x20)) {
                s.dropLsb[msb] = !pickup(s, d, controlOf(msb), b[0] << 8 | b[1], msb, b[2]);
                if (s.dropLsb[msb]) return true;
                g_lastValue[b[0] << 8 | b[1]] = b[2];
                if (isTempoCC(msb)) g_tempoSent[d] = b[2], g_tempoSentAt[d] = nowSeconds();
            } else if (s.dropLsb[msb]) return true;
        }
    }
    if (b[0] != (kind | ch) || g_log) logMessage(" ->", b, 3);
    return RBM_ORIG(midi_in, Orig)(dev, m);
}

// rekordbox's messages to the controller as they go out, whichever of its queues they come from (rekordbox's MIDI
// output threads): to where the deck they're for is on the controller, if it is.
static void sendRouted(SendFn orig, void* out, const MidiMessage& m) {
    if (!g_on || out != g_output || m.size != 3) return orig(out, m);
    MidiMessage copy = m;
    uint8_t* b = copy.inl;
    bool meter = (b[0] & 0xF0) == 0xB0 && b[1] == 0x02;  // level meters: many a second, counted
    if (meter && (b[0] & 0x0F) < 4) g_meters[b[0] & 0x0F]++, g_rbMeter[b[0] & 0x0F] = b[2];
    if (!meter) {
        std::lock_guard<std::mutex> l(g_ledLock);
        g_led[b[0] << 8 | b[1]] = b[2];
    }
    if ((b[0] & 0xF0) == 0x90 && (b[0] & 0x0F) <= 3 && b[1] == N_ISOLATOR) g_isolator[b[0] & 0x0F] = b[2] != 0;
    uint8_t st = b[0], n = b[1];
    if (!route(st, n)) {
        if (!meter) logMessage("out", b, 3, " (hidden)");
        else if (g_log && nowSeconds() < g_meterLogUntil) logf("flx4: meter rb %02X %02X hidden", b[0], b[2]);
        return;
    }
    if (!meter) logMessage("out", b, 3);
    else if (g_log && nowSeconds() < g_meterLogUntil) logf("flx4: meter rb %02X %02X -> %02X", b[0], b[2], st);
    b[0] = st, b[1] = n;
    orig(out, copy);
}
extern "C" void rbm_hook_midi_send1(void* out, const MidiMessage& m) { sendRouted(RBM_ORIG(midi_send1, SendFn), out, m); }
extern "C" void rbm_hook_midi_send2(void* out, const MidiMessage& m) { sendRouted(RBM_ORIG(midi_send2, SendFn), out, m); }
#ifndef _WIN32
// (On Windows send1 and send3 are one call, in MidiOutputHelper's helper at 0x1424be460, and send1 has its site: send3
// has none there. Its name is kept out of tools/win/hookmods.py's text scan by the asm label, or flx4 would count as
// half-hooked. All four hooks' originals are sendMessageNow.)
extern "C" void flx4_send3(void* out, const MidiMessage& m) __asm__("_rbm_hook_midi_send3");
extern "C" void flx4_send3(void* out, const MidiMessage& m) { sendRouted(RBM_ORIG(midi_send1, SendFn), out, m); }
#endif
extern "C" void rbm_hook_midi_send4(void* out, const MidiMessage& m) { sendRouted(RBM_ORIG(midi_send4, SendFn), out, m); }

// Development: messages as if from the controller, and the on-screen mixer's components (see the top).
static void dumpComponent(void* c, int depth, int maxDepth) {
    JRect r = screenBounds(c);
    char text[64] = "";
    if (isA(c, "N4juce5LabelE")) {
        snprintf(text, sizeof text, " \"%s\"", memberString(A_labelText, c, false).c_str());
    } else if (isSlider(c)) snprintf(text, sizeof text, " value %d", sliderValue(c));
    logf("flx4: %*s%s %d,%d %dx%d%s%s", depth * 2, "", className(c), r.x, r.y, r.w, r.h, showing(c) ? "" : " (hidden)", text);
    if (depth < maxDepth)
        for (int i = 0, n = numChildren(c); i < n; i++) dumpComponent(child(c, i), depth + 1, maxDepth);
}
static void devTools() {
    std::string inject = cfgStr("flx4_inject");
    if (!inject.empty() && g_dev) {
        setCfg("flx4_inject", "");
        for (size_t at0 = 0; at0 < inject.size();) {
            size_t end = inject.find(',', at0);
            std::string one = inject.substr(at0, end == std::string::npos ? std::string::npos : end - at0);
            unsigned a, b, c;
            if (sscanf(one.c_str(), "%x %x %x", &a, &b, &c) == 3) {
                MidiMessage m = message(a, b, c);
                logMessage("in (injected)", m.inl, 3);
                // as from the controller, through the mod: on the MIDI thread's path, else the main thread's (as inject)
                if (!rbm_hook_midi_in(g_dev, m)) at<void (*)(void*, MidiMessage&)>(A_midiInData)(g_dev, m);
            }
            if (end == std::string::npos) break;
            at0 = end + 1;
        }
    }
    if (cfgInt("flx4_dump", 0)) {
        setCfg("flx4_dump", "0");
        void* main = at<void* (*)()>(A_mainComponent)();
        void* panel = main ? findComponent(main, "17MixerControlPanel") : nullptr;
        if (panel) dumpComponent(panel, 0, 4);
    }
}

// The FLX4's mapping is made (MidiMap::create -> its constructor): jog processors for decks 3/4 too.
namespace {
struct DeviceVariant {  // djplay::DeviceVariant, as the constructor uses it
    void* vtable;
    int type;           // +0x08, 0: none
    int pad0;
    bool b;             // +0x10, the value, for a bool
    uint8_t pad[7];
    uint8_t block[16];  // +0x18, juce::MemoryBlock
};
static_assert(offsetof(DeviceVariant, b) == 0x10 && offsetof(DeviceVariant, block) == 0x18, "DeviceVariant layout");
}  // namespace
static void addJogDecks(char* map) {
    using Perf = void**;  // IControllerPerformanceProcessor (vtable: [1] resolution(int), [3] vinyl(bool))
    for (int s = 2; s < 4; s++) {
        int deck = s + 1;
        if (*(void**)(map + O_jogScratch + 8 * s) || *(void**)(map + O_jogSearch + 8 * s)) continue;
        for (int search = 0; search < 2; search++) {
            void* proc = ::operator new(0xc0);
            StrArg name(search ? "@JogSearch" : "@JogScratch");
#ifdef _WIN32
            const double kScratchSpeed = 2.1;  // (the Windows constructor's own; the mac's is 2.4)
#else
            const double kScratchSpeed = 2.4;
#endif
            void* perf = search ? at<void* (*)(int, int, int, int)>(A_scanProcessor)(deck, 1, 10, 0)
                                : at<void* (*)(int, double, double, int)>(A_timeStampProcessor)(deck, 1.0, kScratchSpeed, 3);
            at<void (*)(void*, int, JStr*, void*)>(A_jogProcessor)(proc, deck, name.get(), perf);
            *(void**)(map + (search ? O_jogSearch : O_jogScratch) + 8 * s) = proc;
            Perf p = *(Perf*)((char*)proc + O_jogPerf);
            ((void (*)(Perf, int))(*(void***)p)[1])(p, kJogResolution);
            if (search) continue;
            // VINYL (scratch when the jog's top is touched), as set for the deck: what the constructor does for 1/2
            alignas(16) uint8_t comp[0x400] = {};
            StrArg unit("@JogScratch");
            at<void (*)(void*, int, JStr*, bool)>(A_deviceComponent)(comp, deck, unit.get(), true);
            DeviceVariant v{};
            v.vtable = (void*)(A_variantVtable + g_slide);
            at<void (*)(void*)>(A_memoryBlock)(v.block);
            at<void (*)(void*, int, void*, const void*, DeviceVariant*)>(A_getProperty)(comp, *(int*)(comp + 8), comp + 0x10, (void*)(A_vinylId + g_slide), &v);
            if (v.type) ((void (*)(Perf, bool))(*(void***)p)[3])(p, v.b);
            logf("flx4: deck %d jog processors made (vinyl %s)", deck, v.type ? (v.b ? "on" : "off") : "unset");
            v.vtable = (void*)(A_variantVtable + g_slide);
            at<void (*)(void*)>(A_memoryBlockDtor)(v.block);
            at<void (*)(void*)>(A_deviceComponentDtor)(comp);
        }
    }
}
// MidiMap's children, a std::deque<DeviceObject*>: libc++'s (map split buffer, start, size) on the mac; on Windows
// MSVC's (container proxy, map, mapsize, myoff, mysize: 0x28 bytes, as rekordbox's own inline push_back in the
// MidiMapDDJFLX4 constructor shows), which is what <deque> is when compiled here.
static void pushChild(char* map, void* unit) {
#ifdef _WIN32
    static_assert(sizeof(std::deque<void*>) == 0x28, "MSVC deque layout (proxy, map, mapsize, myoff, mysize)");
    ((std::deque<void*>*)(map + O_mapChildStart - 0x18))->push_back(unit);
#else
    ((std::deque<void*>*)(map + O_mapChildStart - 0x20))->push_back(unit);
#endif
}
static void addUnits(char* map) {
    for (int deck = 3; deck <= 4; deck++)
        for (const char* name : {"@LoopInCDJ", "@LoopOutCDJ", "@JogPause"}) {
            void* unit = ::operator new(0xb8);
            StrArg n(name);
            at<void (*)(void*, int, JStr*, bool)>(A_deviceUnit)(unit, deck, n.get(), true);
            at<void (*)(void*)>(A_enter)(map + O_mapLock);
            (*(void (**)(void*, void*))(*(char**)unit + V_setParent))(unit, map);  // DeviceObject::setParent
            *(uint8_t*)((char*)unit + 0x1c) = 1;  // (same on Windows: the constructor's own units do this after setParent)
            pushChild(map, unit);  // deleted with the map
            at<void (*)(void*)>(A_exit)(map + O_mapLock);
        }
    logf("flx4: decks 3/4 loop and jog pause units made");
}
// Returns the map: an MSVC constructor returns this, and MidiMap::create uses it (a void hook left rax clobbered there,
// so rekordbox got a bad map and dropped the controller on Windows, even with the mod off).
extern "C" void* rbm_hook_flx4_map(void* map, int id, void* name) {
    RBM_ORIG(flx4_map, void (*)(void*, int, void*))(map, id, name);
    if (g_on) {
        addJogDecks((char*)map);
        addUnits((char*)map);  // after the jog processors: @JogPause uses them
    }
    return map;
}

// The level meters of decks 3/4: rekordbox computes them (ChannelLevelDeviceUnit) but its FLX4 support sends only
// those of channels 1/2 (MidiMapDDJFLX4::eventAbsoluteValueChangedDelegate), so the mod sends a side's while it
// shows deck 3 or 4, converted as that function does: whole dB (from hundredths), then one value per lit LED.
// A side's meter is its deck's, with flx4_mixer=fixed too (the meter follows the deck select, not the strip).
static uint8_t meterValue(float db) {
    double hundredths = std::clamp((double)db * 100, (double)INT_MIN, (double)INT_MAX);  // as ChannelLevelDeviceUnit (silence: -FLT_MAX)
    int whole = (int)floorf((int)hundredths * 0.01f);
    if (whole > -7) return 127;
    if (whole < -52) return 0;
    return whole >= -7 ? 120 : whole >= -13 ? 101 : whole >= -22 ? 87 : whole >= -37 ? 65 : 43;  // rekordbox's table (-52..-7 dB)
}
static void sendMeters() {  // main thread, every 30 ms (as rekordbox's meters)
    static int last[2] = {-1, -1}, since[2];
    for (int side = 0; side < 2; side++) {
        int d = g_side[side].deck;
        if (g_on && g_output && g_meterResend[side].exchange(0)) {  // just switched: the new deck's level now
            last[side] = meterValue(channelDb(d)), since[side] = 0;
            sendOut(0xB0 | side, 0x02, (uint8_t)last[side]);
            continue;
        }
        if (!g_on || !g_output || d < 2) {
            last[side] = -1;
            continue;
        }
        int v = meterValue(channelDb(d));
        if (v == last[side] && ++since[side] < 16) continue;  // on change, and twice a second anyway
        last[side] = v, since[side] = 0;
        sendOut(0xB0 | side, 0x02, (uint8_t)v);
    }
}
static void startMeters() {
    static bool started;
    if (started) return;
    started = true;
    plat::everyOnMain(0.03, [] { sendMeters(); });
}

// Finds the controller among rekordbox's devices.
// Each deck's tempo as a tempo fader MSB, for pickup (tick). A tempo fader that has picked its deck up lets go again
// when the deck's tempo moved by other means since the fader last set it (BEAT SYNC, a track loaded, the screen).
static void updateTempo() {
    void* e = engine();
    for (int d = 0; d < 4; d++) {
        int msb = -1;
        void* player = e && d < deckCount() && deckLoaded(d) ? at<void* (*)(int)>(A_getUiPlayer)(d) : nullptr;
        int range = player ? *(const int*)((const char*)player + O_tempoRange) : 0;
        if (range > 0) {
            double v = 8192 + at<float (*)(void*, int)>(A_getTempo)(e, d) * 8191.0 / range;
            msb = (int)std::clamp(std::floor(v / 128), 0.0, 127.0);
        }
        g_tempoAt[d] = msb;
    }
    for (int side = 0; side < 2; side++) {
        Side& s = g_side[side];
        int d = s.deck, sent = g_tempoSent[d], cur = g_tempoAt[d];
        if (sent < 0 || cur < 0 || abs(sent - cur) <= 1 || nowSeconds() - g_tempoSentAt[d] < 0.5) continue;
        for (uint8_t cc : {0x00, 0x05}) {
            if (!s.engaged[cc].exchange(false)) continue;
            logf("flx4: deck %d's tempo moved without its fader (%d -> %d): the fader picks it up again", d + 1, sent, cur);
        }
        g_tempoSent[d] = cur;  // (once)
    }
}

static void tick() {
    g_log = cfgInt("flx4_log", 0) != 0;
    g_mixerFollows = cfgStr("flx4_mixer") != "fixed";
    g_stemsOn = at<bool (*)(void*)>(A_stemsEnabled)(nullptr);
    std::string want = cfgStr("flx4_device", "DDJ-FLX4");
    void* found = nullptr;
    char* dm = (char*)at<void* (*)(uintptr_t)>(A_deviceManagerGet)(A_deviceManager + g_slide);
    static bool once;
    if (!once) once = true, logf("flx4: looking for %s (device manager %s)", want.c_str(), dm ? "found" : "missing");
    if (dm) {
        at<void (*)(void*)>(A_enter)(dm + O_unitsLock);
#ifdef _WIN32
        // (MSVC's std::map is {head, size}; the key a 32-byte std::string at the node + 0x20: both as connectMidiDevice's lookup reads it)
        static_assert(sizeof(std::map<std::string, void*>) == 16 && sizeof(std::string) == 32, "MSVC map / string layout");
#endif
        std::string names;
        for (auto& [name, unit] : *(std::map<std::string, void*>*)(dm + O_units)) {
            if (name == want) found = unit;
            names += (names.empty() ? "'" : ", '") + name + "'";
        }
        at<void (*)(void*)>(A_exit)(dm + O_unitsLock);
        static std::string lastNames = "?";
        if (!found && names != lastNames) logf("flx4: no %s among rekordbox's %zu devices: %s", want.c_str(), ((std::map<std::string, void*>*)(dm + O_units))->size(), names.empty() ? "none" : names.c_str());
        lastNames = names;
    }
    if (found != g_dev) {
        void* helper = found ? *(void**)((char*)found + O_outHelper) : nullptr;
        g_output = helper ? *(void**)((char*)helper + O_output) : nullptr;
        g_dev = found;
        for (Side& s : g_side) {
            s.stems = s.shift = false;  // (g_isolator stays: rekordbox's own state)
            for (auto& d : g_deckStems) d = false;
            for (int i = 0; i < 128; i++) s.phys[i] = -1, s.engaged[i] = false, s.dropLsb[i] = false;
            memset(s.held, 0, sizeof s.held);
        }
        g_side[0].deck = 0, g_side[1].deck = 1;
        logf("flx4: %s %s", want.c_str(), found ? "connected" : "gone");
    }
    updateMixer();
    if (g_dev) updateTempo();
    if (g_dev) startMeters();
    if (g_log) {
        static int ticks;
        if (++ticks % 50 == 0 && (g_meters[0] || g_meters[1] || g_meters[2] || g_meters[3]))
            logf("flx4: level meters in 5 s: ch1 %d ch2 %d ch3 %d ch4 %d", g_meters[0].exchange(0), g_meters[1].exchange(0), g_meters[2].exchange(0), g_meters[3].exchange(0));
    }
    devTools();
    static int n;
    if (cfgInt("flx4_meterlog", 0) && ++n % 5 == 0) {  // development: levels, rekordbox's meters and the mod's, TRIM
        char line[256] = "";
        for (int ch = 0; ch < 4; ch++) {
            size_t len = strlen(line);
            float db = channelDb(ch);
            snprintf(line + len, sizeof line - len, " ch%d %.1fdB rb %d mod %d trim %d |", ch + 1, std::max(db, -99.f), g_rbMeter[ch].load(), meterValue(db), g_onScreen[ch][C_TRIM].load());
        }
        logf("flx4: meters%s", line);
    }
}

RBM_MOD({.name = "flx4", .tick = tick, .enabled = &g_on});
