#!/bin/zsh
# Builds rbglitch.cpp (the dropout test tool, see docs/windows.md "Speed") into build/win/rbglitch.exe.
# Copy it to the PC as C:\rb\rbglitch.exe; the desktop task rbglitch runs "conhost --headless C:\rb\rbglitch.exe".
set -euo pipefail
here=${0:A:h}
root=${here:h:h:h}
xwin=${XWIN:-$HOME/rbwin/xwin}
out=$root/build/win
mkdir -p $out/obj
$(brew --prefix llvm)/bin/clang-cl --target=x86_64-pc-windows-msvc /MT /O2 /EHsc /std:c++20 /W3 /utf-8 \
    -Wno-deprecated-declarations -D_CRT_SECURE_NO_WARNINGS -DNOMINMAX -D_USE_MATH_DEFINES \
    /imsvc $xwin/crt/include /imsvc $xwin/sdk/include/ucrt /imsvc $xwin/sdk/include/um /imsvc $xwin/sdk/include/shared \
    /c /Fo$out/obj/rbglitch.obj -- $here/rbglitch.cpp
$(brew --prefix lld)/bin/lld-link /nologo /subsystem:console /out:$out/rbglitch.exe $out/obj/rbglitch.obj \
    /libpath:$xwin/crt/lib/x86_64 /libpath:$xwin/sdk/lib/um/x86_64 /libpath:$xwin/sdk/lib/ucrt/x86_64 \
    kernel32.lib user32.lib gdi32.lib ole32.lib psapi.lib ntdll.lib
print built $out/rbglitch.exe
