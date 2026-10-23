#Requires -Version 7
<#
.SYNOPSIS
  Builds dist/Orbit-<version>-setup.exe (Inno Setup) from the folder that
  scripts/package.ps1 stages (dist/Orbit-<version>). Run package.ps1 first.
  Finds ISCC.exe, or installs Inno Setup with Chocolatey / winget if missing.
#>
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$version = (Select-String -Path (Join-Path $root 'CMakeLists.txt') -Pattern 'project\(orbit VERSION ([0-9.]+)').Matches[0].Groups[1].Value
# Marks which build this is in the file name (CI passes the commit).
$build = if ($env:ORBIT_BUILD_ID) { $env:ORBIT_BUILD_ID } else { 'local' }
$stage = Join-Path $root "dist\Orbit-$version"
if (-not (Test-Path (Join-Path $stage 'orbit.exe'))) { throw "Nothing staged at $stage; run scripts/package.ps1 first." }

function Find-Iscc {
  $cmd = Get-Command ISCC.exe -ErrorAction SilentlyContinue
  if ($cmd) { return $cmd.Source }
  foreach ($base in ${env:ProgramFiles(x86)}, $env:ProgramFiles, "$env:LOCALAPPDATA\Programs") {
    $p = Join-Path $base 'Inno Setup 6\ISCC.exe'
    if (Test-Path $p) { return $p }
  }
}
$iscc = Find-Iscc
if (-not $iscc -and (Get-Command choco -ErrorAction SilentlyContinue)) { choco install innosetup -y --no-progress | Out-Host; $iscc = Find-Iscc }
if (-not $iscc -and (Get-Command winget -ErrorAction SilentlyContinue)) {
  winget install --id JRSoftware.InnoSetup -e --accept-package-agreements --accept-source-agreements | Out-Host; $iscc = Find-Iscc
}
if (-not $iscc) { throw 'Inno Setup (ISCC.exe) was not found and could not be installed.' }

& $iscc "/DAppVersion=$version" "/DBuildId=$build" "/DStageDir=$stage" (Join-Path $root 'installer\orbit.iss')
if ($LASTEXITCODE) { throw "ISCC failed ($LASTEXITCODE)" }
$setup = Join-Path $root "dist\Orbit-$version-$build-setup.exe"
Write-Host "Installer: $setup"
Write-Host "SHA-256: $((Get-FileHash $setup -Algorithm SHA256).Hash)"
Write-Host 'Note: unsigned; Windows SmartScreen will warn until it is code-signed.'
