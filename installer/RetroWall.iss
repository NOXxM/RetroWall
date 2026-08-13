; RetroWall.iss — Inno Setup script for the RetroWall live-wallpaper engine.
; Produces RetroWallSetup.exe with a folder-selection page, Start Menu +
; optional desktop/startup shortcuts, and a clean uninstaller.
; Build:  ISCC.exe RetroWall.iss     (output lands next to this script)

#define MyAppName "RetroWall"
#define MyAppVersion "1.0.1"
#define MyAppPublisher "RetroWall"
#define MyAppExeName "RetroWall.exe"

[Setup]
; A stable AppId keeps upgrades/uninstall linked across versions.
AppId={{9F3B7C41-6E2A-4D8B-B1F5-7A0C9E2D4A10}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppPublisher={#MyAppPublisher}
; The user can change this to ANY folder on the "Select Destination" page.
DefaultDirName={autopf}\{#MyAppName}
DefaultGroupName={#MyAppName}
DisableProgramGroupPage=yes
AllowNoIcons=yes
; Per-user install by default (no UAC); user may elevate to install anywhere.
PrivilegesRequired=lowest
PrivilegesRequiredOverridesAllowed=dialog
OutputDir=.
OutputBaseFilename=RetroWallSetup
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
SetupIconFile=..\assets\app.ico
UninstallDisplayIcon={app}\{#MyAppExeName}

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "desktopicon"; Description: "Create a &desktop shortcut"; GroupDescription: "Additional icons:"
Name: "startupicon"; Description: "Start {#MyAppName} automatically when Windows starts"; GroupDescription: "Startup:"; Flags: unchecked

[Files]
; The built engine.
Source: "..\build\Release\RetroWall.exe"; DestDir: "{app}"; DestName: "{#MyAppExeName}"; Flags: ignoreversion
Source: "..\README.md"; DestDir: "{app}"; Flags: ignoreversion isreadme

[Icons]
Name: "{group}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"; Comment: "Start the live wallpaper"
Name: "{group}\{#MyAppName} Settings"; Filename: "{app}\{#MyAppExeName}"; Parameters: "--settings"; Comment: "Open the settings panel"
Name: "{group}\Uninstall {#MyAppName}"; Filename: "{uninstallexe}"
Name: "{autodesktop}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"; Tasks: desktopicon
Name: "{userstartup}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"; Tasks: startupicon

[Run]
Filename: "{app}\{#MyAppExeName}"; Description: "Launch {#MyAppName} now"; Flags: nowait postinstall skipifsilent

[UninstallRun]
; Make sure the running instance is stopped before files are removed.
Filename: "{cmd}"; Parameters: "/C taskkill /IM ""{#MyAppExeName}"" /F"; Flags: runhidden; RunOnceId: "StopRetroWall"
