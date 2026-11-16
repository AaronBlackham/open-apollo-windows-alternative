; ApolloSetup.iss: test-mode installer for the Apollo open driver (apollo.sys) on the Apollo x8p and
; the Apollo Twin X Gen 1. Compile with build.cmd (it stages the payload and passes these defines):
;   Payload    absolute path of the staged payload (installed 1:1 into {app})
;   DriverVer  the package's DriverVer version, e.g. 12.36.20.8
;   OutDir     output folder
; The payload is UNSIGNED: no file in it carries anyone's signature. Setup signs apollo.sys and
; apollo.cat on the user's PC with a certificate made there (sign-local.ps1), whose private key is
; deleted straight after signing. Setup and the helper are unsigned too.
; Every elevated step runs a binary or script from {app} (Program Files, admin-only) or from
; System32. Nothing is executed from {tmp} or any other user-writable folder.

#ifndef Payload
  #error Payload is not defined: build with build.cmd
#endif
#ifndef DriverVer
  #error DriverVer is not defined: build with build.cmd
#endif
#ifndef OutDir
  #define OutDir "out"
#endif

#define AppName "Apollo open driver"

[Setup]
AppId={{4C8B501E-BA5A-4B5A-A7B9-186B4395E230}
AppName={#AppName}
AppVersion={#DriverVer}
AppVerName={#AppName} {#DriverVer} (test build)
AppPublisher=Avium
AppComments=Test-mode driver for the Universal Audio Apollo x8p and Apollo Twin X Gen 1 (Thunderbolt). Needs Windows test mode.
VersionInfoVersion={#DriverVer}
VersionInfoCompany=Avium
VersionInfoDescription=Apollo open driver setup (test build)
VersionInfoProductName={#AppName}
VersionInfoProductVersion={#DriverVer}
DefaultDirName={autopf}\Avium\Apollo
DisableDirPage=yes
DefaultGroupName=Avium Apollo
DisableProgramGroupPage=yes
DisableWelcomePage=no
PrivilegesRequired=admin
ArchitecturesAllowed=x64os
ArchitecturesInstallIn64BitMode=x64os
MinVersion=10.0.22621
OutputDir={#OutDir}
OutputBaseFilename=ApolloSetup-{#DriverVer}
OutputManifestFile=ApolloSetup-{#DriverVer}-files.txt
SetupIconFile={#SourcePath}..\app\res\app.ico
UninstallDisplayIcon={app}\apollo-control.exe
UninstallDisplayName={#AppName} {#DriverVer} (test build)
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern
WizardSizePercent=120
SetupLogging=yes
UninstallLogging=yes
CloseApplications=yes
RestartApplications=no
UsedUserAreasWarning=no

[Messages]
WindowsVersionNotSupported=The Apollo open driver needs 64-bit Windows 11, version 22H2 (build 22621) or later, on an x64 PC.
WelcomeLabel2=This installs the Apollo open driver {#DriverVer}, a TEST build of an open-source driver for the Universal Audio Apollo x8p and Apollo Twin X Gen 1 (Thunderbolt): Windows sound devices, an ASIO driver and the Apollo Control app.%n%nThe driver is not signed by Microsoft. Setup signs it on this PC with a certificate made for this PC only, and Windows loads it only in test mode, with Secure Boot off. Setup explains each system change before making it.%n%nYou choose which of your units move to this driver. Gen 2 units and every other Universal Audio device stay on UA's driver.%n%nBefore you continue, close programs that use your Apollo: DAWs, music players, Apollo Control.
FinishedLabelNoIcons=Setup has finished installing [name].%n%nUAD plug-ins are optional: "Set up UAD plug-ins" in Apollo Control (the plug-in browser, or Settings) downloads a private Python and its packages (about 42 MB from python.org and PyPI, each file checked against a pinned SHA-256) and maps the plug-ins of your own UAD install, read offline. Nothing is sent to Universal Audio.%n%nNot in this release:%n- Anything made by Universal Audio. The mic preamp input strips and the plug-ins use modules copied from your own UAD install, on this PC.%n- Gen 2 units and other models: they stay on UA's driver.
FinishedLabel=Setup has finished installing [name]. Start Apollo Control from the Start menu (Avium Apollo).%n%nUAD plug-ins are optional: "Set up UAD plug-ins" in the Start menu (or in Apollo Control: the plug-in browser, or Settings) downloads a private Python and its packages (about 42 MB from python.org and PyPI, each file checked against a pinned SHA-256) and maps the plug-ins of your own UAD install, read offline. Nothing is sent to Universal Audio.%n%nNot in this release:%n- Anything made by Universal Audio. The mic preamp input strips and the plug-ins use modules copied from your own UAD install, on this PC.%n- Gen 2 units and other models: they stay on UA's driver.
FinishedRestartLabel=Windows must restart to finish (test mode, or a unit's driver switch, takes effect then). Restart now?%n%nUAD plug-ins are optional: "Set up UAD plug-ins" in the Start menu or in Apollo Control maps the plug-ins of your own UAD install (it downloads a private Python, about 42 MB). Not in this release: anything made by Universal Audio (the mic preamp strips and the plug-ins use modules copied from your own UAD install); Gen 2 units and other models (they stay on UA's driver).

[Tasks]
Name: autostart; Description: "Start Apollo Control when I sign in (it sits in the notification area)"; Flags: unchecked
; [PlugSetup] opens "Set up UAD plug-ins" (plugin-setup\tools\setup-plugins.ps1) as you, in a window, when Setup is done
Name: setupplugins; Description: "Set up UAD plug-in hosting after install (a window explains it first: about 42 MB of downloads from python.org and PyPI, then your UAD plug-ins are mapped, read offline)"; Flags: unchecked

[Dirs]
Name: "{app}\state"

[Files]
; The staged payload, as is: build.cmd wrote its SHA-256 manifest (payload-manifest.sha256, also installed).
; driver\apollo.sys and driver\apollo.cat arrive unsigned and are signed on this PC after the copy.
Source: "{#Payload}\*"; DestDir: "{app}"; Flags: recursesubdirs ignoreversion

[InstallDelete]
; The shortcut's name before Nov 16 2026 (x8p only).
Type: files; Name: "{group}\Connect x8p to Apollo driver.lnk"

[Icons]
Name: "{group}\Apollo Control"; Filename: "{app}\apollo-control.exe"; WorkingDir: "{app}"; Comment: "Mixer, monitor, cues, preamps and outputs of your Apollo units"
Name: "{group}\Connect Apollo units"; Filename: "{app}\apollo-helper.exe"; Parameters: "connect"; WorkingDir: "{app}"; IconFilename: "{app}\apollo-control.exe"; Comment: "Switch a connected Apollo x8p or Twin X Gen 1 from UA's driver to the Apollo driver (asks for administrator rights)"
Name: "{group}\Switch Apollo units back to UA's driver"; Filename: "{app}\apollo-helper.exe"; Parameters: "disconnect"; WorkingDir: "{app}"; IconFilename: "{app}\apollo-control.exe"; Comment: "Use Universal Audio's own driver again (for UA's firmware updates or UA Console); the Apollo driver stays installed, Connect Apollo units switches back (asks for administrator rights)"
Name: "{group}\Uninstall Apollo driver"; Filename: "{uninstallexe}"; Comment: "Put your Apollo units back on UA's driver and remove the Apollo driver"
; [PlugSetup] the same setup Apollo Control runs, in a window: it explains, asks, shows progress; runs as you (no elevation
; except one prompt for the driver's module store while plug-in hosting is on)
Name: "{group}\Set up UAD plug-ins"; Filename: "{sys}\WindowsPowerShell\v1.0\powershell.exe"; Parameters: "-NoProfile -ExecutionPolicy Bypass -File ""{app}\plugin-setup\tools\setup-plugins.ps1"""; WorkingDir: "{app}"; IconFilename: "{app}\apollo-control.exe"; Comment: "Map the UAD plug-ins of your own UAD install for Apollo Control (downloads a private Python, about 42 MB; never contacts Universal Audio)"

[Registry]
; The same value Apollo Control's own "start with Windows" switch writes, so the switch in the app shows it.
Root: HKCU; Subkey: "Software\Microsoft\Windows\CurrentVersion\Run"; ValueType: string; ValueName: "Avium Apollo Control"; ValueData: """{app}\apollo-control.exe"" --minimized"; Tasks: autostart; Flags: uninsdeletevalue

[Run]
Filename: "{app}\apollo-control.exe"; Description: "Open Apollo Control"; Flags: postinstall nowait skipifsilent runasoriginaluser unchecked; Check: not RestartPending

[UninstallDelete]
Type: filesandordirs; Name: "{app}\state"
; Strip modules apollo-diag copied from the UAD install (UA's bytes: they leave with the driver).
Type: filesandordirs; Name: "{sys}\drivers\ApolloStrips"
; [PlugSetup] Plug-in modules Apollo Control copied from the user's maps (UA's bytes too). The maps themselves stay in each
; user's %LOCALAPPDATA%\Avium\ApolloPlugins (installer\README.md says how to remove them).
Type: filesandordirs; Name: "{sys}\drivers\ApolloModules"
Type: dirifempty; Name: "{app}"

[Code]
const
  HELPER_OK = 0;
  HELPER_NO_DEVICE = 2;
  HELPER_BUSY = 3;
  HELPER_NOT_ELEVATED = 4;
  HELPER_BLOCKED = 5;           { stopped before changing anything: another Apollo unit is in the way }
  HELPER_REBOOT = 3010;
  RUN_KEY = 'Software\Microsoft\Windows\CurrentVersion\Run';
  RUN_VALUE = 'Avium Apollo Control';
  X8P_ID = 'PCI\VEN_1A00&DEV_0002&SUBSYS_00141A00';
  TWINX_ID = 'PCI\VEN_1A00&DEV_0002&SUBSYS_00191A00';
  UA_PREFIX = 'PCI\VEN_1A00&DEV_0002&';

var
  TestSignPage: TInputOptionWizardPage;
  BitLockerPage: TInputOptionWizardPage;
  SigningPage: TOutputMsgWizardPage;
  UnitsPage: TInputOptionWizardPage;
  ResultsPage: TOutputMsgMemoWizardPage;
  SandboxTest: Boolean;         { /SANDBOXTEST: layout/UI/uninstall check in Windows Sandbox }
  TestSigningOn: Boolean;       { active for this boot }
  BitLockerState: Integer;      { 1 on, 0 off, -1 could not tell }
  TurnedTestSigningOn: Boolean;
  RestartNeeded: Boolean;
  Summary: String;
  LastOutput: String;
  { Universal Audio PCI devices found when Setup started (Win32_PnPEntity) }
  X8pCount, TwinCount: Integer;
  X8pService, TwinService: String;
  OtherUa: String;              { lines about devices this driver does not support }
  { UnitsPage item -> 'x8p' / 'twinx' }
  ItemKind: array of String;

{ ------------------------------------------------------------------ }
{ system state, read natively: no program runs before the files are in Program Files }
{ ------------------------------------------------------------------ }

function IsSecureBootOn: Boolean;
var
  V: Cardinal;
begin
  Result := RegQueryDWordValue(HKLM64, 'SYSTEM\CurrentControlSet\Control\SecureBoot\State',
                               'UEFISecureBootEnabled', V) and (V <> 0);
end;

function IsTestSigningActive: Boolean;
var
  S: String;
begin
  Result := False;
  if RegQueryStringValue(HKLM64, 'SYSTEM\CurrentControlSet\Control', 'SystemStartOptions', S) then
    Result := Pos(' TESTSIGNING ', ' ' + Uppercase(S) + ' ') > 0;
end;

{ 1 = BitLocker protects the Windows drive, 0 = it does not, -1 = could not tell }
function BitLockerStatus: Integer;
var
  Locator, Service, Volumes, Vol, Letter, Status: Variant;
  Drive, S: String;
  I: Integer;
begin
  Result := -1;
  Drive := Uppercase(Copy(ExpandConstant('{win}'), 1, 2));
  try
    Locator := CreateOleObject('WbemScripting.SWbemLocator');
    Service := Locator.ConnectServer('.', 'root\CIMV2\Security\MicrosoftVolumeEncryption');
    Volumes := Service.ExecQuery('SELECT DriveLetter, ProtectionStatus FROM Win32_EncryptableVolume');
    Result := 0;
    for I := 0 to Volumes.Count - 1 do
    begin
      Vol := Volumes.ItemIndex(I);
      Letter := Vol.DriveLetter;
      S := '';
      if not VarIsNull(Letter) then
        S := Letter;
      if Uppercase(S) = Drive then
      begin
        Status := Vol.ProtectionStatus;
        if VarIsNull(Status) then
          Result := -1
        else if Status = 1 then
          Result := 1
        else if Status = 0 then
          Result := 0
        else
          Result := -1;
      end;
    end;
  except
    Log('BitLocker status: ' + GetExceptionMessage);
  end;
  Log('BitLocker status of ' + Drive + ': ' + IntToStr(Result));
end;

{ Every connected Universal Audio PCI device (Win32_PnPEntity lists present devices), by its
  exact four-part hardware ID: the x8p, the Twin X Gen 1, or a device this driver leaves alone. }
procedure DetectUnits;
var
  Locator, Service, Items, Item, V: Variant;
  I: Integer;
  Id, Svc: String;
begin
  X8pCount := 0;
  TwinCount := 0;
  X8pService := '';
  TwinService := '';
  OtherUa := '';
  try
    Locator := CreateOleObject('WbemScripting.SWbemLocator');
    Service := Locator.ConnectServer('.', 'root\CIMV2');
    Items := Service.ExecQuery('SELECT DeviceID, Service FROM Win32_PnPEntity WHERE DeviceID LIKE ''PCI\\VEN_1A00&DEV_0002&%''');
    for I := 0 to Items.Count - 1 do
    begin
      Item := Items.ItemIndex(I);
      Id := '';
      Svc := '';
      V := Item.DeviceID;
      if not VarIsNull(V) then
      begin
        Id := V;
        Id := Uppercase(Id);
      end;
      V := Item.Service;
      if not VarIsNull(V) then
        Svc := V;
      if Pos(UA_PREFIX, Id) <> 1 then
        continue;
      Log('Universal Audio device: ' + Id + ' (service ' + Svc + ')');
      if Pos(X8P_ID + '&', Id) = 1 then
      begin
        X8pCount := X8pCount + 1;
        X8pService := Svc;
      end else if Pos(TWINX_ID + '&', Id) = 1 then
      begin
        TwinCount := TwinCount + 1;
        TwinService := Svc;
      end else
        OtherUa := OtherUa + '- ' + Copy(Id, Length(UA_PREFIX) + 1, 15) + ': not supported by this driver, stays on its own driver.' + #13#10;
    end;
  except
    Log('Unit detection: ' + GetExceptionMessage);
  end;
  Log('Units found: x8p ' + IntToStr(X8pCount) + ', Twin X ' + IntToStr(TwinCount));
end;

{ /FAKEUNITS=x8p=UAD2Pcie,twinx=apollo,other=000F1A00 (x8p2= for two x8ps): with /SANDBOXTEST only, pretend
  these units are connected, so the units page and the ready list can be checked where no unit exists.
  Nothing is switched in a sandbox test anyway. }
function FakeValue(const P, Key: String; var Value: String): Boolean;
var
  I, J: Integer;
  S: String;
begin
  S := ',' + P + ',';
  I := Pos(',' + Key + '=', S);
  Result := I > 0;
  if not Result then
    exit;
  S := Copy(S, I + Length(Key) + 2, Length(S));
  J := Pos(',', S);
  Value := Copy(S, 1, J - 1);
end;

procedure FakeUnits;
var
  P, V: String;
begin
  P := ExpandConstant('{param:FAKEUNITS|}');
  if (not SandboxTest) or (P = '') then
    exit;
  Log('/FAKEUNITS=' + P + ' (sandbox test: pretend units)');
  if FakeValue(P, 'x8p', V) then
  begin
    X8pCount := 1;
    X8pService := V;
  end;
  if FakeValue(P, 'x8p2', V) then
  begin
    X8pCount := 2;
    X8pService := V;
  end;
  if FakeValue(P, 'twinx', V) then
  begin
    TwinCount := 1;
    TwinService := V;
  end;
  if FakeValue(P, 'other', V) then
    OtherUa := OtherUa + '- SUBSYS_' + V + ': not supported by this driver, stays on its own driver.' + #13#10;
end;

function ServiceText(const Svc: String): String;
begin
  if CompareText(Svc, 'apollo') = 0 then
    Result := 'already on the Apollo driver: it is updated to this version'
  else if CompareText(Svc, 'UAD2Pcie') = 0 then
    Result := 'now on Universal Audio''s driver'
  else if Svc = '' then
    Result := 'now without a driver'
  else
    Result := 'now on the driver "' + Svc + '"';
end;

function UadInstalled: Boolean;
begin
  Result := DirExists(ExpandConstant('{commonpf64}\Universal Audio\Powered Plugins')) or
            DirExists(ExpandConstant('{commonpf32}\Universal Audio\Powered Plugins')) or
            FileExists(ExpandConstant('{sysnative}\UAD2SDK.dll'));
end;

function RestartPending: Boolean;
begin
  Result := RestartNeeded;
end;

{ A 64-bit System32 tool, for Exec: in 64-bit install mode Exec turns WOW64 file system
  redirection off, so the real System32 path is the right one (not the Sysnative alias). }
function Sys64(const Name: String): String;
begin
  Result := ExpandConstant('{win}\System32\') + Name;
end;

{ ------------------------------------------------------------------ }
{ running tools                                                       }
{ ------------------------------------------------------------------ }

procedure SetStatus(const S: String);
begin
  Log('== ' + S);
  if IsUninstaller then
    UninstallProgressForm.StatusLabel.Caption := S
  else
    WizardForm.StatusLabel.Caption := S;
end;

procedure AddSummary(const S: String);
begin
  Log('summary: ' + S);
  Summary := Summary + '- ' + S + #13#10#13#10;
end;

procedure CaptureLine(const S: String; const Error, FirstLine: Boolean);
begin
  if Error then
    Log('  | (output error) ' + S)
  else begin
    Log('  | ' + S);
    LastOutput := LastOutput + S + #13#10;
  end;
end;

{ Runs Exe hidden, logs its output line by line. Returns the exit code, or -1 if it did not start. }
function RunLogged(const Exe, Params, Status: String): Integer;
var
  RC: Integer;
begin
  SetStatus(Status);
  LastOutput := '';
  Log('run: ' + Exe + ' ' + Params);
  try
    if ExecAndLogOutput(Exe, Params, ExtractFileDir(Exe), SW_HIDE, ewWaitUntilTerminated, RC, @CaptureLine) then
      Result := RC
    else begin
      Log('could not start ' + Exe + ': ' + SysErrorMessage(RC));
      LastOutput := SysErrorMessage(RC);
      Result := -1;
    end;
  except
    Log('could not run ' + Exe + ': ' + GetExceptionMessage);
    LastOutput := GetExceptionMessage;
    Result := -1;
  end;
  Log('exit code ' + IntToStr(Result));
end;

function Helper(const Params, Status: String): Integer;
begin
  Result := RunLogged(ExpandConstant('{app}\apollo-helper.exe'), Params, Status);
end;

function Q(const S: String): String;
begin
  Result := AddQuotes(S);
end;

{ The "  - reason" lines of the last helper run. }
function ReasonLines: String;
var
  Lines: TStringList;
  I: Integer;
begin
  Result := '';
  Lines := TStringList.Create;
  try
    Lines.Text := LastOutput;
    for I := 0 to Lines.Count - 1 do
      if Copy(Lines[I], 1, 4) = '  - ' then
        Result := Result + Lines[I] + #13#10;
  finally
    Lines.Free;
  end;
  if Result = '' then
    Result := LastOutput;
end;

{ Each line of the last helper run that starts with Prefix ('WARNING:', 'Note:', 'Unit:') as a
  summary entry, the prefix dropped for 'Unit:'. }
procedure SummaryLines(const Prefix: String);
var
  Lines: TStringList;
  I: Integer;
  S: String;
begin
  Lines := TStringList.Create;
  try
    Lines.Text := LastOutput;
    for I := 0 to Lines.Count - 1 do
      if Copy(Lines[I], 1, Length(Prefix)) = Prefix then
      begin
        S := Lines[I];
        if Prefix = 'Unit:' then
          S := Trim(Copy(S, Length(Prefix) + 1, Length(S)));
        if (Prefix = 'Unit:') and (Length(S) > 0) then
          S := Uppercase(Copy(S, 1, 1)) + Copy(S, 2, Length(S));
        AddSummary(S);
      end;
  finally
    Lines.Free;
  end;
end;

{ The first line of the last run that starts with Prefix, or ''. }
function FirstLineStarting(const Prefix: String): String;
var
  Lines: TStringList;
  I: Integer;
begin
  Result := '';
  Lines := TStringList.Create;
  try
    Lines.Text := LastOutput;
    for I := 0 to Lines.Count - 1 do
      if (Result = '') and (Copy(Lines[I], 1, Length(Prefix)) = Prefix) then
        Result := Lines[I];
  finally
    Lines.Free;
  end;
end;

{ The last few lines of the last run, for messages. }
function OutputTail: String;
var
  Lines: TStringList;
  I, First: Integer;
begin
  Result := '';
  Lines := TStringList.Create;
  try
    Lines.Text := LastOutput;
    First := Lines.Count - 6;
    if First < 0 then
      First := 0;
    for I := First to Lines.Count - 1 do
      Result := Result + Trim(Lines[I]) + ' ';
  finally
    Lines.Free;
  end;
  Result := Trim(Result);
end;

function Silent: Boolean;
begin
  if IsUninstaller then
    Result := UninstallSilent
  else
    Result := WizardSilent;
end;

{ Nothing may stream on a unit while PnP stops its driver (Windows then wants a restart).
  Units: the helper's --units list. True = go ahead, False = the user chose to stop. }
function WaitForIdle(const Units: String; const Uninstalling: Boolean): Boolean;
var
  RC, Answer: Integer;
  Stop: String;
begin
  Result := True;
  RC := Helper('idle-wait 0 --units ' + Units, 'Checking that nothing is using your Apollo units...');
  while RC = HELPER_BUSY do
  begin
    if Silent then
    begin
      Helper('idle-wait 60 --units ' + Units, 'Waiting up to 60 seconds for your Apollo units to be free...');
      exit;
    end;
    if Uninstalling then
      Stop := 'Abort stops the uninstall; nothing has been changed yet.'
    else
      Stop := 'Abort skips the driver switch: the files are installed, and "Connect Apollo units" (Start menu) does it later.';
    Answer := MsgBox('Something is still using your Apollo:' + #13#10#13#10 + ReasonLines + #13#10 +
                     'Close those programs, or stop their playback or recording (in Ableton: Preferences, Audio, ' +
                     'Driver Type away from ASIO), then click Retry.' + #13#10#13#10 +
                     'Ignore goes ahead anyway: Windows may then need a restart to finish.' + #13#10 + Stop,
                     mbError, MB_ABORTRETRYIGNORE);
    if Answer = IDRETRY then
      RC := Helper('idle-wait 10 --units ' + Units, 'Waiting for your Apollo units to be free...')
    else if Answer = IDIGNORE then
      exit
    else begin
      Result := False;
      exit;
    end;
  end;
end;

{ BitLocker: suspend protection for one restart so the boot-setting change does not trigger recovery. }
function SuspendBitLocker: Boolean;
var
  Drive: String;
begin
  Drive := Copy(ExpandConstant('{win}'), 1, 2);
  Result := RunLogged(Sys64('manage-bde.exe'), '-protectors -disable ' + Drive + ' -RebootCount 1',
                      'Suspending BitLocker for one restart...') = 0;
end;

{ ------------------------------------------------------------------ }
{ which units to switch                                               }
{ ------------------------------------------------------------------ }

{ The helper's --units list: the ticked units (silent: /UNITS=, default all found). A unit that is
  already on the Apollo driver is always in it (every unit on the driver shares one loaded image).
  '' = none. }
function SelectedUnits: String;
var
  I: Integer;
  X8p, Twin: Boolean;
  P: String;
begin
  X8p := False;
  Twin := False;
  if WizardSilent then
  begin
    P := Lowercase(ExpandConstant('{param:UNITS|all}'));
    X8p := (X8pCount = 1) and ((P = 'all') or (Pos('x8p', P) > 0));
    Twin := (TwinCount = 1) and ((P = 'all') or (Pos('twinx', P) > 0));
  end else if UnitsPage <> nil then
  begin
    for I := 0 to GetArrayLength(ItemKind) - 1 do
      if UnitsPage.Values[I] then
      begin
        if ItemKind[I] = 'x8p' then
          X8p := True
        else if ItemKind[I] = 'twinx' then
          Twin := True;
      end;
  end;
  if (X8pCount = 1) and (CompareText(X8pService, 'apollo') = 0) then
    X8p := True;
  if (TwinCount = 1) and (CompareText(TwinService, 'apollo') = 0) then
    Twin := True;
  Result := '';
  if X8p then
    Result := 'x8p';
  if Twin then
  begin
    if Result <> '' then
      Result := Result + ',';
    Result := Result + 'twinx';
  end;
end;

function UnitNames(const Units: String): String;
begin
  if Units = 'x8p,twinx' then
    Result := 'the Apollo x8p and the Apollo Twin X'
  else if Units = 'x8p' then
    Result := 'the Apollo x8p'
  else if Units = 'twinx' then
    Result := 'the Apollo Twin X'
  else
    Result := 'no unit';
end;

procedure AddUnitItem(const Kind, Name: String; Count: Integer; const Svc: String);
var
  I: Integer;
begin
  if Count = 0 then
    exit;
  if Count > 1 then
    I := UnitsPage.Add(Name + ': ' + IntToStr(Count) + ' are connected. Windows would switch all of them together, ' +
                       'so Setup switches none: connect one at a time, then use "Connect Apollo units"')
  else
    I := UnitsPage.Add(Name + ' (' + ServiceText(Svc) + ')');
  SetArrayLength(ItemKind, I + 1);
  ItemKind[I] := Kind;
  UnitsPage.Values[I] := Count = 1;
  if (Count > 1) or (CompareText(Svc, 'apollo') = 0) then
    UnitsPage.CheckListBox.ItemEnabled[I] := False;
end;

{ ------------------------------------------------------------------ }
{ wizard                                                              }
{ ------------------------------------------------------------------ }

function InitializeSetup: Boolean;
begin
  Result := True;
  SandboxTest := ExpandConstant('{param:SANDBOXTEST|0}') = '1';
  if IsSecureBootOn then
  begin
    if SandboxTest then
      Log('Secure Boot is on; /SANDBOXTEST given, continuing (the driver cannot load here).')
    else begin
      SuppressibleMsgBox('Secure Boot is on, so this driver cannot be installed.' + #13#10#13#10 +
        'This is a test build: it is not signed by Microsoft. Windows loads such drivers only in test mode, ' +
        'and Secure Boot does not allow test mode.' + #13#10#13#10 +
        'This build is only for PCs where Secure Boot has been turned off in the firmware (UEFI) settings. ' +
        'Turning it off is your decision; Setup never changes it.' + #13#10#13#10 +
        'Setup will now close without changing anything.',
        mbCriticalError, MB_OK, IDOK);
      Result := False;
      exit;
    end;
  end;
  TestSigningOn := IsTestSigningActive;
  Log('Test signing active: ' + IntToStr(Ord(TestSigningOn)));
  BitLockerState := 0;
  if (not TestSigningOn) and (not SandboxTest) then
  begin
    BitLockerState := BitLockerStatus;
    if WizardSilent and (ExpandConstant('{param:TESTSIGNING|}') <> 'on') then
    begin
      Log('Test signing is off and /TESTSIGNING=on was not given: a silent install cannot ask, stopping.');
      Result := False;
    end;
  end;
  DetectUnits;
  FakeUnits;
end;

procedure InitializeWizard;
var
  After: Integer;
  Text: String;
begin
  After := wpWelcome;
  if (not TestSigningOn) and (not SandboxTest) then
  begin
    if BitLockerState <> 0 then
    begin
      if BitLockerState = 1 then
        Text := 'BitLocker is protecting drive ' + Copy(ExpandConstant('{win}'), 1, 2) + '. '
      else
        Text := 'Setup could not tell whether BitLocker protects drive ' + Copy(ExpandConstant('{win}'), 1, 2) + '. If it does: ';
      Text := Text + 'Turning on test signing (next page) changes a Windows boot setting that BitLocker checks, ' +
        'so at the next restart Windows may stop and ask for the 48-digit BitLocker recovery key.' + #13#10#13#10 +
        'Have the recovery key ready before you continue: it is at https://aka.ms/myrecoverykey (your Microsoft ' +
        'account), on a printout or USB stick you saved, or with your IT department.' + #13#10#13#10 +
        'Setup can suspend BitLocker for that one restart so Windows does not ask. The drive stays encrypted, ' +
        'and protection turns itself back on after the restart.';
      BitLockerPage := CreateInputOptionPage(After, 'BitLocker', 'Have your BitLocker recovery key ready', Text, False, False);
      BitLockerPage.Add('Suspend BitLocker for the next restart (recommended)');
      BitLockerPage.Values[0] := True;
      After := BitLockerPage.ID;
    end;
    TestSignPage := CreateInputOptionPage(After, 'Test signing', 'Windows must be allowed to load test-signed drivers',
      'This driver is not signed by Microsoft. Windows loads such drivers only when test signing is on.' + #13#10#13#10 +
      'Setup will run "bcdedit /set testsigning on". After one restart, Windows shows "Test Mode" in the ' +
      'bottom-right corner of the desktop; everything else works as before. Test mode lets any test-signed ' +
      'driver load, so only install test drivers you trust.' + #13#10#13#10 +
      'You can turn it off again later: the uninstaller offers it.',
      False, False);
    TestSignPage.Add('Turn on test signing (Setup cannot install the driver without it)');
    TestSignPage.Values[0] := False;
    After := TestSignPage.ID;
  end;

  SigningPage := CreateOutputMsgPage(After, 'Driver signing', 'The driver is signed on this PC, for this PC only',
    'This download carries no signature of the driver''s developers, and none from Microsoft. Windows needs ' +
    'the driver package signed, so Setup signs it here, after copying the files:' + #13#10#13#10 +
    '1. It makes a new certificate, "Apollo driver (signed on ' + ExpandConstant('{computername}') + ')", that exists ' +
    'only on this PC.' + #13#10 +
    '2. It signs apollo.sys and its catalog with it.' + #13#10 +
    '3. It adds the public part of the certificate to this PC''s Trusted Root and Trusted Publishers stores, ' +
    'so Windows accepts the driver package.' + #13#10 +
    '4. It deletes the private key straight away, so nothing else can ever be signed with that certificate.' + #13#10#13#10 +
    'Uninstall removes the certificate again. Running Setup again (a repair or an update) makes a new ' +
    'certificate the same way. If signing fails, Setup stops before any unit is switched and says why.');
  After := SigningPage.ID;

  if X8pCount + TwinCount > 0 then
    Text := 'Setup found these Apollo units. A ticked unit is switched from Universal Audio''s driver to the Apollo ' +
      'driver; an unticked one stays on the driver it has. A unit already on the Apollo driver is always updated, ' +
      'since every unit on it shares one loaded driver.' + #13#10 +
      'Close programs that play or record through these units first.'
  else
    Text := 'No Apollo x8p or Apollo Twin X Gen 1 is connected now. Setup installs and prepares the driver anyway. ' +
      'Once a unit is connected over Thunderbolt and switched on, run "Connect Apollo units" from the Start menu ' +
      '(Avium Apollo).';
  if OtherUa <> '' then
    Text := Text + #13#10#13#10 + 'Other Universal Audio devices:' + #13#10 + OtherUa;
  UnitsPage := CreateInputOptionPage(After, 'Apollo units', 'Which units should use the Apollo driver?', Text, False, False);
  AddUnitItem('x8p', 'Apollo x8p', X8pCount, X8pService);
  AddUnitItem('twinx', 'Apollo Twin X Gen 1', TwinCount, TwinService);

  ResultsPage := CreateOutputMsgMemoPage(wpInstalling, 'Results', 'What Setup did',
    'Read this before you restart. The full log is in your TEMP folder (Setup Log *.txt).', '');
end;

function NextButtonClick(CurPageID: Integer): Boolean;
begin
  Result := True;
  if (TestSignPage <> nil) and (CurPageID = TestSignPage.ID) and (not TestSignPage.Values[0]) then
  begin
    MsgBox('The Apollo driver cannot load without test signing, so Setup cannot continue without it.' + #13#10#13#10 +
           'Tick the box to go on, or click Cancel to leave Setup without changing anything.', mbInformation, MB_OK);
    Result := False;
  end;
end;

function WantSuspendBitLocker: Boolean;
begin
  if WizardSilent then
    Result := (BitLockerState <> 0) and (ExpandConstant('{param:SUSPENDBITLOCKER|yes}') <> 'no')
  else
    Result := (BitLockerPage <> nil) and BitLockerPage.Values[0];
end;

function UpdateReadyMemo(Space, NewLine, MemoUserInfoInfo, MemoDirInfo, MemoTypeInfo, MemoComponentsInfo,
  MemoGroupInfo, MemoTasksInfo: String): String;
var
  S, Units: String;
begin
  Units := SelectedUnits;
  S := 'Setup will:' + NewLine;
  if SandboxTest then
  begin
    S := S + Space + '(/SANDBOXTEST: test signing is left alone and no unit is switched;' + NewLine;
    S := S + Space + '  a real install would switch ' + UnitNames(Units) + ')' + NewLine;
  end;
  if (not TestSigningOn) and (not SandboxTest) then
  begin
    if WantSuspendBitLocker then
      S := S + Space + 'Suspend BitLocker on ' + Copy(ExpandConstant('{win}'), 1, 2) + ' for one restart' + NewLine;
    S := S + Space + 'Turn on test signing (bcdedit /set testsigning on)' + NewLine;
  end;
  S := S + Space + 'Copy the driver, its tools and Apollo Control to ' + ExpandConstant('{app}') + NewLine;
  S := S + Space + 'Sign the driver with a new certificate made for this PC, trust it on this PC only,' + NewLine;
  S := S + Space + '  and delete its private key' + NewLine;
  S := S + Space + 'Register the Avium ASIO drivers' + NewLine;
  S := S + Space + 'Write the driver settings from defaults.json' + NewLine;
  if SandboxTest or (Units = '') then
    S := S + Space + 'Prepare the driver (switch no unit now; "Connect Apollo units" does it later)' + NewLine
  else begin
    S := S + Space + 'Save your default playback and recording devices' + NewLine;
    S := S + Space + 'Switch ' + UnitNames(Units) + ' to the Apollo driver, closing UA''s apps first only if' + NewLine;
    S := S + Space + '  no Universal Audio unit that stays on UA''s driver still needs them' + NewLine;
    S := S + Space + 'Put your default playback and recording devices back' + NewLine;
  end;
  if UadInstalled then
    S := S + Space + 'Copy the five input-strip modules from your UAD install (apollo-diag strips install)' + NewLine
  else
    S := S + Space + 'Skip the input-strip modules: UA''s UAD software is not installed' + NewLine;
  S := S + Space + 'Add Start-menu shortcuts (Avium Apollo)' + NewLine;
  if (not TestSigningOn) and (not SandboxTest) then
    S := S + Space + 'Ask you to restart Windows (test signing takes effect then)' + NewLine;
  if MemoTasksInfo <> '' then
    S := S + NewLine + MemoTasksInfo + NewLine;
  Result := S;
end;

{ ------------------------------------------------------------------ }
{ install                                                             }
{ ------------------------------------------------------------------ }

procedure TurnOnTestSigning;
begin
  if TestSigningOn or SandboxTest then
    exit;
  if WantSuspendBitLocker then
  begin
    if SuspendBitLocker then
      AddSummary('BitLocker is suspended for the next restart; it turns itself back on afterwards.')
    else if Silent then
      Log('Could not suspend BitLocker; continuing (silent).')
    else if MsgBox('Setup could not suspend BitLocker (' + OutputTail + ').' + #13#10#13#10 +
                   'If you continue, Windows will probably ask for the BitLocker recovery key at the next restart. ' +
                   'Continue only if you have it.' + #13#10#13#10 + 'Continue?', mbConfirmation, MB_YESNO) <> IDYES then
      Abort;
  end;
  if RunLogged(Sys64('bcdedit.exe'), '/set {current} testsigning on',
               'Turning on test signing...') <> 0 then
  begin
    SuppressibleMsgBox('Windows refused to turn on test signing:' + #13#10#13#10 + LastOutput + #13#10 +
      'If it mentions Secure Boot, Secure Boot is still on in the firmware settings. Setup will now close; ' +
      'nothing has been installed.', mbCriticalError, MB_OK, IDOK);
    Abort;
  end;
  TurnedTestSigningOn := True;
  RestartNeeded := True;
  AddSummary('Test signing is turned on. It takes effect after the restart, when Windows shows "Test Mode" on the desktop.');
end;

{ sign-local.ps1, elevated, from the install folder: the unsigned apollo.sys and apollo.cat get a signature made
  on this PC. Only once test signing is on (this boot, or turned on for the next one). On failure
  whatever it trusted is removed again (apollo-helper cert remove-local) and nothing is switched. }
function SignDriver: Boolean;
var
  App, Detail: String;
  RC: Integer;
begin
  App := ExpandConstant('{app}');
  Result := False;
  if not (TestSigningOn or TurnedTestSigningOn or SandboxTest) then
  begin
    AddSummary('Test signing is not on, so the driver was not signed or switched. Run Setup again.');
    exit;
  end;
  RC := RunLogged(Sys64('WindowsPowerShell\v1.0\powershell.exe'),
                  '-NoProfile -NonInteractive -ExecutionPolicy Bypass -File ' + Q(App + '\sign-local.ps1') +
                  ' -Package ' + Q(App + '\driver') + ' -State ' + Q(App + '\state'),
                  'Signing the driver on this PC (a certificate made for this PC only)...');
  if RC = 0 then
  begin
    Result := True;
    AddSummary('The driver is signed on this PC with the certificate "Apollo driver (signed on ' +
               ExpandConstant('{computername}') + ')", trusted on this PC only; its private key is deleted.');
    exit;
  end;
  Detail := OutputTail;
  if FileExists(App + '\state\local-cert.txt') then
    Helper('cert remove-local ' + Q(App + '\state\local-cert.txt'), 'Removing the certificate again...');
  AddSummary('Signing the driver on this PC FAILED (' + Detail + '). Nothing was switched: your Apollo units stay on ' +
             'their current driver. Run Setup again, or uninstall it from Settings, Apps.');
  SuppressibleMsgBox('Setup could not sign the driver on this PC, so it stopped before switching any unit.' + #13#10#13#10 +
    Detail + #13#10#13#10 +
    'Your Apollo units stay on the driver they have, and the certificate Setup made was removed again. ' +
    'Windows PowerShell must be able to run a script from the install folder (Group Policy can forbid it). ' +
    'Run Setup again to retry, or uninstall it from Settings, Apps. The Setup log in your TEMP folder has the details.',
    mbCriticalError, MB_OK, IDOK);
end;

procedure InstallDriverAndSettings;
var
  App, Defaults, Units: String;
  RC: Integer;
  SavedDefaults: Boolean;
begin
  App := ExpandConstant('{app}');
  Defaults := App + '\state\audio-defaults.txt';
  Helper('check', 'Checking the system...');

  if not SignDriver then
    exit;

  RC := RunLogged(Sys64('regsvr32.exe'), '/s ' + Q(App + '\asio\avium-asio.dll'),
                  'Registering the ASIO drivers...');
  if RC = 0 then
    AddSummary('The ASIO drivers "Apollo x8p (Avium)" and "Apollo (Avium)" (both units as one device) are registered.')
  else
    AddSummary('Registering the ASIO drivers failed (regsvr32 exit code ' + IntToStr(RC) + ').');

  { Before any unit switches, so the driver's first start already reads them. }
  RC := Helper('regdefaults ' + Q(App + '\defaults.json'), 'Writing the driver settings...');
  if RC = HELPER_OK then
    AddSummary('Driver settings written to Services\apollo\Parameters (the x8p''s input strips; the Twin X in full mode with its strips).')
  else
    AddSummary('Writing the driver settings failed (' + OutputTail + ').');

  Units := SelectedUnits;
  Log('Units to switch: ' + Units);
  if SandboxTest or (Units = '') then
  begin
    RC := Helper('bind ' + Q(App + '\driver\apollo.inf') + ' --stage-only', 'Preparing the driver...');
    SummaryLines('Note:');
    if (RC = HELPER_NO_DEVICE) or (RC = HELPER_OK) then
    begin
      if SandboxTest then
        AddSummary('/SANDBOXTEST: the driver package is staged; no unit was switched.')
      else if X8pCount + TwinCount > 0 then
        AddSummary('No unit was switched (none was ticked). The driver is prepared: "Connect Apollo units" in the ' +
                   'Start menu switches a unit later.')
      else
        AddSummary('No Apollo unit is connected, so the driver is prepared for later. Connect and switch on your ' +
                   'unit, then run "Connect Apollo units" from the Start menu: Windows prefers UA''s driver whenever ' +
                   'it is installed.');
    end else
      AddSummary('Preparing the driver failed (exit code ' + IntToStr(RC) + ': ' + OutputTail + ').');
  end
  else if not WaitForIdle(Units, False) then
    AddSummary('You skipped the driver switch. Run "Connect Apollo units" from the Start menu when nothing uses your units.')
  else begin
    SavedDefaults := Helper('defaults save ' + Q(Defaults), 'Saving your default audio devices...') = HELPER_OK;
    Helper('quit-ua --units ' + Units, 'Closing Universal Audio''s apps if no unit that stays on UA''s driver needs them...');
    SummaryLines('Note:');
    RC := Helper('bind ' + Q(App + '\driver\apollo.inf') + ' --units ' + Units,
                 'Switching ' + UnitNames(Units) + ' to the Apollo driver...');
    { What happened to each unit, another unit that changed, a unit apollo.inf could take on its own. }
    SummaryLines('Unit:');
    SummaryLines('WARNING:');
    SummaryLines('Note:');
    if (RC = HELPER_REBOOT) or (FirstLineStarting('Restart Windows') <> '') then
      RestartNeeded := True;
    if RC = HELPER_BLOCKED then
      AddSummary('No unit was switched because another Apollo unit is in the way (' + OutputTail +
                 '). The files are installed; once that is sorted out, run "Connect Apollo units" from the Start menu.')
    else if RC = HELPER_NO_DEVICE then
      AddSummary('None of the chosen units is connected now, so the driver is prepared for later: run "Connect Apollo ' +
                 'units" from the Start menu once it is.')
    else if (RC <> HELPER_OK) and (RC <> HELPER_REBOOT) and (FirstLineStarting('Unit:') = '') then
      AddSummary('Switching to the Apollo driver failed (exit code ' + IntToStr(RC) + ': ' + OutputTail +
                 '). The files are installed; fix the cause, then run "Connect Apollo units" from the Start menu.');
    if SavedDefaults then
    begin
      Helper('defaults restore ' + Q(Defaults) + ' --passes 3', 'Putting your default audio devices back...');
      DeleteFile(Defaults);
    end;
  end;

  if UadInstalled then
  begin
    RC := RunLogged(App + '\tools\apollo-diag.exe', 'strips install', 'Copying the input-strip modules from UAD...');
    if RC = 0 then
      AddSummary('The five input-strip modules are copied from your UAD install (System32\drivers\ApolloStrips).')
    else
      AddSummary('The input-strip modules were not found in your UAD install with the expected contents (' +
                 OutputTail + '). The mic strips stay off until they are.');
  end else
    AddSummary('UA''s UAD software is not installed, so the input-strip modules were skipped. The mic preamp strips ' +
               'need them: install UAD, then run "apollo-diag strips install" from an elevated prompt in ' +
               App + '\tools.');
end;

{ [PlugSetup] The task "Set up UAD plug-in hosting after install": the Start-menu shortcut, opened as the user who started
  Setup (never elevated: everything it makes goes to that user's profile). It explains and asks before it downloads. }
procedure StartPluginSetup;
var
  Lnk: String;
  RC: Integer;
begin
  Lnk := ExpandConstant('{group}\Set up UAD plug-ins.lnk');
  if Silent then
    AddSummary('UAD plug-ins: not set up in a silent install. Run "Set up UAD plug-ins" from the Start menu.')
  else if not FileExists(Lnk) then
    AddSummary('UAD plug-ins: the "Set up UAD plug-ins" shortcut is missing, so the setup did not start.')
  else if ShellExecAsOriginalUser('open', Lnk, '', ExpandConstant('{app}'), SW_SHOWNORMAL, ewNoWait, RC) then
    AddSummary('UAD plug-ins: the setup opened in its own window. It explains what it downloads and asks before it starts; ' +
               'run "Set up UAD plug-ins" from the Start menu again any time (it continues where it stopped).')
  else
    AddSummary('UAD plug-ins: the setup did not open (' + SysErrorMessage(RC) + '). Run "Set up UAD plug-ins" from the Start menu.');
end;

procedure CurStepChanged(CurStep: TSetupStep);
begin
  if CurStep = ssInstall then
    TurnOnTestSigning
  else if CurStep = ssPostInstall then
  begin
    InstallDriverAndSettings;
    if WizardIsTaskSelected('setupplugins') then
      StartPluginSetup;
    if RestartNeeded then
      AddSummary('Restart Windows to finish (Setup asks on the last page).');
    if ResultsPage <> nil then
      ResultsPage.RichEditViewer.Lines.Text := Summary;
  end;
end;

function NeedRestart: Boolean;
begin
  Result := RestartNeeded;
end;

{ ------------------------------------------------------------------ }
{ uninstall                                                           }
{ ------------------------------------------------------------------ }

var
  UninstallRestart: Boolean;

procedure RemoveDriverAndSettings;
var
  App, Defaults, S, PowerCycle: String;
  RC: Integer;
  SavedDefaults: Boolean;
begin
  App := ExpandConstant('{app}');
  Defaults := App + '\state\uninstall-defaults.txt';
  if FileExists(App + '\apollo-helper.exe') then
  begin
    { Every Apollo unit and its driver, into the uninstall log. }
    Helper('check', 'Checking the system...');
    if not WaitForIdle('all', True) then
      Abort;
    SavedDefaults := Helper('defaults save ' + Q(Defaults), 'Saving your default audio devices...') = HELPER_OK;
    { unbind moves every unit other than the x8p on the Apollo driver (a Twin X) back to UA's driver
      before it removes the package (the x8p goes back with the package). If UA's driver for such a
      unit is not installed it stops with nothing changed: the package must stay, or that unit would
      be left with no driver. }
    RC := Helper('unbind', 'Removing the Apollo driver (your units go back to UA''s driver)...');
    if RC = HELPER_BLOCKED then
    begin
      Log('unbind stopped: another Apollo unit would be left without a driver; uninstall aborted.');
      if not Silent then
        MsgBox('The uninstall stopped before removing anything.' + #13#10#13#10 +
               'An Apollo unit is on the Apollo driver, and UA''s driver for it is not installed:' + #13#10 +
               ReasonLines + #13#10 +
               'Removing the Apollo driver now would leave that unit with no driver at all. Install UA Connect ' +
               '(Universal Audio''s software and driver), then run the uninstall again.', mbError, MB_OK);
      Abort;
    end;
    if RC = HELPER_REBOOT then
      UninstallRestart := True;
    PowerCycle := FirstLineStarting('Power-cycle ');
    if PowerCycle = '' then
      PowerCycle := FirstLineStarting('Restart Windows to finish');
    if SavedDefaults then
      Helper('defaults restore ' + Q(Defaults) + ' --passes 3', 'Putting your default audio devices back...');
    if (PowerCycle <> '') and (not Silent) then
      MsgBox(PowerCycle, mbInformation, MB_OK);
    RunLogged(Sys64('regsvr32.exe'), '/u /s ' + Q(App + '\asio\avium-asio.dll'),
              'Unregistering the ASIO drivers...');
    { The certificates Setup made on this PC (one per install or repair), by thumbprint only. }
    if FileExists(App + '\state\local-cert.txt') then
      Helper('cert remove-local ' + Q(App + '\state\local-cert.txt'), 'Removing the certificates Setup made on this PC...');
    { An install from before Nov 16 2026 added driver\apollo.cer and recorded which stores it added
      it to; only those, and only with that record. }
    if FileExists(App + '\driver\apollo.cer') and FileExists(App + '\state\cert-record.txt') then
      Helper('cert remove ' + Q(App + '\driver\apollo.cer') + ' --record ' + Q(App + '\state\cert-record.txt'),
             'Removing the test certificate an earlier version added...');
  end;
  { Apollo Control's own "start with Windows" value, if it points here. }
  if RegQueryStringValue(HKCU, RUN_KEY, RUN_VALUE, S) and
     (Pos(Lowercase(App), Lowercase(S)) > 0) then
    RegDeleteValue(HKCU, RUN_KEY, RUN_VALUE);
end;

procedure OfferTestSigningOff;
begin
  if UninstallSilent or (not IsTestSigningActive) then
    exit;
  if MsgBox('Turn test signing off too?' + #13#10#13#10 +
            'Leave it on (No) if you plan to install an Apollo test build again, or use other test-signed drivers. ' +
            'Turn it off (Yes) to go back to normal Windows mode after a restart; the "Test Mode" text then disappears.',
            mbConfirmation, MB_YESNO or MB_DEFBUTTON2) <> IDYES then
    exit;
  if BitLockerStatus <> 0 then
  begin
    if MsgBox('BitLocker may ask for the recovery key at the next restart because this changes a boot setting.' + #13#10#13#10 +
              'Suspend BitLocker for that one restart (recommended)?', mbConfirmation, MB_YESNO) = IDYES then
      SuspendBitLocker;
  end;
  if RunLogged(Sys64('bcdedit.exe'), '/set {current} testsigning off',
               'Turning off test signing...') = 0 then
    UninstallRestart := True
  else
    MsgBox('Windows did not turn test signing off: ' + LastOutput, mbError, MB_OK);
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
begin
  if CurUninstallStep = usUninstall then
  begin
    RemoveDriverAndSettings;
    OfferTestSigningOff;
  end;
end;

function UninstallNeedRestart: Boolean;
begin
  Result := UninstallRestart;
end;
