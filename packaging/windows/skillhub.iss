; Inno Setup script. Built by CI:  iscc /DAppVersion=1.0.0 /DSourceDir=dist\SkillHub packaging\windows\skillhub.iss
#ifndef AppVersion
  #define AppVersion "0.0.0"
#endif
#ifndef SourceDir
  #define SourceDir "..\..\dist\SkillHub"
#endif

[Setup]
CloseApplications=yes
AppId={{B3A2C4E8-5D61-4F0B-9A57-2C8E1D7F6A90}
AppName=Skill Hub
AppVersion={#AppVersion}
AppPublisher=nimonht
AppPublisherURL=https://github.com/nimonht/skillhub
AppSupportURL=https://github.com/nimonht/skillhub/issues
DefaultDirName={autopf}\Skill Hub
DefaultGroupName=Skill Hub
DisableProgramGroupPage=yes
UninstallDisplayIcon={app}\skillhub.exe
OutputDir=..\..\dist
OutputBaseFilename=SkillHub-{#AppVersion}-windows-x64-setup
SetupIconFile=..\..\branding\favicon.ico
LicenseFile=..\..\LICENSE
Compression=lzma2
SolidCompression=yes
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
; per-user by default (no administrator needed); the wizard offers "for all users"
PrivilegesRequired=lowest
PrivilegesRequiredOverridesAllowed=dialog
WizardStyle=modern
ChangesEnvironment=yes

[Tasks]
Name: "desktopicon"; Description: "Create a desktop shortcut"; Flags: unchecked
Name: "addtopath"; Description: "Add the skillhub-cli command to PATH (for terminals and scripts)"

[Files]
Source: "{#SourceDir}\*"; DestDir: "{app}"; Flags: recursesubdirs ignoreversion

[Icons]
Name: "{autoprograms}\Skill Hub"; Filename: "{app}\skillhub.exe"
Name: "{autodesktop}\Skill Hub"; Filename: "{app}\skillhub.exe"; Tasks: desktopicon

[Run]
Filename: "{app}\skillhub.exe"; Description: "Start Skill Hub"; Flags: nowait postinstall skipifsilent

[UninstallRun]
Filename: "schtasks.exe"; Parameters: "/End /TN ""SkillHub\Stray watcher"""; Flags: runhidden; RunOnceId: "EndWatcher"
Filename: "schtasks.exe"; Parameters: "/Delete /TN ""SkillHub\Stray watcher"" /F"; Flags: runhidden; RunOnceId: "DeleteWatcher"
Filename: "schtasks.exe"; Parameters: "/Delete /TN ""SkillHub\Daily update"" /F"; Flags: runhidden; RunOnceId: "DeleteUpdate"
Filename: "taskkill.exe"; Parameters: "/F /IM skillhubw.exe"; Flags: runhidden; RunOnceId: "StopBackend"

[Code]
const
  PathKeyUser = 'Environment';
  PathKeyMachine = 'SYSTEM\CurrentControlSet\Control\Session Manager\Environment';

function PathRoot(): Integer;
begin
  if IsAdminInstallMode then Result := HKEY_LOCAL_MACHINE else Result := HKEY_CURRENT_USER;
end;

function PathKey(): String;
begin
  if IsAdminInstallMode then Result := PathKeyMachine else Result := PathKeyUser;
end;

procedure AddToPath();
var
  Paths: String;
begin
  if not RegQueryStringValue(PathRoot(), PathKey(), 'Path', Paths) then Paths := '';
  if Pos(';' + Uppercase(ExpandConstant('{app}')) + ';', ';' + Uppercase(Paths) + ';') > 0 then exit;
  if (Paths <> '') and (Copy(Paths, Length(Paths), 1) <> ';') then Paths := Paths + ';';
  RegWriteExpandStringValue(PathRoot(), PathKey(), 'Path', Paths + ExpandConstant('{app}'));
end;

procedure RemoveFromPath();
var
  Paths, App: String;
  P: Integer;
begin
  if not RegQueryStringValue(PathRoot(), PathKey(), 'Path', Paths) then exit;
  App := ExpandConstant('{app}');
  P := Pos(';' + Uppercase(App) + ';', ';' + Uppercase(Paths) + ';');
  if P = 0 then exit;
  Delete(Paths, P, Length(App) + 1);
  RegWriteExpandStringValue(PathRoot(), PathKey(), 'Path', Paths);
end;

procedure CurStepChanged(CurStep: TSetupStep);
begin
  if (CurStep = ssPostInstall) and WizardIsTaskSelected('addtopath') then AddToPath();
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
begin
  if CurUninstallStep = usPostUninstall then RemoveFromPath();
end;
