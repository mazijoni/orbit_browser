#Requires -Version 7
<#
.SYNOPSIS
  Configures and builds Orbit with MSVC + Ninja.
.EXAMPLE
  ./scripts/build.ps1                 # Debug build + unit tests
  ./scripts/build.ps1 -Config Release
#>
param(
  [ValidateSet('Debug', 'Release')] [string] $Config = 'Debug',
  [switch] $NoTests,
  [switch] $Clean
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot

# Locate Visual Studio (any edition / Build Tools) with the x64 C++ toolset.
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vs) { throw 'Visual Studio 2022 with the C++ x64 toolset was not found.' }

# Import the x64 developer environment into this PowerShell session.
$env:PATH = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer;$env:PATH"
$vcvars = Join-Path $vs 'VC\Auxiliary\Build\vcvars64.bat'
cmd /c "`"$vcvars`" >nul && set" | ForEach-Object {
  if ($_ -match '^([^=]+)=(.*)$') { Set-Item -Path "env:$($matches[1])" -Value $matches[2] }
}
$cmakeDir = Join-Path $vs 'Common7\IDE\CommonExtensions\Microsoft\CMake'
$env:PATH = "$cmakeDir\CMake\bin;$cmakeDir\Ninja;$env:PATH"

Set-Location $root
$preset = $Config.ToLower()
$buildDir = Join-Path $root "out\$preset"
if ($Clean -and (Test-Path $buildDir)) { Remove-Item -Recurse -Force $buildDir }

cmake --preset $preset
if ($LASTEXITCODE) { throw "configure failed ($LASTEXITCODE)" }
cmake --build --preset $preset
if ($LASTEXITCODE) { throw "build failed ($LASTEXITCODE)" }
if (-not $NoTests) {
  ctest --preset $preset
  if ($LASTEXITCODE) { throw "tests failed ($LASTEXITCODE)" }
}
Write-Host "Built: $buildDir\bin\orbit.exe"
