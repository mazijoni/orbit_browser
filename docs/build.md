# Building Orbit

## Requirements

| Tool | Tested version | Notes |
|---|---|---|
| Windows | 11 x64 (10.0.26200) | |
| Visual Studio 2022 or Build Tools 2022 | 17.14, MSVC 14.44 | Workload: *Desktop development with C++* (x64 toolset, Windows 11 SDK) |
| Windows SDK | 10.0.26100.0 | |
| CMake, Ninja | 3.31.6, 1.12.1 | The copies bundled with Visual Studio are used automatically |
| PowerShell | 7.x (`pwsh`) | Build/test scripts |
| Python | 3.x | Only used to verify the SQLite SHA3-256 checksum and to regenerate the icon (Pillow) |
| Disk | ~3 GB | CEF binaries plus Debug/Release outputs |

No administrator rights are required.

## Steps

```powershell
pwsh ./scripts/fetch_deps.ps1     # downloads + verifies CEF 154.0.34 (minimal, H.264/AAC build) and SQLite 3.54.1
pwsh ./scripts/build.ps1          # Debug build + unit tests       -> out/debug/bin/orbit.exe
pwsh ./scripts/build.ps1 -Config Release
pwsh ./scripts/run_selftests.ps1  # in-browser integration suites (launches the real browser)
```

`out/<config>/bin` is a complete, relocatable layout:

* `orbit.exe` is CEF's `bootstrap.exe`, renamed. It creates the sandbox and loads `orbit.dll`.
* `orbit.dll` contains all of Orbit's code.
* `libcef.dll` and the other CEF runtime files sit beside them.

## Running against a scratch profile

```powershell
out/debug/bin/orbit.exe --user-data-dir=C:\temp\orbit-test https://example.com
```

Useful switches:

* `--user-data-dir=<dir>`: the profile root. Default: `%LOCALAPPDATA%\Orbit\User Data`.
* `--orbit-verbose-log`: verbose application log (`<user-data>\orbit.log`). Chromium's own log is `chromium.log`.
* `--orbit-self-test=<suite>`: run an integration suite. This requires `--user-data-dir` and never touches a real profile.

## Notes and pitfalls

* **OneDrive.** This checkout lives under OneDrive. Build outputs (`out/`) and `third_party/` are large and change often, which causes needless syncing and occasional file locks during linking. Excluding them from sync (or moving the checkout) is recommended.
* **Profile directories** must be direct children of the user-data root (`User Data\Profile-<uuid>`). Chromium rejects deeper paths and *silently* falls back to an in-memory profile. Orbit logs an error if this ever happens, and the `isolation` self-test checks for it.
* **Debug builds** link the Release `libcef.dll` (the minimal distribution has no Debug binaries). This is supported because libcef exposes a C ABI.
* **Code style:** C++20, `/W4 /WX`, no exceptions, no RTTI. These match CEF's own flags.

## Video codecs and licensing

Orbit uses a third-party CEF build (github.com/psmux/godwinmix-cef, release `cef-154.0.34`) compiled with `proprietary_codecs=true ffmpeg_branding=Chrome`, so H.264 and AAC play (X/Twitter, news sites, HLS via MSE players). The archive is pinned by SHA-256 in `scripts/fetch_deps.ps1`. The checksum only proves the download is the file we pinned, not who built it; to remove that trust, build CEF yourself with `automate-git.py` and those GN flags and point `fetch_deps.ps1` at your archive. The official build (Spotify CDN, 152.0.14, SHA-1 `0bad9c7d50f2fbf508bdf47e8f885590bb4d19ae`) has no H.264/AAC.

H.264/AAC are covered by patent pools; the build comes with no patent licence. Whoever distributes Orbit is responsible for licensing. Widevine DRM (Netflix, Disney+) is not included and is unsupported.

## Extensions test fixture

`tests/fixtures/extension_basic` is an unpacked Manifest V3 extension (a content script and an action popup). `scripts/run_selftests.ps1` loads it for the `extensions` suite with `--orbit-test-extension=<dir>`, which `OrbitApp::OnBeforeCommandLineProcessing` turns into Chromium's `--load-extension` (self-test runs only).

## Installer (Windows setup program)

`pwsh ./scripts/package.ps1` builds Release, stages `dist/Orbit-<version>` and zips it; `pwsh ./scripts/make_installer.ps1` then compiles `installer/orbit.iss` (Inno Setup, installed with Chocolatey or winget if missing) into `dist/Orbit-<version>-setup.exe`. The setup is per user (no administrator rights): it unpacks to a temporary folder and runs Orbit's own `orbit.exe --install --quiet`, which copies Orbit to `%LOCALAPPDATA%\Programs\Orbit`, registers it as a browser, makes the Start menu shortcut and the Apps & features entry (uninstall there, or `orbit.exe --uninstall`). User data is never touched by install.

GitHub Actions (`.github/workflows/installer.yml`) does this on every push and uploads the setup as the `Orbit-setup` artifact of the run; pushing a tag such as `v0.1.0` attaches it (and the zip) to a GitHub release. The binaries are unsigned, so SmartScreen warns until they are code-signed.
