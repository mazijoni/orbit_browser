#Requires -Version 7
<#
.SYNOPSIS
  Repeatable memory/CPU benchmark for Orbit, with Microsoft Edge as baseline.

  Every browser is measured the same way, from outside, by summing its whole
  process tree (browser, GPU, network/utility, renderers):
    * Private WS: private working set (Task Manager "Memory").
    * WS:         working set (includes pages shared with other processes).
    * Commit:     private committed bytes ("Private Bytes").
  Each scenario uses a fresh profile directory, loads synthetic pages from a
  local server (tools/bench_server.ps1), waits a fixed settle time, then takes
  the median of three samples. Idle CPU = CPU time used by the tree over a 10 s
  window after settling, as % of one core.

.EXAMPLE
  pwsh ./scripts/benchmark.ps1                      # Release build, Orbit + Edge
  pwsh ./scripts/benchmark.ps1 -Browsers orbit -Tabs 1,10
  pwsh ./scripts/benchmark.ps1 -Web                 # adds real websites (needs internet)
#>
param(
  [ValidateSet('Debug', 'Release')] [string] $Config = 'Release',
  [string[]] $Browsers = @('orbit', 'edge'),
  [int[]] $Tabs = @(1, 5, 10, 25, 50),
  [switch] $Web,
  [int] $Port = 8765,
  [string] $Out = ''
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$orbitExe = Join-Path $root "out\$($Config.ToLower())\bin\orbit.exe"
$edgeExe = "${env:ProgramFiles(x86)}\Microsoft\Edge\Application\msedge.exe"
$work = Join-Path ([IO.Path]::GetTempPath()) "orbit-bench-$PID"
New-Item -ItemType Directory -Force $work | Out-Null
$webSites = @('https://en.wikipedia.org/wiki/Web_browser', 'https://github.com/chromiumembedded/cef',
              'https://www.bbc.com/news', 'https://developer.mozilla.org/en-US/docs/Web/HTML',
              'https://www.youtube.com/', 'https://www.reddit.com/r/programming/',
              'https://stackoverflow.com/questions', 'https://www.nytimes.com/',
              'https://maps.google.com/', 'https://news.ycombinator.com/')

function Get-Tree([int] $rootPid) {
  $all = Get-CimInstance Win32_PerfFormattedData_PerfProc_Process |
         Where-Object { $_.IDProcess -ne 0 } |
         Select-Object IDProcess, CreatingProcessID, WorkingSetPrivate, WorkingSet, PrivateBytes
  $byParent = $all | Group-Object CreatingProcessID -AsHashTable -AsString
  $tree = @($all | Where-Object IDProcess -eq $rootPid)
  for ($i = 0; $i -lt $tree.Count; $i++) {
    $kids = $byParent["$($tree[$i].IDProcess)"]
    if ($kids) { $tree += $kids }
  }
  return $tree
}

function Get-Sample([int] $rootPid) {
  $t = Get-Tree $rootPid
  [pscustomobject]@{
    Processes = $t.Count
    PrivateWS = ($t | Measure-Object WorkingSetPrivate -Sum).Sum
    WS        = ($t | Measure-Object WorkingSet -Sum).Sum
    Commit    = ($t | Measure-Object PrivateBytes -Sum).Sum
  }
}

function Get-CpuMs([int[]] $ids) {
  # Sum over a fixed PID set: re-enumerating the tree between samples would
  # count the whole lifetime CPU of any process that appeared in between.
  ($ids | ForEach-Object {
     $p = Get-Process -Id $_ -ErrorAction SilentlyContinue
     if ($p) { $p.TotalProcessorTime.TotalMilliseconds } else { 0 } } | Measure-Object -Sum).Sum
}

function Stop-Tree([int] $rootPid) {
  & taskkill /T /F /PID $rootPid 2>$null | Out-Null
}

function Invoke-Scenario([string] $browser, [string] $name, [string[]] $urls, [int] $settleSec,
                         [string[]] $extraArgs = @(), [string] $dataDir = '') {
  if (-not $dataDir) { $dataDir = Join-Path $work "$browser-$name" }
  $launchArgs = @("--user-data-dir=`"$dataDir`"") + $extraArgs
  if ($browser -eq 'edge') {
    $launchArgs += @("--no-first-run", "--no-default-browser-check", "--disable-sync", "--window-size=1280,820")
    $exe = $edgeExe
  } else { $exe = $orbitExe }
  $launchArgs += $urls
  $sw = [Diagnostics.Stopwatch]::StartNew()
  $p = Start-Process -FilePath $exe -ArgumentList $launchArgs -PassThru
  while ($p.MainWindowHandle -eq 0 -and $sw.Elapsed.TotalSeconds -lt 30) { Start-Sleep -Milliseconds 20; $p.Refresh() }
  $windowMs = $sw.ElapsedMilliseconds
  Start-Sleep -Seconds $settleSec
  $samples = 1..3 | ForEach-Object { $s = Get-Sample $p.Id; Start-Sleep -Seconds 2; $s }
  $median = { param($prop) ($samples | Sort-Object $prop)[1].$prop }
  $ids = (Get-Tree $p.Id).IDProcess
  $cpu0 = Get-CpuMs $ids; Start-Sleep -Seconds 10; $cpu1 = Get-CpuMs $ids
  $result = [pscustomobject]@{
    Browser = $browser; Scenario = $name; Tabs = $urls.Count
    WindowMs = $windowMs
    Processes = & $median 'Processes'
    PrivateWS_MB = [math]::Round((& $median 'PrivateWS') / 1MB, 1)
    WS_MB = [math]::Round((& $median 'WS') / 1MB, 1)
    Commit_MB = [math]::Round((& $median 'Commit') / 1MB, 1)
    IdleCpuPct = [math]::Round(($cpu1 - $cpu0) / 10000 * 100, 2)
  }
  if ($browser -eq 'orbit') {
    # Only this instance: never touch a Orbit the user is running.
    & (Join-Path $root 'tools\close_orbit.ps1') -ProcessId $p.Id | Out-Null
    if (-not $p.HasExited) { Stop-Tree $p.Id }
  } else { Stop-Tree $p.Id }
  Start-Sleep -Seconds 2
  Write-Host ("{0,-7} {1,-12} tabs={2,-3} PrivateWS={3,8} MB  Commit={4,8} MB  procs={5,-3} idleCPU={6}%  window={7} ms" -f `
    $browser, $name, $result.Tabs, $result.PrivateWS_MB, $result.Commit_MB, $result.Processes, $result.IdleCpuPct, $windowMs)
  return $result
}

# --- run ---------------------------------------------------------------------
$server = Start-Process pwsh -ArgumentList '-NoProfile', '-File', (Join-Path $root 'tools\bench_server.ps1'), $Port -PassThru -WindowStyle Hidden
Start-Sleep -Seconds 2
$results = @()
try {
  foreach ($browser in $Browsers) {
    foreach ($n in $Tabs) {
      $urls = 1..$n | ForEach-Object { "http://localhost:$Port/p/$_" }
      $results += Invoke-Scenario $browser "pages-$n" $urls (10 + [math]::Ceiling($n / 2))
    }
    if ($browser -eq 'orbit' -and $Tabs -contains 25) {
      $urls = 1..25 | ForEach-Object { "http://localhost:$Port/p/$_" }
      # Background tabs put to sleep 20 s after start (all but the active tab).
      $results += Invoke-Scenario 'orbit' 'pages-25-slept' $urls 30 @('--orbit-bench-sleep-after=20')
      # Restored session: phase 1 loads 25 tabs and exits normally; phase 2
      # relaunches with no URLs and measures the lazily restored session.
      $dir = Join-Path $work 'orbit-restore'
      $null = Invoke-Scenario 'orbit' 'restore-src' $urls 15 @() $dir
      $results += Invoke-Scenario 'orbit' 'restored-25' @() 10 @() $dir
    }
    if ($Web) { $results += Invoke-Scenario $browser 'web-10' $webSites 30 }
  }
} finally {
  try { Invoke-WebRequest "http://localhost:$Port/quit" -TimeoutSec 2 | Out-Null } catch {}
  if (-not $server.HasExited) { Stop-Process -Id $server.Id -Force }
}

$meta = [pscustomobject]@{
  Date = (Get-Date).ToString('yyyy-MM-dd HH:mm')
  OS = (Get-CimInstance Win32_OperatingSystem).Caption + ' ' + [Environment]::OSVersion.Version
  CPU = (Get-CimInstance Win32_Processor).Name
  RAM_GB = [math]::Round((Get-CimInstance Win32_ComputerSystem).TotalPhysicalMemory / 1GB, 1)
  Orbit = "$Config build, CEF " + ((Select-String -Path (Join-Path $root 'third_party\cef\include\cef_version.h') -Pattern '#define CEF_VERSION "(.*)"').Matches[0].Groups[1].Value)
  Edge = if (Test-Path $edgeExe) { (Get-Item $edgeExe).VersionInfo.ProductVersion } else { 'n/a' }
}
if (-not $Out) { $Out = Join-Path $root "docs\benchmarks\$((Get-Date).ToString('yyyy-MM-dd')).json" }
New-Item -ItemType Directory -Force (Split-Path $Out) | Out-Null
[pscustomobject]@{ Meta = $meta; Results = $results } | ConvertTo-Json -Depth 4 | Set-Content $Out
Write-Host "`nResults written to $Out"
Remove-Item -Recurse -Force $work -ErrorAction SilentlyContinue
