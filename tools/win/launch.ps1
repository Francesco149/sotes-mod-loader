# C:\sml-win\launch.ps1 — run by the interactive task sml-win-launch: start the exe named in launch.txt (line 1 =
# game dir, line 2 = exe) in the desktop session, detached; record the pid.
$l = Get-Content C:\sml-win\launch.txt
$p = Start-Process -FilePath (Join-Path $l[0] $l[1]) -WorkingDirectory $l[0] -PassThru
"$($p.Id)" | Set-Content C:\sml-win\pid.txt
