// badge: the chip itself, per platform (mods/badge_view_mac.mm: an AppKit view; mods/badge_view_win.cpp: a child
// window drawn with GDI+). mods/badge.cpp decides what it shows and where.
#pragma once
#include <string>

namespace badgeview {

struct Bar { int x, y, w, h; };  // rekordbox's status bar, screen bounds in JUCE's logical pixels

// Puts the chip on rekordbox's main window (again, if JUCE replaced its view), right of the "rekordbox" logo in
// the status bar (bar: where that is, or null if unknown); hidden while rekordbox shows a message there.
// on = false takes it away. tip: the hover text. pending: the chip's icon pulses orange (a mod switch in the
// config differs from what is running: it applies after a restart). Main thread.
void update(bool on, bool hidden, const Bar* bar, const std::string& tip, bool pending);
// What a click on the chip does.
void setOnClick(void (*fn)());

}  // namespace badgeview
