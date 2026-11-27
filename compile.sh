#!/bin/zsh
# Compiles core/, mods/ and engines/ (.mm and .cpp) into build/librbmods.dylib; the full edition adds private/mods/.
#   RBM_EDITION=public ./compile.sh   builds the public edition (docs/editions.md); default full
set -euo pipefail
here=${0:A:h}
source "$here/tools/build/edition.sh"
mkdir -p "$here/build"
# platendoos's version, shown by the badge: the commit, "+" when built with uncommitted changes.
# PLATENDOOS_VERSION (set by tools/build/release.sh) names a release build; otherwise the commit, + when uncommitted changes
if [[ -n ${PLATENDOOS_VERSION:-} ]]; then build=$PLATENDOOS_VERSION; else
    build=$(git -C "$here" rev-parse --short HEAD 2>/dev/null || echo dev)
    [[ -n $(git -C "$here" status --porcelain 2>/dev/null) ]] && build="$build+"
fi
# The settings' declarations must be ones tools/build/settings_parse.py reads as this build does (the docs' option
# tables, the patcher and the config list for other platforms are made from what it reads): it says where, if not.
python3 -I "$here/tools/build/settings_parse.py" --check "$here" || exit 1
# Shared sources (core, mods, engines) build for both; files ending in _win.* or _ort.* (and engines/ort.cpp) are Windows-only (tools/win/build.sh).
srcs=("$here"/core/*.(mm|cpp)(N) "$here"/mods/*.(mm|cpp)(N) "$here"/engines/*.(mm|cpp)(N))
defs=()
if [[ $edition == full ]]; then srcs+=("$here"/private/mods/*.(mm|cpp)(N)); else defs=(-DRBM_PUBLIC=1); fi
srcs=(${srcs:#*_win.*})
srcs=(${srcs:#*_ort.*})   # ONNX Runtime backends (and the wrapper, engines/ort.cpp): Windows only, the mac ones are Core ML
srcs=(${srcs:#*/engines/ort.cpp})
# -mmacosx-version-min: the oldest macOS the mods run on. Without it the compiler targets the macOS it runs on, and
# the dylib then needs that version's system libraries (0.4.0 and 0.4.1 didn't start on macOS 13 to 15).
# Universal: the patcher patches both halves of rekordbox, and a patched x86_64 half (an Intel Mac, or rekordbox opened
# with Rosetta) needs the dylib's x86_64 half to start at all (docs/intel-mac.md). RBM_ARCHS=arm64 builds one
# architecture, for a quicker turn while developing. -mf16c: half-float conversions inline on x86_64.
archs=(); for a in ${=RBM_ARCHS:-arm64 x86_64}; do archs+=(-arch $a); done
[[ " ${RBM_ARCHS:-arm64 x86_64} " == *" x86_64 "* ]] && archs+=(-Xarch_x86_64 -mf16c)
clang++ -std=c++20 -O2 $archs -mmacosx-version-min=13.0 -dynamiclib -fobjc-arc -framework AppKit -framework QuartzCore -framework Security -framework WebKit -framework CoreText -framework Accelerate -framework CoreML -framework AudioToolbox -framework AVFoundation -framework CoreMIDI -framework CoreAudio -lsqlite3 -lcompression -Wall -Wno-return-type-c-linkage -DPLATENDOOS_BUILD="\"$build\"" $defs \
    -ffile-prefix-map="$here/=" -ffile-prefix-map="$HOME/=~/" -install_name @executable_path/librbmods.dylib \
    -o "$here/build/librbmods.dylib" $srcs
print -r -- $edition > "$here/build/edition"
