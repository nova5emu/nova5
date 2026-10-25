; Nova5 installer (Inno Setup 6). build-package.ps1 passes Stage, Output and AppVersion.
#ifndef AppVersion
  #define AppVersion "0.1.0"
#endif

[Setup]
AppId={{6B1E2F7A-8C4D-4E59-9A3F-5D2C7B1E0A45}
AppName=Nova5
AppVersion={#AppVersion}
AppVerName=Nova5 {#AppVersion}
AppPublisher=Nova5
AppComments=Emulador de PS5 de código abierto basado en KytyPS5
; Per-user install by default: the emulator writes settings, caches and saves beside itself.
PrivilegesRequired=lowest
PrivilegesRequiredOverridesAllowed=dialog
DefaultDirName={autopf}\Nova5
DisableProgramGroupPage=yes
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0
OutputDir={#Output}
OutputBaseFilename=Nova5-Setup
SetupIconFile=..\Assets\nova5.ico
UninstallDisplayIcon={app}\Nova5.exe
UninstallDisplayName=Nova5
WizardStyle=modern
WizardImageFile=wizard-large.bmp
WizardSmallImageFile=wizard-small.bmp
LicenseFile={#Stage}\LICENSE.txt
InfoBeforeFile={#Stage}\LEEME.txt
Compression=lzma2/ultra64
SolidCompression=yes
; Updates started from the launcher run while Nova5 is closing; close whatever is still open.
CloseApplications=force
RestartApplications=no

[Languages]
Name: "spanish"; MessagesFile: "compiler:Languages\Spanish.isl"
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"

[Files]
Source: "{#Stage}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{autoprograms}\Nova5"; Filename: "{app}\Nova5.exe"
Name: "{autodesktop}\Nova5"; Filename: "{app}\Nova5.exe"; Tasks: desktopicon

[Run]
Filename: "{app}\Nova5.exe"; Description: "{cm:LaunchProgram,Nova5}"; Flags: nowait postinstall skipifsilent
; Silent installs come from the launcher's updater: open Nova5 again when they finish.
Filename: "{app}\Nova5.exe"; Flags: nowait; Check: WizardSilent
