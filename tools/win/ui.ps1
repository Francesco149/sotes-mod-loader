# C:\sml-win\ui.ps1 PORT "LUA" [SHOTNAME] — one mod.ui.synth step (Lua table body), wait for it to drain, optional PrintWindow shot
param([int]$Port, [string]$Synth, [string]$Shot)
[IO.File]::WriteAllText('C:\sml-win\_ui.lua', "return mod.ui.synth{$Synth}")
& C:\sml-win\lua.ps1 $Port C:\sml-win\_ui.lua | Out-Null
Start-Sleep -Milliseconds 700
if ($Shot) { Remove-Item C:\sml-win\shot-*.png -ea 0; Start-ScheduledTask -TaskName sml-win-shot; Start-Sleep 5; Copy-Item C:\sml-win\shot-0.png "C:\sml-win\$Shot.png" -Force }
