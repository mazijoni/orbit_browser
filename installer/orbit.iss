; Orbit setup. The wizard unpacks Orbit to a temporary folder and runs Orbit's
; own per-user installer there (orbit.exe --install --quiet): it copies the
; files to %LOCALAPPDATA%\Programs\Orbit, registers the browser, makes the
; Start menu shortcut and adds the Apps & features entry (with its uninstaller).
; Build with scripts/make_installer.ps1 (passes AppVersion and StageDir).

#ifndef AppVersion
  #define AppVersion "0.0.0"
#endif
#ifndef StageDir
  #define StageDir "..\dist\Orbit"
#endif

[Setup]
AppId={{6C0F1B7E-2A4D-4C8E-9B61-0A1B17B20F51}
AppName=Orbit Browser
AppVersion={#AppVersion}
AppPublisher=Orbit Project
DefaultDirName={autopf}\Orbit
DisableDirPage=yes
DisableProgramGroupPage=yes
DisableReadyPage=yes
PrivilegesRequired=lowest
Uninstallable=no
OutputDir=..\dist
OutputBaseFilename=Orbit Browser Setup
SetupIconFile=..\resources\orbit.ico
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible

[Files]
Source: "{#StageDir}\*"; DestDir: "{tmp}\orbit-payload"; Flags: recursesubdirs createallsubdirs ignoreversion deleteafterinstall

[Run]
; A running Orbit would keep its old files locked, so close it first.
Filename: "{sys}\taskkill.exe"; Parameters: "/F /IM orbit.exe"; Flags: runhidden waituntilterminated
Filename: "{tmp}\orbit-payload\orbit.exe"; Parameters: "--install --quiet"; StatusMsg: "Installing Orbit..."; Flags: waituntilterminated runhidden
Filename: "{localappdata}\Programs\Orbit\orbit.exe"; Description: "Start Orbit"; Flags: nowait postinstall skipifsilent
