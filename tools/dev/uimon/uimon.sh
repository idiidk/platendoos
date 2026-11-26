#!/bin/zsh
# uimon.sh start | stop | snap [name] | layer [drawables sync]
#   start   hot-loads the stall monitor and both command channels into the running platendoos (tools/dev/hotload.sh)
#   stop    ends them (do this before quitting or restarting rekordbox: quit.sh had to kill it once with them running)
#   snap    paints rekordbox's main component into /tmp/rbm-uimon/<name>.png (JUCE's own snapshot: no screen-recording
#           permission needed; the Metal-drawn parts, the waveforms, stay black)
#   layer   logs rekordbox's CAMetalLayer settings; with arguments sets maximumDrawableCount and displaySyncEnabled
set -euo pipefail
here=${0:A:h}
root=${here:h:h:h}
D=/tmp/rbm-uimon
mkdir -p $D/samples
load() { (cd $root && tools/dev/hotload.sh $1 60 2>&1 | grep -v "^hotload" || true) }
case ${1:-} in
start)
    gen=$(( $(cat $D/gen.high 2>/dev/null || echo 0) + 1 ))   # (a generation: older monitors in the app stop when it changes)
    echo $gen > $D/gen.high; echo $gen > $D/gen.txt; echo $gen > $D/gen2.txt
    sed "s/^#define GEN 5/#define GEN $gen/" $here/mon.mm > $D/mon_gen.mm
    sed "s/^#define GEN2 3/#define GEN2 $gen/" $here/cmd2.mm > $D/cmd2_gen.mm
    load $D/mon_gen.mm; load $D/cmd2_gen.mm ;;
stop) echo 0 > $D/gen.txt; echo 0 > $D/gen2.txt ;;
snap)
    sed "s/^#define OUT \"snap.png\"/#define OUT \"${2:-snap}.png\"/" $here/snap.mm > $D/snap_gen.mm
    load $D/snap_gen.mm; ls -la $D/${2:-snap}.png ;;
layer)
    sed -e "s/^#define COUNT 0/#define COUNT ${2:-0}/" -e "s/^#define SYNC -1/#define SYNC ${3:--1}/" $here/layer.mm > $D/layer_gen.mm
    load $D/layer_gen.mm ;;
*) sed -n 2,8p $0 ;;
esac
