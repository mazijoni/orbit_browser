<#
.SYNOPSIS
  Sends WM_CLOSE to every Orbit window (like clicking X), or only those of
  -ProcessId, and waits for the browser to exit. Exit code 1 on timeout.
#>
param([int] $TimeoutMs = 15000, [int] $ProcessId = 0)
Add-Type @'
using System; using System.Runtime.InteropServices; using System.Text;
public static class OrbitClose {
  public delegate bool EnumProc(IntPtr h, IntPtr p);
  [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr p);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassName(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll")] public static extern IntPtr PostMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
  [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
  public static int CloseAll(uint onlyPid) {
    int n = 0;
    EnumWindows((h, p) => {
      var sb = new StringBuilder(64); GetClassName(h, sb, 64);
      uint wpid; GetWindowThreadProcessId(h, out wpid);
      if (sb.ToString() == "OrbitBrowserWindow" && (onlyPid == 0 || wpid == onlyPid)) {
        PostMessage(h, 0x10, IntPtr.Zero, IntPtr.Zero); n++;
      }
      return true;
    }, IntPtr.Zero);
    return n;
  }
}
'@
$count = [OrbitClose]::CloseAll([uint32]$ProcessId)
$sw = [Diagnostics.Stopwatch]::StartNew()
function Remaining {
  if ($ProcessId) { return @(Get-Process -Id $ProcessId -ErrorAction SilentlyContinue) }
  @(Get-Process -Name orbit -ErrorAction SilentlyContinue | Where-Object { $_.Path -like '*\bin\orbit.exe' })
}
while ((Remaining).Count -gt 0 -and $sw.ElapsedMilliseconds -lt $TimeoutMs) { Start-Sleep -Milliseconds 50 }
$left = (Remaining).Count
Write-Host "closed $count window(s); $left process(es) remaining after $($sw.ElapsedMilliseconds) ms"
if ($left -gt 0) { exit 1 }
