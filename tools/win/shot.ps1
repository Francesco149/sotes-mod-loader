# C:\sml-win\shot.ps1 — run in the desktop session by the task sml-win-shot: PrintWindow every visible top-level window of
# the sotes* processes to C:\sml-win\shot-<n>.png and list them (class / title / window + client size) in shot.txt.
Add-Type -AssemblyName System.Drawing
Add-Type @"
using System; using System.Text; using System.Runtime.InteropServices;
public class SW {
  public delegate bool EnumProc(IntPtr h, IntPtr l);
  [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr l);
  [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassName(IntPtr h, StringBuilder s, int m);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetWindowText(IntPtr h, StringBuilder s, int m);
  [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
  [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr dc, uint f);
  public struct RECT { public int L, T, R, B; }
}
"@
[SW]::SetProcessDPIAware() | Out-Null
$pids = @(Get-Process sotes* -ea 0 | % Id)
$out = @(); $n = 0
[SW]::EnumWindows({ param($h, $l)
  $p = 0; [SW]::GetWindowThreadProcessId($h, [ref]$p) | Out-Null
  if ($pids -contains $p -and [SW]::IsWindowVisible($h)) {
    $c = New-Object Text.StringBuilder 128; [SW]::GetClassName($h, $c, 128) | Out-Null
    $t = New-Object Text.StringBuilder 128; [SW]::GetWindowText($h, $t, 128) | Out-Null
    $r = New-Object SW+RECT; [SW]::GetWindowRect($h, [ref]$r) | Out-Null
    $cr = New-Object SW+RECT; [SW]::GetClientRect($h, [ref]$cr) | Out-Null
    $w = $r.R - $r.L; $hh = $r.B - $r.T
    $script:out += "win $script:n cls='$c' title='$t' window ${w}x${hh} client $($cr.R)x$($cr.B) at $($r.L),$($r.T)"
    if ($w -gt 0 -and $hh -gt 0) {
      $b = New-Object Drawing.Bitmap $w, $hh; $g = [Drawing.Graphics]::FromImage($b)
      $dc = $g.GetHdc(); [SW]::PrintWindow($h, $dc, 2) | Out-Null; $g.ReleaseHdc($dc)   # the window's OWN content (2 = full, D3D too), not the screen
      $b.Save("C:\sml-win\shot-$script:n.png"); $g.Dispose(); $b.Dispose()
    }
    $script:n++
  }
  $true }, [IntPtr]::Zero) | Out-Null
$out | Set-Content C:\sml-win\shot.txt
