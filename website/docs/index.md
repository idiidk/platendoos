---
layout: home
title: platendoos
titleTemplate: Mods for rekordbox 7

hero:
  name: platendoos
  text: A box of records, and a box of tweaks for rekordbox.
  tagline: Mods for rekordbox 7 on macOS and Windows. Faster and smoother, especially with Spotify, plus a few features rekordbox doesn't have. Your own rekordbox is never touched.
  image:
    src: /logo.png
    alt: PLATENDOOS
  actions:
    - theme: brand
      text: Download
      link: https://github.com/idiidk/platendoos/releases/latest
    - theme: alt
      text: Get started
      link: /getting-started/introduction
    - theme: alt
      text: Browse the mods
      link: /mods/

features:
<!--@public home-spotify-->
  - icon: 🎧
    title: Spotify, without the waiting
    details: Analyse whole Spotify playlists ahead, so tracks show their waveforms, grid and key the moment they're on a deck. Spotify tracks get stems on the deck, auto cues and genres, and the Spotify lock-ups are fixed.
    link: /guides/spotify
    linkText: The Spotify guide
<!--@/public-->
  - icon: 📈
    title: Better beat grids and keys
    details: Every analysed track also goes through the Beat This! beat tracker, combined with rekordbox's own grid. A neural key detector gets 67.5 % of the GiantSteps key dataset right, against rekordbox's 56.8 %.
    link: /mods/bettergrid
    linkText: bettergrid
  - icon: ⚡
    title: Whole playlists, analysed
    details: Analyse any playlist, Spotify ones too, several tracks at once, in a window with progress per track. Genre first lets the genre pick the tempo (140, not 70).
    link: /mods/analyzer
    linkText: The analysis window
  - icon: 🎚️
    title: Stems that stay
    details: Separated stems are kept on disk, so a track you've played before has its stems at once. HTDemucs v4 separates cleaner stems on the GPU.
    link: /mods/stemcache
    linkText: stemcache
  - icon: 🎛️
    title: A DDJ-FLX4 with four decks
    details: SHIFT combinations the FLX4 doesn't use switch a side to deck 3 or 4, turn the pads into stem pads, and the EQ knobs into stem knobs. Nothing jumps when you switch.
    link: /guides/flx4-four-decks
    linkText: Four decks on an FLX4
  - icon: 🔇
    title: Quieter, cooler, fewer clicks
    details: rekordbox does a lot of work while you do nothing. The mods trim it, keep the audio's memory in RAM, and stop playback from clicking when the Mac runs short of memory. On one tester's fanless MacBook Air M3, with two decks, stems, some FX and browsing for 15 minutes each, plain rekordbox 7.2.20 used 570% CPU and reached 118 °C at its hottest sensor. platendoos used 67% CPU and reached 94 °C.
    link: /guides/performance
    linkText: Performance and dropouts
---

<div class="pd-home">

## See it in rekordbox

<Trailer />

## A few of the windows

<div class="pd-shots">
<figure><img src="/img/analysis.webp" alt="The Playlist Analysis window analysing a Spotify playlist, several tracks at once" loading="lazy"><figcaption>The analysis window: a whole Spotify playlist, several tracks at a time, with Beat This! grids, CNN keys and genres.</figcaption></figure>
<figure><img src="/img/autocue.webp" alt="The Auto cues window with a track's waveform, its stems' sections and eight cue cards" loading="lazy"><figcaption>Auto cues: cues placed from the track's arrangement, then fine-tuned by the bar.</figcaption></figure>
<figure><img src="/img/trackmenu.webp" alt="rekordbox's track menu without greyed-out items, with the platendoos submenu open" loading="lazy"><figcaption>tidymenu: only what can be clicked, and the mods' windows in one submenu.</figcaption></figure>
<figure><img src="/img/genres.webp" alt="The Genres window suggesting a genre for five selected tracks" loading="lazy"><figcaption>genres: a style model fills the Genre field from what the music sounds like.</figcaption></figure>
</div>

## How it works, in one paragraph

On macOS the patcher makes a separate copy of rekordbox, `~/Applications/platendoos.app`, that loads the mods; on
Windows `platendoos.exe` starts your installed rekordbox with the mods loaded, without changing any of its files.
Open rekordbox the usual way whenever you want it plain. A small **PLATENDOOS** chip next to the rekordbox logo
tells you which one you're in. Made for rekordbox **7.2.19** and **7.2.20**. [Read the introduction →](/getting-started/introduction)

</div>
