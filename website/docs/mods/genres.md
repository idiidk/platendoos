# genres

Fills the Genre field from what the music sounds like: a style model (Discogs-EffNet from Essentia, **400 Discogs
styles**, best with electronic music) listens to each track.

<!--@public genres-info-->
<ModInfo name="genres" group="Spotify" needs="[spotifyaudio] for Spotify tracks; [bettergrid] uses its answer to pick the tempo; [analyzer] has the Genre first switch" />
<!--@/public-->

<figure class="shot"><img src="/img/genres.webp" alt="The Genres window on five selected tracks: each with its Genre now, a suggestion and the model's confidence"><figcaption>The Genres window on five selected tracks, suggesting broad genres with the model's confidence.</figcaption></figure>

## The genres window

Right-click a local playlist and choose **Open genres window**, or select tracks and use the track menu (*Genres
window* in the *platendoos* submenu). It lists the tracks with their **Genre now**.

1. **Detect genres** listens to them and suggests a genre per track, with the model's **confidence**. A dropdown
   offers the next two guesses.
2. **Suggest**: *Specific style* ("Liquid Funk", "Deep House") or *Broad genre* ("Drum & Bass", "House").
3. Tick the suggestions you want. A ✓ means the Genre field already has it; an unsure suggestion is unticked
   ("The model is unsure: tick it to write it anyway").
4. **Apply** writes the ticked ones: into empty Genre fields only, unless **Replace existing** is on.
5. **Undo** puts the old genres back. **Remove genres** empties them.

<!--@public genres-spotify-->
Spotify tracks are listened to from the stream: [spotifyaudio](./spotifyaudio) reads a track into memory when the
window asks for it, and nothing of it is written to disk. A Spotify track gets a Genre field once it's in
rekordbox's collection (analysed).
<!--@/public-->
rekordbox doesn't show a Spotify track's genre in Spotify playlists; the mod puts
the ones it wrote into the Genre column there.

What the model found is kept per track, so it's there next time and for the other mods.

## Fill on analysis ("Genre first")

Optionally an empty Genre field is filled whenever rekordbox analyses a track: tick **Fill empty genres on
analysis** in the genres window, or **Genre first** in the [analysis window](./analyzer) (the same switch, also
`genres_on_analyse=1`). Off by default.

With [bettergrid](./bettergrid) the model listens **before the beat grid is made**: the track's genre picks which
tempo is meant (dubstep at 140, not 70; drum & bass at 174, not 87), and the genre it writes is the one the tempo was
picked with. A Genre field that is already filled is used as it is and never changed, and only a guess the model is
sure enough of is written.

bettergrid uses the genre to pick the tempo whether or not this switch is on (`bettergrid_genre`): the switch only
decides whether the genre is also **written** to the Genre field.

## Options

| Option | Default | What it does |
|---|---|---|
| `genres` | `1` | `0` switches the mod off (restart). |
| `genres_detail` | `style` | `style`: the specific style ("Deep House"); `broad`: the broad genre ("House"). |
| `genres_on_analyse` | `0` | `1`: fill empty Genre fields when rekordbox analyses a track. |
| `genres_warmup` | `1` | `0`: the style model loads when it is first needed, not at launch. |

## Platform notes

- macOS: the model runs on Core ML. Windows: on rekordbox's own ONNX Runtime, on the GPU through DirectML when there
  is one (`gpu=0` keeps it on the processor). On by default on both since 0.3.0.

## Licence

The style model is **CC BY-NC-SA 4.0** (Music Technology Group, Universitat Pompeu Fabra / Essentia):
non-commercial use only. See [Licence and credits](/internals/licence).

## Related

<!--@public genres-related-->
[bettergrid](./bettergrid) · [analyzer](./analyzer) · [spotifyaudio](./spotifyaudio) · [multisort](./multisort)
<!--@/public-->
(sort by Genre, then key)
