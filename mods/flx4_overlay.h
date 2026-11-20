// What flx4 shows over rekordbox's window: toasts for the controller's switches, and the marks on rekordbox's
// on-screen mixer. They are native views, so each platform draws them (macOS: flx4_overlay_mac.mm); flx4.cpp only
// says what to show. Rectangles are in juce's screen coordinates (origin at the main screen's top left).
#pragma once
#include <string>

namespace flx4ui {

struct Rect { int x, y, w, h; };   // w == 0: not there

constexpr int kControls = 6;   // a channel's mixer controls: TRIM, HI, MID, LOW, FADER, CFX
constexpr int kFader = 4;

// The marks on the on-screen mixer. Sides are the controller's (0 left, 1 right), indexing the channel each strip
// controls; the CFX knobs are the mod's own drawing, for all four channels.
struct MixerMarks {
    bool shown = false;                    // the mixer is on screen (else nothing is drawn)
    Rect column[2] = {};                   // outline of the channel each side's strip controls
    Rect deck[4] = {};                     // where each deck is (four-deck layout; else none): toasts go over theirs
    Rect control[2][kControls] = {};       // that channel's controls
    int phys[2][kControls];                // where the controller's knob or fader is (0-127) if its control has yet to pick up
                                           // the channel's value (marked in orange), else -1
    Rect cfxKnob[4] = {}, cfxLabel[4] = {};  // the CFX knob and the label under it, per channel
    int cfxValue[4];                       // 0-127, -1 none
    MixerMarks() {
        for (auto& s : phys) for (int& p : s) p = -1;
        for (int& v : cfxValue) v = -1;
    }
};

bool windowAvailable();   // rekordbox's main window is there to draw on (main thread)
// Shows a message for a moment (main thread): over the deck if it's known (MixerMarks::deck), else over the side's half
// of rekordbox's decks.
void toast(int side, const std::string& title, const std::string& detail, int deck = -1);
// Draws the marks (main thread, every tick; cheap when unchanged).
void setMixerMarks(const MixerMarks& marks);

}  // namespace flx4ui
