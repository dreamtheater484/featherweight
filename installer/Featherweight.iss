; Featherweight installer (Inno Setup 6).
; Built by scripts\release.ps1:  ISCC.exe /DAppVersion=1.2.3 /DSourceDir=<staged files> installer\Featherweight.iss
;
; Zero friction: per-user install (no admin, no UAC), one "Install" click, and Featherweight opens when done.

#ifndef AppVersion
  #define AppVersion "0.0.0"
#endif
#ifndef SourceDir
  #define SourceDir "..\build\dist\Featherweight"
#endif
#ifndef OutputDir
  #define OutputDir "..\build\dist"
#endif

[Setup]
; AppId must never change: it identifies the installation for upgrades and uninstall
; (the uninstall key is "Featherweight_is1", which the app updates after self-updates).
AppId=Featherweight
AppName=Featherweight
AppVersion={#AppVersion}
AppVerName=Featherweight {#AppVersion}
AppPublisher=Featherweight
AppPublisherURL=https://github.com/dreamtheater484/featherweight
AppSupportURL=https://github.com/dreamtheater484/featherweight/issues
AppUpdatesURL=https://github.com/dreamtheater484/featherweight/releases
VersionInfoVersion={#AppVersion}
DefaultDirName={localappdata}\Programs\Featherweight
DisableDirPage=yes
DisableProgramGroupPage=yes
DisableWelcomePage=yes
DisableReadyMemo=yes
DisableFinishedPage=yes
PrivilegesRequired=lowest
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0.18362
WizardStyle=modern
WizardSizePercent=100
SetupIconFile=..\src\Featherweight.ico
UninstallDisplayIcon={app}\Featherweight.exe
UninstallDisplayName=Featherweight
OutputDir={#OutputDir}
OutputBaseFilename=Featherweight-{#AppVersion}-Setup
Compression=lzma2/ultra64
SolidCompression=yes
CloseApplications=yes
RestartApplications=no
ShowLanguageDialog=no

[Languages]
Name: "en"; MessagesFile: "compiler:Default.isl"

[Messages]
SetupWindowTitle=Install Featherweight
WizardReady=Ready to install
ReadyLabel1=Featherweight {#AppVersion} will be installed for you. No administrator rights are needed.
ReadyLabel2a=Click Install to continue.
FinishedHeadingLabel=Featherweight is installed

[Files]
Source: "{#SourceDir}\Featherweight.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#SourceDir}\pdfium.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#SourceDir}\LICENSE.txt"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#SourceDir}\THIRD_PARTY_NOTICES.md"; DestDir: "{app}"; Flags: ignoreversion

[Icons]
Name: "{autoprograms}\Featherweight"; Filename: "{app}\Featherweight.exe"; Comment: "A light, fast PDF reader"

[Registry]
; Tells Featherweight it is the installed copy (settings go to %APPDATA%\Featherweight).
Root: HKCU; Subkey: "Software\Featherweight"; ValueType: string; ValueName: "InstallDir"; ValueData: "{app}"; Flags: uninsdeletevalue uninsdeletekeyifempty

[Run]
Filename: "{app}\Featherweight.exe"; Parameters: "--register"; Flags: runhidden waituntilterminated
Filename: "{app}\Featherweight.exe"; Flags: nowait postinstall skipifsilent

[UninstallRun]
Filename: "{app}\Featherweight.exe"; Parameters: "--unregister"; Flags: runhidden waituntilterminated; RunOnceId: "Unregister"

[UninstallDelete]
Type: files; Name: "{app}\*.old"
Type: files; Name: "{app}\*.new"

[Code]
// Settings are kept on uninstall unless the user asks to remove them.
procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
var
  SettingsDir: String;
begin
  if CurUninstallStep = usPostUninstall then
  begin
    SettingsDir := ExpandConstant('{userappdata}\Featherweight');
    if DirExists(SettingsDir) and not UninstallSilent then
      if MsgBox('Also remove your Featherweight settings (recent files, window position)?',
                mbConfirmation, MB_YESNO or MB_DEFBUTTON2) = IDYES then
        DelTree(SettingsDir, True, True, True);
  end;
end;
