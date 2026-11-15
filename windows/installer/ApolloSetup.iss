; ApolloSetup.iss: test-mode installer for the Apollo x8p open driver (apollo.sys).
; Compile with build.cmd (it stages the payload and passes these defines):
;   Payload    absolute path of the staged payload (installed 1:1 into {app})
;   DriverVer  the package's DriverVer version, e.g. 12.36.20.8
;   CertThumb  SHA-1 thumbprint of driver\apollo.cer (the helper refuses any other certificate)
;   OutDir     output folder
;   Sign       optional: sign Setup and the uninstaller with the "aviumtest" sign tool (/S on ISCC)
; Every elevated step runs a binary from {app} (Program Files, admin-only) or from System32.
; Nothing is executed from {tmp} or any other user-writable folder.

#ifndef Payload
  #error Payload is not defined: build with build.cmd
#endif
#ifndef DriverVer
  #error DriverVer is not defined: build with build.cmd
#endif
#ifndef CertThumb
  #error CertThumb is not defined: build with build.cmd
#endif
#ifndef OutDir
  #define OutDir "out"
#endif

#define AppName "Apollo x8p open driver"

[Setup]
AppId={{4C8B501E-BA5A-4B5A-A7B9-186B4395E230}
AppName={#AppName}
AppVersion={#DriverVer}
AppVerName={#AppName} {#DriverVer} (test build)
AppPublisher=Avium
AppComments=Test-signed driver for the Universal Audio Apollo x8p. Needs Windows test mode.
VersionInfoVersion={#DriverVer}
VersionInfoCompany=Avium
VersionInfoDescription=Apollo x8p open driver setup (test build)
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
SetupLogging=yes
UninstallLogging=yes
CloseApplications=yes
RestartApplications=no
UsedUserAreasWarning=no
#ifdef Sign
SignTool=aviumtest
SignedUninstaller=yes
#endif

[Messages]
WindowsVersionNotSupported=The Apollo x8p driver needs 64-bit Windows 11, version 22H2 (build 22621) or later, on an x64 PC.
WelcomeLabel2=This installs the Apollo x8p open driver {#DriverVer}, a TEST build for the Universal Audio Apollo x8p (Thunderbolt).%n%nThe driver is signed with a test certificate, not by Microsoft. Windows loads it only in test mode and only with Secure Boot off. Setup explains each system change before making it.%n%nOnly the x8p moves to this driver. Other Universal Audio devices (a Twin X, for example) stay on the driver they have, and UA's apps stay open while one of them still uses UA's driver. The mic preamp strips need UA's UAD software on this PC.%n%nBefore you continue, close programs that use the x8p: DAWs, Apollo Control and the Apollo test tools.

[Tasks]
Name: autostart; Description: "Start Apollo Control when I sign in (it sits in the notification area)"; Flags: unchecked

[Dirs]
Name: "{app}\state"

[Files]
; The staged payload, as is: build.cmd wrote its SHA-256 manifest (payload-manifest.sha256, also installed).
Source: "{#Payload}\*"; DestDir: "{app}"; Flags: recursesubdirs ignoreversion

[Icons]
Name: "{group}\Apollo Control"; Filename: "{app}\apollo-control.exe"; WorkingDir: "{app}"; Comment: "Preamps, monitor and device status for the Apollo x8p"
Name: "{group}\Connect x8p to Apollo driver"; Filename: "{app}\apollo-helper.exe"; Parameters: "connect"; WorkingDir: "{app}"; IconFilename: "{app}\apollo-control.exe"; Comment: "Switch the x8p from UA's driver to the Apollo driver (asks for administrator rights)"
Name: "{group}\Uninstall Apollo driver"; Filename: "{uninstallexe}"; Comment: "Put the x8p (and any other unit on the Apollo driver) back on UA's driver and remove the Apollo driver"

[Registry]
; The same value Apollo Control's own "start with Windows" switch writes, so the switch in the app shows it.
Root: HKCU; Subkey: "Software\Microsoft\Windows\CurrentVersion\Run"; ValueType: string; ValueName: "Avium Apollo Control"; ValueData: """{app}\apollo-control.exe"" --minimized"; Tasks: autostart; Flags: uninsdeletevalue

[Run]
Filename: "{app}\apollo-control.exe"; Description: "Open Apollo Control"; Flags: postinstall nowait skipifsilent runasoriginaluser unchecked; Check: not RestartPending

[UninstallDelete]
Type: filesandordirs; Name: "{app}\state"
; Strip modules apollo-diag copied from the UAD install (UA's bytes: they leave with the driver).
Type: filesandordirs; Name: "{sys}\drivers\ApolloStrips"
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

var
  TestSignPage: TInputOptionWizardPage;
  BitLockerPage: TInputOptionWizardPage;
  ResultsPage: TOutputMsgMemoWizardPage;
  SandboxTest: Boolean;         { /SANDBOXTEST: layout/UI/uninstall check in Windows Sandbox }
  TestSigningOn: Boolean;       { active for this boot }
  BitLockerState: Integer;      { 1 on, 0 off, -1 could not tell }
  TurnedTestSigningOn: Boolean;
  RestartNeeded: Boolean;
  Summary: String;
  LastOutput: String;

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

{ Each line of the last helper run that starts with Prefix ('WARNING:', 'Note:') as a summary entry. }
procedure SummaryLines(const Prefix: String);
var
  Lines: TStringList;
  I: Integer;
begin
  Lines := TStringList.Create;
  try
    Lines.Text := LastOutput;
    for I := 0 to Lines.Count - 1 do
      if Copy(Lines[I], 1, Length(Prefix)) = Prefix then
        AddSummary(Lines[I]);
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

{ Nothing may stream on the x8p while PnP stops its driver (Windows then wants a restart).
  True = go ahead, False = the user chose to stop. }
function WaitForIdle(const Uninstalling: Boolean): Boolean;
var
  RC, Answer: Integer;
  Stop: String;
begin
  Result := True;
  RC := Helper('idle-wait 0', 'Checking that nothing is using the x8p...');
  while RC = HELPER_BUSY do
  begin
    if Silent then
    begin
      Helper('idle-wait 60', 'Waiting up to 60 seconds for the x8p to be free...');
      exit;
    end;
    if Uninstalling then
      Stop := 'Abort stops the uninstall; nothing has been changed yet.'
    else
      Stop := 'Abort skips the driver switch: the files are installed, and "Connect x8p to Apollo driver" (Start menu) does it later.';
    Answer := MsgBox('Something is still using the Apollo x8p:' + #13#10#13#10 + ReasonLines + #13#10 +
                     'Close those programs, or stop their playback or recording (in Ableton: Preferences, Audio, ' +
                     'Driver Type away from ASIO), then click Retry.' + #13#10#13#10 +
                     'Ignore goes ahead anyway: Windows may then need a restart to finish.' + #13#10 + Stop,
                     mbError, MB_ABORTRETRYIGNORE);
    if Answer = IDRETRY then
      RC := Helper('idle-wait 10', 'Waiting for the x8p to be free...')
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
        'This is a test build: it is signed with a developer test certificate, not by Microsoft. ' +
        'Windows loads test-signed drivers only in test mode, and Secure Boot does not allow test mode.' + #13#10#13#10 +
        'A release signed through Microsoft, which works with Secure Boot on, is planned. ' +
        'Until then this test build is only for PCs where Secure Boot has been turned off in the ' +
        'firmware (UEFI) settings.' + #13#10#13#10 + 'Setup will now close without changing anything.',
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
      'This driver is signed with a test certificate, not by Microsoft. Windows loads such drivers only when ' +
      'test signing is on.' + #13#10#13#10 +
      'Setup will run "bcdedit /set testsigning on". After a restart, Windows shows "Test Mode" in the ' +
      'bottom-right corner of the desktop; everything else works as before. Test mode lets any test-signed ' +
      'driver load, so only install test drivers you trust.' + #13#10#13#10 +
      'You can turn it off again later: the uninstaller offers it.',
      False, False);
    TestSignPage.Add('Turn on test signing (Setup cannot install the driver without it)');
    TestSignPage.Values[0] := False;
  end;
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
  S: String;
begin
  S := 'Setup will:' + NewLine;
  if SandboxTest then
    S := S + Space + '(/SANDBOXTEST: leave Secure Boot and test signing alone; the driver cannot load here)' + NewLine;
  if (not TestSigningOn) and (not SandboxTest) then
  begin
    if WantSuspendBitLocker then
      S := S + Space + 'Suspend BitLocker on ' + Copy(ExpandConstant('{win}'), 1, 2) + ' for one restart' + NewLine;
    S := S + Space + 'Turn on test signing (bcdedit /set testsigning on)' + NewLine;
  end;
  S := S + Space + 'Copy the driver, its tools and Apollo Control to ' + ExpandConstant('{app}') + NewLine;
  S := S + Space + 'Add the driver''s test certificate to the Trusted Root and Trusted Publishers stores' + NewLine;
  S := S + Space + 'Register the Apollo x8p (Avium) ASIO driver' + NewLine;
  S := S + Space + 'Write the driver settings from defaults.json (keeps values already set)' + NewLine;
  S := S + Space + 'Save your default playback and recording devices' + NewLine;
  S := S + Space + 'Switch the x8p (only the x8p) to the Apollo driver, closing UA''s apps first if the x8p is on' + NewLine;
  S := S + Space + '  UA''s driver and no other Universal Audio unit still is; if no x8p is connected, prepare' + NewLine;
  S := S + Space + '  the driver for when it is' + NewLine;
  S := S + Space + 'Put your default playback and recording devices back' + NewLine;
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

procedure InstallDriverAndSettings;
var
  App, Defaults: String;
  RC: Integer;
  CertOk, SavedDefaults: Boolean;
begin
  App := ExpandConstant('{app}');
  Defaults := App + '\state\audio-defaults.txt';
  Helper('check', 'Checking the system...');

  RC := Helper('cert add ' + Q(App + '\driver\apollo.cer') + ' --expect {#CertThumb} --record ' +
               Q(App + '\state\cert-record.txt'), 'Adding the driver''s test certificate...');
  CertOk := RC = HELPER_OK;
  if CertOk then
    AddSummary('The driver''s test certificate is in the Trusted Root and Trusted Publishers stores.')
  else
    AddSummary('Adding the test certificate failed (' + OutputTail + '). The driver was not installed: run Setup again.');

  RC := RunLogged(Sys64('regsvr32.exe'), '/s ' + Q(App + '\asio\avium-asio.dll'),
                  'Registering the ASIO driver...');
  if RC = 0 then
    AddSummary('The "Apollo x8p (Avium)" ASIO driver is registered.')
  else
    AddSummary('Registering the ASIO driver failed (regsvr32 exit code ' + IntToStr(RC) + ').');

  { Before the driver binds, so its first start already reads them. }
  RC := Helper('regdefaults ' + Q(App + '\defaults.json'), 'Writing the driver settings...');
  if RC = HELPER_OK then
    AddSummary('Driver settings written to Services\apollo\Parameters (values already set were kept).')
  else
    AddSummary('Writing the driver settings failed (' + OutputTail + ').');

  if CertOk then
  begin
    if not WaitForIdle(False) then
      AddSummary('You skipped the driver switch. Run "Connect x8p to Apollo driver" from the Start menu when nothing uses the x8p.')
    else begin
      SavedDefaults := Helper('defaults save ' + Q(Defaults), 'Saving your default audio devices...') = HELPER_OK;
      if SandboxTest then
        { Test signing is not on: binding would leave an x8p on a driver that cannot load. }
        RC := Helper('bind ' + Q(App + '\driver\apollo.inf') + ' --stage-only', 'Staging the driver (sandbox test)...')
      else begin
        Helper('quit-ua', 'Closing Universal Audio''s apps if the x8p is on UA''s driver and no other unit needs them...');
        SummaryLines('Note:');
        RC := Helper('bind ' + Q(App + '\driver\apollo.inf'), 'Installing the driver on the x8p...');
      end;
      { Another unit that changed, or a unit apollo.inf could take on its own: the helper says so. }
      SummaryLines('WARNING:');
      SummaryLines('Note:');
      if RC = HELPER_OK then
        AddSummary('The x8p now runs on the Apollo driver. Apollo Control shows it once it has connected (up to a minute).')
      else if RC = HELPER_BLOCKED then
        AddSummary('The x8p was left on its current driver because another Apollo unit is in the way (' + OutputTail +
                   '). The files are installed; once that is sorted out, run "Connect x8p to Apollo driver" from the Start menu.')
      else if RC = HELPER_NO_DEVICE then
        AddSummary('No x8p is connected, so the driver is staged for later. When you connect the x8p, run ' +
                   '"Connect x8p to Apollo driver" from the Start menu: Windows prefers UA''s driver whenever ' +
                   'it is installed. (Without UA''s driver, Windows picks the Apollo driver by itself.)')
      else if RC = HELPER_REBOOT then
      begin
        RestartNeeded := True;
        AddSummary('The x8p switches to the Apollo driver after the restart.');
      end else
        AddSummary('Switching the x8p to the Apollo driver failed (exit code ' + IntToStr(RC) + ': ' + OutputTail +
                   '). The files are installed; fix the cause, then run "Connect x8p to Apollo driver" from the Start menu.');
      if SavedDefaults then
      begin
        Helper('defaults restore ' + Q(Defaults) + ' --passes 3', 'Putting your default audio devices back...');
        DeleteFile(Defaults);
      end;
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

procedure CurStepChanged(CurStep: TSetupStep);
begin
  if CurStep = ssInstall then
    TurnOnTestSigning
  else if CurStep = ssPostInstall then
  begin
    InstallDriverAndSettings;
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
    if not WaitForIdle(True) then
      Abort;
    SavedDefaults := Helper('defaults save ' + Q(Defaults), 'Saving your default audio devices...') = HELPER_OK;
    { unbind moves every other unit on the Apollo driver (a Twin X) back to UA's driver before it removes
      the package. If UA's driver for such a unit is not installed it stops with nothing changed: the
      package must stay, or that unit would be left with no driver. }
    RC := Helper('unbind', 'Removing the Apollo driver (units on it go back to UA''s driver)...');
    if RC = HELPER_BLOCKED then
    begin
      Log('unbind stopped: another Apollo unit would be left without a driver; uninstall aborted.');
      if not Silent then
        MsgBox('The uninstall stopped before removing anything.' + #13#10#13#10 +
               'Another Universal Audio unit is on the Apollo driver, and UA''s driver for it is not installed:' + #13#10 +
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
              'Unregistering the ASIO driver...');
    Helper('cert remove ' + Q(App + '\driver\apollo.cer') + ' --record ' + Q(App + '\state\cert-record.txt'),
           'Removing the test certificate...');
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
