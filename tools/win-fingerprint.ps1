# tools/win-fingerprint.ps1 — dump a process's top-level windows (class/title/style/size).
#
# Use it to fingerprint a game's MAIN window when adding a profile: the loader's bootstrap
# latches the window whose class == profile.window_class (POSITIVELY — never the launcher, a
# DirectShow "ActiveMovie" window, an IME window, or an early transient), so you need the real
# class name.  Launch the game, reach a stable screen, then run this against its PID and read
# off the top-level (owner=0), visible window with a real size + game title.
#
#   powershell.exe -NoProfile -ExecutionPolicy Bypass -File tools/win-fingerprint.ps1 -TargetPid <pid>
#
# (from WSL:  P=$(wslpath -w tools/win-fingerprint.ps1); powershell.exe -File "$P" -TargetPid 1234)
# Find the PID with:  Get-Process <exe-without-.exe> | Select Id,MainWindowTitle
#
# SotES EN-SE result (the profile's window_class):
#   hwnd=... vis=True owner=0 style=0x94CA0000 646x509 cls='CLASS_LIZSOFT_SOTES' title='Fortune Summoners Ver2...'
param([Parameter(Mandatory=$true)][int]$TargetPid)
Add-Type @"
using System;
using System.Text;
using System.Runtime.InteropServices;
public class W {
  public delegate bool EnumProc(IntPtr h, IntPtr l);
  [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr l);
  [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassName(IntPtr h, StringBuilder s, int m);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetWindowText(IntPtr h, StringBuilder s, int m);
  [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
  [DllImport("user32.dll")] public static extern IntPtr GetWindow(IntPtr h, uint c);
  [DllImport("user32.dll")] public static extern int GetWindowLong(IntPtr h, int i);
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int l,t,r,b; }
  public static System.Collections.Generic.List<string> Dump(uint want) {
    var outp = new System.Collections.Generic.List<string>();
    EnumProc cb = (h, l) => {
      uint wp; GetWindowThreadProcessId(h, out wp);
      if (wp == want) {
        var c = new StringBuilder(256); GetClassName(h, c, 256);
        var t = new StringBuilder(256); GetWindowText(h, t, 256);
        RECT r; GetWindowRect(h, out r);
        outp.Add(String.Format("hwnd={0:X8} vis={1} owner={2:X} style=0x{3:X8} {4}x{5} cls='{6}' title='{7}'",
          h.ToInt64(), IsWindowVisible(h), GetWindow(h,4).ToInt64(), GetWindowLong(h,-16),
          r.r-r.l, r.b-r.t, c.ToString(), t.ToString()));
      }
      return true;
    };
    EnumWindows(cb, IntPtr.Zero);
    return outp;
  }
}
"@
[W]::Dump([uint32]$TargetPid) | ForEach-Object { $_ }
