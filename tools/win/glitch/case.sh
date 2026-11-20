#!/bin/bash
# case.sh <name> <title> [idle s] [extra script lines]: the track list is searched for <title> ('_' = space) and its one
# row selected; deck 1 (playing the demo track) jumps to its start; 6 s into the recording the row is loaded on deck 2 with
# shift+right; everything is recorded for 39 s after the load.
name=$1; title=$2; idle=${3:-0}
{
  echo "0 click 1770 860"; echo "500 key ctrl+a"; echo "800 type $title"
  echo "15500 click 700 895"
  echo "16200 key shift+a"
  for c in "rec 45" "ui 45" "threads 45 50" "log 45" "strip 1118 357 300 16 45 100 title" "strip 1062 393 850 26 45 100 over" "strip 30 202 1870 50 45 100 lane"; do echo "17000 $c"; done
  [ -n "$4" ] && printf '%b\n' "$4"
  echo "23000 key shift+right"
  echo "62500 end"
} | sort -n -s -k1,1 | GQ=1 "$(dirname "$0")/g.sh" "$name" "$idle"
python3 "$(dirname "$0")/summ.py" ~/rbwin/glitch/$name | tee -a ~/rbwin/glitch/results.txt
