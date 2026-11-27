#!/bin/zsh
# Builds platendoos for Windows on a Mac: build/win/platendoos/{platendoos.exe, rbmods.dll, rbmods/...}.
# Cross-compiles with clang-cl + lld-link against the MSVC CRT and Windows SDK fetched by xwin, in the MSVC
# C++ ABI (rekordbox.exe is built with MSVC and the mods call into it).
#   brew install llvm lld xwin
#   xwin --accept-license splat --output ~/rbwin/xwin      (or set XWIN)
# Shared sources are core/*.cpp, mods/*.cpp, engines/*.cpp (and private/mods/*.cpp for the full edition); files
# ending in _mac.* or .mm are macOS-only.
#   RBM_EDITION=public tools/win/build.sh   the public edition (docs/editions.md); default full
set -euo pipefail
here=${0:A:h}
root=${here:h:h}
source $root/tools/build/edition.sh
xwin=${XWIN:-$HOME/rbwin/xwin}
llvm=$(brew --prefix llvm)/bin
lld=$(brew --prefix lld)/bin
out=$root/build/win
obj=$out/obj-$edition   # (each edition's objects apart: they are compiled with different defines)
pkg=$out/platendoos
# a release starts clean: nothing left over from earlier builds (removed models or tables, stale objects) ships
[[ -n ${PLATENDOOS_VERSION:-} ]] && rm -rf $pkg $obj
mkdir -p $obj $pkg/rbmods

# PLATENDOOS_VERSION (set by tools/build/release.sh) names a release build; otherwise the commit, + when uncommitted changes
if [[ -n ${PLATENDOOS_VERSION:-} ]]; then build=$PLATENDOOS_VERSION; else
    build=$(git -C "$root" rev-parse --short HEAD 2>/dev/null || echo dev)
    [[ -n $(git -C "$root" status --porcelain 2>/dev/null) ]] && build="$build+"
fi
# a file that changes with the build id: the sources that use PLATENDOOS_BUILD are recompiled when it does
print -r -- $build > $obj/build_id.new
cmp -s $obj/build_id.new $obj/build_id && rm $obj/build_id.new || mv $obj/build_id.new $obj/build_id

cflags=(--target=x86_64-pc-windows-msvc /MT /O2 /EHsc /std:c++20 /W3 /Zi /utf-8
    -Wno-microsoft-cast -Wno-deprecated-declarations -D_CRT_SECURE_NO_WARNINGS -DNOMINMAX -D_USE_MATH_DEFINES
    "-DPLATENDOOS_BUILD=\"$build\""
    /imsvc $xwin/crt/include /imsvc $xwin/sdk/include/ucrt /imsvc $xwin/sdk/include/um /imsvc $xwin/sdk/include/shared /imsvc $xwin/sdk/include/winrt
    -I $root/core -I $here/include
    # no build paths in the binaries (__FILE__, debug info): they would carry the builder's user name
    "/clang:-ffile-prefix-map=$root/=" "/clang:-ffile-prefix-map=$HOME/=~/")
[[ $edition == public ]] && cflags+=(-DRBM_PUBLIC=1)
# changed compiler flags recompile everything (objects are otherwise only rebuilt when their sources change)
mkdir -p $obj
if [[ "$(cat $obj/cflags 2>/dev/null)" != "$cflags" ]]; then rm -f $obj/*.obj(N); print -r -- "$cflags" > $obj/cflags; fi
libpaths=(/libpath:$xwin/crt/lib/x86_64 /libpath:$xwin/sdk/lib/um/x86_64 /libpath:$xwin/sdk/lib/ucrt/x86_64)

srcs=($root/core/*.cpp(N) $root/mods/*.cpp(N) $root/engines/*.cpp(N))
[[ $edition == full ]] && srcs+=($root/private/mods/*.cpp(N))
srcs=(${srcs:#*_mac.*})
srcs+=($root/third_party/lzfse/*.c)  # LZFSE for stemcache's lossless chunks (macOS: libcompression)
# which mod each hook belongs to (core/image_win.cpp switches off a mod whose hooks aren't all in place)
python3 -I $here/hookmods.py $root > $obj/hookmods.cpp.new
cmp -s $obj/hookmods.cpp.new $obj/hookmods.cpp || mv $obj/hookmods.cpp.new $obj/hookmods.cpp
srcs+=$obj/hookmods.cpp
# the platendoos chip, drawn by the macOS code (mods/badge_chip.h) at Windows' display scales: one look on both
chip=$obj/badge_chip_png.h
if [[ ! -f $chip || $root/mods/badge_chip.h -nt $chip || $root/tools/dev/render_chip.mm -nt $chip ]]; then
    clang++ -std=c++20 -fobjc-arc -framework AppKit -o $obj/render_chip $root/tools/dev/render_chip.mm
    for sc in 1 1.25 1.5 1.75 2 2.5 3; do $obj/render_chip $obj/chip-$sc.png $sc; done
    python3 -I $here/embed_pngs.py $obj/chip-{1,1.25,1.5,1.75,2,2.5,3}.png > $chip
fi
cflags+=(-I $obj)
# the platendoos icon (the chip's record tile): the windows' title bars, the patcher and platendoos.exe (resource 1)
ico=$obj/platendoos.ico
if [[ ! -f $ico || $root/mods/badge_chip.h -nt $ico || $root/tools/dev/render_icon.mm -nt $ico ]]; then
    clang++ -std=c++20 -fobjc-arc -framework AppKit -o $obj/render_icon $root/tools/dev/render_icon.mm
    for n in 16 20 24 32 40 48 64 256; do $obj/render_icon $obj/icon-$n.png $n; done
    python3 -I $here/make_ico.py $ico $obj/icon-{16,20,24,32,40,48,64,256}.png
fi
print -r -- '1 ICON "platendoos.ico"' > $obj/icon.rc
[[ $obj/icon.res -nt $ico ]] || ( cd $obj && $llvm/llvm-rc -no-preprocess -fo icon.res icon.rc )

# compile in parallel, only what changed. Any header of ours newer than an object rebuilds it (headers aren't
# tracked per file: a changed header once left core.cpp's object, and so the mods' defaults, stale).
newest_h=( $root/(core|mods|engines|patcher)/*.(h|inc)(N) $root/private/mods/*.(h|inc)(N) )
newest_h=$(ls -t $newest_h | head -1)   # (one file: a glob qualifier on the array sorted nothing, and no header was ever newer)
# (RBM_JOBS=<n>: at most n compiles at a time, for when the Mac is close to its process limit and forks fail)
objs=()
pids=()
fail=0
waited=0
for s in $srcs; do
    o=$obj/${${s#$root/}//\//_}.obj
    objs+=$o
    if [[ ! -f $o || $s -nt $o || ( $s == *badge_view_win* && $chip -nt $o ) || ( $s == */badge.cpp && $obj/build_id -nt $o ) || $newest_h -nt $o || $root/hooks.def -nt $o ]]; then
        ( $llvm/clang-cl $cflags /c /Fo$o -- $s > $o.log 2>&1 || { cat $o.log; exit 1 } ) &
        pids+=$!
        if (( ${RBM_JOBS:-0} > 0 && ${#pids} - waited >= ${RBM_JOBS:-0} )); then
            for p in ${pids[waited+1,-1]}; do wait $p || fail=1; done
            waited=${#pids}
        fi
    fi
done
for p in ${pids[waited+1,-1]}; do wait $p || fail=1; done
(( fail )) && { echo "compile failed"; exit 1; }

# rekordbox ships sqlite3.dll and loads it itself: link against that copy (its exports in sqlite3.def)
$llvm/llvm-dlltool -m i386:x86-64 -d $here/sqlite3.def -l $obj/sqlite3.lib

$lld/lld-link /nologo /dll /debug /pdbaltpath:%_PDB% /out:$pkg/rbmods.dll $objs $obj/icon.res $obj/sqlite3.lib $libpaths \
    kernel32.lib user32.lib gdi32.lib shell32.lib ole32.lib bcrypt.lib advapi32.lib version.lib gdiplus.lib comctl32.lib winhttp.lib normaliz.lib shlwapi.lib
$llvm/clang-cl $cflags /c /Fo$obj/launcher.obj -- $here/launcher.cpp
$lld/lld-link /nologo /subsystem:windows /out:$pkg/platendoos.exe $obj/launcher.obj $obj/icon.res $libpaths kernel32.lib shell32.lib user32.lib
# the patcher (patcher/): installs this folder for the rekordbox on a PC, with a window; shares core/webui_win.cpp
python3 -I $root/tools/build/settings_parse.py --check $root || exit 1  # (as compile.sh: the declarations must be readable)
python3 -I $root/tools/build/patcher_gen.py $root > $obj/patcher_gen.h.new
cmp -s $obj/patcher_gen.h.new $obj/patcher_gen.h || mv $obj/patcher_gen.h.new $obj/patcher_gen.h
rm -f $obj/patcher_gen.h.new
pobjs=()
for s in $root/patcher/patcher.cpp $root/patcher/install_win.cpp; do
    o=$obj/${${s#$root/}//\//_}.obj
    pobjs+=$o
    if [[ ! -f $o || $s -nt $o || $obj/patcher_gen.h -nt $o || $obj/build_id -nt $o || $newest_h -nt $o ]]; then
        $llvm/clang-cl $cflags /c /Fo$o -- $s || exit 1
    fi
done
$lld/lld-link /nologo /subsystem:windows /out:"$pkg/platendoos patcher.exe" $pobjs $obj/core_webui_win.cpp.obj $obj/icon.res $libpaths \
    kernel32.lib user32.lib gdi32.lib shell32.lib ole32.lib advapi32.lib version.lib shlwapi.lib

# the tables for the rekordbox builds we know, and the models (the edition's tables: made from its hooks and fields)
inputs=$root/tools/win
rm -f $pkg/rbmods/*.tbl(N)   # (the other edition's, from an earlier build)
ins=($inputs/tables/*.tbl(N) $inputs/sigs/*.sig(N))
(( ${#ins} )) && { python3 -I $here/checkinputs.py $root $ins || exit 1 }
cp $inputs/tables/*.tbl $pkg/rbmods/ 2>/dev/null || echo "note: no tables in ${inputs#$root/}/tables yet"
# and their signatures: the patcher makes a table from them for a rekordbox build without one (patcher/retable.h)
rm -rf $pkg/rbmods/sigs; mkdir -p $pkg/rbmods/sigs
cp $inputs/sigs/*.sig $pkg/rbmods/sigs/ 2>/dev/null || echo "note: no signatures in ${inputs#$root/}/sigs (tools/win/makesigs.py)"
cp $root/models/keycnn.bin $pkg/rbmods/ 2>/dev/null || echo "note: no models/keycnn.bin (tools/models/export_keycnn.py)"
cp $root/models/*.onnx $pkg/rbmods/ 2>/dev/null || echo "note: no models/*.onnx yet (tools/models/make_onnx.sh)"
edition_ui $pkg/rbmods
# the licences: platendoos CC BY-NC-SA 4.0, the models and third-party code (NOTICE.md)
cp $root/LICENSE $root/NOTICE.md $pkg/
# codectest.exe (tools/dev/codectest.cpp): the codecs without rekordbox, for testing; not part of the package
lz=(${objs:#*third_party_lzfse*})
lz=(${objs:|lz})
$llvm/clang-cl $cflags /c /Fo$obj/codectest.obj -- $root/tools/dev/codectest.cpp
extraobj=()
$lld/lld-link /nologo /subsystem:console /out:$out/codectest.exe $obj/codectest.obj $obj/mods_stemcodec_win.cpp.obj \
    $obj/mods_audiofile_win.cpp.obj $extraobj $lz $libpaths kernel32.lib ole32.lib mfplat.lib mfreadwrite.lib mfuuid.lib
# the zip to send: unzip, run "platendoos patcher.exe"
rm -f $out/platendoos-windows.zip
(cd $out && zip -qr platendoos-windows.zip platendoos -x 'platendoos/rekordbox-mods.log' 'platendoos/*.tmp*' 'platendoos/*.lib' 'platendoos/*.pdb')  # no build leftovers or debug symbols
echo "built $pkg and $out/platendoos-windows.zip ($(du -h $out/platendoos-windows.zip | cut -f1))"
