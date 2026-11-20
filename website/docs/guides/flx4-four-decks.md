# DDJ-FLX4 with four decks

The Pioneer DDJ-FLX4 is a two-deck controller. With the [flx4](/mods/flx4) mod it controls four decks and stems,
on SHIFT combinations it doesn't use itself.

## Setting it up

1. In the patcher, keep **DDJ-FLX4 mapping** ticked (it is by default) and install. It adds decks 3 and 4 and the
   stems rows to rekordbox's FLX4 mapping. On Windows, Windows asks for permission to change the mapping file.
2. For stems, turn on rekordbox's **STEMS** feature: *Preferences > Extensions > STEMS*.
3. Plug in the FLX4 and start platendoos.
4. Press **SHIFT + SMART FADER** for rekordbox's 4-deck layout.

## The combinations

| Press | What it does |
|---|---|
| **SHIFT + PLAY** | That side switches deck: left 1 ↔ 3, right 2 ↔ 4. Mixer strip, jog wheel, pads, loop buttons and beat FX go along; lights and level meter show the new deck. |
| **SHIFT + LOOP IN** | STEMS pads: 1–4 turn vocal, inst, bass and drums on and off (lit while playing), 5–8 pick the stems the beat FX applies to. Again, or any pad mode button, goes back. |
| **SHIFT + LOOP OUT** | The EQ knobs control the stems: HI vocal, MID inst, LOW drums. Again for EQ. |
| **SHIFT + SMART FADER** | rekordbox's 2 or 4 deck layout. |

Each switch shows a short message over rekordbox ("DECK 4: the right side and its mixer strip control deck 4").

## Mixing on four decks

- **Which decks are you on?** With four decks, rekordbox's on-screen mixer opens. The two channels the FLX4's strips
  control are outlined, and their deck numbers are blue (macOS).
- **Knobs and faders pick up.** After a switch, a knob or fader takes over a deck only once you move it to where that
  deck has it. Until then an **orange mark** on the on-screen mixer shows where the FLX4's knob is: turn it to the
  on-screen value and it takes over. Nothing jumps.
- **Tempo never jumps.** Switching a side never changes a deck's tempo, and the tempo fader takes over only once it
  reaches that deck's real tempo, even if sync moved it meanwhile.
- **Beat FX follows the side.** The CH SELECT lever picks sides: **1** is the deck the left side controls, **2** the
  right side's, **1&2** both.
- **All four filters visible.** The on-screen strips are laid out like the FLX4's, with each channel's CFX filter
  knob under LOW.

## Practice routine

1. Load tracks on decks 1 and 2 and mix as usual.
2. **SHIFT + PLAY** on the left: the left side now controls deck 3. Load the next track there; its channel fader on
   the FLX4 now moves channel 3.
3. Bring deck 3 in. Meanwhile deck 1 keeps playing on its own channel: the on-screen mixer shows its settings.
4. To touch deck 1 again, **SHIFT + PLAY** on the left. The knobs pick up where deck 1's settings are.
5. **SHIFT + LOOP IN** and tap pad 1 to drop the vocal; **SHIFT + LOOP OUT** to ride stems with the EQ knobs.

## Options

| Option | Default | What it does |
|---|---|---|
| `flx4_mixer` | *(empty)* | `fixed` keeps the mixer strips and beat FX on channels 1 and 2 (only the deck controls switch). |
| `flx4_keep_tempo` | `1` | `0`: rekordbox's own behaviour, a newly controlled deck takes the tempo fader position rekordbox remembers. |

The full list is on the [flx4](/mods/flx4) page. Related: [keepmaster](/mods/keepmaster) (master changes that don't
change tempo), [xfadelock](/mods/xfadelock) (crossfader lock), [stemcache](/mods/stemcache) (stems on Spotify
tracks).
