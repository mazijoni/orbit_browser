#Requires -Version 7
<#
.SYNOPSIS
  Produces the distributable: dist/Orbit-<version>-win64.zip.

  The zip holds a self-contained folder. Users run "orbit.exe --install" from
  it (per-user install, no admin rights) or simply run orbit.exe in place.
  Steps: Release build + unit tests, assemble files (no tests/symbols),
  verify imports resolve to system DLLs or bundled files only, smoke-test the
  assembled copy from a different path, zip, print SHA-256.
#>
param([switch] $SkipBuild)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
if (-not $SkipBuild) { & (Join-Path $PSScriptRoot 'build.ps1') -Config Release }

$version = (Select-String -Path (Join-Path $root 'CMakeLists.txt') -Pattern 'project\(orbit VERSION ([0-9.]+)').Matches[0].Groups[1].Value
$bin = Join-Path $root 'out\release\bin'
$dist = Join-Path $root 'dist'
$stage = Join-Path $dist "Orbit-$version"
if (Test-Path $stage) { Remove-Item -Recurse -Force $stage }
New-Item -ItemType Directory -Force $stage | Out-Null

Get-ChildItem $bin -Recurse -File | Where-Object {
  $_.Name -notlike 'orbit_unit_tests*' -and $_.Extension -notin '.pdb', '.ilk', '.exp', '.lib'
} | ForEach-Object {
  $rel = [IO.Path]::GetRelativePath($bin, $_.FullName)
  $dest = Join-Path $stage $rel
  New-Item -ItemType Directory -Force (Split-Path $dest) | Out-Null
  Copy-Item $_.FullName $dest
}
Copy-Item (Join-Path $root 'third_party\cef\LICENSE.txt') (Join-Path $stage 'CEF-LICENSE.txt')
Copy-Item (Join-Path $root 'third_party\cef\CREDITS.html') (Join-Path $stage 'CREDITS.html')

# --- Import check: every imported DLL must be a system DLL or shipped here. ---
$vs = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath
$dumpbin = Get-ChildItem (Join-Path $vs 'VC\Tools\MSVC') -Recurse -Filter dumpbin.exe | Where-Object FullName -like '*Hostx64\x64*' | Select-Object -First 1
$shipped = (Get-ChildItem $stage -File).Name | ForEach-Object { $_.ToLower() }
$problems = @()
foreach ($file in 'orbit.exe', 'orbit.dll') {
  $imports = & $dumpbin.FullName /nologo /dependents (Join-Path $stage $file) |
             Where-Object { $_ -match '^\s+\S+\.(dll|drv)\s*$' } | ForEach-Object { $_.Trim().ToLower() }
  foreach ($dll in $imports) {
    $isSystem = (Test-Path (Join-Path $env:WINDIR "System32\$dll")) -or $dll.StartsWith('api-ms-win') -or $dll.StartsWith('ext-ms-')
    if (-not $isSystem -and $shipped -notcontains $dll) { $problems += "$file imports $dll (not shipped, not a system DLL)" }
    if ($dll -match '^(vcruntime|msvcp|ucrtbased|vcruntime140d)') { $problems += "$file depends on the VC++ runtime ($dll); static CRT expected" }
  }
}
if ($problems) { $problems | ForEach-Object { Write-Host "IMPORT PROBLEM: $_" }; throw 'import check failed' }
Write-Host 'Import check: only system DLLs and bundled files.'

# --- Smoke test from a path unrelated to the build tree. ---
$smoke = Join-Path ([IO.Path]::GetTempPath()) "orbit-smoke-$PID"
Copy-Item -Recurse $stage $smoke
$data = Join-Path $smoke 'data'
$p = Start-Process (Join-Path $smoke 'orbit.exe') -ArgumentList "--user-data-dir=`"$data`"", '--orbit-self-test=basic' -PassThru
if (-not $p.WaitForExit(120000)) { $p.Kill(); throw 'smoke test timed out' }
if ($p.ExitCode -ne 0) { Get-Content (Join-Path $data 'selftest-basic.txt') -ErrorAction SilentlyContinue; throw "smoke test failed ($($p.ExitCode))" }
Remove-Item -Recurse -Force $smoke
Write-Host 'Smoke test (relocated copy): passed.'

$zip = Join-Path $dist "Orbit-$version-win64.zip"
if (Test-Path $zip) { Remove-Item $zip }
Compress-Archive -Path $stage -DestinationPath $zip -CompressionLevel Optimal
$hash = (Get-FileHash $zip -Algorithm SHA256).Hash
Write-Host "Package: $zip"
Write-Host "SHA-256: $hash"
Write-Host 'Note: binaries are unsigned. Sign orbit.exe/orbit.dll (signtool) before distribution.'
