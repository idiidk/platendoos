#!/bin/zsh
# Quits a rekordbox app (by path: the original and the patched copy share a bundle id) and the rekordboxAgent it
# started, which otherwise outlives it; a new rekordbox that finds the old agent fails with "Communication with
# rekordboxAgent failed".
#   tools/build/quit.sh <app>
app=${1:A}
pgrep -f "^$app/Contents/MacOS/rekordbox$" >/dev/null && osascript -e "with timeout of 15 seconds" -e "tell application \"$app\" to quit" -e "end timeout" 2>/dev/null
for i in {1..40}; do pgrep -f "^$app/Contents/MacOS/rekordbox$" >/dev/null || break; sleep 1; done
# Stuck (e.g. on an error dialog): force it.
pgrep -f "^$app/Contents/MacOS/rekordbox$" >/dev/null && { echo "rekordbox did not quit; killing it"; pkill -f "^$app/Contents/MacOS/rekordbox$"; sleep 2; }
for i in {1..10}; do pgrep -f "^$app/Contents/MacOS/rekordboxAgent" >/dev/null || exit 0; sleep 1; done
pkill -f "^$app/Contents/MacOS/rekordboxAgent"
sleep 1
exit 0
