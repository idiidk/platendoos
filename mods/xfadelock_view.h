// xfadelock: the lock button itself, per platform (mods/xfadelock_mac.mm: an AppKit view on rekordbox's window;
// mods/xfadelock_win.cpp: a child window of rekordbox's main window drawn with GDI+). mods/xfadelock.cpp decides
// where it goes and what a click does.
#pragma once

namespace xfadelockview {

enum { kFree, kLocked, kPickup };  // the lock's state (kPickup: unlocked, the controller's crossfader on its way)
struct Rect { int x, y, w, h; };   // screen bounds in JUCE's logical pixels

// Whether rekordbox's main window is there to put the button on. Main thread.
bool hostReady();
// Puts the button at r (screen) in the look of state with the hover text tip; r null hides it. Main thread.
void update(const Rect* r, int state, const char* tip);
// What a click on the button does; it returns the state to show from then on.
void setOnClick(int (*fn)());

}  // namespace xfadelockview
