#Requires -Version 7
<#
.SYNOPSIS
  Downloads and verifies Orbit's pinned third-party dependencies into
  third_party/. Safe to re-run; skips anything already present and verified.
  Keep the pins in sync with cmake/deps.cmake.
#>
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$tp = Join-Path $root 'third_party'
$dl = Join-Path $tp 'downloads'
New-Item -ItemType Directory -Force $dl | Out-Null

$cefVersion = '152.0.14+g8a0b5b9+chromium-152.0.7977.167'
$cefFile = "cef_binary_${cefVersion}_windows64_minimal.tar.bz2"
$cefSha1 = '0bad9c7d50f2fbf508bdf47e8f885590bb4d19ae'
$cefUrl = 'https://cef-builds.spotifycdn.com/' + [Uri]::EscapeDataString($cefFile)

$sqliteFile = 'sqlite-amalgamation-3540100.zip'
$sqliteSha3 = '92281c86e2957f9b9305890f9ac04204caa7b677cc868dc326d942689df28e30'
$sqliteUrl = "https://sqlite.org/2026/$sqliteFile"

function Get-Verified([string] $url, [string] $path, [scriptblock] $verify) {
  if ((Test-Path $path) -and (& $verify $path)) { return }
  Write-Host "Downloading $url"
  Invoke-WebRequest -Uri $url -OutFile $path -UseBasicParsing
  if (-not (& $verify $path)) { Remove-Item $path; throw "Checksum mismatch for $path" }
}

# --- CEF ---
$cefArchive = Join-Path $dl 'cef_minimal.tar.bz2'
Get-Verified $cefUrl $cefArchive { param($p) (Get-FileHash $p -Algorithm SHA1).Hash -eq $cefSha1 }
$cefDir = Join-Path $tp 'cef'
$versionHeader = Join-Path $cefDir 'include\cef_version.h'
if (-not ((Test-Path $versionHeader) -and (Select-String -Path $versionHeader -SimpleMatch "`"$cefVersion`"" -Quiet))) {
  if (Test-Path $cefDir) { Remove-Item -Recurse -Force $cefDir }
  Write-Host 'Extracting CEF...'
  tar -xjf $cefArchive -C $tp
  if ($LASTEXITCODE) { throw 'CEF extraction failed' }
  Move-Item (Get-ChildItem $tp -Directory -Filter 'cef_binary_*' | Select-Object -First 1).FullName $cefDir
}

# --- SQLite (SHA3-256 is what sqlite.org publishes; verified with Python's hashlib) ---
$sqliteArchive = Join-Path $dl 'sqlite.zip'
Get-Verified $sqliteUrl $sqliteArchive {
  param($p)
  $h = python -c "import hashlib,sys;print(hashlib.sha3_256(open(sys.argv[1],'rb').read()).hexdigest())" $p
  $h.Trim() -eq $sqliteSha3
}
$sqliteDir = Join-Path $tp 'sqlite'
if (-not (Test-Path (Join-Path $sqliteDir 'sqlite3.c'))) {
  Expand-Archive -Force $sqliteArchive $dl
  if (Test-Path $sqliteDir) { Remove-Item -Recurse -Force $sqliteDir }
  Move-Item (Join-Path $dl 'sqlite-amalgamation-3540100') $sqliteDir
}
Write-Host 'Dependencies ready.'
