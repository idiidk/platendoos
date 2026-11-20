# Notices

platendoos itself (everything in this repository unless a file or the list below says otherwise) is licensed under
[CC BY-NC-SA 4.0](https://creativecommons.org/licenses/by-nc-sa/4.0/): see [LICENSE](LICENSE). In short: you may
share and change it, with credit, for non-commercial purposes only, and what you make from it must use the same
licence.

platendoos is not made by, endorsed by or affiliated with AlphaTheta / Pioneer DJ or Spotify. It contains no part
of rekordbox: the patcher works on the copy of rekordbox already on the computer, and rekordbox stays under its
own licence.

## Models

The models are not in this repository. `tools/models/` makes them from their upstream releases, and the release
packages carry them (see the README).

| Model | Used by | From | Licence |
|---|---|---|---|
| Beat This! | bettergrid | CPJKU, Foscarini et al., "Beat This! Accurate Beat Tracking Without DBN Postprocessing", ISMIR 2024, https://github.com/CPJKU/beat_this | MIT (below) |
| HTDemucs v4 | betterstems | Meta, Rouard, Massa & Défossez, "Hybrid Transformers for Music Source Separation", ICASSP 2023, https://github.com/facebookresearch/demucs | MIT (below) |
| Key CNN (madmom 0.16.1, `models/key/2018/key_cnn.pkl`) | betterkey | Korzeniowski & Widmer, "Genre-Agnostic Key Classification with Convolutional Neural Networks", ISMIR 2018, https://github.com/CPJKU/madmom | CC BY-NC-SA 4.0 |
| Discogs-EffNet (`discogs-effnet-bsdynamic-1`) | genres | Music Technology Group, Universitat Pompeu Fabra (Essentia), Alonso-Jiménez et al., https://essentia.upf.edu/models.html | CC BY-NC-SA 4.0 |

Changes made: the models are converted to Core ML (macOS) or ONNX (Windows), or their weights exported to a flat
file (keycnn.bin), by the scripts in `tools/models/`. The networks and their weights are not otherwise changed.

## Code

- `third_party/lzfse`: LZFSE, Copyright (c) 2015-2016, Apple Inc., BSD 3-clause (`third_party/lzfse/LICENSE`).
- `tools/win/include/WebView2.h`, `WebView2EnvironmentOptions.h`: from the Microsoft WebView2 SDK, Copyright (C)
  Microsoft Corporation, under the SDK's licence (BSD-style).
- `tools/win/include/onnxruntime_c_api.h`: ONNX Runtime, Copyright (c) Microsoft Corporation, MIT. At run time the
  Windows mods use the `onnxruntime.dll` that comes with rekordbox; none is shipped.
- `tools/win/include/sqlite3.h`: SQLite, public domain. At run time rekordbox's own SQLite is used.

## Licence texts

### Beat This! (MIT)

    MIT License
    
    Copyright (c) 2024 Institute of Computational Perception, JKU Linz, Austria
    
    Permission is hereby granted, free of charge, to any person obtaining a copy
    of this software and associated documentation files (the "Software"), to deal
    in the Software without restriction, including without limitation the rights
    to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
    copies of the Software, and to permit persons to whom the Software is
    furnished to do so, subject to the following conditions:
    
    The above copyright notice and this permission notice shall be included in all
    copies or substantial portions of the Software.
    
    THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
    IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
    FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
    AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
    LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
    OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
    SOFTWARE.

### Demucs (MIT)

    MIT License
    
    Copyright (c) Meta Platforms, Inc. and affiliates.
    
    Permission is hereby granted, free of charge, to any person obtaining a copy
    of this software and associated documentation files (the "Software"), to deal
    in the Software without restriction, including without limitation the rights
    to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
    copies of the Software, and to permit persons to whom the Software is
    furnished to do so, subject to the following conditions:
    
    The above copyright notice and this permission notice shall be included in all
    copies or substantial portions of the Software.
    
    THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
    IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
    FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
    AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
    LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
    OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
    SOFTWARE.

### CC BY-NC-SA 4.0

The full text is in [LICENSE](LICENSE).
