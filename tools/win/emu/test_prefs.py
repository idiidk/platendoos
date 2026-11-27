"""prefs (mods/prefs.cpp): rekordbox's own Preferences in the settings window, on Windows, in the emulator.

Real code: the mod's whole Windows path as compiled, reached the way rekordbox reaches it: its settings::Source
(settings::sources(): state, message, shown) and its tick (the "scan" and "msg" test commands), so build, ready,
the RTTI class naming (plainName / namesOf / classes / isA on the exe's real complete object locators),
findShown, tabs, readPage, readItems, show, reread, control, onMessage, save and destroy all run. From rekordbox.exe,
run for real on fake objects laid out as the Windows build lays them out: juce::Component::getNumChildComponents /
getChildComponent / isEnabled (its parent walk), juce::Button::getToggleState, juce::Label::getText,
juce::ComboBox::getNumItems / getItemText / getItemId / getSelectedId (with PopupMenu's MenuItemIterator),
juce::Slider::getValue / getMinimum / getMaximum, juce::String's constructors, copy, compare and toRawUTF8, juce::var
(rekordbox's own VariantType tables) and juce::SimpleValueSource::getValue (its real vtable), and
PreferenceCategoryComponent::CategoryData::find (rekordbox's category table).
Python stand-ins (each records its calls and arguments): the PreferenceComponent constructor (it builds the fake
tree: category list, page select part with its tab buttons and separator, item page with rows), its scalar
deleting destructor, PreferenceComponent::selectCategory, PreferencePageSelectComponent::setPageName and
::buttonClicked, juce::Button::triggerClick (the click's effect is applied at once; in rekordbox it is posted, which is
why the mod keeps the page for kClickTime, 0.2 s, after a click or a choice: messages for another page wait for tick(),
a close is deferred to tick()), juce::ComboBox::setSelectedId,
juce::Slider::setValue, Slider::ScopedDragNotification's constructor and destructor, MainComponent::getInstance /
showPreference, juce::Component::getCurrentlyModalComponent (a fake Preferences dialog), djplay::SettingIF and its
three saves; and at the platform layer rbm::devCommand / plat::configDir / plat::writeFileAtomic (Rbmods.dev_files)
and the clock (GetSystemTimePreciseAsFileTime).
Not covered: rekordbox's real Preferences component (its constructor, what selecting a category and page builds,
the real class of every row and control), what a posted click / async setSelectedId do inside rekordbox, the real
drag notifications and the saves themselves. Freeing across the two C runtimes (the DLL's operator new, rekordbox's
delete) is checked from the code only: both use the process heap.
rekordbox's Preferences command (redirect()): the swap of perform's slot in the real vtable of MainComponent's command
target part (a fake MainComponent with its real vtables), the command through that slot, the next tick opening the
window through "badge.openPrefs" (a Python stand-in) or rekordbox's dialog, and every other command to perform (a
Python stand-in recording its arguments).
A help mark whose text is its "?" button's tooltip: this test found it left out on Windows (that part of readItems
was compiled out there); it is read now, through O_tipButton (0x110) and the tooltip at O_flags + 0x10 (0xf8).
"""
import json
import struct

import msvc
import juce

M = "prefs.cpp"
NS = "`anonymous namespace'::"
TABLE_BUILD_1919 = "6A97B2E4-6573000"
ADJ = 240   # selectCategory / buttonClicked: their Windows `this` is the object + 0xf0 (tools/win/verify/prefs.out.json)

# The verified Windows facts (tools/win/verify/prefs.out.json), as the test reads them from the table
VERIFIED = {
    "__ZN19PreferenceComponentC1E22PreferenceCategoryType18PreferencePageType": 0x141d9f4f0,
    "__ZN27PreferenceCategoryComponent12CategoryData4findERK22PreferenceCategoryType": 0x141d9c0e0,
    "__ZN19PreferenceComponent14selectCategoryERKN4juce6StringE": 0x141d9fc40,
    "__ZN29PreferencePageSelectComponent11setPageNameERKN4juce6StringE": 0x141db11b0,
    "__ZN29PreferencePageSelectComponent13buttonClickedEPN4juce6ButtonE": 0x141db0c80,
    "__ZN13MainComponent14showPreferenceE22PreferenceCategoryType": 0x14182b250,
    "__ZN6djplay9SettingIF29savePreferredSettings_GeneralEv": 0x141dca0f0,
    "__ZN6djplay9SettingIF32savePreferredSettings_AudioMixerEbbb": 0x141dd1df0,
    "__ZN6djplay9SettingIF28savePreferredSettings_PlayerEv": 0x141ddecb0,
    "__ZN4juce6Button12triggerClickEv": 0x142c1e450,
    "__ZNK4juce9Component9isEnabledEv": 0x142c0c360,
    "__ZNK4juce8ComboBox11getNumItemsEv": 0x142ba2c50,
    "__ZNK4juce8ComboBox11getItemTextEi": 0x142ba2bf0,
    "__ZNK4juce8ComboBox9getItemIdEi": 0x142ba2bc0,
    "__ZN4juce8ComboBox13setSelectedIdEiNS_16NotificationTypeE": 0x142ba2560,
    "__ZNK4juce8ComboBox13getSelectedIdEv": 0x142ba27d0,
    "__ZNK4juce6Button14getToggleStateEv": 0x140f84540,
    "__ZN4juce9Component26getCurrentlyModalComponentEi": 0x142c11eb0,
    "juce::Slider::ScopedDragNotification's constructor": 0x142b9a360,
    "__ZN4juce6Slider22ScopedDragNotificationD1Ev": 0x142b9a2a0,
}
VERIFIED_OFF = {"O_flags": 0xe8, "O_name": 0x8, "O_buttonText": 0x198, "O_radioGroup": 0x1ec, "O_sliderPimpl": 0x240,
                "O_sliderInterval": 0x108, "O_comboLabel": 0x1d8, "O_tipText": 0xf8, "O_tipTitle": 0x100, "O_tipButton": 0x110}

# juce::Component (Windows): name String +8, parent +0x30, bounds +0x38, children Array<Component*> {data +0x58,
# allocated +0x60, used +0x64}, flags +0xe8 (bit 1 visible, bit 15 disabled). The rest as rekordbox's code reads it.
C_NAME, C_PARENT, C_BOUNDS, C_KIDS, C_FLAGS = 0x8, 0x30, 0x38, 0x58, 0xe8
B_TEXT, B_RADIO, B_ISON = 0x198, 0x1ec, 0x200          # juce::Button: text String, radio group, isOn Value
L_TEXT = 0x1d8                                         # juce::Label: textValue Value
CB_MENU, CB_ID, CB_LABEL, MENU_ITEM = 0x158, 0x170, 0x1d8, 0x90   # juce::ComboBox: PopupMenu items, currentId, Label*
S_PIMPL, P_VALUE, P_MIN, P_MAX, P_STEP = 0x240, 0x50, 0xf8, 0x100, 0x108   # juce::Slider and its Pimpl
T_TEXT, T_TITLE, T_BUTTON = 0xf8, 0x100, 0x110         # ToolTipComponent (its "?" juce::Button* at +0x110)
TIP_STRING = 0xf8                                      # juce::Button: SettableTooltipClient (+0xf0) tooltipString

CLASSES = {  # plain name -> how the exe's RTTI is found (table typeinfo, else the decorated name)
    "juce::Component": "__ZTIN4juce9ComponentE", "juce::Label": "__ZTIN4juce5LabelE", "CustomLabel": "__ZTI11CustomLabel",
    "juce::ToggleButton": "__ZTIN4juce12ToggleButtonE", "juce::TextButton": "__ZTIN4juce10TextButtonE",
    "CustomTextButton": "__ZTI16CustomTextButton", "juce::HyperlinkButton": "__ZTIN4juce15HyperlinkButtonE",
    "juce::ComboBox": "__ZTIN4juce8ComboBoxE", "juce::Slider": "__ZTIN4juce6SliderE", "juce::TextEditor": "__ZTIN4juce10TextEditorE",
    "ToolTipComponent": "__ZTI16ToolTipComponent", "LockComponent": "__ZTI13LockComponent",
    "ItemPageComponent": "__ZTI17ItemPageComponent", "PreferenceBaseItem": "__ZTI18PreferenceBaseItem",
    "ViewLanguage": "__ZTI12ViewLanguage", "PreferencePageItemComponent": "__ZTI27PreferencePageItemComponent",
    "PreferenceCategoryComponent": "__ZTI27PreferenceCategoryComponent",
    "PreferencePageSelectComponent": "__ZTI29PreferencePageSelectComponent",
    "PreferenceComponent": "__ZTI19PreferenceComponent", "PreferenceDialog": "__ZTI16PreferenceDialog",
    "PreferencePageSelectButtonComponent": ".?AVPreferencePageSelectButtonComponent@",   # anonymous namespace
    "VerticalSeparator": ".?AVVerticalSeparator@",                                       # anonymous namespace
}


def mod_statics(r):
    """The mod's Addr and Offset statics: [(static, key in the table, value, ok)]."""
    e = r.emu
    syms = r.dll.pdb.modules[r.dll.pdb.module(M)]
    out = []
    for n, rva in sorted(syms.items()):
        if not n.startswith(NS):
            continue
        s, a = n[len(NS):], r.dll.base + rva
        if s.startswith("A_"):
            name, plus = e.cstr(e.r64(a)), e.r64(a + 8)
            out.append((s, "addr", name + (f"+{plus:#x}" if plus else ""), e.r64(a + 32), e.r64(a + 32) != 0))
        elif s.startswith("O_"):
            key = f"{e.cstr(e.r64(a + 8))}|{e.cstr(e.r64(a + 16))}|{e.r32(a + 24)}"
            out.append((s, "off", key, e.r64(a + 48), bool(e.r8(a + 56))))
    return out


class Rekordbox:
    """The Python side of rekordbox: the Preferences component's tree and the functions the mod calls."""

    def __init__(self, t):
        self.t, self.r = t, t.r
        r = self.r
        self.e, self.exe = e, exe = r.emu, r.exe
        self.A = A = lambda n: exe.base + r.table_value("addr", n)
        self.j = juce.Juce(r)
        self.td = {k: (msvc.type_descriptor(exe, v) if v.startswith(".") else A(v)) for k, v in CLASSES.items()}
        self.vts = {}
        self.events = []        # (what, args...) of every stand-in call, in order
        self.kids_of = {}
        self.cls_of = {}
        self.clock = 133000000000000000   # GetSystemTimePreciseAsFileTime, 100 ns units
        e.set_api("GetSystemTimePreciseAsFileTime", lambda e: e.w64(e.arg(0), self.clock))
        self.ctor_tree = "good"
        self.dialog_open = False
        hook = e.hook
        hook(A("__ZN19PreferenceComponentC1E22PreferenceCategoryType18PreferencePageType"), self._ctor)
        hook(A("__ZN19PreferenceComponent14selectCategoryERKN4juce6StringE"), self._select_category)
        hook(A("__ZN29PreferencePageSelectComponent11setPageNameERKN4juce6StringE"), self._set_page_name)
        hook(A("__ZN29PreferencePageSelectComponent13buttonClickedEPN4juce6ButtonE"), self._page_clicked)
        hook(A("__ZN4juce6Button12triggerClickEv"), self._trigger_click)
        hook(A("__ZN4juce8ComboBox13setSelectedIdEiNS_16NotificationTypeE"), self._set_selected_id)
        hook(A("__ZN4juce6Slider8setValueEdNS_16NotificationTypeE"), self._set_value)
        hook(A("juce::Slider::ScopedDragNotification's constructor"), self._drag_start)
        hook(A("__ZN4juce6Slider22ScopedDragNotificationD1Ev"), self._drag_end)
        # MainComponent with its real vtables (the mod swaps perform's slot in its command target part's)
        self.main = msvc.fake_instance(e, exe, A("__ZTI13MainComponent"), 0x1000)
        self.setting = e.alloc(0x100)
        hook(A("__ZN13MainComponent11getInstanceEv"), lambda e: self.main)
        hook(A("__ZN13MainComponent14showPreferenceE22PreferenceCategoryType"), self._show_preference)
        hook(A("__ZN4juce9Component26getCurrentlyModalComponentEi"),
             lambda e: self.dialog if self.dialog_open and (e.arg(0) & 0xFFFFFFFF) == 0 else 0)
        hook(A("__ZN6djplay9SettingIF11getInstanceEv"), lambda e: self.setting)
        hook(A("__ZN6djplay9SettingIF29savePreferredSettings_GeneralEv"), lambda e: self.events.append(("saveGeneral", e.arg(0))))
        hook(A("__ZN6djplay9SettingIF32savePreferredSettings_AudioMixerEbbb"),
             lambda e: self.events.append(("saveAudioMixer", e.arg(0), e.arg(1) & 0xFF, e.arg(2) & 0xFF, e.arg(3) & 0xFF)))
        hook(A("__ZN6djplay9SettingIF28savePreferredSettings_PlayerEv"), lambda e: self.events.append(("savePlayer", e.arg(0))))
        self.pref_vts = self.vtables("PreferenceComponent")
        self.dtor = e.r64(self.pref_vts[0])          # the scalar deleting destructor (MSVC slot 0)
        hook(self.dtor, lambda e: self.events.append(("delete", e.arg(0), e.arg(1) & 0xFFFFFFFF)))
        self._make_pages()
        self._make_dialog()

    # ---- fake components --------------------------------------------------------------------------------------
    def vtables(self, cls):
        if cls not in self.vts:
            self.vts[cls] = msvc.class_vtables(self.exe, self.td[cls])
        return self.vts[cls]

    def comp(self, cls, xywh=(0, 0, 10, 10), shown=True, enabled=True, name="", kids=(), size=0x400, at=None):
        e = self.e
        c = at or msvc.fake_instance(e, self.exe, None, size, self.vtables(cls))
        if at:
            for off, vt in self.vtables(cls).items():
                e.w64(c + off, vt)
        e.w64(c + C_NAME, self.j.string(name) if name else self.j.empty)
        e.mem_write(c + C_BOUNDS, struct.pack("<iiii", *xywh))
        e.w32(c + C_FLAGS, (2 if shown else 0) | (0 if enabled else 0x8000))
        self.cls_of[c] = cls
        self.set_kids(c, kids)
        return c

    def set_kids(self, c, kids):
        e = self.e
        kids = list(kids)
        arr = e.alloc(8 * max(1, len(kids)))
        for i, k in enumerate(kids):
            e.w64(arr + 8 * i, k)
            e.w64(k + C_PARENT, c)
        e.w64(c + C_KIDS, arr)
        e.w32(c + C_KIDS + 8, len(kids))
        e.w32(c + C_KIDS + 12, len(kids))
        self.kids_of[c] = kids

    def button(self, cls, text, xywh=(0, 0, 80, 20), on=False, radio=0, **kw):
        c = self.comp(cls, xywh, **kw)
        self.e.w64(c + B_TEXT, self.j.string(text) if text else self.j.empty)
        self.e.w32(c + B_RADIO, radio)
        self.e.w64(c + B_ISON, self.j.value_source(bool(on)))
        return c

    def on(self, b):
        return self.j.get(self.e.r64(b + B_ISON))

    def set_on(self, b, v):
        self.j.set(self.e.r64(b + B_ISON), bool(v))

    def label(self, text, xywh=(0, 0, 100, 20), cls="juce::Label", **kw):
        c = self.comp(cls, xywh, **kw)
        self.e.w64(c + L_TEXT, self.j.value_source(text))
        return c

    def combo(self, items, selected, xywh):
        e = self.e
        lab = self.label(dict(items)[selected], (0, 0, xywh[2], xywh[3]))
        c = self.comp("juce::ComboBox", xywh, kids=[lab])
        arr = e.alloc(MENU_ITEM * len(items))
        for i, (id_, text) in enumerate(items):
            e.w64(arr + MENU_ITEM * i, self.j.string(text))
            e.w32(arr + MENU_ITEM * i + 8, id_)
        e.w64(c + CB_MENU, arr)
        e.w32(c + CB_MENU + 8, len(items))
        e.w32(c + CB_MENU + 12, len(items))
        e.w64(c + CB_ID, self.j.value_source(selected))
        e.w64(c + CB_LABEL, lab)
        self.combo_items = getattr(self, "combo_items", {})
        self.combo_items[c] = dict(items)
        return c

    def slider(self, value, lo, hi, step, xywh):
        e = self.e
        c = self.comp("juce::Slider", xywh)
        p = e.alloc(0x300)
        e.w64(p + P_VALUE, self.j.value_source(float(value)))
        e.wf64(p + P_MIN, lo), e.wf64(p + P_MAX, hi), e.wf64(p + P_STEP, step)
        e.w64(c + S_PIMPL, p)
        return c

    def slider_value(self, s):
        return self.j.get(self.e.r64(self.e.r64(s + S_PIMPL) + P_VALUE))

    def tip(self, text, title, xywh):
        c = self.comp("ToolTipComponent", xywh)
        self.e.w64(c + T_TEXT, self.j.string(text) if text else self.j.empty)
        self.e.w64(c + T_TITLE, self.j.string(title) if title else self.j.empty)
        return c

    def tab(self, page_id, title, on=False):
        b = self.button("juce::TextButton", title, (0, 0, 80, 24), on=on, name=page_id)
        return self.comp("juce::Component", (0, 0, 90, 24), kids=[b]), b

    # ---- the pages ---------------------------------------------------------------------------------------------
    def _make_pages(self):
        B, L = self.button, self.label
        c = {}
        # View / General: three rows (child order isn't the order shown: by y)
        c["lang_label"] = L("  Language  ", (10, 5, 150, 24))
        c["lang"] = self.combo([(1, "English"), (2, "Nederlands"), (3, "Deutsch")], 2, (200, 5, 150, 24))
        c["lang_tip"] = self.tip(" Choose the language ", "Language", (360, 5, 20, 20))
        c["custom_label"] = L("Custom", (400, 5, 60, 20), cls="CustomLabel")
        rowA = self.comp("ViewLanguage", (0, 100, 600, 40), kids=[c["lang_label"], c["lang"], c["lang_tip"], c["custom_label"]])
        rowB = self.comp("PreferenceBaseItem", (0, 0, 600, 30), kids=[L("outputsync's own row", (5, 5, 200, 20))])
        c["clock"] = B("juce::ToggleButton", "Show clock", (10, 10, 120, 20), on=True)
        c["radioA"] = B("juce::ToggleButton", "A", (5, 2, 40, 20), radio=5)
        c["radioB"] = B("juce::ToggleButton", "B", (60, 2, 40, 20), on=True, radio=5)
        c["deep"] = L("Deep", (2, 1, 40, 16))
        part2 = self.comp("juce::Component", (7, 3, 50, 20), kids=[c["deep"]])
        spacer = self.comp("juce::Component", (0, 0, 1, 1))
        part1 = self.comp("juce::Component", (100, 30, 300, 40), kids=[c["radioA"], c["radioB"], spacer, part2])
        c["hidden"] = B("juce::ToggleButton", "Hidden", (10, 40, 80, 20), shown=False)
        c["reset"] = B("juce::TextButton", "Reset", (400, 10, 60, 20), enabled=False)
        c["open"] = B("juce::TextButton", " Open folder ", (470, 10, 80, 20))
        c["browse"] = B("CustomTextButton", "Browse", (470, 40, 80, 20))   # (a juce::Button, not a TextButton)
        c["help"] = B("juce::HyperlinkButton", "Help", (560, 10, 30, 20))
        c["picture"] = B("juce::TextButton", "", (590, 10, 10, 10))
        c["lock"] = self.comp("LockComponent", (0, 0, 16, 16))
        c["empty_tip"] = self.tip("", "", (20, 0, 16, 16))
        c["blank"] = L("   ", (30, 0, 16, 16))
        c["slider"] = self.slider(2.5, 0.0, 10.0, 0.5, (300, 50, 200, 20))
        c["greyed"] = B("juce::ToggleButton", "Greyed", (3, 0, 80, 20))
        part3 = self.comp("juce::Component", (0, 60, 100, 20), enabled=False, kids=[c["greyed"]])
        c["editor"] = self.comp("juce::TextEditor", (120, 60, 100, 20))
        rowC = self.comp("PreferencePageItemComponent", (0, 20, 600, 80),
                         kids=[c["clock"], part1, c["hidden"], c["reset"], c["open"], c["help"], c["picture"], c["lock"],
                               c["empty_tip"], c["blank"], c["slider"], part3, c["editor"], c["browse"]])
        # View / Advanced, Audio (no tabs)
        # a help mark without a text of its own: its "?" button's tooltip is the text (ToolTipComponent::setTooltip:
        # Windows 0x141c5b750 in 7.2.19 is mov rcx,[rcx+0x110]; add rcx,0xf0; jmp [vt+0x10] = the Button's
        # SettableTooltipClient::setTooltip, whose String is at that subobject + 8: Button + 0xf8, as on the mac)
        mark = B("juce::TextButton", "", (0, 0, 16, 16))
        self.e.w64(mark + TIP_STRING, self.j.string("Only in the button's tooltip"))
        c["mark_tip"] = self.tip("", "", (220, 5, 16, 16))
        self.set_kids(c["mark_tip"], [mark])
        self.e.w64(c["mark_tip"] + T_BUTTON, mark)
        rowD = self.comp("PreferencePageItemComponent", (0, 0, 600, 30), kids=[L("Advanced thing", (10, 5, 200, 20)), c["mark_tip"]])
        c["buffer"] = self.slider(10, 1, 100, 1, (200, 5, 200, 20))
        rowE = self.comp("PreferencePageItemComponent", (0, 0, 600, 40), kids=[L("Buffer size", (10, 5, 150, 20)), c["buffer"]])
        self.c = c
        self.rows = {("View", "General"): [rowA, rowB, rowC], ("View", "Advanced"): [rowD], ("Audio", ""): [rowE]}
        self.tabs = {"View": [("General", "General"), ("Advanced", "Advanced")], "Audio": []}

    def _build_tree(self, p):
        """What the constructor builds into p: the category list and the page part (category View, page Advanced:
        "the one it showed last")."""
        B = self.button
        self.cat_buttons = {
            "Audio": B("juce::TextButton", "Audio", (0, 80, 150, 30), name="Audio"),
            "Plan": B("juce::TextButton", "Plan", (0, 0, 150, 30), name="Plan"),
            "DVS": B("juce::TextButton", "DVS", (0, 200, 150, 30), name="DVS", shown=False),
            "View": B("juce::TextButton", " View ", (0, 40, 150, 30), name="View", on=True),
            "Keyboard": B("juce::TextButton", "Keyboard", (0, 120, 150, 30), name="Keyboard"),
            "Cloud": B("juce::TextButton", "", (0, 10, 150, 30), name="Cloud"),
        }
        nameless = B("juce::TextButton", "Nameless", (0, 160, 150, 30))
        heading = self.label("Categories", (0, 0, 150, 20))
        cb = self.cat_buttons
        self.cat_list = self.comp("PreferenceCategoryComponent", (0, 0, 160, 600),
                                  kids=[cb["Audio"], cb["Plan"], cb["DVS"], cb["View"], heading, cb["Keyboard"], nameless, cb["Cloud"]])
        self.holder = self.comp("PreferencePageSelectButtonComponent", (0, 0, 600, 30))
        self.item_page = self.comp("ItemPageComponent", (0, 40, 600, 500))
        self.page_select = self.comp("PreferencePageSelectComponent", (160, 0, 640, 600),
                                     kids=[self.comp("juce::Component", (0, 0, 640, 30), kids=[self.holder]), self.item_page])
        left = self.comp("juce::Component", (0, 0, 160, 600), kids=[self.cat_list])
        self.comp("PreferenceComponent", (0, 0, 800, 600), shown=False, at=p,
                  kids=[left, self.page_select] if self.ctor_tree == "good" else [left])
        self.cat, self.page = None, None
        self._select("View")
        self._page("Advanced")

    def _select(self, cat):
        self.cat = cat
        for k, b in self.cat_buttons.items():
            self.set_on(b, k == cat)
        tabs = []
        self.tab_buttons = {}
        for pid, title in self.tabs.get(cat, []):
            t, b = self.tab(pid, title)
            tabs.append(t)
            self.tab_buttons[pid] = b
        if tabs:
            tabs.append(self.comp("VerticalSeparator", (0, 0, 2, 24)))
            t, b = self.tab("Setup", "Setup Wizard")
            tabs.append(t)
        self.set_kids(self.holder, tabs)
        self._page(self.tabs[cat][0][0] if self.tabs.get(cat) else "")

    def _page(self, pid):
        self.page = pid
        for k, b in getattr(self, "tab_buttons", {}).items():
            self.set_on(b, k == pid)
        self.set_kids(self.item_page, self.rows.get((self.cat, pid), []))

    def _make_dialog(self):
        """rekordbox's own Preferences dialog (modal) as getCurrentlyModalComponent hands it out."""
        self.dialog_tabs = {}
        tabs = []
        for pid, title in self.tabs["View"]:
            t, b = self.tab(pid, title)
            tabs.append(t)
            self.dialog_tabs[pid] = b
        sep = self.comp("VerticalSeparator", (0, 0, 2, 24))
        t, self.dialog_setup = self.tab("Setup", "Setup Wizard")
        holder = self.comp("PreferencePageSelectButtonComponent", (0, 0, 600, 30), kids=tabs + [sep, t])
        self.dialog_select = self.comp("PreferencePageSelectComponent", (160, 0, 640, 600), kids=[holder])
        content = self.comp("juce::Component", (0, 0, 800, 600), kids=[self.dialog_select])
        self.dialog = self.comp("PreferenceDialog", (100, 100, 800, 600), kids=[content])

    # ---- stand-ins ---------------------------------------------------------------------------------------------
    def _string_arg(self, i):
        return self.j.text(self.e.r64(self.e.arg(i)))

    def _ctor(self, e):
        p = e.arg(0)
        heap = next((a[0] for n, a in reversed(e.calls) if n == "HeapAlloc" and a[2] == 0x138), None)
        self.events.append(("ctor", p, e.arg(1) & 0xFFFFFFFF, e.arg(2) & 0xFFFFFFFF, e.sizes.get(p)))
        self.ctor_heap = heap
        for off, vt in self.pref_vts.items():
            e.w64(p + off, vt)
        self.pref = p
        self._build_tree(p)
        return p

    def _select_category(self, e):
        name = self._string_arg(1)
        self.events.append(("selectCategory", e.arg(0), name))
        if name in self.tabs:
            self._select(name)

    def _set_page_name(self, e):
        name = self._string_arg(1)
        self.events.append(("setPageName", e.arg(0), name))
        self._page(name)

    def _page_clicked(self, e):
        self.events.append(("buttonClicked", e.arg(0), e.arg(1)))

    def _trigger_click(self, e):
        b = e.arg(0)
        self.events.append(("triggerClick", b))
        if self.cls_of.get(b) == "juce::ToggleButton":   # (rekordbox: posted, handled a moment later)
            radio = self.e.r32(b + B_RADIO)
            if radio:
                for k in self.kids_of[self.e.r64(b + C_PARENT)]:
                    if self.cls_of.get(k) == "juce::ToggleButton" and self.e.r32(k + B_RADIO) == radio:
                        self.set_on(k, k == b)
            else:
                self.set_on(b, not self.on(b))

    def _set_selected_id(self, e):
        c, id_, n = e.arg(0), e.arg(1) & 0xFFFFFFFF, e.arg(2) & 0xFFFFFFFF
        self.events.append(("setSelectedId", c, id_, n))
        self.j.set(e.r64(c + CB_ID), id_)
        self.j.set(e.r64(e.r64(c + CB_LABEL) + L_TEXT), self.combo_items[c][id_])

    def _set_value(self, e):
        s, v, n = e.arg(0), e.farg(1), e.arg(2) & 0xFFFFFFFF
        self.events.append(("setValue", s, v, n))
        self.j.set(e.r64(e.r64(s + S_PIMPL) + P_VALUE), v)

    def _drag_start(self, e):
        self.events.append(("dragStart", e.arg(0), e.arg(1)))
        e.w64(e.arg(0), e.arg(1))
        return e.arg(0)

    def _drag_end(self, e):
        self.events.append(("dragEnd", e.arg(0), e.r64(e.arg(0))))

    def _show_preference(self, e):
        self.events.append(("showPreference", e.arg(0), e.arg(1) & 0xFFFFFFFF))
        self.dialog_open = True

    def take(self, *kinds):
        out = [ev for ev in self.events if not kinds or ev[0] in kinds]
        self.events.clear()
        return out


# ---- what the mod should write -------------------------------------------------------------------------------------
def own_page(cid, title):
    return [{"id": "", "title": "", "rows": [{"c": cid, "i": 0, "h": 100, "items": [
        {"p": "0", "x": 20, "y": 20, "w": 500, "h": 22, "k": "label", "t": title},
        {"p": "1", "x": 52, "y": 56, "w": 500, "h": 22, "k": "other", "c": "rekordbox"}]}]}]


def general_rows(clock_on=1, a_on=0, b_on=1, lang=(2, "Nederlands"), slider=2.5, has_editor=True):
    rowC = [
        {"p": "2/0", "x": 10, "y": 10, "w": 120, "h": 20, "k": "toggle", "t": "Show clock", "on": clock_on, "r": 0},
        {"p": "2/1/0", "x": 105, "y": 32, "w": 40, "h": 20, "k": "toggle", "t": "A", "on": a_on, "r": 5},
        {"p": "2/1/1", "x": 160, "y": 32, "w": 40, "h": 20, "k": "toggle", "t": "B", "on": b_on, "r": 5},
        {"p": "2/1/3/0", "x": 109, "y": 34, "w": 40, "h": 16, "k": "label", "t": "Deep"},
        {"p": "2/3", "x": 400, "y": 10, "w": 60, "h": 20, "off": 1, "k": "button", "t": "Reset"},
        {"p": "2/4", "x": 470, "y": 10, "w": 80, "h": 20, "k": "button", "t": "Open folder"},
        {"p": "2/5", "x": 560, "y": 10, "w": 30, "h": 20, "k": "button", "t": "Help", "link": 1},
        {"p": "2/6", "x": 590, "y": 10, "w": 10, "h": 10, "k": "other", "c": "juce::TextButton"},
        {"p": "2/7", "x": 0, "y": 0, "w": 16, "h": 16, "k": "lock"},
        {"p": "2/10", "x": 300, "y": 50, "w": 200, "h": 20, "k": "slider", "v": slider, "min": 0, "max": 10, "step": 0.5},
        {"p": "2/11/0", "x": 3, "y": 60, "w": 80, "h": 20, "off": 1, "k": "toggle", "t": "Greyed", "on": 0, "r": 0},
    ]
    if has_editor:
        rowC.append({"p": "2/12", "x": 120, "y": 60, "w": 100, "h": 20, "k": "other", "c": "juce::TextEditor"})
    rowC.append({"p": "2/13", "x": 470, "y": 40, "w": 80, "h": 20, "k": "button", "t": "Browse"})  # (rekordbox's own button class with a text)
    rowA = [
        {"p": "0/0", "x": 10, "y": 5, "w": 150, "h": 24, "k": "label", "t": "Language"},
        {"p": "0/1", "x": 200, "y": 5, "w": 150, "h": 24, "k": "combo", "v": lang[0], "t": lang[1],
         "o": [[1, "English"], [2, "Nederlands"], [3, "Deutsch"]]},
        {"p": "0/2", "x": 360, "y": 5, "w": 20, "h": 20, "k": "tip", "t": "Choose the language", "title": "Language"},
        {"p": "0/3", "x": 400, "y": 5, "w": 60, "h": 20, "k": "label", "t": "Custom"},
    ]
    return [{"c": "PreferencePageItemComponent", "i": 2, "h": 80, "items": rowC}, {"c": "ViewLanguage", "i": 0, "h": 40, "items": rowA}]


ADVANCED_ROWS = [{"c": "PreferencePageItemComponent", "i": 0, "h": 30, "items": [
    {"p": "0/0", "x": 10, "y": 5, "w": 200, "h": 20, "k": "label", "t": "Advanced thing"}]}]


def audio_rows(buffer=10):
    return [{"c": "PreferencePageItemComponent", "i": 0, "h": 40, "items": [
        {"p": "0/0", "x": 10, "y": 5, "w": 150, "h": 20, "k": "label", "t": "Buffer size"},
        {"p": "0/1", "x": 200, "y": 5, "w": 200, "h": 20, "k": "slider", "v": buffer, "min": 1, "max": 100, "step": 1}]}]


def expected_state(general=None, audio=None, shown=("", "")):
    return {"categories": [
        {"id": "Plan", "title": "Plan", "pages": own_page("Plan", "Plan")},
        {"id": "View", "title": "View", "pages": [{"id": "General", "title": "General", "rows": general or general_rows()},
                                                  {"id": "Advanced", "title": "Advanced", "rows": ADVANCED_ROWS}]},
        {"id": "Audio", "title": "Audio", "pages": [{"id": "", "title": "", "rows": audio or audio_rows()}]},
        {"id": "Keyboard", "title": "Keyboard", "pages": own_page("Keyboard", "Keyboard")}],
        "shown": {"cat": shown[0], "page": shown[1]}}


def tooltip_mark(state):
    """Takes the tooltip-only help mark (View/Advanced 0/1) out of a state, so the rest is compared on its own."""
    for c in state.get("categories", []):
        for pg in c["pages"]:
            if c["id"] == "View" and pg["id"] == "Advanced":
                for row in pg["rows"]:
                    found = [it for it in row["items"] if it.get("p") == "0/1"]
                    row["items"] = [it for it in row["items"] if it.get("p") != "0/1"]
                    return found
    return []


def diff(got, want, path=""):
    """The first difference between two JSON values (for a readable failure)."""
    if type(got) != type(want) and not (isinstance(got, (int, float)) and isinstance(want, (int, float))):
        return f"{path}: {got!r} != {want!r}"
    if isinstance(want, dict):
        for k in sorted(set(got) | set(want)):
            if k not in got or k not in want:
                return f"{path}.{k}: {'missing' if k not in got else 'extra'} ({got.get(k, want.get(k))!r})"
            d = diff(got[k], want[k], f"{path}.{k}")
            if d:
                return d
    elif isinstance(want, list):
        for i, (g, w) in enumerate(zip(got, want)):
            d = diff(g, w, f"{path}[{i}]")
            if d:
                return d
        if len(got) != len(want):
            return f"{path}: {len(got)} items, want {len(want)} ({(got if len(got) > len(want) else want)[min(len(got), len(want))]!r})"
    elif got != want:
        return f"{path}: {got!r} != {want!r}"
    return ""


def run(t):
    r = t.r
    e, exe = r.emu, r.exe
    S = lambda n: r.sym(NS + n, M)

    # ---- 1. every Addr and Offset of the mod resolves from this build's table ----------------------------------
    statics = mod_statics(r)
    t.check(len([s for s in statics if s[1] == "addr"]) == 32 and len([s for s in statics if s[1] == "off"]) == 10,
            "the mod's Addr / Offset statics found in the PDB", f"{len(statics)}")
    missing = [(s, key) for s, kind, key, v, ok in statics if not ok]
    if exe.build != TABLE_BUILD_1919:
        # (7.2.20's table hasn't been regenerated for this mod yet: missing entries are expected there; listed, and
        # the mod must then switch itself off cleanly)
        print(f"    NOTE {exe.build}: {len(missing)} of {len(statics)} prefs table entries missing: " +
              ", ".join(f"{s} ({key})" for s, key in missing))
        if missing:   # (else the whole test runs on this build too)
            src = settings_source(t)
            e.w8(S("g_on"), 1)
            called = []
            ctor = r.table_value("addr", "__ZN19PreferenceComponentC1E22PreferenceCategoryType18PreferencePageType")
            if ctor is not None:
                e.hook(exe.base + ctor, lambda e: called.append(e.arg(0)) or e.arg(0))
            e.call(src["shown"], 1)
            first = missing[0][1]
            t.check(any("prefs:" in l and "not found in this rekordbox build" in l or "wasn't found in this rekordbox build" in l
                        for l in r.log), "the mod says what is missing and stays off", r.log[-3:])
            t.eq(called, [], "no Preferences component is made")
            t.eq(call_state(t, src), "", "state() is empty")
            return
    else:
        t.eq(missing, [], "7.2.19: every Addr and Offset of prefs is in the table")
    for s, kind, key, v, ok in statics:
        want = (exe.base + r.table_value("addr", key)) if kind == "addr" else r.table_value("off", key)
        t.eq(v, want, f"{s} = the table's {key[:70]}")
        if exe.build == TABLE_BUILD_1919 and kind == "addr" and key in VERIFIED:   # (prefs.out.json: 7.2.19)
            t.eq(v, VERIFIED[key], f"{s} = prefs.out.json's verified {VERIFIED[key]:#x}")
        if kind == "off":
            t.eq(v, VERIFIED_OFF[s], f"{s} = prefs.out.json's verified {VERIFIED_OFF[s]:#x}")
    t.eq(r.table_value("adj", "__ZN19PreferenceComponent14selectCategoryERKN4juce6StringE"), ADJ, "table adj of selectCategory = 240")
    t.eq(r.table_value("adj", "__ZN29PreferencePageSelectComponent13buttonClickedEPN4juce6ButtonE"), ADJ, "table adj of buttonClicked = 240")

    rb = Rekordbox(t)
    A, td = rb.A, rb.td

    # independent of the table: where the two overrides sit in the classes' real vtables
    for cls, fn, slot in [("PreferenceComponent", "__ZN19PreferenceComponent14selectCategoryERKN4juce6StringE", 0),
                          ("PreferencePageSelectComponent", "__ZN29PreferencePageSelectComponent13buttonClickedEPN4juce6ButtonE", 1)]:
        where = {off: s for off, vt in rb.vtables(cls).items() if (s := msvc.vtable_slots(exe, vt, A(fn)))}
        t.eq(where, {ADJ: [slot]}, f"{cls}: {fn[5:40]}... only in slot {slot} of the +0xf0 subobject's vtable")

    # ---- 2. RTTI: the real class hierarchies the mod's isA relies on (independent of the DLL) --------------------
    names = {k: msvc.class_names(exe, td[k]) for k in CLASSES}
    for k, base in [("CustomLabel", ".?AVLabel@juce@@"), ("CustomTextButton", ".?AVButton@juce@@"),
                    ("juce::ToggleButton", ".?AVButton@juce@@"), ("juce::TextButton", ".?AVButton@juce@@"),
                    ("juce::HyperlinkButton", ".?AVButton@juce@@"), ("ViewLanguage", ".?AVPreferenceBaseItem@@")]:
        t.check(base in names[k], f"RTTI: {k} derives from {base}", names[k][:6])
    for k in ("VerticalSeparator", "PreferencePageSelectButtonComponent"):
        t.check(names[k][0].startswith(f".?AV{k}@?A0x"), f"RTTI: {k} is in an anonymous namespace", names[k][0])

    # the mod's own class naming on objects with the real vtables
    plain = {}
    for k in CLASSES:
        obj = msvc.fake_instance(e, exe, None, 0x400, rb.vtables(k))
        plain[k] = obj

    # ---- the source: as the settings window calls it -------------------------------------------------------------
    src = settings_source(t)
    t.check(src is not None, "prefs registered its settings source \"rb\"")
    if not src:
        return
    r.dev_files()
    e.w8(S("g_on"), 1)

    # ---- 2+3. scan: the whole build, read into prefs-state.json ----------------------------------------------------
    r.commands["prefs"] = ["scan"]
    e.call(S("tick"))
    ev = rb.take()
    ctor = [x for x in ev if x[0] == "ctor"]
    t.eq(len(ctor), 1, "scan: one PreferenceComponent made")
    p = ctor[0][1] if ctor else 0
    if ctor:
        t.eq(ctor[0][2:4], (0xFFFFFFFF, 0xFFFFFFFF), "the constructor gets category -1, page -1")
        t.eq(ctor[0][4], 0x138, "...on 0x138 bytes (sizeof PreferenceComponent, prefs.out.json kPrefSize)")
    sel = [x for x in ev if x[0] in ("selectCategory", "setPageName")]
    want_sel = [("selectCategory", p + ADJ, "View"), ("setPageName", rb.page_select, "General"), ("setPageName", rb.page_select, "Advanced"),
                ("selectCategory", p + ADJ, "Audio"), ("selectCategory", p + ADJ, "View"), ("setPageName", rb.page_select, "Advanced")]
    t.eq(sel, want_sel, "scan: categories and pages selected in order, selectCategory on component+0xf0 (Windows `this`), "
                        "and what rekordbox showed selected again at the end")
    t.eq([x for x in ev if x[0] == "delete"], [("delete", p, 1)], "scan without the window: the component deleted after (scalar deleting dtor, flag 1)")
    t.eq([x for x in ev if x[0].startswith("save")], [], "scan: nothing saved (nothing changed)")
    files = {k.replace("\\", "/").rsplit("/", 1)[-1]: v for k, v in r.files_written.items()}
    t.check("prefs-state.json" in files, "scan wrote prefs-state.json in the config folder", list(r.files_written))
    state = json.loads(files.get("prefs-state.json", b"{}") or b"{}")
    want = expected_state()
    mark = tooltip_mark(state)
    t.check(state == want, "prefs-state.json: categories, pages, rows and items exactly as expected", diff(state, want))
    t.eq(mark, [{"p": "0/1", "x": 220, "y": 5, "w": 16, "h": 16, "k": "tip", "t": "Only in the button's tooltip", "title": ""}],
         "a help mark whose text is its button's tooltip is read")
    t.check(any("rekordbox's preferences read: 4 categories, 3 pages" in l for l in r.log), "logged: 4 categories, 3 pages", r.log[-2:])
    t.check(not any("not found" in l or "wasn't found" in l or "isn't built as expected" in l for l in r.log), "nothing logged as missing")
    t.eq(e.r64(S("g_pref")), 0, "after the scan no component is kept")

    # RTTI as the mod names it: the classes cache filled during the scan
    for k, want_name in [("VerticalSeparator", "VerticalSeparator"), ("PreferencePageSelectButtonComponent", "PreferencePageSelectButtonComponent"),
                         ("juce::Label", "juce::Label"), ("PreferenceCategoryComponent", "PreferenceCategoryComponent")]:
        got = mod_classes(t, plain[k])
        t.check(got and got[0] == want_name, f"the mod names {k} '{want_name}'", got[:3])
    got = mod_classes(t, plain["CustomLabel"])
    t.check(got[:1] == ["CustomLabel"] and "juce::Label" in got and "juce::Component" in got,
            "the mod's classes(CustomLabel): itself, juce::Label, juce::Component", got)
    got = mod_classes(t, plain["CustomTextButton"])
    t.check(got[:1] == ["CustomTextButton"] and "juce::Button" in got and "juce::TextButton" not in got,
            "the mod's classes(CustomTextButton): itself and juce::Button (not a TextButton)", got)

    # ---- 4. the window opens: the component is kept --------------------------------------------------------------
    e.call(src["shown"], 1)
    ev = rb.take()
    p = next((x[1] for x in ev if x[0] == "ctor"), 0)
    t.check(p and e.r64(S("g_pref")) == p, "window open: a component made and kept")
    shown_state = json.loads(call_state(t, src) or "{}")
    tooltip_mark(shown_state)  # (checked on its own above)
    t.check(shown_state == want, "state() is the same JSON", diff(shown_state, want))

    def msg(**m):
        rb.take()
        n = len(r.log)
        call_message(t, src, json.dumps(dict(cmd="rb", **m), separators=(",", ":")))  # (as the page sends it: JSON.stringify)
        return rb.take(), r.log[n:]

    def shown_rows(cat, page):
        st = json.loads(call_state(t, src) or "{}")
        for c in st.get("categories", []):
            for pg in c["pages"]:
                if c["id"] == cat and pg["id"] == page:
                    return pg["rows"], st["shown"]
        return None, st.get("shown")

    WAIT = 3000000   # 0.3 s in GetSystemTimePreciseAsFileTime's 100 ns units: past the mod's kClickTime (0.2 s)

    def now_s():     # the mod's now() (core/plat_win.cpp): seconds since 2001
        return (rb.clock - 126227808000000000) / 1e7

    def click_at():
        return e.f64(S("g_clickAt"))

    def waiting():   # the mod's g_waiting: std::vector<std::string> {first, last, end}
        v = S("g_waiting")
        return [msvc.read_string(e, a) for a in range(e.r64(v), e.r64(v + 8), 32)]

    ev, log = msg(op="show", cat="View", page="General")
    t.eq(ev, [("selectCategory", p + ADJ, "View"), ("setPageName", rb.page_select, "General")], "show View/General: selectCategory(component+0xf0), setPageName")
    rows, shown = shown_rows("View", "General")
    t.eq(shown, {"cat": "View", "page": "General"}, "state: View/General shown")

    # toggles
    ev, log = msg(op="toggle", cat="View", page="General", p="2/0", t="Show clock", on=False)
    t.eq(ev, [("triggerClick", rb.c["clock"])], "toggle a checkbox off: triggerClick on that button")
    rows, _ = shown_rows("View", "General")
    t.check(rows == general_rows(clock_on=0), "the page is read again at once: Show clock off", diff(rows, general_rows(clock_on=0)))
    t.eq(click_at(), now_s(), "a toggle's click starts the click wait (g_clickAt = now)")
    ev, _ = msg(op="toggle", cat="View", page="General", p="2/0", t="Show clock", on=False)
    t.eq(ev, [], "toggle to the state it has: no click")
    ev, log = msg(op="toggle", cat="View", page="General", p="2/0", t="Something else", on=True)
    t.eq(ev, [], "toggle whose text differs: refused")
    t.check(any("it is 'Show clock' now, not 'Something else': nothing done" in l for l in log), "...and logged", log)
    ev, _ = msg(op="toggle", cat="View", page="General", p="2/0", on=True)
    t.eq(ev, [("triggerClick", rb.c["clock"])], "toggle without a text: clicked on")
    ev, _ = msg(op="toggle", cat="View", page="General", p="2/1/0", t="A", on=True)
    t.eq(ev, [("triggerClick", rb.c["radioA"])], "radio: choosing A clicks A")
    t.check(rb.on(rb.c["radioA"]) and not rb.on(rb.c["radioB"]), "(the group follows)")
    ev, _ = msg(op="toggle", cat="View", page="General", p="2/1/0", t="A", on=False)
    t.eq(ev, [], "radio: turning the chosen one off does nothing")
    rows, _ = shown_rows("View", "General")
    t.check(rows == general_rows(a_on=1, b_on=0), "radio state read back: A on, B off", diff(rows, general_rows(a_on=1, b_on=0)))
    ev, log = msg(op="toggle", cat="View", page="General", p="2/11/0", t="Greyed", on=True)
    t.eq(ev, [], "toggle a control under a disabled part: nothing (isEnabled walks the parents)")
    t.check(any("no such control now" in l for l in log), "...logged as no such control", log)
    ev, log = msg(op="toggle", cat="View", page="General", p="0/0", on=True)
    t.check(ev == [] and any("not that kind of control (juce::Label)" in l for l in log), "toggle on a label: refused, named", log)

    # combo
    rb.clock += 1000
    ev, _ = msg(op="combo", cat="View", page="General", p="0/1", id=3)
    t.eq(ev, [("setSelectedId", rb.c["lang"], 3, 1)], "combo: setSelectedId(combo, 3, sendNotification)")
    t.eq(click_at(), now_s(), "a combo choice starts the click wait too")
    rows, _ = shown_rows("View", "General")
    t.check(rows == general_rows(a_on=1, b_on=0, lang=(3, "Deutsch")), "combo read back through rekordbox's getSelectedId: 3 Deutsch",
            diff(rows, general_rows(a_on=1, b_on=0, lang=(3, "Deutsch"))))
    ev, _ = msg(op="combo", cat="View", page="General", p="0/1", id=99)
    t.eq(ev, [], "combo: an id it doesn't have is not set")

    # slider: clamped, between the drag scope's constructor and destructor
    was = click_at()
    rb.clock += 1000
    ev, _ = msg(op="slider", cat="View", page="General", p="2/10", v=12)
    t.eq(click_at(), was, "a slider change is applied at once: no click wait")
    s = rb.c["slider"]
    t.check(len(ev) == 3 and ev[0][0] == "dragStart" and ev[0][2] == s and ev[1] == ("setValue", s, 10.0, 2) and
            ev[2] == ("dragEnd", ev[0][1], s), "slider 12: drag scope (scope, slider), setValue(slider, 10 = max, sync), scope destroyed", ev)
    ev, _ = msg(op="slider", cat="View", page="General", p="2/10", v=-3)
    t.check(len(ev) == 3 and ev[1] == ("setValue", s, 0.0, 2), "slider -3: set to the minimum 0", ev)
    ev, _ = msg(op="slider", cat="View", page="General", p="2/10", v=7.5)
    t.check(len(ev) == 3 and ev[1] == ("setValue", s, 7.5, 2), "slider 7.5: set as is", ev)

    # click
    ev, _ = msg(op="click", cat="View", page="General", p="2/4", t="Open folder")
    t.eq(ev, [("triggerClick", rb.c["open"])], "click a button")
    ev, _ = msg(op="click", cat="View", page="General", p="2/13", t="Browse")
    t.eq(ev, [("triggerClick", rb.c["browse"])], "click a CustomTextButton (a juce::Button by its bases)")
    ev, log = msg(op="click", cat="View", page="General", p="2/5", t="Not help")
    t.check(ev == [] and any("click 2/5: it is 'Help' now, not 'Not help'" in l for l in log), "click whose text differs: refused", log)
    ev, log = msg(op="click", cat="View", page="General", p="2/0", t="Show clock")
    t.check(ev == [] and any("not that kind of control (juce::ToggleButton)" in l for l in log), "click on a toggle: refused", log)
    ev, log = msg(op="click", cat="View", page="General", p="2/3", t="Reset")
    t.check(ev == [] and any("no such control now" in l for l in log), "click on a disabled button: refused", log)
    ev, log = msg(op="click", cat="View", page="General", p="2/2", t="Hidden")
    t.check(ev == [] and any("no such control now" in l for l in log), "click on a hidden button: refused", log)

    # a change on another page than the one shown (a search result): that page first (once the last click's wait is over)
    rb.clock += WAIT
    ev, _ = msg(op="slider", cat="Audio", page="", p="0/1", v=50)
    t.check([x[:3] for x in ev if x[0] == "selectCategory"] == [("selectCategory", p + ADJ, "Audio")] and
            ("setValue", rb.c["buffer"], 50.0, 2) in ev, "a change on Audio while View is shown: Audio selected first, then set", ev)
    rows, shown = shown_rows("Audio", "")
    t.check(rows == audio_rows(50) and shown == {"cat": "Audio", "page": ""}, "Audio's slider read back at 50", diff(rows, audio_rows(50)))
    ev, _ = msg(op="slider", cat="Nope", page="", p="0/1", v=5)
    t.eq(ev, [], "a category that doesn't exist: nothing")
    ev, _ = msg(op="toggle", cat="Plan", page="", p="0", on=True)
    t.eq(ev, [], "a change on an own category (Plan): nothing")

    # ---- a posted click: the page stays for kClickTime; what is for another page waits for tick() ----------------
    msg(op="show", cat="View", page="General")
    ev, _ = msg(op="click", cat="View", page="General", p="2/4", t="Open folder")
    t.eq(ev, [("triggerClick", rb.c["open"])], "wait: a click on View/General")
    ev, _ = msg(op="toggle", cat="View", page="General", p="2/0", on=False)
    t.eq(ev, [("triggerClick", rb.c["clock"])], "wait: a message for the SAME page right after it is handled at once")
    ev, _ = msg(op="slider", cat="Audio", page="", p="0/1", v=30)
    t.eq(ev, [], "wait: a change for another page (Audio) right after the click: nothing done yet")
    ev, _ = msg(op="show", cat="View", page="Advanced")
    t.eq(ev, [], "wait: a show of another page (View/Advanced): nothing done yet")
    ev, _ = msg(op="slider", cat="Audio", page="", p="0/1", v=20)
    t.eq(len(waiting()), 3, "wait: the three messages wait in g_waiting")
    t.check(all('"cmd":"rb"' in m for m in waiting()), "wait: as the page sent them", waiting())
    _, shown = shown_rows("View", "General")
    t.eq(shown, {"cat": "View", "page": "General"}, "wait: the clicked page is still shown")
    rb.clock += 1000000   # 0.1 s: not yet
    e.call(S("tick"))
    t.eq(rb.take("selectCategory", "setPageName", "setValue"), [], "wait: tick 0.1 s after the click handles nothing yet")
    t.eq(len(waiting()), 3, "wait: ...and they still wait")
    rb.clock += 2000000   # 0.3 s after the click
    e.call(S("tick"))
    ev = [x[:3] for x in rb.take("selectCategory", "setPageName", "setValue")]
    t.eq(ev, [("selectCategory", p + ADJ, "Audio"), ("setValue", rb.c["buffer"], 30.0),
              ("selectCategory", p + ADJ, "View"), ("setPageName", rb.page_select, "Advanced"),
              ("selectCategory", p + ADJ, "Audio"), ("setValue", rb.c["buffer"], 20.0)],
         "wait: tick after kClickTime handles the waiting messages in the order they came")
    t.eq(waiting(), [], "wait: none left waiting")
    rows, shown = shown_rows("Audio", "")
    t.check(rows == audio_rows(20) and shown == {"cat": "Audio", "page": ""}, "wait: the last one's result is shown (Audio at 20)",
            diff(rows, audio_rows(20)))
    msg(op="show", cat="View", page="General")
    msg(op="toggle", cat="View", page="General", p="2/0", on=True)   # (Show clock back on, as before this part)
    for i in range(40):
        call_message(t, src, json.dumps({"cmd": "rb", "op": "show", "cat": "Audio", "page": ""}, separators=(",", ":")))
    t.eq(len(waiting()), 32, "wait: at most 32 messages wait (40 sent)")
    rb.take()
    rb.clock += WAIT
    e.call(S("tick"))
    t.eq(len(rb.take("selectCategory")), 32, "wait: the 32 are handled by the next tick after the wait")
    msg(op="show", cat="View", page="General")

    # (the ticks from here on are 2 s apart: the changes above get saved by one of them)
    saved_meanwhile = []
    _take = rb.take

    def take_keep_saves(*kinds):
        saved_meanwhile.extend(x for x in rb.events if x[0].startswith("save"))
        return _take(*kinds)
    rb.take = take_keep_saves

    # the "msg" test command through tick
    r.commands["prefs"] = ['msg {"cmd":"rb","op":"show","cat":"View","page":"Advanced"}']
    rb.clock += 20000000
    e.call(S("tick"))
    ev = rb.take("selectCategory", "setPageName")
    t.eq(ev, [("selectCategory", p + ADJ, "View"), ("setPageName", rb.page_select, "Advanced")], "the msg test command: show View/Advanced")

    # ---- open: rekordbox's own dialog at a category (and page) ----------------------------------------------------
    types = category_types(t, A)
    t.check(types.get("View") is not None and types.get("Plan") is not None, "rekordbox's category table read", types)
    ev, _ = msg(op="open", cat="View", page="General")
    t.eq([x for x in ev if x[0] == "showPreference"], [], "open: the dialog isn't opened from inside the message (modal)")
    rb.clock += 20000000
    e.call(S("tick"))
    ev = rb.take()
    t.eq([x for x in ev if x[0] == "showPreference"], [("showPreference", rb.main, types.get("View"))],
         f"tick: MainComponent::showPreference(main, View = {types.get('View')}) (type from rekordbox's own table)")
    t.eq([x for x in ev if x[0] == "buttonClicked"], [("buttonClicked", rb.dialog_select + ADJ, rb.dialog_tabs["General"])],
         "...then the General tab clicked: buttonClicked(select+0xf0, its button)")
    rb.clock += 20000000
    e.call(S("tick"))
    t.eq([x for x in rb.take() if x[0] == "selectCategory"], [], "while the dialog is open: the page isn't selected again")
    rb.dialog_open = False
    rb.clock += 20000000
    e.call(S("tick"))
    ev = rb.take()
    t.eq([x for x in ev if x[0] in ("selectCategory", "setPageName")], [("selectCategory", p + ADJ, "View"), ("setPageName", rb.page_select, "General")],
         "dialog closed: the shown page made anew and read")
    ev, _ = msg(op="open", cat="Plan", page="")
    rb.clock += 20000000
    e.call(S("tick"))
    ev = rb.take()
    t.eq([x for x in ev if x[0] in ("showPreference", "buttonClicked", "selectCategory")], [("showPreference", rb.main, types.get("Plan"))],
         f"open Plan (rekordbox's own): showPreference(main, {types.get('Plan')}), no tab clicked, nothing selected here")
    rb.dialog_open = False
    ev, _ = msg(op="open", cat="View", page="Advanced")
    rb.clock += 20000000
    e.call(S("tick"))
    ev = rb.take("buttonClicked")
    t.eq(ev, [("buttonClicked", rb.dialog_select + ADJ, rb.dialog_tabs["Advanced"])], "open View/Advanced: the Advanced tab clicked, not the Setup one")
    rb.dialog_open = False
    rb.clock += 20000000
    e.call(S("tick"))
    rb.take()

    # ---- 5. saves: a moment after a change, and when the window closes ---------------------------------------------
    rb.take = _take
    t.check(any(x[0] == "saveGeneral" for x in saved_meanwhile), "the changes above were saved by a tick 2 s later")
    msg(op="toggle", cat="View", page="General", p="2/0", on=True)
    t.check(e.f64(S("g_saveAt")) != 0, "a change asks for a save")
    rb.clock += 10000000
    e.call(S("tick"))
    t.eq(rb.take("saveGeneral", "saveAudioMixer", "savePlayer"), [], "1 s after the change: not saved yet")
    rb.clock += 15000000
    e.call(S("tick"))
    ev = rb.take("saveGeneral", "saveAudioMixer", "savePlayer")
    t.eq(ev, [("saveGeneral", rb.setting), ("saveAudioMixer", rb.setting, 1, 1, 1), ("savePlayer", rb.setting)],
         "2 s after the change: rekordbox's three saves on SettingIF (AudioMixer 1,1,1)")
    t.eq(e.f64(S("g_saveAt")), 0.0, "...and no save pending")
    msg(op="toggle", cat="View", page="General", p="2/0", on=False)
    t.check(e.f64(S("g_saveAt")) != 0, "another change: a save pending")
    rb.clock += WAIT   # (the click's wait over: the window closing deletes at once)
    e.call(src["shown"], 0)
    ev = rb.take("saveGeneral", "saveAudioMixer", "savePlayer", "delete")
    t.eq(ev, [("saveGeneral", rb.setting), ("saveAudioMixer", rb.setting, 1, 1, 1), ("savePlayer", rb.setting), ("delete", p, 1)],
         "window closed: the three saves first, then the component deleted through its virtual destructor")
    t.eq(rb.dtor, e.r64(rb.pref_vts[0]), "(the destructor is PreferenceComponent's vtable slot 0)")
    # The component comes from the DLL's operator new and goes back through rekordbox's delete: the same heap?
    t.eq(rb.ctor_heap, e.r64(e.peb + 0x30), "the DLL's operator new took the component from the process heap (GetProcessHeap)")
    t.check(rekordbox_free_uses_process_heap(t), "rekordbox's free() is HeapFree on the heap its CRT set from GetProcessHeap()")
    t.eq(e.r64(S("g_pref")), 0, "no component kept")
    t.eq(call_state(t, src), "", "state() empty once closed")
    ev, _ = msg(op="toggle", cat="View", page="General", p="2/0", on=True)
    t.eq(ev, [], "a message after the window closed: nothing")
    e.call(src["shown"], 0)
    t.eq(rb.take("delete"), [], "closing again: nothing deleted twice")

    # ---- the window closing right after a click: the delete (and the saves) wait for the click ---------------------
    e.call(src["shown"], 1)
    p2 = next((x[1] for x in rb.take() if x[0] == "ctor"), 0)
    t.check(p2 and e.r64(S("g_pref")) == p2, "close-wait: the window opened again, a new component")
    ev, _ = msg(op="toggle", cat="View", page="General", p="2/0", on=True)
    t.eq(ev[-1:], [("triggerClick", rb.c["clock"])], "close-wait: a click")
    e.call(src["shown"], 0)
    t.eq(rb.take(), [], "close-wait: closed right after the click: nothing deleted or saved yet")
    t.eq(e.r64(S("g_pref")), p2, "close-wait: ...the component is kept for the click")
    rb.clock += 1000000
    e.call(S("tick"))
    t.eq(rb.take("delete", "saveGeneral", "saveAudioMixer", "savePlayer"), [], "close-wait: tick 0.1 s after: still kept")
    e.call(src["shown"], 1)
    t.eq(rb.take("ctor", "delete"), [], "close-wait: reopened during the wait: no new component, none deleted")
    t.eq(e.r64(S("g_pref")), p2, "close-wait: ...the same component kept")
    t.check(call_state(t, src) != "", "close-wait: ...and its state still there")
    rb.clock += WAIT
    e.call(S("tick"))
    t.eq(rb.take("delete"), [], "close-wait: reopened: the tick after the wait deletes nothing (the close was cancelled)")
    t.eq(e.r64(S("g_pref")), p2, "close-wait: ...still kept")
    ev, _ = msg(op="toggle", cat="View", page="General", p="2/0", on=False)
    t.eq(ev[-1:], [("triggerClick", rb.c["clock"])], "close-wait: another click")
    t.check(e.f64(S("g_saveAt")) != 0, "close-wait: ...a save pending")
    e.call(src["shown"], 0)
    rb.clock += 1000000
    e.call(S("tick"))
    t.eq(rb.take("delete", "saveGeneral"), [], "close-wait: closed again, 0.1 s after: nothing yet")
    rb.clock += 2000000
    e.call(S("tick"))
    ev = rb.take("saveGeneral", "saveAudioMixer", "savePlayer", "delete")
    t.eq(ev, [("saveGeneral", rb.setting), ("saveAudioMixer", rb.setting, 1, 1, 1), ("savePlayer", rb.setting), ("delete", p2, 1)],
         "close-wait: tick after kClickTime: the saves, then the component deleted (once)")
    t.eq(e.r64(S("g_pref")), 0, "close-wait: nothing kept (no leak)")
    t.eq(waiting(), [], "close-wait: nothing left waiting")
    t.eq(e.f64(S("g_saveAt")), 0.0, "close-wait: no save pending")
    t.eq(call_state(t, src), "", "close-wait: state() empty")
    rb.clock += WAIT
    e.call(S("tick"))
    e.call(src["shown"], 0)
    t.eq(rb.take("delete"), [], "close-wait: later ticks and closes delete nothing twice")

    redirect(t, rb, src)

    # mod off: the window opening makes nothing
    e.w8(S("g_on"), 0)
    e.call(src["shown"], 1)
    t.eq(rb.take("ctor"), [], "prefs=0: opening the window makes no component")
    t.eq(call_state(t, src), "", "prefs=0: state() empty")
    e.w8(S("g_on"), 1)

    # a component without its parts (another rekordbox build): deleted, the mod gives up
    rb.ctor_tree = "broken"
    e.call(src["shown"], 1)
    ev = rb.take()
    t.check([x[0] for x in ev if x[0] in ("ctor", "delete")] == ["ctor", "delete"] and ev[-1][1:] == (rb.pref, 1),
            "a component without a page part: made, then deleted", ev)
    t.check(any("isn't built as expected" in l for l in r.log), "...and logged")
    e.call(src["shown"], 0)
    e.call(src["shown"], 1)
    t.eq(rb.take("ctor"), [], "...and not tried again")


# ---- rekordbox's Preferences command opens the settings window ----------------------------------------------------
PERFORM = "__ZN13MainComponent7performERKN4juce24ApplicationCommandTarget14InvocationInfoE"
TARGET_PART = 568   # MainComponent's ApplicationCommandTarget part on Windows (656 on the mac, the table's adj -88)


def redirect(t, rb, src):
    """The command (0x200a: the menu item, its shortcut, the gear) through the swapped slot of the real vtable of
    MainComponent's command target part: the window ("badge.openPrefs", a Python stand-in here) at the category
    rekordbox's dialog was left at, or rekordbox's dialog when the window can't open; every other command to
    rekordbox's perform."""
    r, e, A = t.r, t.r.emu, rb.A
    S = lambda n: r.sym(NS + n, M)
    perform = A(PERFORM)
    t.eq(r.table_value("adj", "__ZThn656_N13MainComponent7performERKN4juce24ApplicationCommandTarget14InvocationInfoE"), TARGET_PART - 656,
         "redirect: table adj of perform's thunk = -88 (the part at 568)")
    vts = rb.vtables_of_main = msvc.class_vtables(rb.exe, A("__ZTI13MainComponent"))
    t.check(TARGET_PART in vts, "redirect: MainComponent has a vtable at +568 (its ApplicationCommandTarget part)", sorted(vts))
    t.check(4 in msvc.vtable_slots(rb.exe, vts.get(TARGET_PART, 0), perform), "redirect: perform is slot 4 of it (rekordbox's file)")
    part = rb.main + TARGET_PART
    t.eq(msvc.vcall_target(e, part, 4), S("performCommand"), "redirect: a tick swapped slot 4 for the mod's performCommand")
    t.eq(e.r64(S("g_perform")), perform, "redirect: the original kept is rekordbox's perform")
    t.check(any("Preferences command opens the settings window" in l for l in r.log), "redirect: ...and logged")
    t.eq(e.r64(S("g_performSlot")), e.r64(part) + 8 * 4, "redirect: g_performSlot is that slot")
    flx4 = e.call(r.sym("rbm::provided"), e.alloc_cstr("prefs.perform"))
    t.eq(flx4, S("performCommand"), "redirect: \"prefs.perform\" names what is in the slot (flx4's check)")

    performed = []
    e.hook(perform, lambda e: performed.append((e.arg(0), e.r32(e.arg(1)))) or 1)
    opened, answer = [], [1]
    def open_prefs(e):
        opened.append((e.cstr(e.arg(0)), e.cstr(e.arg(1))))
        return answer[0]
    provide = lambda fn: e.call(r.sym("rbm::provide"), e.alloc_cstr("badge"), e.alloc_cstr("badge.openPrefs"), fn, 0)
    provide(e.py_func(open_prefs, "badge.openPrefs"))
    def command(cid):
        info = e.alloc(0x40)
        e.w32(info, cid)
        return e.call(msvc.vcall_target(e, part, 4), part, info) & 0xFF

    t.eq(command(0x2001), 1, "redirect: another command: rekordbox's answer")
    t.eq(performed, [(part, 0x2001)], "redirect: ...handed to rekordbox's perform with the same part and InvocationInfo")
    performed.clear()
    rb.take()

    t.eq(e.r64(S("g_pref")), 0, "redirect: (the window is closed: no component)")
    t.eq(command(0x200a), 1, "redirect: the Preferences command: handled")
    t.eq(performed, [], "redirect: ...not by rekordbox (no dialog)")
    t.eq(opened, [], "redirect: ...and nothing opened from inside the command")
    e.call(S("tick"))
    t.eq(opened, [("View", "Advanced")], "redirect: the next tick opens the window at where rekordbox's dialog was left (View / Advanced)")
    ev = rb.take()
    t.eq([x[0] for x in ev if x[0] in ("ctor", "showPreference", "delete")], ["ctor"], "redirect: the component made for it, no dialog")
    t.check(e.r64(S("g_pref")) != 0, "redirect: ...and kept for the window")
    e.call(src["shown"], 1)
    t.eq(rb.take("ctor"), [], "redirect: the window's opening makes no second one")

    opened.clear()
    command(0x200a)
    e.call(S("tick"))
    t.eq(opened, [("", "")], "redirect: with the window open: brought to the front where it is (no category)")
    t.eq(rb.take("ctor", "showPreference", "delete"), [], "redirect: ...nothing made, no dialog")
    e.call(src["shown"], 0)
    rb.take()

    opened.clear()
    answer[0] = 0
    command(0x200a)
    e.call(S("tick"))
    t.eq(len(opened), 1, "redirect: the window can't open (badge says no)...")
    ev = rb.take()
    t.eq([x[0] for x in ev if x[0] in ("ctor", "delete", "showPreference")], ["ctor", "delete", "showPreference"],
         "redirect: ...the component made for it goes, then rekordbox's dialog opens")
    t.eq([x for x in ev if x[0] == "showPreference"], [("showPreference", rb.main, 0xFFFFFFFF)], "redirect: ...showPreference(main, -1)")
    t.eq(e.r64(S("g_pref")), 0, "redirect: ...nothing kept")
    rb.dialog_open = False

    opened.clear()
    provide(0)
    t.eq(command(0x200a), 1, "redirect: without badge's service: rekordbox's answer")
    t.eq(performed, [(part, 0x200a)], "redirect: ...rekordbox's perform opens its dialog")
    e.call(S("tick"))
    t.eq(opened, [], "redirect: ...and no window")
    performed.clear()
    rb.take()


# ---- the mod's functions as the window calls them -----------------------------------------------------------------
def settings_source(t):
    """The "rb" entry of settings::sources(): {id, state, message, shown} (core/settings.h)."""
    e, r = t.r.emu, t.r
    v = e.call(r.sym("rbm::settings::sources"))
    first, last = e.r64(v), e.r64(v + 8)
    for s in range(first, last, 32):
        if e.cstr(e.r64(s)) == "rb":
            return {"state": e.r64(s + 8), "message": e.r64(s + 16), "shown": e.r64(s + 24)}
    return None


def call_state(t, src):
    e = t.r.emu
    out = e.alloc(32)
    e.call(src["state"], out)
    return msvc.read_string(e, out)


def call_message(t, src, text):
    e = t.r.emu
    s = e.alloc(32)
    msvc.write_string(e, s, text)
    e.call(src["message"], s)


def mod_classes(t, obj):
    """The mod's classes(obj): its std::vector<std::string> of plain names."""
    e, r = t.r.emu, t.r
    v = e.call(r.sym(NS + "classes", M), obj)
    first, last = e.r64(v), e.r64(v + 8)
    return [msvc.read_string(e, a) for a in range(first, last, 32)]


def rekordbox_free_uses_process_heap(t):
    """rekordbox's operator delete -> free: HeapFree([g], 0, p); g is written once, right after a call of the
    GetProcessHeap import (its CRT's heap init). Read from the exe's code (its CRT start-up doesn't run here)."""
    import re
    import capstone
    r = t.r
    e, exe = r.emu, r.exe
    img = msvc._file_image(exe)
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
    rip = lambda i: i.address + i.size + int(i.op_str.split("rip + ")[1].rstrip("]"), 16)
    stubs = {a: n for a, (n, _) in e.stubs.items()}
    lo, hi = exe.section(".text")
    dele = None
    for i in md.disasm(e.mem_read(rb_delete(t), 0x20), rb_delete(t)):
        if i.mnemonic == "jmp" and i.op_str.startswith("0x"):
            dele = int(i.op_str, 16)
            break
    target = dele
    for _ in range(3):   # (free may be a jmp to _free_base)
        ins = list(md.disasm(e.mem_read(target, 0x40), target))
        if ins and ins[0].mnemonic == "jmp" and ins[0].op_str.startswith("0x"):
            target = int(ins[0].op_str, 16)
            continue
        break
    g = None
    for i in md.disasm(e.mem_read(target, 0x40), target):
        if i.mnemonic == "mov" and i.op_str.startswith("rcx, qword ptr [rip +"):
            g = rip(i)
        if i.mnemonic == "call" and "rip +" in i.op_str and g:
            if stubs.get(e.r64(rip(i))) != "HeapFree":
                return False
            break
    if not g:
        return False
    for m in re.finditer(rb"\x48\x89\x05", img[lo - exe.base:hi - exe.base]):
        o = lo - exe.base + m.start()
        if exe.base + o + 7 + struct.unpack_from("<i", img, o + 3)[0] != g:
            continue
        before = list(md.disasm(img[o - 13:o], exe.base + o - 13))
        calls = [i for i in before if i.mnemonic == "call" and "rip +" in i.op_str]
        return bool(calls) and stubs.get(e.r64(rip(calls[-1]))) == "GetProcessHeap"
    return False


def rb_delete(t):
    """rekordbox's operator delete(void*, size_t): what PreferenceComponent's scalar deleting destructor calls last."""
    import capstone
    e = t.r.emu
    dtor = e.r64(msvc.class_vtables(t.r.exe, t.r.exe.base + t.r.table_value("addr", "__ZTI19PreferenceComponent"))[0])
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
    calls = []
    for i in md.disasm(e.mem_read(dtor, 0x60), dtor):
        if i.mnemonic == "call" and i.op_str.startswith("0x"):
            calls.append(int(i.op_str, 16))
        if i.mnemonic in ("ret", "jmp"):
            if i.mnemonic == "jmp" and i.op_str.startswith("0x"):
                calls.append(int(i.op_str, 16))
            break
    return calls[-1]


def category_types(t, A):
    """rekordbox's category table as CategoryData::find reads it (lea of its start; {name, title, int type} x 0x18),
    read here in Python: name -> type."""
    import capstone
    e = t.r.emu
    fn = A("__ZN27PreferenceCategoryComponent12CategoryData4findERK22PreferenceCategoryType")
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
    leas = [i.address + i.size + int(i.op_str.split("rip + ")[1].rstrip("]"), 16)
            for i in md.disasm(e.mem_read(fn, 0x20), fn) if i.mnemonic == "lea" and "rip +" in i.op_str]
    out = {}
    if len(leas) >= 2:
        for a in range(leas[0], leas[1], 0x18):
            out[e.cstr(e.r64(a))] = e.r32(a + 0x10)
    return out
