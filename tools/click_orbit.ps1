<#
.SYNOPSIS
  Posts a left (or right) click to a Orbit window at client coordinates given
  in DIPs (development helper for visual checks; does not move the real
  cursor). Use -ProcessId to target a specific instance.
#>
param([Parameter(Mandatory)] [double] $X, [Parameter(Mandatory)] [double] $Y, [switch] $Right,
      [int] $ProcessId = 0)
Add-Type @'
using System; using System.Runtime.InteropServices; using System.Text;
public static class OrbitClick {
  public delegate bool EnumProc(IntPtr h, IntPtr p);
  [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr p);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassName(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
  [DllImport("user32.dll")] public static extern IntPtr PostMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
  [DllImport("user32.dll")] public static extern uint GetDpiForWindow(IntPtr h);
  public static IntPtr Find(uint pid) {
    IntPtr found = IntPtr.Zero;
    EnumWindows((h, p) => { var sb = new StringBuilder(64); GetClassName(h, sb, 64);
      uint wpid; GetWindowThreadProcessId(h, out wpid);
      if (sb.ToString() == "OrbitBrowserWindow" && (pid == 0 || wpid == pid)) { found = h; return false; }
      return true; }, IntPtr.Zero);
    return found;
  }
}
'@
$h = [OrbitClick]::Find([uint32]$ProcessId)
if ($h -eq [IntPtr]::Zero) { throw 'No Orbit window' }
$scale = [OrbitClick]::GetDpiForWindow($h) / 96.0
$lp = [IntPtr](([int]($Y * $scale) -shl 16) -bor ([int]($X * $scale) -band 0xFFFF))
$down, $up = if ($Right) { 0x204, 0x205 } else { 0x201, 0x202 }
[OrbitClick]::PostMessage($h, 0x200, [IntPtr]::Zero, $lp) | Out-Null   # WM_MOUSEMOVE
[OrbitClick]::PostMessage($h, $down, [IntPtr]1, $lp) | Out-Null
[OrbitClick]::PostMessage($h, $up, [IntPtr]::Zero, $lp) | Out-Null
