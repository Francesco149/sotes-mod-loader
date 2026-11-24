# C:\sml-win\drive.ps1 PORT STEP... — post UI input to the game window from inside the game (no real mouse/keyboard):
#   click:X,Y (client px)   type:TEXT   key:VK (decimal)   wait:MS   shot:NAME (PrintWindow -> C:\sml-win\NAME.png)
param([int]$Port, [Parameter(ValueFromRemainingArguments=$true)][string[]]$Steps)
$pre = 'local ffi = require "ffi"; pcall(ffi.cdef, "void *FindWindowA(const char*, const char*); int PostMessageA(void*, unsigned, unsigned, long);"); local h = ffi.C.FindWindowA("CLASS_LIZSOFT_SOTES", nil); local P = function(m, w, l) ffi.C.PostMessageA(h, m, w, l) end; '
function L([string]$code) { [IO.File]::WriteAllText('C:\sml-win\_drive.lua', $pre + $code); & C:\sml-win\lua.ps1 $Port C:\sml-win\_drive.lua | Out-Null }
foreach ($s in $Steps) {
  $k, $v = $s -split ':', 2
  switch ($k) {
    'click' { $x, $y = $v -split ','; $l = [int]$y * 65536 + [int]$x
              L ("P(0x200, 0, $l) P(0x201, 1, $l) return 1") | Out-Null; Start-Sleep -Milliseconds 150
              L ("P(0x202, 0, $l) return 1") | Out-Null }
    'type'  { foreach ($ch in $v.ToCharArray()) { L ("P(0x102, $([int]$ch), 1) return 1") | Out-Null; Start-Sleep -Milliseconds 60 } }
    'key'   { L ("P(0x100, $v, 1) return 1") | Out-Null; Start-Sleep -Milliseconds 80
              L ("P(0x101, $v, 0xC0000001) return 1") | Out-Null }
    'wait'  { Start-Sleep -Milliseconds ([int]$v) }
    'shot'  { Remove-Item C:\sml-win\shot-*.png -ea 0; Start-ScheduledTask -TaskName sml-win-shot; Start-Sleep 5; Copy-Item C:\sml-win\shot-0.png "C:\sml-win\$v.png" -Force }
  }
  Start-Sleep -Milliseconds 250
}
