#!/bin/bash
# Test platendoos in a Windows VM over ssh.
#
#   tools/win/vm.sh setup          once: helper scripts + scheduled tasks in the VM
#   tools/win/vm.sh deploy         copy build/win/platendoos (models only when changed)
#   tools/win/vm.sh run|stop       start rekordbox with the mods in the desktop session / quit it
#   tools/win/vm.sh log [n]        last n lines of rekordbox-mods.log (default 80)
#   tools/win/vm.sh shot [file]    screenshot of the VM desktop (default ~/rbwin/vm/shot.png)
#   tools/win/vm.sh ps <command>   run a PowerShell command
#
# ssh host: $RBVM (default rbvm from ~/rbwin/vm/ssh_config). GUI programs can't start from an ssh
# session (no desktop), so run/shot go through scheduled tasks that run in the logged-in user's session.
set -e
cd "$(dirname "$0")/../.."
HOST=${RBVM:-rbvm}
CFG=${RBVM_SSHCONFIG:-$HOME/rbwin/vm/ssh_config}
SSH=(ssh -F "$CFG" "$HOST")
# stdin -> a .ps1 in the VM, run with -File (powershell -Command - mishandles multi-line blocks)
ps() {
    local f; f=$(mktemp); cat > "$f"
    scp -q -F "$CFG" "$f" "$HOST:C:/rb/cmd.ps1"; rm -f "$f"
    "${SSH[@]}" powershell -NoProfile -NonInteractive -ExecutionPolicy Bypass -File 'C:\rb\cmd.ps1'
}
REMOTE='C:\rb'

case "$1" in
setup)
    ps <<'EOF'
New-Item -ItemType Directory -Force C:\rb | Out-Null
@'
Add-Type -AssemblyName System.Windows.Forms,System.Drawing
Add-Type 'using System.Runtime.InteropServices; public class D { [DllImport("user32.dll")] public static extern bool SetProcessDPIAware(); }'
[D]::SetProcessDPIAware() | Out-Null
$b = [System.Windows.Forms.SystemInformation]::VirtualScreen
$bmp = New-Object System.Drawing.Bitmap $b.Width, $b.Height
$g = [System.Drawing.Graphics]::FromImage($bmp)
$g.CopyFromScreen($b.Left, $b.Top, 0, 0, $bmp.Size)
$bmp.Save('C:\rb\shot.png', [System.Drawing.Imaging.ImageFormat]::Png)
'@ | Set-Content -Encoding ascii C:\rb\shot.ps1
$u = (whoami)
$set = New-ScheduledTaskSettingsSet -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries -ExecutionTimeLimit 0
$p = New-ScheduledTaskPrincipal -UserId $u -LogonType Interactive -RunLevel Highest
Register-ScheduledTask -Force -TaskName platendoos -Principal $p -Settings $set `
  -Action (New-ScheduledTaskAction -Execute 'C:\rb\platendoos\platendoos.exe' -WorkingDirectory 'C:\rb\platendoos') | Out-Null
Register-ScheduledTask -Force -TaskName rbshot -Principal $p -Settings $set `
  -Action (New-ScheduledTaskAction -Execute 'conhost.exe' -Argument '--headless powershell.exe -NoProfile -ExecutionPolicy Bypass -File C:\rb\shot.ps1') | Out-Null
'setup ok'
EOF
    ;;
deploy)
    B=build/win/platendoos
    [ -f $B/rbmods.dll ] || { echo "build first: tools/win/build.sh"; exit 1; }
    "$0" stop >/dev/null 2>&1 || true
    tmp=$(mktemp -d); trap 'rm -rf "$tmp"' EXIT
    # everything but the big models in one zip; models only when their size changed
    (cd build/win && zip -qr "$tmp/p.zip" platendoos -x 'platendoos/rbmods/*.onnx')
    scp -q -F "$CFG" "$tmp/p.zip" "$HOST:C:/rb/p.zip"
    ps <<'EOF'
Expand-Archive -Force C:\rb\p.zip C:\rb; Remove-Item C:\rb\p.zip
EOF
    for m in $B/rbmods/*.onnx; do
        [ -f "$m" ] || continue
        n=$(basename "$m"); size=$(stat -f %z "$m")
        have=$(echo "(Get-Item 'C:\\rb\\platendoos\\rbmods\\$n' -ea 0).Length" | ps | tr -d '\r')
        [ "$have" = "$size" ] || { echo "copying $n"; scp -q -F "$CFG" "$m" "$HOST:C:/rb/platendoos/rbmods/$n"; }
    done
    # the extended DDJ-FLX4 mapping (decks 3/4, stems), as the patcher installs it; rekordbox's own kept as .original
    for d in $(echo '(Get-ChildItem "C:\Program Files\rekordbox\rekordbox *\MidiMappings\DDJ-FLX4.midi.csv").DirectoryName -replace "\\","/" -replace " ","%20"' | ps | tr -d '\r'); do
        d=${d//%20/ }
        src="$d/DDJ-FLX4.midi.csv"
        echo "\$s = '$src'; if (Test-Path \"\$s.original\") { \$s += '.original' }; Copy-Item -Force \$s C:\rb\flx4_orig.csv" | ps
        scp -q -F "$CFG" "$HOST:C:/rb/flx4_orig.csv" "$tmp/orig.csv" || continue
        python3 -I tools/build/flx4_mapping.py "$tmp/orig.csv" "$tmp/ext.csv" || continue
        scp -q -F "$CFG" "$tmp/ext.csv" "$HOST:C:/rb/flx4_ext.csv"
        echo "if (-not (Test-Path '$src.original')) { Copy-Item '$src' '$src.original' }; Copy-Item -Force C:\rb\flx4_ext.csv '$src'; 'FLX4 mapping: $src'" | ps
    done
    echo deployed
    ;;
run)
    echo 'Start-ScheduledTask -TaskName platendoos; "started"' | ps
    ;;
stop)
    # CloseMainWindow only sees windows on its own desktop, so it runs in the user's session (task rbclose)
    ps <<'EOF'
# (WM_CLOSE to rekordbox's own window, class JUCE_*: CloseMainWindow may pick the mods' Preferences window instead)
@'
Add-Type @"
using System; using System.Text; using System.Runtime.InteropServices;
public class C2 {
  public delegate bool P(IntPtr h, IntPtr l);
  [DllImport("user32.dll")] public static extern bool EnumWindows(P p, IntPtr l);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetWindowText(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassName(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
  [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
  public static int Close(uint want) { int n = 0;
    EnumWindows((h, l) => { uint pid; GetWindowThreadProcessId(h, out pid); if (pid != want) return true;
      var t = new StringBuilder(256); GetWindowText(h, t, 256); var c = new StringBuilder(256); GetClassName(h, c, 256);
      if (t.ToString() == "rekordbox" && c.ToString().StartsWith("JUCE_")) { PostMessage(h, 0x10, IntPtr.Zero, IntPtr.Zero); n++; }
      return true; }, IntPtr.Zero);
    return n; } }
"@
foreach ($p in Get-Process rekordbox -ea 0) { if ([C2]::Close([uint32]$p.Id) -eq 0) { $p.CloseMainWindow() | Out-Null } }
Get-Process platendoos -ea 0 | % { $_.CloseMainWindow() | Out-Null }
'@ | Set-Content -Encoding ascii C:\rb\close.ps1
$p = New-ScheduledTaskPrincipal -UserId (whoami) -LogonType Interactive -RunLevel Highest
Register-ScheduledTask -Force -TaskName rbclose -Principal $p `
  -Action (New-ScheduledTaskAction -Execute 'conhost.exe' -Argument '--headless powershell.exe -NoProfile -ExecutionPolicy Bypass -File C:\rb\close.ps1') | Out-Null
if (Get-Process rekordbox,platendoos -ea 0) { Start-ScheduledTask -TaskName rbclose }
# wait for a normal exit (a crash while quitting leaves a dump in %APPDATA%\Pioneer\rekordbox\Crashes)
# (plain rekordbox too sometimes takes ~76 s to quit: 1 in 3 on the PC on 2026-11-04; that one gets killed)
for ($i = 0; $i -lt 60 -and (Get-Process rekordbox -ea 0); $i++) { Start-Sleep -m 500 }
$left = [bool](Get-Process rekordbox -ea 0)
Get-Process rekordbox,platendoos,rekordboxAgent -ea 0 | Stop-Process -Force
if ($left) { 'stopped (rekordbox did not quit within 30 s: killed)' } else { 'stopped' }
EOF
    ;;
log)  # the file as is (Get-Content would read the UTF-8 as ANSI); last N lines (default 80, 0 = all)
    tmp=$(mktemp -t rbwinlog)
    scp -q -F "$CFG" "$HOST:AppData/Local/rekordbox-mods/rekordbox-mods.log" "$tmp" || { rm -f "$tmp"; exit 1; }
    if [[ ${2:-80} == 0 ]]; then cat "$tmp"; else tail -n ${2:-80} "$tmp"; fi
    rm -f "$tmp"
    ;;
shot)
    out=${2:-$HOME/rbwin/vm/shot.png}; mkdir -p "$(dirname "$out")"
    echo 'Remove-Item C:\rb\shot.png -ea 0; Start-ScheduledTask -TaskName rbshot; for ($i=0; $i -lt 120 -and !(Test-Path C:\rb\shot.png); $i++) { Start-Sleep -m 250 }; Start-Sleep -m 500' | ps
    scp -q -F "$CFG" "$HOST:C:/rb/shot.png" "$out" && echo "$out"
    ;;
ps)
    shift; if [ $# -gt 0 ]; then echo "$*" | ps; else ps; fi
    ;;
*)
    sed -n '2,12p' "$0"; exit 1
    ;;
esac
