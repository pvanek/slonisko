; SPDX-FileCopyrightText: 2026 Petr Vanek
; SPDX-License-Identifier: GPL-3.0-or-later
;
; Inno Setup script for the folder build.ps1 assembles; build.ps1 runs it as
;   iscc /DVersion=x.y.z /DStageDir=<folder> /DSourceDir=<checkout> slonisko.iss

#ifndef Version
  #error Pass /DVersion=x.y.z
#endif

[Setup]
; Never change: Windows tells this program's installs apart by it.
AppId={{6F0E3B6A-2C41-4F7E-9B57-1D8E5C3A9F12}
AppName=Slonisko
AppVersion={#Version}
AppPublisher=Petr Vanek
AppPublisherURL=https://github.com/pvanek/slonisko
DefaultDirName={autopf}\Slonisko
DefaultGroupName=Slonisko
DisableProgramGroupPage=yes
LicenseFile={#StageDir}\LICENSE.txt
OutputBaseFilename=Slonisko-{#Version}-setup
SetupIconFile={#SourceDir}\src\app\icons\slonisko.ico
UninstallDisplayIcon={app}\bin\slonisko.exe
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
; Per user without asking for administrator rights, or for everyone.
PrivilegesRequired=lowest
PrivilegesRequiredOverridesAllowed=dialog
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern

[Tasks]
Name: desktopicon; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked

[Files]
Source: "{#StageDir}\*"; DestDir: "{app}"; Flags: recursesubdirs ignoreversion

[Icons]
Name: "{autoprograms}\Slonisko"; Filename: "{app}\bin\slonisko.exe"
Name: "{autodesktop}\Slonisko"; Filename: "{app}\bin\slonisko.exe"; Tasks: desktopicon

[Run]
Filename: "{app}\bin\slonisko.exe"; Description: "{cm:LaunchProgram,Slonisko}"; Flags: nowait postinstall skipifsilent
