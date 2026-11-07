; NovaS installer (Inno Setup 6): the Switch 2 emulator on its own. build-novas-package.ps1 passes Stage, Output and
; AppVersion.
#ifndef AppVersion
  #define AppVersion "0.1.0"
#endif

[Setup]
AppId={{3F8A2C61-5D7B-4E0A-9B14-C26E7D9A51F3}
AppName=NovaS
AppVersion={#AppVersion}
AppVerName=NovaS {#AppVersion}
AppPublisher=Nova5
AppComments=Emulador de Switch 2 de código abierto
; Per-user install by default: the emulator writes settings, caches and saves beside itself.
PrivilegesRequired=lowest
PrivilegesRequiredOverridesAllowed=dialog
DefaultDirName={autopf}\NovaS
DisableProgramGroupPage=yes
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0
OutputDir={#Output}
OutputBaseFilename=NovaS-Setup
SetupIconFile=..\Assets\novas.ico
UninstallDisplayIcon={app}\NovaS.exe
UninstallDisplayName=NovaS
WizardStyle=modern
WizardImageFile=novas-wizard-large.bmp
WizardSmallImageFile=novas-wizard-small.bmp
LicenseFile={#Stage}\LICENSE.txt
InfoBeforeFile={#Stage}\LEEME.txt
Compression=lzma2/ultra64
SolidCompression=yes
; Updates started from NovaS run while it is closing; close whatever is still open.
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
Name: "{autoprograms}\NovaS"; Filename: "{app}\NovaS.exe"
Name: "{autodesktop}\NovaS"; Filename: "{app}\NovaS.exe"; Tasks: desktopicon

[Run]
Filename: "{app}\NovaS.exe"; Description: "{cm:LaunchProgram,NovaS}"; Flags: nowait postinstall skipifsilent
; Silent installs come from NovaS's updater: open it again when they finish.
Filename: "{app}\NovaS.exe"; Flags: nowait; Check: WizardSilent
