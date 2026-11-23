#!/bin/zsh
# Builds dmltest's two Windows programs (README.md) into build/win/dmltest, with the toolchain of tools/win/build.sh,
# and links rekordbox's onnxruntime.dll and DirectML.dll next to them.
#   RB_DLLS=<folder with rekordbox's onnxruntime.dll and DirectML.dll>   (an unpacked installer, or a copy of
#   C:\Program Files\rekordbox\rekordbox 7.x.y)
set -euo pipefail
here=${0:A:h}
root=${here:h:h:h}
xwin=${XWIN:-$HOME/rbwin/xwin}
llvm=$(brew --prefix llvm)/bin
lld=$(brew --prefix lld)/bin
out=$root/build/win/dmltest
[[ -f ${RB_DLLS:?set RB_DLLS}/onnxruntime.dll && -f $RB_DLLS/DirectML.dll ]] || { echo "no onnxruntime.dll / DirectML.dll in $RB_DLLS"; exit 1 }
mkdir -p $out/obj
cflags=(--target=x86_64-pc-windows-msvc /MT /O2 /EHsc /std:c++20 /W3 /utf-8
    -Wno-microsoft-cast -Wno-deprecated-declarations -D_CRT_SECURE_NO_WARNINGS -DNOMINMAX -D_USE_MATH_DEFINES
    /imsvc $xwin/crt/include /imsvc $xwin/sdk/include/ucrt /imsvc $xwin/sdk/include/um /imsvc $xwin/sdk/include/shared /imsvc $xwin/sdk/include/winrt
    -I $root/core -I $root/tools/win/include -I $root/engines)
libs=(/libpath:$xwin/crt/lib/x86_64 /libpath:$xwin/sdk/lib/um/x86_64 /libpath:$xwin/sdk/lib/ucrt/x86_64 kernel32.lib user32.lib ole32.lib advapi32.lib)
objs=()
for s in $here/harness.cpp $here/ort_fault.cpp $root/mods/betterstems.cpp $root/mods/betterstems_ort.cpp $root/engines/dsp_win.cpp; do
    o=$out/obj/${s:t}.obj; objs+=$o
    $llvm/clang-cl $cflags /c /Fo$o -- $s
done
$lld/lld-link /nologo /subsystem:console /out:$out/harness.exe $objs $libs
$llvm/clang-cl $cflags /c /Fo$out/obj/probe.obj -- $here/probe.cpp
$lld/lld-link /nologo /subsystem:console /out:$out/probe.exe $out/obj/probe.obj $libs
ln -sf $RB_DLLS/onnxruntime.dll $RB_DLLS/DirectML.dll $out/
echo "built $out/harness.exe and probe.exe (DLLs from $RB_DLLS)"
