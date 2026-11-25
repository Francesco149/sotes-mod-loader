# C:\sml-win\deploy.ps1 — create / refresh the isolated modded copy of the SE game (C:\sotes-modded) from the payload
# that tools/win/deploy-modded.sh uploaded to C:\sml-win\payload (version.dll + mods\<id>\...).  Idempotent.
#
#  * first run: copy the Steam install (steamapps\common\sotes) minus loader / test artefacts — its OWN saves from then
#    on (user\), never synced back; steam_appid.txt so SteamStub runs the copy in place (else it restarts the Steam
#    install's EN exe); a desktop shortcut "SotES with mods" -> sotes_en.exe.
#  * every run: version.dll (renamed aside first if the game has it loaded — NTFS allows renaming a mapped DLL, the
#    new one applies at the next launch) + each payload mod replaced whole.  Mods not in the payload are left alone.
$ErrorActionPreference = 'Stop'
$steam = 'C:\Program Files (x86)\Steam\steamapps\common\sotes'
$dst   = 'C:\sotes-modded'
$pay   = 'C:\sml-win\payload'

if (-not (Test-Path "$dst\sotes_en.exe")) {
  New-Item -ItemType Directory -Force $dst | Out-Null
  robocopy $steam $dst /E /NFL /NDL /NJH /NJS /NP /XF version.dll version.dll.bak oss_loader.cfg oss_mods.cfg `
    oss_modloader.log oss_voice.log fs_boot.log fs_warning.log steam_appid.txt steam_autocloud.vdf /XD mods | Out-Null
  if ($LASTEXITCODE -ge 8) { throw "robocopy failed ($LASTEXITCODE)" }
  if (-not (Test-Path "$dst\realver.dll")) { Copy-Item "$env:WINDIR\SysWOW64\version.dll" "$dst\realver.dll" }
  '1381770' | Set-Content -NoNewline "$dst\steam_appid.txt"
  "created $dst"
}

# loader
Get-ChildItem $dst -Filter 'version.dll.old-*' | Remove-Item -ea 0          # leftovers of earlier in-use swaps
if (Test-Path "$pay\version.dll") {
  if ((Test-Path "$dst\version.dll") -and (Get-FileHash "$dst\version.dll").Hash -eq (Get-FileHash "$pay\version.dll").Hash) {
    'loader unchanged'
  } else {
    if (Test-Path "$dst\version.dll") {
      try { Remove-Item "$dst\version.dll" } catch { Rename-Item "$dst\version.dll" ("version.dll.old-" + (Get-Date -f yyyyMMddHHmmss)); 'loader in use: swapped, applies next launch' }
    }
    Copy-Item "$pay\version.dll" "$dst\version.dll"
    'loader updated'
  }
}
if (Test-Path "$pay\oss_loader.cfg") { if (-not (Test-Path "$dst\oss_loader.cfg")) { Copy-Item "$pay\oss_loader.cfg" $dst } }

# exe: the unpacked EN exe (no SteamStub -> no Steam needed) as sotes_en.exe; the packed one kept as sotes_en.steam.exe
if (Test-Path "$pay\sotes_en.exe") {
  if (-not (Test-Path "$dst\sotes_en.steam.exe")) { Rename-Item "$dst\sotes_en.exe" 'sotes_en.steam.exe' }
  else { Remove-Item "$dst\sotes_en.exe" -ea 0 }
  Copy-Item "$pay\sotes_en.exe" "$dst\sotes_en.exe"
  'exe: unpacked sotes_en.exe (SteamStub copy: sotes_en.steam.exe)'
}

# mods
New-Item -ItemType Directory -Force "$dst\mods" | Out-Null
foreach ($m in Get-ChildItem "$pay\mods" -Directory -ea 0) {
  Remove-Item -Recurse -Force "$dst\mods\$($m.Name)" -ea 0
  Copy-Item -Recurse $m.FullName "$dst\mods\$($m.Name)"
  "mod $($m.Name)"
}

# shortcut
$lnk = Join-Path ([Environment]::GetFolderPath('Desktop')) 'SotES with mods.lnk'
if (-not (Test-Path $lnk)) {
  $sh = (New-Object -ComObject WScript.Shell).CreateShortcut($lnk)
  $sh.TargetPath = "$dst\sotes_en.exe"; $sh.WorkingDirectory = $dst; $sh.IconLocation = "$dst\sotes_en.exe,0"
  $sh.Description = 'Fortune Summoners SE (EN) with the mod loader - isolated copy in C:\sotes-modded'
  $sh.Save()
  "shortcut $lnk"
}
"deployed $(Get-Date -f s)"
