<#
.SYNOPSIS
  Captures a Orbit window to a PNG (development / test helper).
  Uses PrintWindow(PW_RENDERFULLCONTENT) so GPU-composited content is included.
#>
param(
  [Parameter(Mandatory)] [string] $Out,
  [int] $ProcessId = 0
)
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing
Add-Type @'
using System;
using System.Runtime.InteropServices;
using System.Text;
public static class OrbitCapture {
  public delegate bool EnumProc(IntPtr h, IntPtr p);
  [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr p);
  [DllImport("user32.dll")] public static extern int GetClassName(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
  [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr dc, uint flags);
  [DllImport("user32.dll")] public static extern bool SetProcessDpiAwarenessContext(IntPtr v);
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
  public static IntPtr Find(uint pid) {
    IntPtr found = IntPtr.Zero;
    EnumWindows((h, p) => {
      var sb = new StringBuilder(64);
      GetClassName(h, sb, 64);
      uint wpid; GetWindowThreadProcessId(h, out wpid);
      if (sb.ToString() == "OrbitBrowserWindow" && IsWindowVisible(h) && (pid == 0 || wpid == pid)) {
        found = h; return false;
      }
      return true;
    }, IntPtr.Zero);
    return found;
  }
}
'@
[OrbitCapture]::SetProcessDpiAwarenessContext([IntPtr]-4) | Out-Null  # per-monitor v2
$h = [OrbitCapture]::Find([uint32]$ProcessId)
if ($h -eq [IntPtr]::Zero) { throw 'No visible Orbit window found.' }
$r = New-Object OrbitCapture+RECT
[OrbitCapture]::GetWindowRect($h, [ref]$r) | Out-Null
$bmp = New-Object System.Drawing.Bitmap ($r.R - $r.L), ($r.B - $r.T)
$g = [System.Drawing.Graphics]::FromImage($bmp)
$dc = $g.GetHdc()
[OrbitCapture]::PrintWindow($h, $dc, 2) | Out-Null
$g.ReleaseHdc($dc)
$bmp.Save($Out, [System.Drawing.Imaging.ImageFormat]::Png)
$g.Dispose(); $bmp.Dispose()
Write-Host "saved $Out ($($r.R - $r.L)x$($r.B - $r.T))"
