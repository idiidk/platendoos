#!/bin/zsh
# Compiles core/, mods/ and engines/ (.mm and .cpp) into build/librbmods.dylib; the full edition adds private/mods/.
#   RBM_EDITION=public ./compile.sh   builds the public edition (docs/editions.md); default full
set -euo pipefail
here=${0:A:h}
source "$here/tools/build/edition.sh"
mkdir -p "$here/build"
python3 -I "$here/tools/build/config_gen.py" "$here" > "$here/core/config_template.h.new"
cmp -s "$here/core/config_template.h.new" "$here/core/config_template.h" || mv "$here/core/config_template.h.new" "$here/core/config_template.h"
rm -f "$here/core/config_template.h.new"
# platendoos's version, shown by the badge: the commit, "+" when built with uncommitted changes.
# PLATENDOOS_VERSION (set by tools/build/release.sh) names a release build; otherwise the commit, + when uncommitted changes
if [[ -n ${PLATENDOOS_VERSION:-} ]]; then build=$PLATENDOOS_VERSION; else
    build=$(git -C "$here" rev-parse --short HEAD 2>/dev/null || echo dev)
    [[ -n $(git -C "$here" status --porcelain 2>/dev/null) ]] && build="$build+"
fi
# Shared sources (core, mods, engines) build for both; files ending in _win.* or _ort.* (and engines/ort.cpp) are Windows-only (tools/win/build.sh).
srcs=("$here"/core/*.(mm|cpp)(N) "$here"/mods/*.(mm|cpp)(N) "$here"/engines/*.(mm|cpp)(N))
defs=()
if [[ $edition == full ]]; then srcs+=("$here"/private/mods/*.(mm|cpp)(N)); else defs=(-DRBM_PUBLIC=1); fi
srcs=(${srcs:#*_win.*})
srcs=(${srcs:#*_ort.*})   # ONNX Runtime backends (and the wrapper, engines/ort.cpp): Windows only, the mac ones are Core ML
srcs=(${srcs:#*/engines/ort.cpp})
clang++ -std=c++20 -O2 -arch arm64 -dynamiclib -fobjc-arc -framework AppKit -framework QuartzCore -framework Security -framework WebKit -framework CoreText -framework Accelerate -framework CoreML -framework AudioToolbox -framework AVFoundation -framework CoreMIDI -framework CoreAudio -lsqlite3 -lcompression -Wall -Wno-return-type-c-linkage -DPLATENDOOS_BUILD="\"$build\"" $defs \
    -ffile-prefix-map="$here/=" -ffile-prefix-map="$HOME/=~/" -install_name @executable_path/librbmods.dylib \
    -o "$here/build/librbmods.dylib" $srcs
print -r -- $edition > "$here/build/edition"
