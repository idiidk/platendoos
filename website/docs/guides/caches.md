# Caches and external drives

One mod keeps a large cache on disk: [stemcache](/mods/stemcache), the separated stems of your local tracks.
(Stems of Spotify tracks are only kept in memory; Spotify audio is never written to disk.)
[artcache](/mods/artcache) keeps cover art in memory.

## Where they are

In the mods' cache folder:

| | macOS | Windows |
|---|---|---|
| The mods' cache folder | `~/Library/Caches/rekordbox-mods` | `%LOCALAPPDATA%\rekordbox-mods\cache` |

| Cache | Folder | Per 5-minute track | Limit |
|---|---|---|---|
| Stems | `stems/` | ~35 MB (AAC 256; 192 on Windows, the most its encoder makes) | `stemcache_max_gb=10` |

Above its limit, the stems cache deletes the least recently used chunks. Set the limit to `0` for no limit.
stemcache also stores nothing new while the disk has less than `stemcache_min_free_gb` (5 GB) free, and an analysis
run stops below `analyze_min_free_gb` (5 GB).

## Moving a cache to another drive

The stems cache can be on another drive: a USB stick, an SD card, an external disk. Set the folder in the config:

```ini
stemcache_dir=/Volumes/MyStick/stems
```

(On Windows, for example `stemcache_dir=E:\platendoos-cache\stems`.)

New stems are then **staged** on the internal disk (`stems-staging/` in the mods' cache folder, up to
`stemcache_staging_gb=4`) and moved to the drive in the background, a piece at a time, only while nothing reads
from it. What is staged counts as cached and is used from there until it has moved.

::: tip The better cache drive
An SSD in a USB case. A cheap USB stick reads fast but can't take writes and reads together; the staging below
works around that, but a slow stick stays a slow stick.
:::

### Why the writes give way to reads

A track's
stems on a deck are read from that drive as it plays. A cheap USB stick reads fast (120 MB/s measured) but can't take writes and reads together: after
about 70 MB of writes the one measured answered one request a second for minutes, and a read waited behind
everything queued. So what the mods write to an external drive in the background gives way to reads:

- **Everything is staged on the internal disk first.** New stems go to `stems-staging` (in the mods' cache
  folder on the internal disk), and are used from there until they have moved.
- **One writer, a piece at a time** (at most 1 MB): moving stems, removing the oldest when the cache is full. No piece starts while a deck or an analysis reads from the drive, or while a
  deck loads.
- **Nothing is written to a slow drive while a deck plays.** A drive that once took half a second or more for
  a piece counts as slow for the next 30 minutes; while a deck plays (or played in the last 30 s) nothing is
  written to it. What waits stays on the internal disk and moves once the decks have stopped. The log says so:
  `slowdrive: a deck plays and the cache drive has been slow: nothing is written to it until the decks have stopped`.

Measured with it: one deck playing, another loading a new track (28 stem chunks), and three tracks analysed
meanwhile: no write reached the stick.

### What it can't do

Make a slow drive fast. Staging has a size (`stemcache_staging_gb=4`); when a long set with many new tracks
fills it, stem chunks are left out (and separated again next time). With no deck playing the writes go on, a read
can still wait for the piece in flight, and a stick that has just taken writes stays slow for a while. An SSD in a USB case doesn't have this problem.

## When the drive is gone

Only background threads touch the cache folder. A watcher looks at it every 2 s; a folder it hasn't found, or that
hasn't answered in 10 s, counts as not there. While the drive takes no files at all (unplugged, full, write errors),
nothing new is moved: what is staged stays, is still served, and moves once the drive is back. A USB stick that
hangs blocks the background thread that touches it until it's replugged; since only background threads touch the
folder, rekordbox itself keeps going.

## Disk space

- Leave room on the internal disk for the staging folder (4 GB by default) as well as the cache itself.
- When a long set with many new tracks fills the staging, stem chunks are left out (separated again next time)
  until it has emptied.
- The patcher's *Uninstall* leaves the cache alone; delete the folder above to free the space.

## Options

| Option | Default | What it does |
|---|---|---|
| `stemcache_dir` | *(the mods' cache folder)/stems* | Folder of the cached stems. |
| `stemcache_max_gb` | `10` | Size limit (`0` = no limit). |
| `stemcache_staging` | `auto` | `auto`: staged when the cache folder is on an external drive; `1` always; `0` never. |
| `stemcache_staging_gb` | `4` | How much may wait on the internal disk. |
| `stemcache_min_free_gb` | `5` | Store no new stems while the disk has less free space than this. |
