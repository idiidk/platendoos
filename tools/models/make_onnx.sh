#!/bin/bash
# The Windows models: models/beatthis.onnx (Beat This!), models/htdemucs.onnx (HTDemucs v4) and models/effnet.onnx
# (Discogs-EffNet, downloaded as is from Essentia), for the ONNX Runtime backends (engines/beatthis_ort.cpp,
# mods/betterstems_ort.cpp, mods/genres_model_ort.cpp). tools/win/build.sh copies models/*.onnx into the package.
# Weights are downloaded once into ~/.cache/torch (77 + 80 MB); the .onnx files are ~85 + ~170 MB and not in git.
# Usage: tools/models/make_onnx.sh [python3.11]
set -euo pipefail
cd "$(dirname "$0")/../.."
PY=${1:-$(command -v python3.11 || echo "$HOME/.pyenv/versions/3.11.10/bin/python3")}
VENV=${ONNX_VENV:-$HOME/Library/Caches/rekordbox-mods/onnx-venv}
if [ ! -x "$VENV/bin/python" ]; then
    "$PY" -m venv "$VENV"
    "$VENV/bin/pip" install -q torch onnx onnxruntime numpy einops rotary-embedding-torch soxr julius openunmix dora-search omegaconf pyyaml tqdm lameenc
    "$VENV/bin/pip" install -q --no-deps demucs==4.0.1 "beat_this @ git+https://github.com/CPJKU/beat_this"
fi
mkdir -p models
"$VENV/bin/python" tools/models/export_beatthis_onnx.py ${BEATTHIS_AUDIO:+--audio "$BEATTHIS_AUDIO"} --out models/beatthis.onnx
"$VENV/bin/python" tools/models/htdemucs_onnx.py models/htdemucs.onnx
[ -f models/effnet.onnx ] || curl -fL -o models/effnet.onnx https://essentia.upf.edu/models/feature-extractors/discogs-effnet/discogs-effnet-bsdynamic-1.onnx
