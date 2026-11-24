# C:\sml-win\install-task.ps1 — register the interactive one-shot task sml-win-launch (runs launch.ps1 in the desktop session).
# Separate from OpenSummoners' oss-rhost-runner; never touch that task.
$u = "$env:COMPUTERNAME\$env:USERNAME"
$a = New-ScheduledTaskAction -Execute 'powershell.exe' -Argument '-NoProfile -WindowStyle Hidden -ExecutionPolicy Bypass -File C:\sml-win\launch.ps1'
$p = New-ScheduledTaskPrincipal -UserId $u -LogonType Interactive -RunLevel Limited
$s = New-ScheduledTaskSettingsSet -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries -MultipleInstances IgnoreNew
Register-ScheduledTask -TaskName 'sml-win-launch' -Action $a -Principal $p -Settings $s -Force | Out-Null
Get-ScheduledTask -TaskName 'sml-win-launch' | ft TaskName,State
$a2 = New-ScheduledTaskAction -Execute 'powershell.exe' -Argument '-NoProfile -WindowStyle Hidden -ExecutionPolicy Bypass -File C:\sml-win\shot.ps1'
Register-ScheduledTask -TaskName 'sml-win-shot' -Action $a2 -Principal $p -Settings $s -Force | Out-Null
Get-ScheduledTask -TaskName 'sml-win-shot' | ft TaskName,State
