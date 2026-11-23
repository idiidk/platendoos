# autocue

Sets cues on a playlist's tracks from their arrangement, as hot cues A–H, as memory cues, or both: **intro, build,
drop, breakdown, build 2, drop 2,
<!--@public autocue-intro-->
outro, fake drop**. Spotify tracks too.

<ModInfo name="autocue" group="Spotify" needs="[spotifyaudio] for Spotify tracks; [betterstems] to analyse per stem; [stemcache] stores the stems it separates" />
<!--@/public-->

<figure class="shot"><img src="/img/autocue.webp" alt="The Auto cues window with the Four pads (A–D) + memory cues layout: the selected track's waveform with its stems' sections, the cue lane with pad letters and M for memory-only cues, and a card per cue with a square pad badge and a round M badge; the Layout button in the header"><figcaption>The Auto cues window with the layout <i>Four pads (A–D) + memory cues</i>: square badges are hot cue pads, round <b>M</b> badges memory cues. A cue that is only a memory cue shows <b>M</b> in the cue lane.</figcaption></figure>

## What it does

You open the window on a playlist, press **Analyse**, check the result and press **Write cues**. Each track then has
a cue at its intro, build, drop, breakdown, second build, second drop, outro and fake drop (where the track has
them).

- **You choose what each cue becomes**: a hot cue on a pad of your choice, a memory cue, or both
  ([the layout](#the-layout-hot-cues-and-memory-cues)). Useful on older CDJs that show only three or four hot cues.
- **You choose the rules for where cues go**: one of five [rulesets](#rulesets-your-own-rules-without-a-model),
  e.g. build cues 16 bars before the drop.
- **A language model is optional.** The built-in rules place the cues for free; a model is only needed to follow
  instructions you type ([Do I need a language model?](#do-i-need-a-language-model)).

The layout, memory cues, the rulesets and **No model** are in autocue since 0.6.0.

Cues are only made from the window, never when a track is loaded on a deck.

## How it works

1. **Analyse** reads each ticked track and works out its arrangement: which bars repeat which, per stem (drums,
   bass, melody and vocals, from [betterstems](./betterstems)' HTDemucs) or, without it, in three bands of the mix.
   It writes that out as a *sheet*: the track's arrangement as text. This runs on your computer.
2. **The cues are placed** on that sheet, either by the built-in rules or by a language model that reads the sheet
   (it never hears the track) and follows the instructions you type, e.g. *"drop cues one bar early"*. Your
   [ruleset](#rulesets-your-own-rules-without-a-model) then moves the intro, build and outro cues where it says.
3. **You check and fine-tune** each track in the same window.
4. **Write cues** sets them in rekordbox, as hot cues and memory cues as the
   [layout](#the-layout-hot-cues-and-memory-cues) says. Nothing reaches rekordbox before that, and **Undo write**
   takes it back.

Analyses, the model's answers and your edits are remembered (in an `autocue` folder next to the config), so the
window opens where you left it.

## Do I need a language model?

No. You don't need a subscription or an account to use autocue. There are three ways to place the cues, chosen
under **⚙**:

| Choice | What you need | What it costs | Your typed instructions |
|---|---|---|---|
| **No model** | Nothing | Nothing. Nothing is sent anywhere. | Not used: the instructions box makes way for the ruleset dropdown |
| **OpenRouter** (default) | An OpenRouter account and key, pasted in the window | Pay as you go, per request, at the price of the model you pick. Some models are free. | Followed |
| **Claude Code** | The Claude Code program on this computer, logged in | No key. It counts toward the usage of the Claude plan you're logged in with. | Followed |

<figure class="shot"><img src="/img/autocue-nomodel.webp" alt="The Auto cues window with No model chosen under the gear button: the settings panel is open, and at the top of the window a ruleset dropdown sits where the instructions box otherwise is"><figcaption><b>No model</b> chosen under ⚙: the built-in rules place the cues, and the ruleset dropdown takes the place of the instructions box at the top.</figcaption></figure>

With **No model**, the built-in rules place the cues and the window stops asking you to set up a model. Answers a
model gave earlier are kept: pick **Cues from: The model** on a track to use them.

A model is only needed for your own instructions ("drop cues one bar early", "no fake drop"): the rules can't follow
typed text. Many things don't need a model at all, so they also work with **No model** and cost nothing:

- where each cue goes: which pad, which memory cues ([the layout](#the-layout-hot-cues-and-memory-cues));
- where the intro, build and outro cues go ([rulesets](#rulesets-your-own-rules-without-a-model));
- moving, adding and removing cues by hand.

### How much does it cost?

With **No model**, nothing. With a model, it is not a one-off cost. The model doesn't learn your rules once and then
work for free: it reads one sheet per track and answers for that track. So:

- **Each analysed track is one small request** to the model, with your instructions.
- **Pressing Apply with new instructions** asks the model again about every analysed track.
- **The audio analysis is free**: it runs on your computer, whatever you choose.
- **Answers are saved.** A track the model already answered, with the same instructions and the same model, isn't
  asked again.
- **Changing the layout or the ruleset is free**: it never asks the model again.

With OpenRouter, **⚙** shows the model cost of this session, so you can try a few tracks and see what it comes to
before doing a whole playlist. OpenRouter's model list in the window marks the free models *(free)*; their ids end
in `:free`. OpenRouter limits how many requests a day they take, and they tend to follow instructions less well
than paid models.

### Setting up a model

| Provider | Setup | Notes |
|---|---|---|
| **OpenRouter** | Paste your OpenRouter key under **⚙**. It is kept in the system's keychain, not in the config. | Any model from openrouter.ai/models (`autocue_model`). |
| **Claude Code** on this computer | Install the Claude Code program and log in. No key. | The window lists the models it offers (`autocue_claude_model`). |

If the model fails for a track, or no key is set, that track gets the built-in rules' cues.

## Using the window

Right-click a playlist and choose **Open auto cues window** (or select some tracks and use the track menu).

- **The track list** (left): tick the tracks to analyse. Each shows whether it's analysed and the colours of the
  cues it has.
- **The header**:
  - **Instructions**: type how you want the cues placed; **Apply** asks the model again about every track with
    them. With **No model** the ruleset dropdown is here instead.
  - **Analyse** runs the ticked tracks (three at a time by default; one of them separates stems at a time).
  - **Layout** says where each cue goes and holds the rules for placing them
    ([below](#the-layout-hot-cues-and-memory-cues)).
  - **⚙** chooses the model (or none), the key and the stems.
- **The track view**: the waveform with bar numbers, the cue lane, and each stem's sections (clips with the same
  letter are copies of each other). Its header says the BPM, bars, length, whether stems or mix bands were used, and
  how many cues are already in rekordbox.
  - Click the waveform to listen from there; double-click a cue to play it.
  - The cue lane shows each cue's pad letter, or **M** for a cue that is only a memory cue.
  - Drag a cue to move it; **Snap to** *Bar*, *Beat* or *½ beat* sets the step (← → move the selected cue; ⇧ moves
    four times as far).
  - The letter keys A–H select the cue on that pad. Click an empty spot in the cue lane to put the selected cue
    there; ⌫ removes a cue.
  - ⌘Z / Ctrl+Z undoes an edit, ⇧⌘Z redoes.
  - **Ask the model again**, **Analyse again**, **Sheet** (what the model reads).
- **The cue cards**: one per cue, with its pad letter in a square badge (and a round **M** when it is a memory
  cue), bar and time and the model's (or the rules') reason. *pad taken* means that pad already holds another cue
  in rekordbox. A dimmed card is a cue the layout doesn't write.
- **Cues from** *The model* / *The rules*: switch a track between the two sets.
- **Footer**:
  - **Replace existing cues**: pads that already hold another cue get the new one, and when new memory cues are
    written, the track's other memory cues are removed. Without it, a taken pad is left alone (*pad taken*) and
    the memory cues already there stay.
  - **Remove cues** removes every hot and memory cue from the ticked tracks, also ones you set yourself.
  - **Undo write** takes back the last **Write cues** or **Remove cues** (only the last one).
  - **Write cues** sets the cues in rekordbox.

<!--@public autocue-stems-->
Stems already separated ([stemcache](./stemcache)) are read from disk, and stems autocue separates are kept for the
decks. A Spotify track's audio is read from the stream into memory by [spotifyaudio](./spotifyaudio), and its stems
are only kept in memory: nothing of a Spotify track is written to disk.
<!--@/public-->

## The layout: hot cues and memory cues

**Layout** (top right of the window, next to **⚙**) has two halves: *Where each cue goes* (this section) and *Rules
for placing the cues* ([rulesets](#rulesets-your-own-rules-without-a-model)).

<figure class="shot"><img src="/img/autocue-layout.webp" alt="The Auto cues window with the Layout panel open: a preset dropdown and a table with a hot cue pad and a memory cue tick per cue, and below it the rules dropdown with the Intro cue, Build cues and Outro cue rules"><figcaption>The <b>Layout</b> panel: per cue a hot cue pad and a memory cue tick, with presets above; below, the ruleset and its three rules.</figcaption></figure>

### Older CDJs: hot cues and memory cues apart

Many older CDJs show only three or four hot cue pads (A–C or A–D): hot cues on E–H are not shown there. Every CDJ
has ten memory cues. So the layout lets you choose, per cue:

- its **hot cue pad**, A to H, or none;
- whether it is also written as a **memory cue**. A cue with no pad and a memory cue is written as a memory cue only.

A cue with neither isn't written; its card is dimmed. Picking a pad another cue already has swaps the two.

### Presets

A preset sets all eight cues at once:

| Preset | Intro | Build | Drop | Breakdown | Build 2 | Drop 2 | Outro | Fake drop | Memory cues |
|---|---|---|---|---|---|---|---|---|---|
| **Eight pads (A–H)** (default) | A | B | C | D | E | F | G | H | none |
| **Eight pads + memory cues** | A | B | C | D | E | F | G | H | all eight |
| **Main four on A–D, the rest on E–H + memory cues** | A | E | B | C | F | D | G | H | all eight |
| **Four pads (A–D) + memory cues** | A | – | B | C | – | D | – | – | all eight |
| **Three pads (A–C) + memory cues** | A | – | B | C | – | – | – | – | all eight |
| **Memory cues only** | – | – | – | – | – | – | – | – | all eight |

Anything else shows as **Your own**.

For a deck with four pads, **Main four on A–D, the rest on E–H + memory cues** puts intro, drop, breakdown and second
drop on A–D, and the builds, outro and fake drop on E–H for decks that have them. Every cue is also a memory cue, so
on the older deck all of them are still there to skip to.

### Memory cues

- A memory cue gets the cue's name as its comment, like the hot cues, and no colour.
- Two cues on the same spot give one memory cue, and a memory cue already on that spot isn't written again.
- A track holds ten memory cues. The ones already on it count; what doesn't fit is left out, and the status line
  says how many.
- **Replace existing cues** makes room: when new memory cues are written to a track, its other memory cues are
  removed. Without it, the memory cues already there stay.

### What a layout change does

- It applies to every track in the window at once, and never asks the model again, so it costs nothing.
- On a track where you moved cues yourself, the cues stay where you put them; the layout still decides which pad
  and memory cue each gets.

## Rulesets: your own rules without a model

The lower half of **Layout**, *Rules for placing the cues*, says where the intro, build and outro cues go. With
**No model** the same dropdown also sits at the top of the window, where the instructions box otherwise is.

The rules work without a model, and they hold for a model's cues too, so you don't need to type them as
instructions. A change applies to every track at once without asking the model again. On a track where you moved
cues yourself, they stay where you put them.

### The five rulesets

| Ruleset | Intro cue | Build cues | Outro cue |
|---|---|---|---|
| **Arrangement** (default): every cue where its part starts | the first bar with sound | where the build starts | where the last loud part ends |
| **Builds 8 bars before the drop** | the first bar with sound | 8 bars before their drop | where the last loud part ends |
| **Builds 16 bars before the drop** | the first bar with sound | 16 bars before their drop | where the last loud part ends |
| **Mix points** | the first bar with drums | 16 bars before their drop | 32 bars before the track ends |
| **Long blends** | the first bar with drums | 32 bars before their drop | 64 bars before the track ends |

**Arrangement** is how autocue placed cues before 0.6.0. **Mix points** and **Long blends** are meant for mixing in
and out: the intro cue on the first drums, the outro cue 32 or 64 bars before the end.

### Setting the rules one by one

Under the dropdown each of the three rules can be set by itself. Anything that isn't one of the five rulesets shows
as **Your own rules**.

| Rule | Choices | Example |
|---|---|---|
| **Intro cue** | the first bar with sound; the first bar with drums; the track's first bar | The drums' first steady stretch starts at bar 17: with *the first bar with drums* the intro cue goes on bar 17. |
| **Build cues** | where the build starts; 4, 8, 16 or 32 bars before their drop | The drop is at bar 49: with *16 bars before the drop* the build cue goes on bar 33. |
| **Outro cue** | where the last loud part ends; 16, 32 or 64 bars before the track ends | The last bar with sound is bar 160: with *32 bars before the track ends* the outro cue goes on bar 129, so 32 bars play from it. |

- **The first bar with drums** is where the drums' first steady stretch (four bars or more) starts, so a short fill
  before it doesn't count. Without stems it is the low end's.
- **Build cues** count from their own drop: the build from the first drop, build 2 from drop 2. A track with a drop
  but no build found gets a build cue too.
- **The outro cue** counts from the last bar with sound, not from the end of the file, so silence at the end
  doesn't shift it.

### When a rule doesn't fit a track

A rule that doesn't fit a track leaves that cue where the arrangement (or the model) put it:

- a build cue that would land before the intro cue (build 2: before the breakdown cue) stays where the build starts,
  e.g. a drop at bar 25 with *32 bars before the drop*;
- a track without a drop gets no build cue from the rule;
- the intro cue is only moved if it stays before every other cue, and the outro cue only if it stays after every
  other cue.

### What the rules can't do

The rules are these three: the intro cue, the build cues and the outro cue. They don't move the drop, breakdown or
fake drop cues, and they can't say things like "drop cues one bar early" or "no fake drop". For anything beyond them
there are two ways:

- **type it as instructions** and use a model (OpenRouter or Claude Code, see
  [Do I need a language model?](#do-i-need-a-language-model));
- **move the cues by hand** in the window before **Write cues**.

## How long it takes

One measurement, on an Apple Silicon Mac with betterstems' HTDemucs: a local track of 3:43 took about **21 s** to
analyse.

| Step | Time |
|---|---|
| Reading the audio | 0.7 s |
| Separating stems | 18.5 s |
| Working out the arrangement | 1.5 s |

Your computer and tracks will give other numbers. What makes the difference:

- **Stems are the slow part.** They are stored ([stemcache](./stemcache)), so a track that was separated before (on a
  deck, in the stems window or here) is read back instead of separated again.
- **Unticking "Listen to stems"** under **⚙** skips separation: three bands of the mix are used instead. Faster, with
  less detail.
- **A Spotify track** takes longer: its audio has to be read from the stream first.
- **The model's answer** comes on top, and how long it takes depends on the model. With **No model** there is none.

The log (`~/Library/Logs/rekordbox-mods.log` on macOS) has a line per analysis saying where the time went: reading,
stems and arrangement (and, if it had to wait, how long another track's stem separation held it up).

## Options

Everything here except `autocue` is also set in the window: the provider and models under **⚙**, the pads, memory
cues and rules under **Layout**.

| Option | Default | What it does |
|---|---|---|
| `autocue` | `1` | `0` switches the mod off (restart). |
| `autocue_provider` | `openrouter` | Who places the cues: `openrouter`; `claude`: the Claude Code program on this computer; `none`: no model, the built-in rules place the cues. |
| `autocue_model` | `anthropic/claude-haiku-5.5` | Any model id from openrouter.ai/models. |
| `autocue_claude_model` | `haiku` | Claude Code's model (an alias or a full name). |
| `autocue_claude_path` | *(empty)* | The `claude` program, if it isn't found by itself. |
| `autocue_stems` | `1` | `0` analyses three bands of the mix instead of stems (faster: no separation). |
| `autocue_parallel` | `3` | Tracks analysed at once (one of them separates stems at a time). |
| `autocue_pads` | `ABCDEFGH` | The hot cue pad of Intro, Build, Drop, Breakdown, Build 2, Drop 2, Outro and Fake drop, in that order: `A` to `H`, or `-` for none. A pad holds one cue: a letter used twice counts only the first time. |
| `autocue_memory` | `00000000` | Per cue, in `autocue_pads`' order: `1` writes it as a memory cue too (or only, without a pad), `0` doesn't. |
| `autocue_intro` | `sound` | The intro cue: `sound` (the first bar with sound), `drums` (the first bar with drums) or `start` (the track's first bar). |
| `autocue_build_bars` | `0` | `4`, `8`, `16` or `32` puts build cues that many bars before their drop; `0`: where the build starts. |
| `autocue_outro_bars` | `0` | `16`, `32` or `64` puts the outro cue that many bars before the track ends; `0`: where the last loud part ends. |

Any other value of the last three counts as their default.

## Platform notes

- Runs on both. On Windows it was tested writing 220 cues; the Claude Code option builds there but hadn't been run
  on Windows yet.
- The layout, memory cues, the rulesets and **No model** (0.6.0) were tested on the Mac in rekordbox: writing hot
  and memory cues, **Replace existing cues**, **Undo write**, and the rulesets on four tracks. On Windows they build
  but haven't been run yet.
- Analysing per stem needs [betterstems](./betterstems) and its model; without it the window says so and uses three
  bands of the mix.

## Tips

- Try a few tracks first, check them, then do the rest. With a paid model this also shows you the cost.
- Playing on older CDJs: pick a preset with memory cues, so every cue is there even where only three or four pads
  show.
- Your own rules without a model: pick **No model** and a ruleset, or set the three rules one by one under
  **Layout**. What they can't do, move by hand.
- With a model, type the rest of your rules once in the instructions box: "drop cues one bar early", "no fake drop".
  Pads, memory cues and the three rules don't need typing: they are in **Layout**.
- "few repeats: cues are less certain" in a track's header means its sections don't repeat much: check that one.

## Related

<!--@public autocue-related-->
[stemcache](./stemcache) · [betterstems](./betterstems) · [spotifyaudio](./spotifyaudio) · [analyzer](./analyzer)
<!--@/public-->
