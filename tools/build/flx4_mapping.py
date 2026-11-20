"""Writes the DDJ-FLX4 mapping the flx4 mod needs: rekordbox's own, for four decks and with stems.

    python3 tools/build/flx4_mapping.py <rekordbox's DDJ-FLX4.midi.csv> <out.csv> [device name]
patcher/flx4map.h is the same in C++ for the patcher app (byte-identical output): change both.

The mod (mods/flx4.mm) moves a side of the controller to deck 3 or 4 by rewriting its messages to the channels
the DDJ-FLX6 (the FLX4's four-deck sibling) uses for them, so the mapping gets the FLX6's deck 3/4 columns:
  - deck controls on channels 1/2 (deck columns 0,1) also on channels 3/4 (2,3)
  - pads on channels 8/10 (shift 9/11) also on 12/14 (shift 13/15)
  - LOAD 3/4, the loaded indicators of decks 3/4, CH3/4 CFX, and the FLX6's deck select and deck state
and rows for messages only the mod sends (on notes and CCs the FLX4 never uses), for stems:
  - per deck: the four stems on/off, the STEMS pad mode, the part isolator on/off and its three knobs
  - FX part select (which stems the beat FX applies to), from any deck
  - the FLX6's beat FX channel assignment: the FLX4 has FX1 on CH1 and FX2 on CH2, the mod moves each to its side's deck
The device name (default DDJ-FLX4) is for testing the mapping on another MIDI device, e.g. a virtual one.
"""
import csv
import sys

# The mod's own messages, on the deck's channel (status 0x90/0xB0 + deck - 1). Keep in sync with mods/flx4.mm.
STEM_NOTES = {"Vocal": 0x30, "Inst": 0x31, "Bass": 0x32, "Drums": 0x33}  # ActivePart<stem>
FX_PART_NOTES = {"Vocal": 0x38, "Inst": 0x39, "Bass": 0x3A, "Drums": 0x3B}  # FXPartSelect<stem>On
ISOLATOR_NOTE = 0x34   # PartIsolator on/off
STEMS_NOTE = 0x35      # STEMS pad mode on/off (the deck's pads on screen)
DECK_SELECT_NOTE = 0x72  # as the FLX6
DECK_STATE_NOTE = 0x3C   # as the FLX6
FX_ASSIGN_NOTE = 0x1C    # + channel 0-3, on the FX units' channels (5/6), as the FLX6
ISOLATOR_CCS = {"Vocal": 0x08, "Inst": 0x0C, "Drums": 0x10}  # PartIsolator<stem> knobs (14-bit: + LSB at +0x20)

DECKS = {"0,1": "0,1,2,3", "7,9": "7,9,11,13", "8,10": "8,10,12,14"}


def row(name, function, kind, inp, decks, out="", out_decks=("", "", "", ""), option="", comment=""):
    return [name, function, kind, inp, *decks, out, *out_decks, option, comment]


def main():
    src, dst = sys.argv[1], sys.argv[2]
    device = sys.argv[3] if len(sys.argv) > 3 else "DDJ-FLX4"
    rows = list(csv.reader(open(src, encoding="utf-8-sig")))
    used = set()  # (status, data1) the mapping already takes
    out = []
    for r in rows:
        if not r:
            out.append(r)
            continue
        if r[0].startswith("@file"):
            out.append(["@file", "1", device] + [""] * 12)
            continue
        r = r + [""] * (15 - len(r))
        if not r[0].startswith("#"):
            for col, decks in ((3, r[4:8]), (8, r[9:13])):
                if len(r[col]) == 4:
                    st, n = int(r[col][:2], 16), int(r[col][2:], 16)
                    for d in [d for d in decks if d.isdigit()] or [str(st & 15)]:
                        used.add(((st & 0xF0) | int(d), n))
        for col in (4, 9):  # input / output deck columns
            k = ",".join(r[col:col + 2])
            if k in DECKS and not r[col + 2] and not r[col + 3]:
                r[col:col + 4] = DECKS[k].split(",")
        if r[1] == "Load" and r[4:6] == ["9646", "9647"]:
            r[6:8] = ["9648", "9649"]
        if r[0] == "LoadedIndicator":
            r[11:13] = ["9F02", "9F03"]
        out.append(r)

    def take(st, n):
        assert (st, n) not in used, f"{st:02X}{n:02X} is already in the FLX4 mapping"
        used.add((st, n))

    all4 = ("0", "1", "2", "3")
    extra = [
        ["# platendoos flx4 (mods/flx4.mm): decks 3/4 as on the DDJ-FLX6, and stems"] + [""] * 14,
        row("Deck", "DeckSelect", "Button", f"90{DECK_SELECT_NOTE:02X}", all4, option="RO", comment="Select Deck"),
        row("DeckState", "DeckState", "Button", f"90{DECK_STATE_NOTE:02X}", all4, option="RO", comment="LED State of the DECK 1/2/3/4"),
        row("CFXParameterCH3", "CFXParameterCH3", "KnobSliderHiRes", "B619", ("",) * 4, option="Fast", comment="CH3 CFX Depth"),
        row("CFXParameterCH4", "CFXParameterCH4", "KnobSliderHiRes", "B61A", ("",) * 4, option="Fast", comment="CH4 CFX Depth"),
        row("Stems", "", "Button", f"90{STEMS_NOTE:02X}", all4, f"90{STEMS_NOTE:02X}", all4, "Fast", "STEMS Mode On/Off"),
        row("PartIsolator", "", "Button", f"90{ISOLATOR_NOTE:02X}", all4, f"90{ISOLATOR_NOTE:02X}", all4, "", "PART ISOLATOR On/Off"),
    ]
    for c in range(4):
        take(0x90 | c, DECK_SELECT_NOTE), take(0x90 | c, DECK_STATE_NOTE)
        take(0x90 | c, STEMS_NOTE), take(0x90 | c, ISOLATOR_NOTE)
    take(0xB6, 0x19), take(0xB6, 0x1A)
    for stem, n in STEM_NOTES.items():
        extra.append(row(f"ActivePart{stem}", "", "Button", f"90{n:02X}", all4, f"90{n:02X}", all4, "", f"ACTIVE PART {stem.upper()}"))
        for c in range(4):
            take(0x90 | c, n)
    for stem, n in FX_PART_NOTES.items():  # one setting for all decks: a row per deck's channel, as on the DDJ-GRV6
        for c in range(4):
            m = f"9{c:X}{n:02X}"
            extra.append(row(f"FXPartSelect{stem}On", "", "Button", m, ("",) * 4, m, ("",) * 4, "", f"FX PART SELECT {stem.upper()}"))
            take(0x90 | c, n)
    for unit in (1, 2):  # FX<unit>Assign.CH1-4: assigns the beat FX unit to a channel
        for c in range(4):
            m = f"9{3 + unit:X}{FX_ASSIGN_NOTE + c:02X}"
            extra.append(row(f"FX{unit}Assign.CH{c + 1}", "", "Button", m, ("",) * 4, comment=f"FX{unit} on CH{c + 1}"))
            take(0x93 + unit, FX_ASSIGN_NOTE + c)
    for stem, cc in ISOLATOR_CCS.items():
        extra.append(row(f"PartIsolator{stem}", "", "KnobSliderHiRes", f"B0{cc:02X}", all4, option="Fast", comment=f"PART ISOLATOR {stem.upper()}"))
        for c in range(4):
            take(0xB0 | c, cc), take(0xB0 | c, cc + 0x20)

    with open(dst, "w", newline="", encoding="utf-8") as f:
        w = csv.writer(f, lineterminator="\r\n")
        w.writerows(out + extra)


if __name__ == "__main__":
    main()
