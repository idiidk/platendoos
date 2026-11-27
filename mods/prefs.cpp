// prefs: rekordbox's own preferences in the platendoos settings window.
//
// rekordbox's Preferences dialog is ten categories with tabs and long pages. The settings window (mods/badge.cpp,
// ui/settings.html, the Preferences window) shows the same settings next to the mods' own, as cards and rows with a search over all of them.
// Nothing about what rekordbox has to set is written down here: it is read from rekordbox's own dialog, in the
// language rekordbox is set to, and every change goes through rekordbox's own control, so its own code applies it.
//
// How: while the settings window is open the mod keeps a Preferences component of its own (the dialog's content,
// PreferenceComponent, made the way PreferenceDialog makes it) that is never put on screen. A category and a page
// are selected in it as the dialog's buttons do it, and the rows it then holds are read: juce labels, toggle
// buttons (with their radio group), drop-downs, sliders and buttons, each with where it sits in its row, plus
// rekordbox's "?" help texts and plan locks. The page lays them out again (ui/settings.html: the layout rules are
// there). A change in the window is done to the hidden control: a click for a toggle or a button, the chosen id for
// a drop-down, the value (and the end of a drag, which the buffer size waits for) for a slider. rekordbox saves its
// settings when its dialog closes; here they are saved a moment after a change and when the window closes, with
// the same three calls.
//
// What isn't mirrored: rows with something else in them (lists, text fields, the keyboard shortcut editor, signal
// scopes) are shown as a line with a button that opens rekordbox's own Preferences at that category, and the
// Plan, Keyboard and DVS categories as a whole (an account, an editor, and scopes that listen to an input).
//
// rekordbox's Preferences command (its menu item, shortcut and gear button) opens the settings window instead of
// rekordbox's dialog while the badge mod (the window's) is on: see "rekordbox's Preferences open this window" below.
//
// Test commands (command-prefs in the config folder): "scan" reads every page and writes it to prefs-state.json
// there; "msg <json>" is handled as a message from the page; "command" is rekordbox's Preferences command, as its
// menu item gives it (MainComponent's command target, through the vtable slot).
//
//   config: prefs=0 to disable
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <tlhelp32.h>
#endif
#include "../core/rbmod.h"
#include "../core/webui.h"
#include <algorithm>
#include <map>
#include <set>
#include <string>
#include <vector>
using namespace rbm;
using rbm::webui::jsonGet;
using rbm::webui::jsonStr;

namespace {  // file-local

volatile bool g_on;

// rekordbox's Preferences
const Addr A_prefCtor{"__ZN19PreferenceComponentC1E22PreferenceCategoryType18PreferencePageType"};  // (this, category, page: -1 = the last one)
const Addr A_selectCategory{"__ZN19PreferenceComponent14selectCategoryERKN4juce6StringE"};
const Addr A_setPageName{"__ZN29PreferencePageSelectComponent11setPageNameERKN4juce6StringE"};
const Addr A_pageClicked{"__ZN29PreferencePageSelectComponent13buttonClickedEPN4juce6ButtonE"};  // a click on a tab (or its other buttons)
const Addr A_categoryOfType{"__ZN27PreferenceCategoryComponent12CategoryData4findERK22PreferenceCategoryType"};  // {name, ...}* of a type (0..10)
const Addr A_mainComponent{"__ZN13MainComponent11getInstanceEv"};
const Addr A_showPreference{"__ZN13MainComponent14showPreferenceE22PreferenceCategoryType"};  // rekordbox's own dialog
// MainComponent::perform as its ApplicationCommandTarget part's (the part at +656; Windows: no thunk, the part at
// 656 + thisDelta = 568 and perform itself)
const Addr A_performThunk{"__ZThn656_N13MainComponent7performERKN4juce24ApplicationCommandTarget14InvocationInfoE"};
const Addr A_settingIF{"__ZN6djplay9SettingIF11getInstanceEv"};
const Addr A_saveGeneral{"__ZN6djplay9SettingIF29savePreferredSettings_GeneralEv"};
const Addr A_saveAudioMixer{"__ZN6djplay9SettingIF32savePreferredSettings_AudioMixerEbbb"};
const Addr A_savePlayer{"__ZN6djplay9SettingIF28savePreferredSettings_PlayerEv"};
// juce
const Addr A_numChildren{"__ZNK4juce9Component21getNumChildComponentsEv"};
const Addr A_child{"__ZNK4juce9Component17getChildComponentEi"};
const Addr A_enabled{"__ZNK4juce9Component9isEnabledEv"};
const Addr A_modal{"__ZN4juce9Component26getCurrentlyModalComponentEi"};
const Addr A_labelText{"__ZNK4juce5Label7getTextEb"};
const Addr A_toggleState{"__ZNK4juce6Button14getToggleStateEv"};
const Addr A_triggerClick{"__ZN4juce6Button12triggerClickEv"};  // a click, handled by the button as one of the mouse (posted)
const Addr A_comboN{"__ZNK4juce8ComboBox11getNumItemsEv"};
const Addr A_comboItem{"__ZNK4juce8ComboBox11getItemTextEi"};
const Addr A_comboItemId{"__ZNK4juce8ComboBox9getItemIdEi"};
const Addr A_comboSelected{"__ZNK4juce8ComboBox13getSelectedIdEv"};
const Addr A_comboSelect{"__ZN4juce8ComboBox13setSelectedIdEiNS_16NotificationTypeE"};
const Addr A_sliderValue{"__ZNK4juce6Slider8getValueEv"};
const Addr A_sliderMin{"__ZNK4juce6Slider10getMinimumEv"};
const Addr A_sliderMax{"__ZNK4juce6Slider10getMaximumEv"};
const Addr A_sliderSet{"__ZN4juce6Slider8setValueEdNS_16NotificationTypeE"};
// The start and the end of a drag of a slider, as its listeners hear them. Windows has the two only inside
// Slider::ScopedDragNotification (an object holding the Slider*), the mac only its destructor: each its own pair.
#ifdef _WIN32
const Addr A_dragScope{"juce::Slider::ScopedDragNotification's constructor"};  // (scope*, Slider*)
const Addr A_dragScopeEnd{"__ZN4juce6Slider22ScopedDragNotificationD1Ev"};       // (scope*)
#else
const Addr A_dragStart{"__ZN4juce6Slider5Pimpl13sendDragStartEv"};
const Addr A_dragEnd{"__ZN4juce6Slider5Pimpl11sendDragEndEv"};
#endif
const Addr A_stringUtf8{"__ZNK4juce6String9toRawUTF8Ev"};
// Fields
const Offset O_flags{0xe8, "__ZNK4juce9Component9isShowingEv", "ldr x0", 0};          // juce::Component's flags: bit 1 = visible
const Offset O_name{0x8, "__ZN4juce9Component7setNameERKNS_6StringE", "add x0", 0};   // its name (a juce::String)
const Offset O_buttonText{0x158, "__ZN4juce6Button13setButtonTextERKNS_6StringE", "add x0", 0, X86{0x178}};
const Offset O_radioGroup{0x1ac, "__ZN4juce6Button15setRadioGroupIdEiNS_16NotificationTypeE", "ldr x0", 0, X86{0x1cc}};
const Offset O_sliderPimpl{0x1a0, "__ZNK4juce6Slider8getValueEv", "ldr x0", 0, X86{0x1f0}};
const Offset O_sliderInterval{0x108, "__ZN4juce6Slider5Pimpl11updateRangeEv", "ldr x19", 0, X86{0x110}};  // in the Pimpl: the range's step
const Offset O_comboLabel{0x1b8, "__ZN4juce8ComboBox13setSelectedIdEiNS_16NotificationTypeE", "ldr x20", 2, X86{0x1d0}};  // juce::ComboBox: the Label* that shows its text
const Offset O_tipText{0xf8, "__ZN16ToolTipComponent7setTextERKN4juce6StringES3_", "add x20", 0};  // ToolTipComponent: its text...
const Offset O_tipTitle{0x100, "__ZN16ToolTipComponent7setTextERKN4juce6StringES3_", "add x0", 0};  // ...and its title
// A help mark without a text of its own has it as its button's tooltip (ToolTipComponent::setTooltip).
const Offset O_tipButton{0x110, "__ZN16ToolTipComponent10setTooltipERKN4juce6StringE", "ldr x0", 0};  // its juce::Button*
// A Button's tooltip (a juce::String). MSVC's Button::setTooltip works on the button's tooltip part alone, so no
// Windows code reads the field from the Button: there it is taken as the string of the part that follows
// juce::Component, whose last field is the flags word (0xe8 + 0x10 = 0xf8, as on the mac; the emulator test reads
// real tooltips through it).
#ifdef _WIN32
size_t buttonTip() { return O_flags + 0x10; }
#else
const Offset O_buttonTip{0xf8, "__ZN4juce6Button10setTooltipERKNS_6StringE", "add x0", 0};
size_t buttonTip() { return O_buttonTip; }
#endif
const int kBounds = 0x38;      // juce::Component: x, y, width, height (ints; its code reads them in pairs, no recipe)
const int kPrefSize = 0x138;   // sizeof(PreferenceComponent) (PreferenceDialog::createComponent's operator new)
const int kCategories = 11;    // PreferenceCategoryType: 0..10

template <typename F> F fn(const Addr& a) { return at<F>(a); }
int kids(void* c) { return fn<int (*)(void*)>(A_numChildren)(c); }
void* kid(void* c, int i) { return fn<void* (*)(void*, int)>(A_child)(c, i); }
bool shown(void* c) { return (*(const uint8_t*)((const char*)c + O_flags)) & 2; }
int field(void* c, int i) { return *(const int*)((const char*)c + kBounds + 4 * i); }  // 0 x, 1 y, 2 width, 3 height
const char* str(void* c, size_t off) {  // a juce::String field's text
    const char* s = *(const char* const*)((const char*)c + off);
    return s ? s : "";
}

// ---- which class a component is ------------------------------------------------------------------------------
// Its class and that class's bases, by plain name ("juce::Label", "ToolTipComponent": the same on both systems),
// from its RTTI. Kept per class: a page is a thousand questions about the same thirty classes.
#ifdef _WIN32
// ".?AVLabel@juce@@" -> "juce::Label" (a part "?A0x...": an anonymous namespace, left out)
std::string plainName(const char* decorated) {
    if (strncmp(decorated, ".?A", 3) || !decorated[3]) return decorated;
    std::vector<std::string> parts;
    const char* p = decorated + 4;
    while (*p && *p != '@') {
        const char* e = strchr(p, '@');
        if (!e) break;
        if (*p != '?') parts.emplace_back(p, e - p);
        p = e + 1;
    }
    std::string out;
    for (size_t i = parts.size(); i-- > 0;) out += (out.empty() ? "" : "::") + parts[i];
    return out.empty() ? decorated : out;
}
// MSVC: the vptr's slot -1 is the complete object locator; its class hierarchy lists the class and every base.
const void* typeOf(void* c) { return (*(void***)c)[-1]; }
std::vector<std::string> namesOf(const void* locator) {
    const char* col = (const char*)locator;
    const char* base = col - *(const uint32_t*)(col + 20);      // pSelf: the locator's own offset in the image
    const char* chd = base + *(const uint32_t*)(col + 16);      // the class hierarchy descriptor
    uint32_t n = *(const uint32_t*)(chd + 8);
    const uint32_t* bases = (const uint32_t*)(base + *(const uint32_t*)(chd + 12));
    std::vector<std::string> names;
    for (uint32_t i = 0; i < n && i < 32; i++) names.push_back(plainName(base + *(const uint32_t*)(base + bases[i]) + 16));  // (the first is the class itself)
    if (names.empty()) names.push_back(plainName(base + *(const uint32_t*)(col + 12) + 16));
    return names;
}
#else
// "12ViewLanguage" -> "ViewLanguage", "juce::ListBox" -> "juce::ListBox" (a part "_GLOBAL__N_1": an anonymous
// namespace, left out)
std::string plainName(const char* mangled) {
    std::string s = mangled, out;
    size_t i = !s.empty() && s[0] == 'N' ? 1 : 0;
    while (i < s.size() && isdigit((unsigned char)s[i])) {
        size_t n = 0;
        while (i < s.size() && isdigit((unsigned char)s[i])) n = n * 10 + (s[i++] - '0');
        if (i + n > s.size()) break;
        if (s.compare(i, n, "_GLOBAL__N_1")) out += (out.empty() ? "" : "::") + s.substr(i, n);
        i += n;
    }
    return out.empty() ? s : out;
}
// Itanium: the vptr's slot -1 is the type_info, which names its base (single inheritance) or bases.
const void* typeOf(void* c) { return (*(void***)c)[-1]; }
void addNames(const void* type, std::vector<std::string>& names, int depth) {
    if (!type || depth > 8) return;
    names.push_back(plainName(*(const char* const*)((const char*)type + 8)));
    const char* kind = *(const char* const*)((const char*)(*(void***)type)[-1] + 8);  // the type_info's own class
    if (strstr(kind, "__si_class_type_info")) addNames(*(const void* const*)((const char*)type + 16), names, depth + 1);
    if (strstr(kind, "__vmi_class_type_info")) {
        unsigned n = *(const unsigned*)((const char*)type + 20);
        for (unsigned i = 0; i < n && i < 8; i++) addNames(*(const void* const*)((const char*)type + 24 + 16 * i), names, depth + 1);
    }
}
std::vector<std::string> namesOf(const void* type) {
    std::vector<std::string> names;
    addNames(type, names, 0);
    return names;
}
#endif
const std::vector<std::string>& classes(void* c) {
    static std::map<const void*, std::vector<std::string>>* known = new std::map<const void*, std::vector<std::string>>;
    const void* type = typeOf(c);
    auto it = known->find(type);
    if (it == known->end()) it = known->emplace(type, namesOf(type)).first;
    return it->second;
}
const std::string& cls(void* c) { return classes(c)[0]; }
bool isA(void* c, const char* name) {
    for (const std::string& n : classes(c))
        if (n == name) return true;
    return false;
}
void* findShown(void* c, const char* className, int depth = 0) {
    if (!c || !shown(c)) return nullptr;
    if (cls(c) == className) return c;
    if (depth > 12) return nullptr;
    for (int i = 0, n = kids(c); i < n; i++)
        if (void* r = findShown(kid(c, i), className, depth + 1)) return r;
    return nullptr;
}
std::string trimmed(std::string s) {
    while (!s.empty() && isspace((unsigned char)s.back())) s.pop_back();
    size_t a = 0;
    while (a < s.size() && isspace((unsigned char)s[a])) a++;
    return s.substr(a);
}
std::string num(double v) {
    char b[40];
    snprintf(b, sizeof b, "%.6g", v);
    return b;
}
// (ComboBox::getText itself is no function of its own on Windows: its label's text is the same)
std::string comboText(void* combo) {
    void* label = *(void* const*)((const char*)combo + O_comboLabel);
    return label ? memberString(A_labelText, label, false) : "";
}
std::string buttonText(void* b) { return *(const char* const*)((const char*)b + O_buttonText) ? fn<const char* (*)(const void*)>(A_stringUtf8)((const char*)b + O_buttonText) : ""; }

// ---- reading a page ------------------------------------------------------------------------------------------

// The controls under c, flat: each with what it is, where it sits in its row and its path of child indexes from the
// page ("2/1/0": what a message of the window names it by). Hidden ones are left out.
void readItems(void* c, const std::string& path, int ox, int oy, std::string& out, int depth = 0) {
    for (int i = 0, n = kids(c); i < n; i++) {
        void* k = kid(c, i);
        if (!k || !shown(k)) continue;
        int x = ox + field(k, 0), y = oy + field(k, 1);
        std::string p = path + "/" + std::to_string(i), j;
        if (cls(k) == "ToolTipComponent") {
            std::string text = trimmed(str(k, O_tipText));
            void* mark = *(void* const*)((const char*)k + O_tipButton);
            if (text.empty() && mark) text = trimmed(str(mark, buttonTip()));
            if (text.empty()) continue;  // (nothing to say)
            j = "\"k\":\"tip\",\"t\":" + jsonStr(text) + ",\"title\":" + jsonStr(trimmed(str(k, O_tipTitle)));
        } else if (cls(k) == "LockComponent") {
            j = "\"k\":\"lock\"";
        } else if (isA(k, "juce::Label")) {
            std::string t = trimmed(memberString(A_labelText, k, false));
            if (t.empty()) continue;
            j = "\"k\":\"label\",\"t\":" + jsonStr(t);
        } else if (isA(k, "juce::ToggleButton")) {
            j = "\"k\":\"toggle\",\"t\":" + jsonStr(trimmed(buttonText(k))) + ",\"on\":" + (fn<bool (*)(void*)>(A_toggleState)(k) ? "1" : "0") +
                ",\"r\":" + std::to_string(*(const int*)((const char*)k + O_radioGroup));
        } else if (isA(k, "juce::TextButton") || isA(k, "juce::HyperlinkButton") || cls(k) == "CustomTextButton") {  // (rekordbox's own: a juce::Button with a text)
            std::string t = trimmed(buttonText(k));
            j = t.empty() ? "\"k\":\"other\",\"c\":" + jsonStr(cls(k))  // (a button with only a picture)
                          : "\"k\":\"button\",\"t\":" + jsonStr(t) + (isA(k, "juce::HyperlinkButton") ? ",\"link\":1" : "");
        } else if (isA(k, "juce::ComboBox")) {
            j = "\"k\":\"combo\",\"v\":" + std::to_string(fn<int (*)(void*)>(A_comboSelected)(k)) + ",\"t\":" + jsonStr(comboText(k)) + ",\"o\":[";
            for (int a = 0, m = fn<int (*)(void*)>(A_comboN)(k); a < m && a < 300; a++)
                j += std::string(a ? "," : "") + "[" + std::to_string(fn<int (*)(void*, int)>(A_comboItemId)(k, a)) + "," + jsonStr(memberString(A_comboItem, k, a)) + "]";
            j += "]";
        } else if (isA(k, "juce::Slider")) {
            const char* pimpl = *(const char* const*)((const char*)k + O_sliderPimpl);
            j = "\"k\":\"slider\",\"v\":" + num(fn<double (*)(void*)>(A_sliderValue)(k)) + ",\"min\":" + num(fn<double (*)(void*)>(A_sliderMin)(k)) +
                ",\"max\":" + num(fn<double (*)(void*)>(A_sliderMax)(k)) + ",\"step\":" + num(pimpl ? *(const double*)(pimpl + O_sliderInterval) : 0);
        } else if (isA(k, "juce::Button") || isA(k, "juce::TextEditor") || isA(k, "juce::ListBox") || isA(k, "juce::TreeView") ||
                   isA(k, "juce::Viewport") || kids(k) == 0 || depth > 6) {
            if (cls(k) == "juce::Component") continue;  // (a spacer)
            j = "\"k\":\"other\",\"c\":" + jsonStr(cls(k));  // something the window can't show: the row is left to rekordbox
        } else {
            readItems(k, p, x, y, out, depth + 1);  // a part of the row: what it holds
            continue;
        }
        out += std::string(out.empty() ? "" : ",") + "{\"p\":\"" + p + "\",\"x\":" + std::to_string(x) + ",\"y\":" + std::to_string(y) + ",\"w\":" +
               std::to_string(field(k, 2)) + ",\"h\":" + std::to_string(field(k, 3)) + (fn<bool (*)(void*)>(A_enabled)(k) ? "" : ",\"off\":1") + "," + j + "}";
    }
}

// The rows of the page the component shows now ("[...]"), top to bottom. Not the mods' own rows in it (outputsync's
// is a PreferenceBaseItem itself: its settings have their place in the window).
std::string readPage(void* pageSelect) {
    void* page = findShown(pageSelect, "ItemPageComponent");
    if (!page) return "[]";
    std::vector<std::pair<int, std::string>> rows;  // (the children aren't in the order they are shown in)
    for (int i = 0, n = kids(page); i < n; i++) {
        void* r = kid(page, i);
        if (!r || !shown(r) || cls(r) == "PreferenceBaseItem") continue;
        std::string items;
        readItems(r, std::to_string(i), 0, 0, items);
        rows.push_back({field(r, 1), "{\"c\":" + jsonStr(cls(r)) + ",\"i\":" + std::to_string(i) + ",\"h\":" + std::to_string(field(r, 3)) + ",\"items\":[" + items + "]}"});
    }
    std::stable_sort(rows.begin(), rows.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    std::string out;
    for (auto& r : rows) out += (out.empty() ? "" : ",") + r.second;
    return "[" + out + "]";
}

// ---- the hidden Preferences component ------------------------------------------------------------------------

struct Page {
    std::string id, title, rows;  // rows: JSON, as read last
    bool own = false;             // left to rekordbox's dialog (never built in the hidden component)
};
struct Category {
    std::string id, title;
    int y = 0;       // where its button is in rekordbox's list: the order
    bool own = false;  // left to rekordbox's dialog as a whole
    std::vector<Page> pages;
};
void* g_pref;        // PreferenceComponent, never shown
void* g_catList;     // its PreferenceCategoryComponent
void* g_pageSelect;  // its PreferencePageSelectComponent
std::vector<Category> g_cats;
std::string g_cat, g_page;  // what the component shows now ("" before the first selection)
bool g_selected;
std::string g_json;         // what the window gets (state())
double g_readAt[3];         // when the shown page is read again: a click is handled a moment later
double g_saveAt;            // when rekordbox's settings are saved (0: nothing to save)
int g_openType = -1;        // rekordbox's own dialog is to open at this category...
std::string g_openPage;     // ...and this page of it
bool g_dialogOpened;        // ...and did: the shown page is read anew once it has closed
bool g_broken;
double g_clickAt;           // when a click or a choice was last handed to a control: it is handled a moment later
std::vector<std::string> g_waiting;  // messages for another page that came meanwhile: after it
const double kClickTime = 0.2;       // s a posted click gets before the page it is on may go
std::string g_lastCat, g_lastPage;   // where rekordbox's dialog would open (what the component showed when made)

bool ready() {
    static int ok = -1;
    if (ok >= 0) return ok;
    for (const Addr* a : {&A_prefCtor, &A_selectCategory, &A_setPageName, &A_pageClicked, &A_categoryOfType, &A_mainComponent, &A_showPreference, &A_performThunk, &A_settingIF, &A_saveGeneral,
                          &A_saveAudioMixer, &A_savePlayer, &A_numChildren, &A_child, &A_enabled, &A_modal, &A_labelText, &A_toggleState, &A_triggerClick, &A_comboN,
                          &A_comboItem, &A_comboItemId, &A_comboSelected, &A_comboSelect, &A_sliderValue, &A_sliderMin, &A_sliderMax, &A_sliderSet,
#ifdef _WIN32
                          &A_dragScope, &A_dragScopeEnd,
#else
                          &A_dragStart, &A_dragEnd,
#endif
                          &A_stringUtf8})
        if (!(uintptr_t)*a) {
            logf("prefs: %s not found in this rekordbox build: rekordbox's preferences aren't shown in the settings window", a->name);
            return ok = 0;
        }
    for (const Offset* o : {&O_flags, &O_name, &O_buttonText, &O_radioGroup, &O_sliderPimpl, &O_sliderInterval, &O_comboLabel, &O_tipText, &O_tipTitle, &O_tipButton,
#ifndef _WIN32
                            &O_buttonTip,
#endif
                           })
        if (!o->ok) {
            logf("prefs: a field of %s wasn't found in this rekordbox build: rekordbox's preferences aren't shown in the settings window", o->fn);
            return ok = 0;
        }
    return ok = 1;
}

// Categories the window leaves to rekordbox's dialog as a whole: the account, the keyboard shortcut editor, and the
// DVS page, whose scopes listen to an input while it exists.
bool ownCategory(const std::string& id) { return id == "Plan" || id == "Keyboard" || id == "DVS"; }
// ...and pages: none. (Controller > Recordings has a level meter that draws on a thread of its own for as long as the
// component lives: macOS takes the component down when rekordbox quits (init()), Windows stops that thread (stopMeters()).)
bool ownPage(const std::string&, const std::string&) { return false; }
// What the window gets for those: a row with something it can't show, which it turns into the line with the button.
std::string ownRows(const std::string& key, const std::string& title) {
    return "[{\"c\":" + jsonStr(key) + ",\"i\":0,\"h\":100,\"items\":[{\"p\":\"0\",\"x\":20,\"y\":20,\"w\":500,\"h\":22,\"k\":\"label\",\"t\":" + jsonStr(title) +
           "},{\"p\":\"1\",\"x\":52,\"y\":56,\"w\":500,\"h\":22,\"k\":\"other\",\"c\":\"rekordbox\"}]}]";
}

void selectCategory(const std::string& id) {
    JStr name(id.c_str());
    // (Windows: it takes the part of the component it overrides a virtual of)
    fn<void (*)(void*, const JStr*)>(A_selectCategory)((char*)g_pref + thisDelta(A_selectCategory), &name);
}
void selectPage(const std::string& id) {
    if (id.empty()) return;  // (a category without tabs)
    JStr name(id.c_str());
    fn<void (*)(void*, const JStr*)>(A_setPageName)(g_pageSelect, &name);
}
// The tabs the selected category shows: (name, title). Those after the separator open something (Cloud's Setup
// Wizard) and aren't pages.
std::vector<std::pair<std::string, std::string>> tabs() {
    std::vector<std::pair<std::string, std::string>> list;
    void* holder = findShown(g_pageSelect, "PreferencePageSelectButtonComponent");
    for (int t = 0, n = holder ? kids(holder) : 0; t < n; t++) {
        void* tab = kid(holder, t);
        if (!tab || !shown(tab)) continue;
        if (cls(tab) == "VerticalSeparator") break;
        void* b = kids(tab) > 0 ? kid(tab, 0) : nullptr;
        if (b && isA(b, "juce::Button") && *str(b, O_name)) list.push_back({str(b, O_name), trimmed(buttonText(b))});
    }
    return list;
}

void rebuildJson() {
    std::string j = "{\"categories\":[";
    for (size_t c = 0; c < g_cats.size(); c++) {
        const Category& cat = g_cats[c];
        j += std::string(c ? "," : "") + "{\"id\":" + jsonStr(cat.id) + ",\"title\":" + jsonStr(cat.title) + ",\"pages\":[";
        for (size_t p = 0; p < cat.pages.size(); p++)
            j += std::string(p ? "," : "") + "{\"id\":" + jsonStr(cat.pages[p].id) + ",\"title\":" + jsonStr(cat.pages[p].title) + ",\"rows\":" + cat.pages[p].rows + "}";
        j += "]}";
    }
    g_json = j + "],\"shown\":{\"cat\":" + jsonStr(g_cat) + ",\"page\":" + jsonStr(g_page) + "}}";
}

Page* pageOf(const std::string& cat, const std::string& page, Category** in = nullptr) {
    for (Category& c : g_cats)
        if (c.id == cat)
            for (Page& p : c.pages)
                if (p.id == page) {
                    if (in) *in = &c;
                    return &p;
                }
    return nullptr;
}

// Makes the component and reads every page once (a tenth of a second): what the window lists and searches.
void checkTargets();
void build() {
    if (g_pref || g_broken || !ready()) return;
    double t0 = now();
    void* p = ::operator new(kPrefSize);  // (rekordbox's delete frees it: destroy())
    memset(p, 0, kPrefSize);
    fn<void (*)(void*, int, int)>(A_prefCtor)(p, -1, -1);  // (no category of ours: the one it showed last)
    g_pref = p;
    g_catList = g_pageSelect = nullptr;
    for (int i = 0, n = kids(p); i < n; i++) {  // (the component itself isn't visible: its parts are)
        if (!g_catList) g_catList = findShown(kid(p, i), "PreferenceCategoryComponent");
        if (!g_pageSelect) g_pageSelect = findShown(kid(p, i), "PreferencePageSelectComponent");
    }
    if (!g_catList || !g_pageSelect) {
        logf("prefs: rekordbox's Preferences component isn't built as expected (no category list or page part): not shown in the settings window");
        g_broken = true;
        deleteObject(g_pref);
        g_pref = nullptr;
        return;
    }
    g_cats.clear();
    std::string wasCat, wasPage;  // what it showed: selected again afterwards, so rekordbox's own dialog opens where it was left
    for (int i = 0, n = kids(g_catList); i < n; i++) {
        void* b = kid(g_catList, i);
        if (!b || !shown(b) || !isA(b, "juce::Button") || !*str(b, O_name) || trimmed(buttonText(b)).empty()) continue;
        Category c;
        c.id = str(b, O_name);
        c.title = trimmed(buttonText(b));
        c.y = field(b, 1);
        c.own = ownCategory(c.id);
        if (fn<bool (*)(void*)>(A_toggleState)(b)) wasCat = c.id;
        g_cats.push_back(c);
    }
    std::stable_sort(g_cats.begin(), g_cats.end(), [](const Category& a, const Category& b) { return a.y < b.y; });
    g_lastCat = wasCat;
    if (!wasCat.empty() && !ownCategory(wasCat)) {
        void* holder = findShown(g_pageSelect, "PreferencePageSelectButtonComponent");
        for (int t = 0, n = holder ? kids(holder) : 0; t < n; t++) {
            void* tab = kid(holder, t);
            void* b = tab && shown(tab) && kids(tab) > 0 ? kid(tab, 0) : nullptr;
            if (b && isA(b, "juce::Button") && fn<bool (*)(void*)>(A_toggleState)(b)) wasPage = str(b, O_name);
        }
    }
    int pages = 0;
    for (Category& c : g_cats) {
        if (c.own) {  // one line that opens rekordbox's dialog
            Page own{"", "", ownRows(c.id, c.title)};
            own.own = true;
            c.pages.push_back(own);
            continue;
        }
        selectCategory(c.id);
        auto list = tabs();
        if (list.empty()) list.push_back({"", ""});
        for (auto& t : list) {
            if (ownPage(c.id, t.first)) {
                Page own{t.first, t.second, ownRows(t.first, t.second)};
                own.own = true;
                c.pages.push_back(own);
                continue;
            }
            selectPage(t.first);
            c.pages.push_back({t.first, t.second, readPage(g_pageSelect), false});
            pages++;
        }
    }
    g_selected = false;
    g_cat = g_page = "";
    g_lastPage = wasPage;
    if (!wasCat.empty() && !ownCategory(wasCat) && !ownPage(wasCat, wasPage)) selectCategory(wasCat), selectPage(wasPage);
    rebuildJson();
    logf("prefs: rekordbox's preferences read: %zu categories, %d pages in %.0f ms", g_cats.size(), pages, (now() - t0) * 1000);
    checkTargets();
}

// The mods and settings that ask to be shown on one of rekordbox's pages (Mod::rekordboxPage,
// SettingAbout::rekordboxPage): a name that isn't one of the pages read is logged once, as the window then shows it
// on the category's first page, or in its area when the category isn't there either. (Only known now: the build
// can't check rekordbox's names.)
void checkTargets() {
    static std::set<std::string> told;
    auto check = [](const char* who, const char* target) {
        if (!target || !*target || told.count(target)) return;
        std::string t = target, cat = t.substr(0, t.find('/')), page = t.find('/') == std::string::npos ? "" : t.substr(t.find('/') + 1);
        for (const Category& c : g_cats) {
            if (c.id != cat) continue;
            if (page.empty()) return;
            for (const Page& p : c.pages)
                if (p.id == page) return;
            told.insert(target);
            logf("prefs: %s asks for rekordbox's page '%s', which this rekordbox doesn't have: shown on %s's first page instead", who, target, cat.c_str());
            return;
        }
        told.insert(target);
        logf("prefs: %s asks for rekordbox's category '%s', which this rekordbox doesn't have: shown in its own area instead", who, cat.c_str());
    };
    for (const Mod& m : mods()) check(m.name, m.rekordboxPage);
    for (const Setting* s : settings::all()) check(s->key, s->about.rekordboxPage);
}

// rekordbox saves its settings when its Preferences dialog closes (MainComponent::showPreference's callback).
void save() {
    g_saveAt = 0;
    fn<void (*)(void*)>(A_saveGeneral)(fn<void* (*)()>(A_settingIF)());
    fn<void (*)(void*, bool, bool, bool)>(A_saveAudioMixer)(fn<void* (*)()>(A_settingIF)(), true, true, true);
    fn<void (*)(void*)>(A_savePlayer)(fn<void* (*)()>(A_settingIF)());
}

void destroy() {
    if (!g_pref) return;
    if (g_saveAt) save();
    deleteObject(g_pref);
    g_pref = g_catList = g_pageSelect = nullptr;
    g_selected = false;
    g_cats.clear();
    g_json.clear();
    g_cat = g_page = "";
    g_waiting.clear();
    g_clickAt = 0;
    for (double& t : g_readAt) t = 0;
}

// Shows a page in the component (its rows are made anew: they read their settings) and reads it.
bool show(const std::string& cat, const std::string& page) {
    Category* c = nullptr;
    Page* p = pageOf(cat, page, &c);
    if (!g_pref || !p || p->own) return false;
    selectCategory(cat);
    selectPage(page);
    g_cat = cat, g_page = page, g_selected = true;
    p->rows = readPage(g_pageSelect);
    rebuildJson();
    return true;
}
// Reads the shown page again; true if something changed.
bool reread() {
    Page* p = g_selected ? pageOf(g_cat, g_page) : nullptr;
    if (!g_pref || !p) return false;
    std::string rows = readPage(g_pageSelect);
    if (rows == p->rows) return false;
    p->rows = rows;
    rebuildJson();
    return true;
}
void readSoon() {  // a click is posted: its effect (and what it enables or greys out) shows a moment later
    double t = now();
    g_readAt[0] = t + 0.12, g_readAt[1] = t + 0.5, g_readAt[2] = t + 1.5;
}

// The control a message names: by its path in the shown page.
void* control(const std::string& path) {
    void* c = findShown(g_pageSelect, "ItemPageComponent");
    size_t i = 0;
    while (c && i < path.size()) {
        size_t e = path.find('/', i);
        if (e == std::string::npos) e = path.size();
        int index = atoi(path.substr(i, e - i).c_str());
        c = index >= 0 && index < kids(c) ? kid(c, index) : nullptr;
        i = e + 1;
    }
    return c && shown(c) && fn<bool (*)(void*)>(A_enabled)(c) ? c : nullptr;
}

// What the page sent: {cmd:"rb", op, cat, page, ...}.
void onMessage(const std::string& m) {
    if (!g_on || !g_pref) return;
    std::string op = jsonGet(m, "op"), cat = jsonGet(m, "cat"), page = jsonGet(m, "page"), path = jsonGet(m, "p"), text = jsonGet(m, "t");
    Category* c = nullptr;
    Page* target = pageOf(cat, page, &c);
    if (!target) return;
    // A click is posted to its control and handled a moment later: until then the page stays (another page's rows
    // replace this one's, and the control with them). What is for another page waits (tick()).
    if (g_selected && (cat != g_cat || page != g_page) && now() < g_clickAt + kClickTime) {
        if (g_waiting.size() < 32) g_waiting.push_back(m);
        return;
    }
    if (op == "open") {  // rekordbox's own dialog, at this category (and page, when it is one the component can show)
        if (!target->own) show(cat, page);
        for (int t = 0; t < kCategories; t++) {
            const char* const* data = fn<const char* const* (*)(const int*)>(A_categoryOfType)(&t);
            if (data && *data && cat == *data) g_openType = t, g_openPage = page;
        }
        return;
    }
    if (target->own) return;
    if (op == "show") {
        show(cat, page);
        return;
    }
    // A change: on the page it names (a search result is of another page than the one shown).
    if (!g_selected || cat != g_cat || page != g_page) show(cat, page);
    void* k = control(path);
    if (!k) {
        logf("prefs: %s %s/%s %s: no such control now (hidden or disabled)", op.c_str(), cat.c_str(), page.c_str(), path.c_str());
    } else if (op == "toggle" && isA(k, "juce::ToggleButton")) {
        bool want = jsonGet(m, "on") == "true", radio = *(const int*)((const char*)k + O_radioGroup) != 0;
        if (!text.empty() && trimmed(buttonText(k)) != text) logf("prefs: toggle %s: it is '%s' now, not '%s': nothing done", path.c_str(), trimmed(buttonText(k)).c_str(), text.c_str());
        else if (fn<bool (*)(void*)>(A_toggleState)(k) != want && (want || !radio)) fn<void (*)(void*)>(A_triggerClick)(k), g_clickAt = now();
    } else if (op == "click" && isA(k, "juce::Button") && !isA(k, "juce::ToggleButton")) {
        if (!text.empty() && trimmed(buttonText(k)) != text) logf("prefs: click %s: it is '%s' now, not '%s': nothing done", path.c_str(), trimmed(buttonText(k)).c_str(), text.c_str());
        else fn<void (*)(void*)>(A_triggerClick)(k), g_clickAt = now();
    } else if (op == "combo" && isA(k, "juce::ComboBox")) {
        int id = atoi(jsonGet(m, "id").c_str());
        bool has = false;
        for (int a = 0, n = fn<int (*)(void*)>(A_comboN)(k); a < n; a++) has |= fn<int (*)(void*, int)>(A_comboItemId)(k, a) == id;
        if (has) fn<void (*)(void*, int, int)>(A_comboSelect)(k, id, 1), g_clickAt = now();  // (1: its listeners are told, a moment later)
    } else if (op == "slider" && isA(k, "juce::Slider")) {
        double v = atof(jsonGet(m, "v").c_str());
        v = std::min(fn<double (*)(void*)>(A_sliderMax)(k), std::max(fn<double (*)(void*)>(A_sliderMin)(k), v));
        // The value between the start and the end of a drag: the buffer size applies on the end.
#ifdef _WIN32
        void* scope = nullptr;
        fn<void* (*)(void**, void*)>(A_dragScope)(&scope, k);
        fn<void (*)(void*, double, int)>(A_sliderSet)(k, v, 2);  // (2: its listeners are told now)
        fn<void (*)(void**)>(A_dragScopeEnd)(&scope);
#else
        void* pimpl = *(void* const*)((const char*)k + O_sliderPimpl);
        if (pimpl) fn<void (*)(void*)>(A_dragStart)(pimpl);
        fn<void (*)(void*, double, int)>(A_sliderSet)(k, v, 2);  // (2: its listeners are told now)
        if (pimpl) fn<void (*)(void*)>(A_dragEnd)(pimpl);
#endif
    } else {
        logf("prefs: %s %s/%s %s: not that kind of control (%s)", op.c_str(), cat.c_str(), page.c_str(), path.c_str(), cls(k).c_str());
        return;
    }
    if (k) g_saveAt = now() + 2.0;
    reread();
    readSoon();
}

std::string state() { return g_on && g_pref ? g_json : ""; }
bool g_closing;  // the window closed while a click was still on its way: the component goes once it is done (tick())
void onShown(bool open) {
    g_closing = false;
    if (open && g_on) build();
    else if (g_pref && now() < g_clickAt + kClickTime) g_closing = true;
    else destroy();
}

// ---- rekordbox's Preferences open this window ----------------------------------------------------------------
// The Preferences menu item, its shortcut (Cmd+, / Ctrl+,) and the gear in rekordbox's top bar are one command of
// rekordbox's, which MainComponent::perform handles with showPreference(-1): the dialog at the category it showed
// last. While this mod and badge (whose window it is) are on, that command opens the settings window there instead.
// perform is swapped in the vtable of MainComponent's ApplicationCommandTarget part, which the command manager calls
// it through (no patched call site: nothing in hooks.def, the same on both systems). What calls showPreference
// itself still opens rekordbox's dialog: the Plan pages (an account, an activation), the library sync's settings,
// and this mod's own "Open in rekordbox" (tick()).
// The command's id is rekordbox's own number for it, the same in 7.2.19 and 7.2.20 on macOS (the gear's in
// MainComponent::buttonClicked, the showPreference case of perform's jump table) and on Windows (perform's jump
// table): a build that isn't one of those (checkedBuild) is left as it is.
const int kPreferencesCommand = 0x200a;
const int kTargetPart = 656;                       // the ApplicationCommandTarget part (the thunk's name)
using PerformFn = bool (*)(void*, const int*);     // (that part, InvocationInfo: its command id first)
PerformFn g_perform;                               // rekordbox's
void** g_performSlot;                              // its slot, swapped
bool g_swapTried;
bool g_redirect;                                   // the command came: the window opens at the next tick

bool redirecting() {
    return g_on && !g_broken && ready() && modEnabled("badge") && provided("badge.openPrefs") && checkedBuild();
}
bool performCommand(void* target, const int* info) {
    if (info && *info == kPreferencesCommand && redirecting()) {
        g_redirect = true;  // (not from inside the command: the window opens on the next tick)
        return true;
    }
    return g_perform(target, info);
}
// (for flx4, which checks the slot before it gives MainComponent a command: what is there while swapped)
RBM_PROVIDE("prefs", "prefs.perform", &performCommand, &g_on);
// Swaps perform's slot, once MainComponent exists. Slots of a class's vtable are the class's: every MainComponent.
void swapPerform() {
    void* main = fn<void* (*)()>(A_mainComponent)();
    if (!main) return;  // (not made yet: the next tick)
    g_swapTried = true;
    void** vt = *(void***)((char*)main + kTargetPart + thisDelta(A_performThunk));
    void* rb = at<void*>(A_performThunk);
    for (int i = 0; vt && i < 16; i++) {
        if (vt[i] != rb) continue;
        g_perform = (PerformFn)rb;  // (before the slot changes)
        if (patchSlot(&vt[i], (void*)&performCommand)) {
            g_performSlot = &vt[i];
            logf("prefs: rekordbox's Preferences command opens the settings window (while badge is on)");
            return;
        }
        break;
    }
    logf("prefs: MainComponent's perform wasn't found where expected: rekordbox's Preferences command opens its own dialog");
}
// rekordbox's own dialog, as the command would have opened it (type -1: where it was left).
void openDialog(int type) {
    if (void* main = fn<void* (*)()>(A_mainComponent)()) {
        fn<void (*)(void*, int)>(A_showPreference)(main, type);
        if (g_pref) g_dialogOpened = true;
    }
}
// The window, at the category rekordbox's dialog would show (type: a PreferenceCategoryType, -1 the one shown last;
// an open window stays where it is). A category the window leaves to rekordbox opens rekordbox's dialog, except as
// the one shown last: then the window opens at its line with the button.
void openWindow(int type) {
    bool had = g_pref;
    std::string cat, page;
    if (type >= 0) {
        const char* const* data = fn<const char* const* (*)(const int*)>(A_categoryOfType)(&type);
        cat = data && *data ? *data : "";
        if (cat.empty() || ownCategory(cat)) return openDialog(type);
    } else if (!had) {
        build();  // (what the window opens with anyway: made now, to know where rekordbox's dialog was left)
        cat = g_lastCat, page = g_lastPage;
    }
    auto open = service<bool (*)(const char*, const char*)>("badge.openPrefs");
    if (g_pref && open && open(cat.c_str(), page.c_str())) return;
    if (!had && g_pref) destroy();
    openDialog(type);
}

#ifdef _WIN32
// Windows: every hidden component has Controller > Recordings' NoiseLevelMeterComponent (Windows makes a category's
// pages with it, the tab shown or not), whose RenderThread asks for rekordbox's engine for as long as it runs. At quit
// rekordbox deletes the engine before its windows (File > Exit sends no WM_CLOSE first) and rbm::atQuit is never
// called there, so the next round of that thread made a new engine and rekordbox crashed whenever it quit with the
// settings window open. A hidden meter draws nothing: its thread is told to stop (juce's threadShouldExit flag, whose
// offset is read from threadShouldExit itself: movzx eax, byte [rcx+disp32]) as soon as the component has made it.
const Addr A_threadShouldExit{"__ZNK4juce6Thread16threadShouldExitEv"};
int shouldExitOffset() {
    static int off = -1;
    if (off >= 0) return off;
    const uint8_t* f = (const uint8_t*)(uintptr_t)A_threadShouldExit;
    off = f && f[0] == 0x0f && f[1] == 0xb6 && f[2] == 0x81 && f[7] == 0xc3 ? *(const int32_t*)(f + 3) : 0;
    if (!off) logf("prefs: juce::Thread::threadShouldExit isn't the code this knows: Recordings' level meter keeps running (quitting with the settings window open may crash)");
    return off;
}
bool readable(const void* p, size_t n) {
    MEMORY_BASIC_INFORMATION m;
    return p && VirtualQuery(p, &m, sizeof m) && m.State == MEM_COMMIT && !(m.Protect & (PAGE_NOACCESS | PAGE_GUARD)) &&
           (const char*)p + n <= (const char*)m.BaseAddress + m.RegionSize;
}
bool isRenderThread(const void* obj) {  // an object whose vtable's RTTI says NoiseLevelMeterComponent::RenderThread
    static const uintptr_t base = (uintptr_t)GetModuleHandleW(nullptr);
    static const size_t size = ((const IMAGE_NT_HEADERS*)(base + ((const IMAGE_DOS_HEADER*)base)->e_lfanew))->OptionalHeader.SizeOfImage;
    if (!readable(obj, 8)) return false;
    uintptr_t vt = *(const uintptr_t*)obj;
    if (vt < base + 8 || vt >= base + size) return false;
    uintptr_t loc = *(const uintptr_t*)(vt - 8);
    if (loc < base || loc + 24 > base + size || *(const uint32_t*)loc != 1) return false;
    static const char kName[] = ".?AVRenderThread@NoiseLevelMeterComponent@@";
    uintptr_t td = base + *(const uint32_t*)(loc + 12);
    return td + 16 + sizeof kName <= base + size && !memcmp((const char*)(td + 16), kName, sizeof kName);
}
// The meter isn't among the component's children (the pages a category isn't showing are only owned by it), so its
// thread is found from the thread's side: each thread of rekordbox's has its juce::Thread* on its stack, below the
// frame of run() (the top 16 KB of every stack are looked through for an object of that class).
int stopMeters() {
    typedef LONG(NTAPI * QueryFn)(HANDLE, int, void*, ULONG, ULONG*);
    static QueryFn query = (QueryFn)(void*)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryInformationThread");
    int off = shouldExitOffset(), stopped = 0;
    if (!query || !off) return 0;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    THREADENTRY32 te{sizeof te};
    for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
        if (te.th32OwnerProcessID != GetCurrentProcessId() || te.th32ThreadID == GetCurrentThreadId()) continue;
        HANDLE h = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, te.th32ThreadID);
        if (!h) continue;
        struct { LONG status; void* teb; void* pid; void* tid; ULONG_PTR affinity; LONG priority, basePriority; } info{};
        bool got = query(h, 0, &info, sizeof info, nullptr) >= 0 && info.teb;  // ThreadBasicInformation
        CloseHandle(h);
        if (!got) continue;
        const char* top = *(const char* const*)((const char*)info.teb + 8);     // NT_TIB StackBase...
        const char* limit = *(const char* const*)((const char*)info.teb + 16);  // ...and StackLimit (what is committed)
        const char* from = std::max(limit, top - 16384);
        if (top <= from || !readable(from, top - from)) continue;
        for (const char* p = from; p < top; p += 8) {
            void* t = *(void* const*)p;
            if (((uintptr_t)t & 7) || !isRenderThread(t)) continue;
            bool* flag = (bool*)((char*)t + off);
            if (!*flag) *flag = true, stopped++;
            break;
        }
    }
    CloseHandle(snap);
    if (stopped) logf("prefs: the level meter of the hidden Controller > Recordings page stopped (%d)", stopped);
    return stopped;
}
#endif

void tick() {
    if (!g_swapTried) swapPerform();
    if (g_redirect) {
        g_redirect = false;
        openWindow(-1);
    }
    std::string cmd = devCommand("prefs");
    if (cmd == "scan") {
        bool had = g_pref;
        build();
        plat::writeFileAtomic(plat::configDir() + "/prefs-state.json", g_json.data(), g_json.size());
        if (!had) destroy();
    }
    if (cmd.rfind("msg ", 0) == 0) onMessage(cmd.substr(4));
    if (cmd == "command" && g_performSlot) {  // (an InvocationInfo of the command alone, as a menu item's)
        int64_t info[8] = {kPreferencesCommand};
        void* main = fn<void* (*)()>(A_mainComponent)();
        bool done = ((PerformFn)*g_performSlot)((char*)main + kTargetPart + thisDelta(A_performThunk), (const int*)info);
        logf("prefs: command: %s", !done ? "not handled" : g_redirect ? "the settings window opens" : "rekordbox's dialog opened");
    }
    if (!g_pref) return;
    if (quitting()) return destroy();
#ifdef _WIN32
    static void* meterChecked;  // (the component, and the page it shows: a page shown anew makes its rows anew)
    static std::string meterPage;
    if (g_pref != meterChecked || g_cat + "/" + g_page != meterPage) meterChecked = g_pref, meterPage = g_cat + "/" + g_page, stopMeters();
#endif
    double t = now();
    if (g_openType >= 0) {  // rekordbox's own dialog (not from inside the window's message: it is modal)
        int type = g_openType;
        g_openType = -1;
        if (void* main = fn<void* (*)()>(A_mainComponent)()) {
            fn<void (*)(void*, int)>(A_showPreference)(main, type);
            g_dialogOpened = true;
            // The dialog opens at the category's first page: the page asked for is chosen as a click on its tab.
            void* dialog = fn<void* (*)(int)>(A_modal)(0);
            void* select = dialog && !g_openPage.empty() ? findShown(dialog, "PreferencePageSelectComponent") : nullptr;
            void* holder = select ? findShown(select, "PreferencePageSelectButtonComponent") : nullptr;
            for (int i = 0, n = holder ? kids(holder) : 0; i < n; i++) {
                void* tab = kid(holder, i);
                if (!tab || !shown(tab) || cls(tab) == "VerticalSeparator") break;  // (after the separator: no pages)
                void* b = kids(tab) > 0 ? kid(tab, 0) : nullptr;
                if (b && isA(b, "juce::Button") && g_openPage == str(b, O_name)) {
                    fn<void (*)(void*, void*)>(A_pageClicked)((char*)select + thisDelta(A_pageClicked), b);  // (Windows: its Button::Listener part)
                    break;
                }
            }
        }
        return;
    }
    if (g_dialogOpened && !fn<void* (*)(int)>(A_modal)(0)) {  // it closed: the shown page's rows read their settings anew
        g_dialogOpened = false;
        if (g_selected) show(g_cat, g_page);
    }
    if (g_closing && t >= g_clickAt + kClickTime) return g_closing = false, destroy();
    if (!g_waiting.empty() && t >= g_clickAt + kClickTime) {  // the click is done: what it did, then what waited for it
        reread();
        std::vector<std::string> waited;
        waited.swap(g_waiting);
        for (const std::string& m : waited) onMessage(m);
    }
    static double every;
    bool due = t - every >= 1.0;  // (and once a second: what rekordbox changed in its controls by itself)
    for (double& at : g_readAt)
        if (at && t >= at) at = 0, due = true;
    if (due) every = t, reread();
    if (g_saveAt && t >= g_saveAt) save();
}

// rekordbox quits (macOS): the component goes first, with whatever in it still runs (the level meter's thread, which
// otherwise asks for rekordbox's engine after that is gone: a crash at quit with the settings window open).
void init() { atQuit(&destroy); }

int g_source = (settings::source({"rb", &state, &onMessage, &onShown}), 0);

}  // namespace

RBM_MOD({.name = "prefs", .init = init, .tick = tick, .enabled = &g_on,
         .title = "rekordbox's preferences in this window",
         .about = "Shows rekordbox's own preferences in this window, next to the mods' settings: the same settings, set through rekordbox's own controls. rekordbox's Preferences command (⌘, its menu, the gear) opens this window too; pages it can't show open rekordbox's own dialog.",
         .area = "about", .order = 20});
