#!/bin/sh
# Builds build/beatthis_test, the command-line harness for engines/beatthis.cpp (usage: see beatthis_test.mm).
root="$(cd "$(dirname "$0")/../.." && pwd)" || exit 1
mkdir -p "$root/build"
clang++ -std=c++20 -O2 -arch arm64 -fobjc-arc -framework Foundation -framework Accelerate -framework CoreML \
  "$root/engines/beatthis.cpp" "$root/engines/beatthis_mac.mm" "$root/engines/dsp_mac.mm" "$root/tools/dev/beatthis_test.mm" -I"$root/engines" -o "$root/build/beatthis_test"
