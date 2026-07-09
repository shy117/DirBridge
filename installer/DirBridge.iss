#define AppName "DirBridge"

#ifndef AppVersion
#define AppVersion "0.5.8"
#endif

#ifndef ReleaseDir
#error ReleaseDir must be passed by scripts/package_release.ps1
#endif

#ifndef OutputDir
#define OutputDir "..\build\release"
#endif

[Setup]
AppId={{4A332665-5B4C-4F0D-AC5F-4E5662F081F6}
AppName={#AppName}
AppVersion={#AppVersion}
AppPublisher=DirBridge contributors
DefaultDirName={autopf}\DirBridge
DefaultGroupName=DirBridge
DisableProgramGroupPage=yes
OutputDir={#OutputDir}
OutputBaseFilename=DirBridge-v{#AppVersion}-win64-setup
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
SetupIconFile=..\resources\icons\app\dirbridge.ico
UninstallDisplayIcon={app}\DirBridge.exe
LicenseFile=..\LICENSE

[Languages]
Name: "chinesesimp"; MessagesFile: "compiler:Languages\ChineseSimplified.isl"
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked

[Files]
Source: "{#ReleaseDir}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{group}\DirBridge"; Filename: "{app}\DirBridge.exe"
Name: "{autodesktop}\DirBridge"; Filename: "{app}\DirBridge.exe"; Tasks: desktopicon

[Run]
Filename: "{app}\DirBridge.exe"; Description: "{cm:LaunchProgram,DirBridge}"; Flags: nowait postinstall skipifsilent
