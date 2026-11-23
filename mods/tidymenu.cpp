// tidymenu: a calmer right-click menu on tracks. rekordbox lists every command it has there, most of them greyed
// out for the track at hand (ten of its twenty-two for a Spotify track), and the mods add one item each below it.
//  - Greyed-out items are left out, and the separators that would then stand at the top, at the bottom or twice in
//    a row with them. Nothing that can be clicked goes away: what is shown works as before (the items are rekordbox's
//    own, copied with their ids, submenus and ticks), and an item is back as soon as rekordbox enables it.
//  - The mods' window items ("Open genres window for this track" and the others) go in one submenu, "platendoos",
//    under short names; what a mod does right there ("Re-analyze track") stays in the menu itself.
// How: core's hook on the track list's PopupMenu::show asks this mod (services below) for the submenu the mods fill,
// and for the menu to show: a copy of rekordbox's without the disabled items (juce's own Item copy constructor and
// addItem), shown in its place.
//   config: tidymenu=0 to disable (restart)
//           tidymenu_hide=1    0: rekordbox's greyed-out items stay
//           tidymenu_group=1   0: the mods' items stay in the menu itself, under their long names
//           tidymenu_log=0     1 logs the items of each menu shown (for debugging)
// Windows: the same juce functions (tools/win/verify/tidymenu.out.json); MSVC's Item is 0x90 bytes (its std::function
// is 0x40), and a by-value argument (the Item, the submenu's name and PopupMenu) is destroyed by the function called.
#include "../core/rbmod.h"
#include <new>
#include <string.h>
#include <string>

using namespace rbm;

namespace {  // file-local

volatile bool g_on = true;

// juce::PopupMenu: an Array<Item> (Item* data, int allocated, int used) and a look-and-feel reference; all zero is
// an empty one (it has no out-of-line default constructor).
const Addr A_menuDtor{"__ZN4juce9PopupMenuD1Ev"};
const Addr A_addSubMenu{"__ZN4juce9PopupMenu10addSubMenuENS_6StringES0_b"};  // addSubMenu(String name, PopupMenu, bool active)
const Addr A_addItem{"__ZN4juce9PopupMenu7addItemENS0_4ItemE"};              // addItem(Item)
const Addr A_itemCopy{"__ZN4juce9PopupMenu4ItemC1ERKS1_"};
#ifndef _WIN32  // (Windows: addItem destroys its argument itself)
const Addr A_itemDtor{"__ZN4juce9PopupMenu4ItemD1Ev"};
#endif
// (x86_64, Intel Macs: the Item's std::function is 0x30 bytes, not 0x20, and 16-aligned: sizeof 0x80, the flags 0x10
// later; addSeparator's "shl rsi, 7" and the copy constructor's byte copies, 7.2.19 and 7.2.20)
const Offset S_item{0x70, "__ZN4juce9PopupMenu12addSeparatorEv", "movz x10", 0, X86{0x80}};         // sizeof(PopupMenu::Item)
const Offset O_itemEnabled{0x64, "__ZN4juce9PopupMenu4ItemC2ERKS1_", "ldr x20", 9, X86{0x74}};      // Item::isEnabled (bool)
const Offset O_itemSeparator{0x66, "__ZN4juce9PopupMenu4ItemC2ERKS1_", "ldr x20", 11, X86{0x76}};   // Item::isSeparator (bool)
const size_t kMenuSize = 0x40;   // room for a PopupMenu (0x18)
const size_t kItemRoom = 0x200;  // room for an Item (S_item, checked)

struct Items { char* data; int allocated, used; };  // juce::Array<PopupMenu::Item>
const Items& itemsOf(const Menu* m) { return *(const Items*)m; }
void destroyMenu(void* m) { at<void (*)(void*)>(A_menuDtor)(m); }

// Main thread only: one menu is built and shown at a time.
alignas(16) char g_subRoom[kMenuSize], g_tidyRoom[kMenuSize];
bool g_sub, g_tidy;  // the rooms hold a menu

// "tidymenu.submenu": an empty menu for the mods' items of the track menu being built (null: they go in the menu itself).
Menu* submenu() {
    if (g_sub) destroyMenu(g_subRoom);
    g_sub = false;
    if (!cfgInt("tidymenu_group", 1)) return nullptr;
    memset(g_subRoom, 0, sizeof g_subRoom);
    g_sub = true;
    return (Menu*)g_subRoom;
}

// The menu without its disabled items, built in g_tidyRoom; false when it has none (or nothing would be left).
bool withoutDisabled(const Menu* menu) {
    const Items& a = itemsOf(menu);
    size_t size = S_item;
    if (!a.data || a.used <= 0 || size > kItemRoom) return false;
    int kept = 0, hidden = 0;
    for (int i = 0; i < a.used; i++) {
        const char* item = a.data + i * size;
        if (item[O_itemSeparator]) continue;
        (item[O_itemEnabled] ? kept : hidden)++;
    }
    logf("tidymenu: %d of %d items shown (the rest is greyed out)", kept, kept + hidden);
    if (!hidden || !kept) return false;
    memset(g_tidyRoom, 0, sizeof g_tidyRoom);
    g_tidy = true;
    Menu* out = (Menu*)g_tidyRoom;
    bool separator = false;  // one is due before the next item
    for (int i = 0; i < a.used; i++) {
        const char* item = a.data + i * size;
        if (item[O_itemSeparator]) { separator = true; continue; }
        if (!item[O_itemEnabled]) continue;
        if (separator && itemsOf(out).used) menuAddSeparator(out);
        separator = false;
        // addItem(Item) takes the item by value: a copy passed by address, destroyed by the caller (Itanium ABI) or
        // by addItem itself (MSVC).
        alignas(16) char copy[kItemRoom];
        at<void (*)(void*, const void*)>(A_itemCopy)(copy, item);
        at<void (*)(Menu*, void*)>(A_addItem)(out, copy);
#ifndef _WIN32
        at<void (*)(void*)>(A_itemDtor)(copy);
#endif
    }
    return true;
}

// The menu's items, one line (tidymenu_log): "Load > | - | Remove from Playlist | (Reload Tag)": greyed out in
// brackets, a separator as "-".
void logItems(const char* what, const Menu* menu) {
    const Items& a = itemsOf(menu);
    std::string s;
    for (int i = 0; i < a.used; i++) {
        const char* item = a.data + i * (size_t)S_item;
        const char* text = *(const char* const*)item;  // Item::text, a juce::String
        std::string name = item[O_itemSeparator] ? "-" : text ? text : "";
        s += (i ? " | " : "") + (item[O_itemSeparator] || item[O_itemEnabled] ? name : "(" + name + ")");
    }
    logf("tidymenu: %s: %s", what, s.c_str());
}

// "tidymenu.finish": the track menu is built. Hangs the submenu in it (when the mods put something in) and gives
// the menu to show: rekordbox's, or the copy without its disabled items.
Menu* finish(Menu* menu) {
    bool log = cfgInt("tidymenu_log", 0) != 0;
    if (g_sub) {
        if (log) logItems("the submenu", (Menu*)g_subRoom);
        bool destroyed = false;
        if (itemsOf((Menu*)g_subRoom).used > 0) {
#ifdef _WIN32
            // (by-value String and PopupMenu: passed by address; addSubMenu destroys both, MSVC)
            alignas(JStr) char nb[sizeof(JStr)];
            at<void (*)(Menu*, JStr*, void*, bool)>(A_addSubMenu)(menu, new (nb) JStr("platendoos"), g_subRoom, true);
            destroyed = true;
#else
            JStr name("platendoos");
            // (by-value String and PopupMenu: passed by address; this side destroys them)
            at<void (*)(Menu*, JStr*, void*, bool)>(A_addSubMenu)(menu, &name, g_subRoom, true);
#endif
        }
        if (!destroyed) destroyMenu(g_subRoom);
        g_sub = false;
    }
    if (g_tidy) destroyMenu(g_tidyRoom), g_tidy = false;
    if (log) logItems("rekordbox's menu", menu);
    if (!cfgInt("tidymenu_hide", 1) || !withoutDisabled(menu)) return menu;
    if (log) logItems("shown", (Menu*)g_tidyRoom);
    return (Menu*)g_tidyRoom;
}

// "tidymenu.done": the menu was shown.
void done() {
    if (g_tidy) destroyMenu(g_tidyRoom);
    g_tidy = false;
}

}  // namespace

RBM_PROVIDE("tidymenu", "tidymenu.submenu", submenu, &g_on);
RBM_PROVIDE("tidymenu", "tidymenu.finish", finish, &g_on);
RBM_PROVIDE("tidymenu", "tidymenu.done", done, &g_on);
RBM_MOD({.name = "tidymenu", .enabled = &g_on});
