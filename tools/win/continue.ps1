# C:\sml-win\continue.ps1 PORT SLOT — title -> save via mod.game.save.load, wait for a room with a party (180 s)
param([int]$Port, [int]$Slot = -1)
$L = 'C:\sml-win\lstr.ps1'
$scene = 'local w = mod.game.roster.world() return w ~= nil and w.room ~= 0 and #mod.game.roster.members() > 0'
& $L $Port "mod.game.attract.set(false) return {mod.game.save.load($Slot)}"
$t0 = Get-Date
while (((Get-Date) - $t0).TotalSeconds -lt 180) {
  $st = & $L $Port 'return {mod.game.save.status()}'
  if ($st -match '"failed"') { "FAILED $st"; exit 4 }
  if ($st -match '"loaded"' -and (& $L $Port $scene) -eq 'ok true') { "in scene after $([int]((Get-Date) - $t0).TotalSeconds) s"; exit 0 }
  Start-Sleep -Milliseconds 500
}
"TIMEOUT $st"; exit 5
