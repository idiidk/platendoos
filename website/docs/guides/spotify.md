# Spotify

rekordbox can play Spotify tracks, but it treats them as second-class: they're only analysed once they're on a deck,
they get no stems, and the Spotify client has a few habits that make it slow or stuck. Most of platendoos's Spotify
mods fix that.

## What changes

| Plain rekordbox | With platendoos | Mod |
|---|---|---|
| No stems for Spotify tracks | Stems on the deck, separated in memory | [stemcache](/mods/stemcache) |
| Only analysed on a deck, one at a time | Whole Spotify playlists analysed ahead, six at once | [analyzer](/mods/analyzer) |
| A deck shows nothing until the analysis is done | Waveforms and grid as soon as they exist | [quickdeck](/mods/quickdeck) |
| Track lists load one after another | Several at once, a big playlist about twice as fast | [spotifyapi](/mods/spotifyapi) |
| Playlists reload themselves in a loop; downloads can lock up | Fixed | [spotifyfix](/mods/spotifyfix) |
| (7.2.19 Windows) Spotify stuck "loading the library" | Fixed | [spotifystart](/mods/spotifystart) |
| Covers downloaded over and over | Far more of them kept in memory | [artcache](/mods/artcache) |
| Only rekordbox itself can read a Spotify track | The auto cues and genres windows read it too, into memory | [spotifyaudio](/mods/spotifyaudio) |
| No genre for Spotify tracks in Spotify playlists | Genres detected and shown | [genres](/mods/genres) |
| No cues on a Spotify track until you set them | Hot cues A–H from the arrangement | [autocue](/mods/autocue) |

Spotify's audio is only ever streamed: platendoos doesn't write it to disk. What it keeps of a Spotify track is
what rekordbox keeps too, its analysis (grid, key, waveforms) and your cues.

## Preparing a set

1. **Analyse** the playlist: right-click it, *Open analysis window*, **Analyze**. Every track gets a grid, key and
   waveform, so it's ready the moment it's on a deck. Turn on *Genre first* for genres and the right tempo octave
   (140, not 70).
2. Optionally **auto cues**: *Open auto cues window*, **Analyse**, check, **Write cues**. Spotify tracks are read
   from the stream into memory for it ([spotifyaudio](/mods/spotifyaudio)).

On the decks, a Spotify track then shows its waveforms, grid and cues at once, and its stems are separated as it
loads.

## Spotify's limits

- **Downloads:** Spotify allows about 30 track downloads a minute. Past that, a track waits out Spotify's "streaming
  limit reached" (up to a minute) instead of failing, and a track that still fails is tried again at the end. The
  analysis window does the same and spaces its loads (`analyze_gap`, `analyze_streams`).
- **Three on the decks:** rekordbox plays at most three Spotify tracks at once, as without the mods.
- rekordbox needs Spotify connected to list, load and play its tracks.

## Stems on Spotify tracks

rekordbox never separates Spotify tracks. [stemcache](/mods/stemcache) does, on a deck: the stems are separated from
the audio as it streams in and never written to disk. The last ones stay in memory, compressed
(`stemcache_memory_mb`), so a track loaded again in the same session has its stems at once. With [betterstems](/mods/betterstems) they come from HTDemucs. It needs
rekordbox's STEMS feature (*Preferences > Extensions > STEMS*).
