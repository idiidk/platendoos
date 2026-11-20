# xfadelock

A small lock button next to the on-screen crossfader holds the crossfader exactly in the middle.

<ModInfo name="xfadelock" group="Browsing and playing" />

<figure class="shot mid"><img src="/img/xfadelock-open.webp" alt="The on-screen crossfader with an open grey padlock to its right"><figcaption>Unlocked: the grey open padlock right of the crossfader.</figcaption></figure>

<figure class="shot mid"><img src="/img/xfadelock-locked.webp" alt="The crossfader in the middle with the padlock closed and lit blue"><figcaption>Locked: lit blue, the crossfader stays in the middle.</figcaption></figure>

## Using it

- The lock sits right of rekordbox's on-screen crossfader, at the bottom of its mixer, between the crossfader assign
  buttons.
- **Click it** to lock: it lights blue and the crossfader goes to the centre. While it's lit, neither a controller's
  crossfader (the DDJ-FLX4's or any other) nor dragging the on-screen one moves it.
- **Click it again** to unlock. The controller's crossfader takes over again only once it reaches the middle (or
  passes where the crossfader is), so nothing jumps; until then the lock shows **orange**.
- It starts **unlocked** every time rekordbox starts: a lock left on from an earlier session would look like a broken
  crossfader.

Handy when you mix with the channel faders only and don't want a knocked crossfader to cut a deck.

## Options

| Option | Default | What it does |
|---|---|---|
| `xfadelock` | `1` | `0` switches the mod off (restart). |

## Windows

The same lock since 0.3.2 (rekordbox 7.2.19 and 7.2.20): the button is a small window of its own on rekordbox's
window, drawn like the Mac's. Pictures above are from the Mac.

## Related

[flx4](./flx4) · [keepmaster](./keepmaster)
