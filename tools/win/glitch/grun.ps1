param([string]$name, [int]$idle = 0)
# waits until rekordbox is quiet (under 12% of one core... of all cores 2%) for 2 x 3 s, at most $idle seconds
if ($idle -gt 0) {
    $ok = 0; $t0 = Get-Date
    while (((Get-Date) - $t0).TotalSeconds -lt $idle -and $ok -lt 2) {
        $p = Get-Process rekordbox -ea 0 | sort Threads.Count -desc | select -first 1
        if (!$p) { break }
        $a = $p.TotalProcessorTime.TotalSeconds; Start-Sleep 3; $p.Refresh(); $b = $p.TotalProcessorTime.TotalSeconds
        if (($b - $a) / 3 -lt 0.45) { $ok++ } else { $ok = 0 }
    }
    "idle after {0:0} s" -f ((Get-Date) - $t0).TotalSeconds
}
Remove-Item -Recurse -Force "C:\rb\g\$name" -ea 0
$start = Get-Date
$sl = "$env:APPDATA\Pioneer\rekordbox6\Spotify.log"; $slen = (Get-Item $sl -ea 0).Length
[IO.File]::WriteAllText('C:\rb\glitch.args', "run C:\rb\g\$name.txt C:\rb\g\$name")
Start-ScheduledTask rbglitch
for ($i = 0; $i -lt 1200 -and !(Test-Path "C:\rb\g\$name\done"); $i++) { Start-Sleep -m 250 }
$ev = Get-WinEvent -FilterHashtable @{LogName='Microsoft-Windows-Audio/GlitchDetection'; StartTime=$start} -ea 0
$ev | % { '{0:HH:mm:ss.fff} id={1} {2}' -f $_.TimeCreated, $_.Id, (($_.Message -replace '\s+', ' ')) } | Set-Content "C:\rb\g\$name\glitchlog.txt"
if (!$ev) { '' | Set-Content "C:\rb\g\$name\glitchlog.txt" }
$fs = [IO.File]::Open($sl, 'Open', 'Read', 'ReadWrite,Delete'); $fs.Seek($slen, 'Begin') | Out-Null
(New-Object IO.StreamReader($fs)).ReadToEnd() -split "`n" | ? { $_ -match 'token lost|RATE_LIMITED|rror' } | % { $_.Substring(0, [Math]::Min(160, $_.Length)) } | Set-Content "C:\rb\g\$name\spotify.txt"; $fs.Close()
'ran'
