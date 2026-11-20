# Licence and credits

## platendoos

platendoos (everything in its repository unless a file or the list below says otherwise) is licensed under
[**CC BY-NC-SA 4.0**](https://creativecommons.org/licenses/by-nc-sa/4.0/). In short: you may share and change it,
with credit, for **non-commercial purposes only**, and what you make from it must use the same licence. The full text
is in the repository's `LICENSE` file.

platendoos is not made by, endorsed by or affiliated with AlphaTheta / Pioneer DJ or Spotify. It contains no part of
rekordbox: the patcher works on the copy of rekordbox already on the computer, and rekordbox stays under its own
licence.

## Models

The models are not in the repository. `tools/models/` makes them from their upstream releases, and the release
packages carry them.

| Model | Used by | From | Licence |
|---|---|---|---|
| Beat This! | [bettergrid](/mods/bettergrid) | CPJKU, Foscarini et al., "Beat This! Accurate Beat Tracking Without DBN Postprocessing", ISMIR 2024, [github.com/CPJKU/beat_this](https://github.com/CPJKU/beat_this) | MIT |
| HTDemucs v4 | [betterstems](/mods/betterstems) | Meta, Rouard, Massa & Défossez, "Hybrid Transformers for Music Source Separation", ICASSP 2023, [github.com/facebookresearch/demucs](https://github.com/facebookresearch/demucs) | MIT |
| Key CNN (madmom 0.16.1) | [betterkey](/mods/betterkey) | Korzeniowski & Widmer, "Genre-Agnostic Key Classification with Convolutional Neural Networks", ISMIR 2018, [github.com/CPJKU/madmom](https://github.com/CPJKU/madmom) | **CC BY-NC-SA 4.0** |
| Discogs-EffNet (`discogs-effnet-bsdynamic-1`) | [genres](/mods/genres) | Music Technology Group, Universitat Pompeu Fabra (Essentia), Alonso-Jiménez et al., [essentia.upf.edu/models.html](https://essentia.upf.edu/models.html) | **CC BY-NC-SA 4.0** |

Changes made: the models are converted to Core ML (macOS) or ONNX (Windows), or their weights exported to a flat file
(the key CNN), by the scripts in `tools/models/`. The networks and their weights are not otherwise changed.

madmom's key CNN and Discogs-EffNet are non-commercial, like platendoos itself.

## Code

- `third_party/lzfse`: LZFSE, Copyright (c) 2015-2016, Apple Inc., BSD 3-clause.
- `tools/win/include/WebView2.h`, `WebView2EnvironmentOptions.h`: from the Microsoft WebView2 SDK, Copyright (C)
  Microsoft Corporation, under the SDK's licence (BSD-style).
- `tools/win/include/onnxruntime_c_api.h`: ONNX Runtime, Copyright (c) Microsoft Corporation, MIT. At run time the
  Windows mods use the `onnxruntime.dll` that comes with rekordbox; none is shipped.
- `tools/win/include/sqlite3.h`: SQLite, public domain. At run time rekordbox's own SQLite is used.

The full licence texts (MIT for Beat This! and Demucs) are in the repository's `NOTICE.md`.

## This website

The site is built with [VitePress](https://vitepress.dev) (MIT). Its text and screenshots are part of platendoos and
under the same licence. The screenshots show rekordbox's interface, which belongs to AlphaTheta.
