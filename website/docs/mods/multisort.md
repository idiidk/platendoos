# multisort

Sort the track list by several columns at once: BPM, then key within each BPM.

<ModInfo name="multisort" group="Browsing and playing" />

<figure class="shot"><img src="/img/multisort.webp" alt="The track list sorted by Genre (1, descending), CNN Key (2, ascending) and Artist (3, ascending), the three headers in blue"><figcaption>Sorted by Genre, then CNN Key, then Artist: the sorted headers are blue, numbered and with their arrows.</figcaption></figure>

## Using it

- **Click a column header** to cycle it through **ascending**, **descending** and **off**.
- The columns that are on sort **together, in the order you turned them on**: click BPM, then Key, and tracks are
  sorted by BPM and, within one BPM, by key.
- The first one is rekordbox's own sort column (with its arrow). With two or more, each sorted header is drawn blue
  with its place and direction ("BPM 1", "Key ▼2").
- Turn a column off with another click (or two) on it; the others keep sorting.
- A list sorted by its track order (#) starts over at the column you click, as before.
- With [keycolumn](./keycolumn), *Key @ Master* re-sorts within the other sorted columns as the master tempo moves.

Ties within all sorted columns fall back on rekordbox's own tie-breakers (artist, album, ID).

## Options

| Option | Default | What it does |
|---|---|---|
| `multisort` | `1` | `0` gives rekordbox's own one-column sort back (restart). |

## Related

[keycolumn](./keycolumn) · [betterkey](./betterkey) (the CNN Key column) · [genres](./genres)
