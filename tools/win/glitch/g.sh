#!/bin/bash
# (once: scp grun.ps1 to the PC as C:\rb\grun.ps1)
# g.sh <name> [idle seconds]: runs the rbglitch script on stdin on the PC (desktop session), fetches the results to ~/rbwin/glitch/<name>/
set -e
name=$1; D=~/rbwin/glitch/$name; mkdir -p "$D"
CFG=~/rbwin/vm/ssh_config
cat > "$D/script.txt"
scp -q -F $CFG "$D/script.txt" rbpc:C:/rb/g/$name.txt
ssh -F $CFG rbpc "powershell -NoProfile -ExecutionPolicy Bypass -File C:\\rb\\grun.ps1 $name ${2:-0}" | tr -d '\r' | grep -v '^ran' || true
scp -q -r -F $CFG "rbpc:C:/rb/g/$name/*" "$D/"
[ -n "$GQ" ] || cat "$D/run.log"
