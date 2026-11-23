#!/bin/zsh
# Builds "platendoos patcher.app" (patcher/): an app to send people, which makes platendoos from the rekordbox they
# have installed. It carries the mods (librbmods.dylib), their models and windows, never any of rekordbox's files.
#   tools/build/patcher_mac.sh      -> build/patcher/platendoos patcher.app and build/platendoos-patcher-mac.zip
#   RBM_EDITION=public tools/build/patcher_mac.sh   the public edition (docs/editions.md); default full
set -euo pipefail
root=${0:A:h:h:h}
source $root/tools/build/edition.sh
out=$root/build/patcher
app="$out/platendoos patcher.app"
$root/compile.sh

mkdir -p $out
python3 -I $root/tools/build/patcher_gen.py $root > $out/patcher_gen.h.new
cmp -s $out/patcher_gen.h.new $out/patcher_gen.h || mv $out/patcher_gen.h.new $out/patcher_gen.h
rm -f $out/patcher_gen.h.new

# PLATENDOOS_VERSION (set by tools/build/release.sh) names a release build; otherwise the commit, + when uncommitted changes
if [[ -n ${PLATENDOOS_VERSION:-} ]]; then build=$PLATENDOOS_VERSION; else
    build=$(git -C "$root" rev-parse --short HEAD 2>/dev/null || echo dev)
    [[ -n $(git -C "$root" status --porcelain 2>/dev/null) ]] && build="$build+"
fi

rm -rf "$app"
mkdir -p "$app/Contents/MacOS" "$app/Contents/Resources/payload/rbmods"
pdefs=(); [[ $edition == public ]] && pdefs=(-DRBM_PUBLIC=1)
# (no build paths in the binary: they would carry the builder's user name)
clang++ -std=c++20 -O2 -arch arm64 -fobjc-arc -Wall -I $out $pdefs -ffile-prefix-map="$root/=" -ffile-prefix-map="$HOME/=~/" -framework AppKit -framework WebKit \
    -o "$app/Contents/MacOS/platendoos patcher" \
    $root/patcher/patcher.cpp $root/patcher/install_mac.mm $root/core/webui_mac.mm
cat > "$app/Contents/Info.plist" <<PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
<key>CFBundleExecutable</key><string>platendoos patcher</string>
<key>CFBundleIconFile</key><string>patcher</string>
<key>CFBundleIdentifier</key><string>nl.platendoos.patcher</string>
<key>CFBundleName</key><string>platendoos patcher</string>
<key>CFBundlePackageType</key><string>APPL</string>
<key>CFBundleShortVersionString</key><string>$build</string>
<key>CFBundleVersion</key><string>1</string>
<key>LSMinimumSystemVersion</key><string>13.0</string>
<key>LSArchitecturePriority</key><array><string>arm64</string></array>
<key>NSHighResolutionCapable</key><true/>
</dict></plist>
PLIST
# its icon: the platendoos icon (the chip's record tile) on macOS's icon grid
clang++ -std=c++20 -fobjc-arc -framework AppKit -o $out/render_icon $root/tools/dev/render_icon.mm
iconset=$out/patcher.iconset
rm -rf $iconset; mkdir -p $iconset
for n in 16 32 128 256 512; do
    $out/render_icon $iconset/icon_${n}x${n}.png $n mac
    $out/render_icon $iconset/icon_${n}x${n}@2x.png $((n * 2)) mac
done
iconutil -c icns -o "$app/Contents/Resources/patcher.icns" $iconset
# the payload: what build.sh puts in the app (the Core ML models; the .onnx ones are Windows')
pay="$app/Contents/Resources/payload"
cp $root/build/librbmods.dylib "$pay/"
for m in $root/models/*; do [[ $m == *.onnx || $m == */README.md ]] || cp -R $m "$pay/rbmods/"; done
edition_ui "$pay/rbmods"
# the licences: platendoos CC BY-NC-SA 4.0, the models and third-party code (NOTICE.md); installed with the mods
cp $root/LICENSE $root/NOTICE.md "$pay/rbmods/"
codesign --force --sign - "$pay/librbmods.dylib"
codesign --force --sign - "$app"
codesign --verify --strict "$app"
rm -f $root/build/platendoos-patcher-mac.zip
ditto -c -k --keepParent "$app" $root/build/platendoos-patcher-mac.zip
echo "built the $edition edition: $app ($(du -sh "$app" | cut -f1)), $root/build/platendoos-patcher-mac.zip ($(du -h $root/build/platendoos-patcher-mac.zip | cut -f1))"
