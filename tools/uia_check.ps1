#Requires -Version 7
<#
.SYNOPSIS
  Accessibility check through Windows UI Automation (what screen readers use).
  Launches Orbit on a scratch profile with two tabs, lists the exposed
  elements, selects the inactive tab via SelectionItemPattern and verifies
  that the browser switched to it. Exit code 0 on success.
#>
param([ValidateSet('Debug', 'Release')] [string] $Config = 'Debug')
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName UIAutomationClient, UIAutomationTypes
$root = Split-Path -Parent $PSScriptRoot
$exe = Join-Path $root "out\$($Config.ToLower())\bin\orbit.exe"
$data = Join-Path ([IO.Path]::GetTempPath()) "orbit-uia-$PID"
$p = Start-Process $exe -ArgumentList "--user-data-dir=`"$data`"", 'https://example.com/', 'https://example.org/' -PassThru
$ok = $true
function Check($cond, $what) { if (-not $cond) { $script:ok = $false }; Write-Host ("[{0}] {1}" -f ($(if ($cond) { ' OK ' } else { 'FAIL' })), $what) }
try {
  $A = [System.Windows.Automation.AutomationElement]
  $cond = [System.Windows.Automation.AndCondition]::new(
    [System.Windows.Automation.PropertyCondition]::new($A::ProcessIdProperty, $p.Id),
    [System.Windows.Automation.PropertyCondition]::new($A::ClassNameProperty, 'OrbitBrowserWindow'))
  $win = $null
  for ($i = 0; $i -lt 100 -and -not $win; $i++) {
    Start-Sleep -Milliseconds 100
    $win = $A::RootElement.FindFirst([System.Windows.Automation.TreeScope]::Children, $cond)
  }
  Check ($null -ne $win) 'Orbit window found via UIA'
  Start-Sleep -Seconds 4  # let both pages load (titles)
  $all = $win.FindAll([System.Windows.Automation.TreeScope]::Children, [System.Windows.Automation.Condition]::TrueCondition)
  $items = foreach ($e in $all) { [pscustomobject]@{ Type = $e.Current.ControlType.ProgrammaticName -replace 'ControlType\.', ''; Name = $e.Current.Name; Class = $e.Current.ClassName; El = $e } }
  $items | Select-Object Type, Name, Class | Format-Table -AutoSize | Out-String | Write-Host
  Check (($items | Where-Object Name -eq 'Back').Count -eq 1) 'toolbar buttons exposed (Back)'
  Check (@($items | Where-Object { $_.Type -eq 'Button' -and $_.Name -ceq 'New tab' }).Count -eq 1) 'New tab button exposed'
  # Win32 controls are checked with the native UIA client (what Narrator
  # uses); the managed client applies its own proxies to them.
  $probe = Join-Path $root "out\$($Config.ToLower())\tools\uia_probe.exe"
  $native = & $probe $p.Id
  Check (@($native | Where-Object { $_ -match "^50004\tAddress and search bar\t" }).Count -eq 1) 'address bar is an Edit named "Address and search bar" (native UIA)'
  # Fresh profile: the two URLs from the command line. The new-tab page of the
  # empty space is Zen's hidden empty tab and is not listed.
  $tabs = @($items | Where-Object Type -eq 'TabItem')
  Check ($tabs.Count -eq 2) "two tab items exposed ($($tabs.Count))"
  $sel = [System.Windows.Automation.SelectionItemPattern]::Pattern
  $inactive = $tabs | Where-Object { -not $_.El.GetCurrentPattern($sel).Current.IsSelected } | Select-Object -First 1
  Check ($null -ne $inactive) "an inactive tab is reported ($($inactive.Name))"
  if ($inactive) {
    $inactive.El.GetCurrentPattern($sel).Select()
    Start-Sleep -Milliseconds 800
    Check ($inactive.El.GetCurrentPattern($sel).Current.IsSelected) 'Select() via UIA activated the tab'
    # Read the title from the Orbit window itself: Process.MainWindowTitle may
    # pick another of the process's top-level windows (floating URL bar, toast).
    $title = $win.Current.Name
    Check ($title -like "$($inactive.Name.Split(',')[0])*") "window title follows ($title)"
  }
} finally {
  & (Join-Path $PSScriptRoot 'close_orbit.ps1') -ProcessId $p.Id | Out-Null
  Remove-Item -Recurse -Force $data -ErrorAction SilentlyContinue
}
if (-not $ok) { exit 1 }
