# keycolumn

A new **Key @ Master** column: each track's key as it would sound at the master deck's tempo, e.g. `8A +1.2`. It
updates live as you move the tempo.

<ModInfo name="keycolumn" group="Browsing and playing" />

## What it shows

Each track's key, transposed by the pitch change it would get when played at the master deck's current BPM (with key
lock off), and how far it moved, e.g. `8A +1.2`. With no master deck playing, the column is empty.

The column sorts by the transposed key like the Key column does, and a list sorted by it re-sorts live while the
master's tempo moves. With [multisort](./multisort), *Key @ Master* re-sorts within the other sorted columns.

## Using it

Add the column like any other (right-click the column headers in rekordbox's track list). On Windows its place
(left neighbour, width, shown) is kept in the config, as `column<id>=` lines, because rekordbox's saved layout doesn't
know it.

## Options

| Option | Default | What it does |
|---|---|---|
| `keycolumn` | `1` | `0` switches the mod off (restart). |
| `keycol_title` | `Key @ Master` | Header name of the column. |

## Related

[betterkey](./betterkey) · [multisort](./multisort) · [keepmaster](./keepmaster)
