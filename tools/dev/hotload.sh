#!/bin/zsh
# Development: compiles one .mm file into a throwaway dylib and runs its extern "C" void rbm_experiment()
# inside the running platendoos app (core's hot-load), printing the log lines written while it ran.
#   tools/dev/hotload.sh experiment.mm [timeout seconds, default 30]
# The file can use everything librbmods exports (rbm::at, g_slide, logf, JStr, ...): include core/rbmod.h.
# Several runs at once (e.g. parallel agents) take turns: the app reads one load request at a time.
set -euo pipefail
here=${0:A:h:h:h}
src=${1:A}
out=${TMPDIR:-/tmp}/rbm-hotload-$$-$(date +%s%N).dylib
# Built for the architecture rekordbox runs as: x86_64 on an Intel Mac or under Rosetta (P_TRANSLATED, 0x20000, in its
# process flags; docs/intel-mac.md), else arm64.
arch=arm64
pid=$(pgrep -x rekordbox 2>/dev/null | head -1 || true)
if [[ $(uname -m) == x86_64 ]] || { [[ -n $pid ]] && (( 0x$(ps -p $pid -o flags= | tr -d ' ') & 0x20000 )) }; then arch=x86_64; fi
clang++ -std=c++20 -O1 -arch $arch -dynamiclib -fobjc-arc -framework AppKit -framework AVFoundation -framework Accelerate \
    -lsqlite3 -undefined dynamic_lookup -I"$here/core" -o "$out" "$src"
codesign --force --sign - "$out" 2>/dev/null
dir=~/Library/Application\ Support/rekordbox-mods
lock=$dir/load.lock
# Take the lock (a directory: mkdir is atomic); a lock older than 2 minutes is left over from a killed run.
for i in {1..1200}; do
    mkdir "$lock" 2>/dev/null && break
    [[ -n $(find "$lock" -maxdepth 0 -mmin +2 2>/dev/null) ]] && rmdir "$lock" 2>/dev/null
    sleep 0.1
done
trap 'rmdir "$lock" 2>/dev/null' EXIT
log=~/Library/Logs/rekordbox-mods.log
start=$(( $(wc -l < $log) + 1 ))
print -r -- "$out" > "$dir/load"
done_=
for i in {1..$(( ${2:-30} * 5 ))}; do
    # Done when this dylib's "hotload: <path> ..." line is followed by "hotload: done" (or it failed to load).
    if tail -n +$start $log | awk -v p="hotload: $out " '
        index($0, p) == 1 { seen = 1; if ($0 !~ / running$/) { ok = 1 } ; next }
        seen && $0 == "hotload: done" { ok = 1 }
        END { exit !ok }'; then done_=1; break; fi
    sleep 0.2
done
tail -n +$start $log
[[ -n $done_ ]] || { print "hotload: timed out (is platendoos running?)"; exit 1; }
