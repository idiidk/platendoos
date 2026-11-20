# betterkey

Detects musical keys with a neural network trained for it, which gets keys right more often than rekordbox's
built-in detector: **67.5 %** of the GiantSteps key dataset's tracks, against rekordbox's **56.8 %**.

<ModInfo name="betterkey" group="Analysis" />

## What it does

When rekordbox analyses a track, betterkey decodes it again at full quality with rekordbox's own reader (so it works
for Spotify tracks too), off the analysis thread, and runs the key CNN of Korzeniowski & Widmer (ISMIR 2018, from
madmom) on it. Then, when rekordbox's key stage asks for the key, it answers with:

- **compare** (the default): rekordbox's own key. The CNN's key is shown next to it in the **CNN Key** column and
  logged; nothing in your collection changes.
- **replace**: the CNN's key. With `betterkey_min_conf`, only when the CNN is at least that sure.

The **CNN Key** column shows the CNN's key and its probability, e.g. `8A 93%`, for every track it has seen (results
are kept in `rekordbox-betterkey.csv` next to the log and reloaded at launch). The percentage is how sure the network
is of that key.

<figure class="shot"><img src="/img/multisort.webp" alt="The track list sorted by Genre, CNN Key and Artist, with the CNN Key column showing keys and percentages"><figcaption>The CNN Key column (right), here also one of three sorted columns.</figcaption></figure>

## Using it

- Look at the CNN Key column next to Key: where they differ, listen. A high percentage means the network is sure.
- To use the CNN's keys in rekordbox itself, set `betterkey_mode=replace` (and maybe `betterkey_min_conf=0.5`), then
  re-analyse the tracks (the [analysis window](./analyzer)'s *Re-analyze*, or *Re-analyze track* in the track
  menu).

## Options

| Option | Default | What it does |
|---|---|---|
| `betterkey` | `1` | `0` switches the mod off (restart). |
| `betterkey_mode` | `compare` | `compare`: keep rekordbox's key, log both; `replace`: use the CNN's key. |
| `betterkey_min_conf` | `0` | Replace only when the CNN's probability is at least this (0..1). |
| `betterkey_wait` | `120` | Seconds the key stage waits for the CNN's result before it keeps rekordbox's key. |
| `betterkey_title` | `CNN Key` | Header name of the column. |
| `streamio_phrase` | `1` | (Listed with betterkey in the config.) `0` reads the audio again for phrase analysis; see [streamio](./streamio). |

## Notes

- On Windows the network runs as the mods' own C++ code on the processor.
- The model (madmom's key CNN) is **CC BY-NC-SA 4.0**: non-commercial use. See [Licence and credits](/internals/licence).
- The accuracy figures come from the GiantSteps key dataset, a set of electronic tracks with known keys.

## Related

[keycolumn](./keycolumn) (key at the master's tempo) · [bettergrid](./bettergrid) · [multisort](./multisort) ·
[analyzer](./analyzer)
