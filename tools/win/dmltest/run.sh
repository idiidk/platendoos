#!/bin/zsh
# Runs one of dmltest's programs under Wine, in build/win/dmltest (README.md).
#   WINE=<path to the wine binary>   WINEPREFIX=<prefix> (default build/win/dmltest/prefix)
#   tools/win/dmltest/run.sh harness.exe 'Z:\path\to\models' music.f32 out.f32 betterstems_safe=1
#   tools/win/dmltest/run.sh probe.exe 'Z:\path\to\models\htdemucs.onnx' mix=in_mix.f32 mag=in_mag.f32 -- a:path=legacy,opt=99
set -euo pipefail
here=${0:A:h}
out=${here:h:h:h}/build/win/dmltest
export WINEPREFIX=${WINEPREFIX:-$out/prefix} WINEDEBUG=${WINEDEBUG:--all}
cd $out
# (Wine's and MoltenVK's own chatter is dropped; the programs' lines are kept)
"${WINE:?set WINE to the wine binary}" "$@" 2>&1 | grep -v -E '^\[mvk-|^[[:space:]]|^$'
