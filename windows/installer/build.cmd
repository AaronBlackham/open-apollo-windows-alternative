@echo off
rem Builds installer\out\ApolloSetup-<DriverVer>.exe from a frozen driver build plus app\bin.
rem   build.cmd [frozen build folder] [EWDK drive letter]
rem   default: ..\builds\2026-11-14-release4 and D:
rem   A build folder with UNSAFE.txt (before the saturating-add fix) is refused.
rem Needs: the EWDK ISO mounted, internet once (Inno Setup is fetched and verified on first use).
rem Writes only below installer\ (helper\bin, helper\obj, tools\, out\). See README.md.
setlocal EnableExtensions
set "HERE=%~dp0"
set "ROOT=%~dp0.."
set "BUILD=%~1"
if "%BUILD%"=="" set "BUILD=%ROOT%\builds\2026-11-14-release4"
set "EWDK=%~2"
if "%EWDK%"=="" set "EWDK=D:"
rem The driver's test certificate ("WDKTestCert aaron"). stage.ps1 refuses any other.
set "EXPECT_THUMB=8ACDF79F5C64144C08650382DB32B1866ABC2EC6"
set "INNO_VER=6.7.4"
set "ISCC=%HERE%tools\inno-%INNO_VER%\ISCC.exe"
set "OUT=%HERE%out"
set "STAGE=%OUT%\stage"

if not exist "%BUILD%\package\apollo.inf" (
    echo No frozen build at "%BUILD%" ^(expected package\apollo.inf, bin\, src\include\^).
    exit /b 1
)
if exist "%BUILD%\UNSAFE.txt" (
    echo Refusing "%BUILD%": it is marked UNSAFE ^(see UNSAFE.txt in that folder^).
    exit /b 1
)
if not exist "%EWDK%\BuildEnv\SetupBuildEnv.cmd" (
    echo EWDK not found at %EWDK%\ - mount the ISO, or pass its drive letter as the second argument.
    exit /b 1
)

echo === 1. Inno Setup %INNO_VER% ===
if exist "%ISCC%" goto have_inno
powershell -NoProfile -ExecutionPolicy Bypass -File "%HERE%get-inno.ps1"
if errorlevel 1 goto fail
:have_inno
echo %ISCC%

echo === 2. EWDK ===
call "%EWDK%\BuildEnv\SetupBuildEnv.cmd" amd64 >nul 2>&1
@echo off
where cl.exe >nul 2>&1
if errorlevel 1 (
    echo The EWDK environment did not set up: cl.exe not found.
    goto fail
)
set "SIGNTOOL=%WindowsSdkDir%bin\%Version_Number%\x64\signtool.exe"

echo === 3. apollo-helper (/W4 /WX) ===
set "IOCTL_INC=%BUILD%\src\include"
if not exist "%IOCTL_INC%\apollo_ioctl.h" set "IOCTL_INC=%ROOT%\include"
echo IOCTL layout checked against %IOCTL_INC%\apollo_ioctl.h
call "%HERE%helper\build.cmd" "%IOCTL_INC%"
if errorlevel 1 goto fail

echo === 4. test-certificate signing ===
set SIGN=0
set SIGNSTORE=
powershell -NoProfile -ExecutionPolicy Bypass -File "%HERE%find-signing-cert.ps1" -Thumbprint %EXPECT_THUMB%
if errorlevel 3 goto no_sign
if errorlevel 2 set SIGNSTORE=PrivateCertStore
if errorlevel 2 goto sign_helper
if errorlevel 1 goto no_sign
set SIGNSTORE=My
goto sign_helper
:no_sign
echo The test certificate's private key is not in CurrentUser\My or PrivateCertStore: Setup will be UNSIGNED.
goto stage
:sign_helper
set SIGN=1
echo Signing with %EXPECT_THUMB% from CurrentUser\%SIGNSTORE% (no timestamp: test certificate).
"%SIGNTOOL%" sign /q /fd SHA256 /s %SIGNSTORE% /sha1 %EXPECT_THUMB% /d "Apollo x8p setup helper" "%HERE%helper\bin\apollo-helper.exe"
if errorlevel 1 goto fail

:stage
echo === 5. stage the payload ===
if not exist "%OUT%" mkdir "%OUT%"
powershell -NoProfile -ExecutionPolicy Bypass -File "%HERE%stage.ps1" -Build "%BUILD%" -Stage "%STAGE%" -Helper "%HERE%helper\bin\apollo-helper.exe" -Thumbprint %EXPECT_THUMB% -Info "%OUT%\stage-info.cmd"
if errorlevel 1 goto fail
call "%OUT%\stage-info.cmd"

echo === 6. compile ApolloSetup-%DRIVERVER%.exe ===
set "ISSLOG=%OUT%\iscc-%DRIVERVER%.log"
if exist "%OUT%\ApolloSetup-%DRIVERVER%.exe" del /q "%OUT%\ApolloSetup-%DRIVERVER%.exe"
if "%SIGN%"=="1" goto compile_signed
"%ISCC%" "/DPayload=%STAGE%" "/DDriverVer=%DRIVERVER%" "/DCertThumb=%CERTTHUMB%" "/DOutDir=%OUT%" "%HERE%ApolloSetup.iss" > "%ISSLOG%" 2>&1
goto compiled
:compile_signed
"%ISCC%" "/DPayload=%STAGE%" "/DDriverVer=%DRIVERVER%" "/DCertThumb=%CERTTHUMB%" "/DOutDir=%OUT%" /DSign "/Saviumtest=$q%SIGNTOOL%$q sign /q /fd SHA256 /s %SIGNSTORE% /sha1 %EXPECT_THUMB% /d $qApollo x8p driver setup$q $f" "%HERE%ApolloSetup.iss" > "%ISSLOG%" 2>&1
:compiled
if errorlevel 1 (
    type "%ISSLOG%"
    echo Inno Setup compile FAILED: see %ISSLOG%
    goto fail
)
echo Compile log: %ISSLOG%

echo === 7. verify the output against the payload manifest ===
powershell -NoProfile -ExecutionPolicy Bypass -File "%HERE%verify.ps1" -Stage "%STAGE%" -Files "%OUT%\ApolloSetup-%DRIVERVER%-files.txt" -CompileLog "%ISSLOG%" -Exe "%OUT%\ApolloSetup-%DRIVERVER%.exe" -Thumbprint %EXPECT_THUMB% -Signed %SIGN%
if errorlevel 1 goto fail
exit /b 0

:fail
echo BUILD FAILED
exit /b 1
