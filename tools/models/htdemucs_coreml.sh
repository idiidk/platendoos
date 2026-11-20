#!/bin/bash
# betterstems: remakes the HTDemucs CoreML model (the compiled one is in models/, in the app) and installs it where
# mods/betterstems.mm looks for it.
# Needs Python 3.11 (torch/coremltools); downloads ~1 GB of packages into a venv and the 80 MB weights once.
# Usage: tools/models/htdemucs_coreml.sh [python3.11]
set -euo pipefail
cd "$(dirname "$0")/../.."
PY=${1:-$(command -v python3.11 || echo "$HOME/.pyenv/versions/3.11.10/bin/python3")}
VENV=${HTDEMUCS_VENV:-$HOME/Library/Caches/rekordbox-mods/htdemucs-venv}
DEST="$HOME/Library/Application Support/rekordbox-mods/htdemucs.mlpackage"
[ -x "$PY" ] || { echo "needs Python 3.11 (pass its path)"; exit 1; }
if [ ! -x "$VENV/bin/python" ]; then
    "$PY" -m venv "$VENV"
    "$VENV/bin/pip" install -q torch coremltools einops julius openunmix dora-search omegaconf pyyaml tqdm lameenc
    "$VENV/bin/pip" install -q --no-deps demucs==4.0.1
fi
OUT=$(mktemp -d)/htdemucs.mlpackage
"$VENV/bin/python" tools/models/htdemucs_coreml.py "$OUT"
rm -rf "$DEST" "$HOME/Library/Caches/rekordbox-mods/htdemucs.mlmodelc"
mkdir -p "$(dirname "$DEST")"
mv "$OUT" "$DEST"
# The compiled model also goes in models/, which install.sh / build.sh copy into the app (it is used first there).
rm -rf models/htdemucs.mlmodelc
xcrun coremlcompiler compile "$DEST" models/
echo "installed $DEST and models/htdemucs.mlmodelc; run ./install.sh"
