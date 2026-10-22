#Requires -Version 7
<#
.SYNOPSIS
  Runs Orbit's in-browser integration suites against fresh, throwaway
  user-data directories. Each suite launches the real browser.
.EXAMPLE
  ./scripts/run_selftests.ps1 -Config Debug
#>
param(
  [ValidateSet('Debug', 'Release')] [string] $Config = 'Debug',
  [string[]] $Only = @(),
  [int] $TimeoutSec = 120
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$exe = Join-Path $root "out\$($Config.ToLower())\bin\orbit.exe"
if (-not (Test-Path $exe)) { throw "Not built: $exe" }
$work = Join-Path ([IO.Path]::GetTempPath()) "orbit-selftest-$PID"

function Invoke-Suite([string] $suite, [string] $dataDir) {
  if ($Only.Count -and -not ($Only | Where-Object { $suite -like "$_*" })) { return $true }
  $sw = [Diagnostics.Stopwatch]::StartNew()
  $p = Start-Process -FilePath $exe -PassThru `
         -ArgumentList "--user-data-dir=`"$dataDir`"", "--orbit-self-test=$suite"
  if (-not $p.WaitForExit($TimeoutSec * 1000)) {
    $p.Kill()
    Write-Host "[TIMEOUT] $suite"
    return $false
  }
  $report = Join-Path $dataDir "selftest-$suite.txt"
  $ok = ($p.ExitCode -eq 0) -and (Test-Path $report)
  Write-Host ("[{0}] {1}  (exit {2}, {3:n1}s)" -f ($(if ($ok) { ' OK ' } else { 'FAIL' })), $suite, $p.ExitCode, $sw.Elapsed.TotalSeconds)
  if (Test-Path $report) {
    Get-Content $report | Where-Object { -not $ok -or $_ -match 'FAIL' } | ForEach-Object { Write-Host "    $_" }
  }
  # Every Chromium sub-process must be gone after a normal exit.
  Start-Sleep -Milliseconds 300
  $left = @(Get-Process -Name orbit -ErrorAction SilentlyContinue | Where-Object { $_.Path -eq $exe })
  if ($left.Count -gt 0) { Write-Host "    FAIL: $($left.Count) process(es) left running"; $ok = $false }
  return $ok
}

$results = [ordered]@{}
New-Item -ItemType Directory -Force $work | Out-Null
$results['basic'] = Invoke-Suite 'basic' (Join-Path $work 'basic')
$results['isolation'] = Invoke-Suite 'isolation' (Join-Path $work 'isolation')
$results['services'] = Invoke-Suite 'services' (Join-Path $work 'services')
$results['memory'] = Invoke-Suite 'memory' (Join-Path $work 'memory')
$results['security'] = Invoke-Suite 'security' (Join-Path $work 'security')
$results['zen'] = Invoke-Suite 'zen' (Join-Path $work 'zen')
$restoreDir = Join-Path $work 'restore'
$results['restore-1'] = Invoke-Suite 'restore-1' $restoreDir
$results['restore-2'] = Invoke-Suite 'restore-2' $restoreDir
$results['restore-3'] = Invoke-Suite 'restore-3' $restoreDir

$failed = @($results.GetEnumerator() | Where-Object { -not $_.Value }).Count
Write-Host "`n$($results.Count - $failed)/$($results.Count) suites passed"
if ($failed) {
  Write-Host "Logs kept in $work"
  exit 1
}
Remove-Item -Recurse -Force $work -ErrorAction SilentlyContinue
