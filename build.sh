#!/bin/zsh
# Builds librbmods.dylib and a patched copy of an unmodified rekordbox (made for 7.2.19) that loads it; the
# patcher finds the hooks' call sites by name and reports any it can't find (see hooks.def).
#   ./build.sh [source .app] [output .app]
#   defaults: /Applications/rekordbox 7/rekordbox.app -> ~/Applications/platendoos.app
#   RBM_EDITION=public ./build.sh ...   the public edition (docs/editions.md); default full
set -euo pipefail
here=${0:A:h}
source "$here/tools/build/edition.sh"
src=${1:-/Applications/rekordbox 7/rekordbox.app}
out=${2:-$HOME/Applications/platendoos.app}

$here/compile.sh

# Replacing the app under a running rekordbox (or its rekordboxAgent) breaks it: quit both first.
[[ -d "$out" ]] && $here/tools/build/quit.sh "$out"
rm -rf "$out"
cp -c -R "$src" "$out"
bin="$out/Contents/MacOS/rekordbox"
python3 -I "$here/patch_binary.py" "$bin" "$bin.new"
mv "$bin.new" "$bin"
chmod +x "$bin"
mkdir -p "$out/Contents/Resources/rbmods"
cp -R "$here"/models/* "$out/Contents/Resources/rbmods/"
edition_ui "$out/Contents/Resources/rbmods"
cp "$here/LICENSE" "$here/NOTICE.md" "$out/Contents/Resources/rbmods/"
cp "$here/build/librbmods.dylib" "$out/Contents/MacOS/"
# flx4: the DDJ-FLX4 mapping for four decks and stems (rekordbox's own, extended).
python3 -I "$here/tools/build/flx4_mapping.py" "$src/Contents/Resources/MidiMappings/DDJ-FLX4.midi.csv" "$out/Contents/Resources/MidiMappings/DDJ-FLX4.midi.csv"
# Only the file is named platendoos; Info.plist keeps rekordbox's names: rekordbox sends CFBundleName to
# rekordboxAgent, which refuses any other ("Communication with rekordboxAgent failed").
# Its icon is rekordbox's inverted: its layered Icon Composer icon rebuilt with the colours swapped (needs Xcode's
# actool), else the .icns inverted (then without CFBundleIconName, so macOS takes the .icns, not Assets.car's icon).
res="$out/Contents/Resources"
if swift "$here/tools/build/layered_icon.swift" "$src/Contents/Resources/Assets.car" "$res"; then
    plutil -replace CFBundleIconName -string platendoos "$out/Contents/Info.plist"
else
    echo "icon: falling back to the inverted .icns"
    swift "$here/tools/build/invert_icon.swift" "$res/rekordbox.icns" "$res/platendoos.icns"
    plutil -remove CFBundleIconName "$out/Contents/Info.plist"
fi
plutil -replace CFBundleIconFile -string platendoos.icns "$out/Contents/Info.plist"

codesign --force --sign - "$out/Contents/MacOS/librbmods.dylib"
# RBM_DEBUGGABLE=1: also lets a debugger (lldb) attach, for development.
ents="$here/entitlements.plist"
if [[ ${RBM_DEBUGGABLE:-0} == 1 ]]; then
    ents=$(mktemp -t rbm-ents).plist
    cp "$here/entitlements.plist" "$ents"
    plutil -insert "com\\.apple\\.security\\.get-task-allow" -bool true "$ents"
fi
codesign --force --sign - --options runtime --entitlements "$ents" "$bin"
codesign --verify --strict "$out"
echo "built $out"
