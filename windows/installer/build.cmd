@echo off
rem Builds installer\out\ApolloSetup-<DriverVer>.exe from a driver build plus app\bin. See README.md.
rem   build.cmd [build folder] [EWDK drive letter] [release]
rem   build folder: a frozen build (package\, bin\, src\include\) or a built source tree (driver\x64\Release\apollo\,
rem                 tools\, asio\bin\, include\). Default: this source tree (the last tools\build-all.cmd run).
rem   EWDK drive:   default D:
rem   release:      the leak scan (scan-release.ps1) fails the build instead of warning. release.cmd passes it; a
rem                 public release is built by release.cmd only (in a folder outside C:\Users).
rem   A build folder with UNSAFE.txt (before the saturating-add fix) is refused.
rem The payload is UNSIGNED: no certificate, no signature of anyone's key (stage.ps1 strips and checks). Setup and the
rem helper are unsigned too. Setup signs the driver on the user's PC (sign-local.ps1).
rem Needs: the EWDK ISO mounted; Inno Setup in tools\inno-6.7.4 (fetched and verified on first use if missing).
rem Writes only below installer\ (helper\bin, helper\obj, tools\, out\).
setlocal EnableExtensions
set "HERE=%~dp0"
set "ROOT=%~dp0.."
set "BUILD=%~1"
if "%BUILD%"=="" set "BUILD=%ROOT%"
set "EWDK=%~2"
if "%EWDK%"=="" set "EWDK=D:"
set "MODE=%~3"
set "INNO_VER=6.7.4"
set "ISCC=%HERE%tools\inno-%INNO_VER%\ISCC.exe"
set "OUT=%HERE%out"
set "STAGE=%OUT%\stage"

if exist "%BUILD%\package\apollo.inf" goto have_build
if exist "%BUILD%\driver\x64\Release\apollo\apollo.inf" goto have_build
echo No driver build at "%BUILD%" ^(expected package\apollo.inf, or driver\x64\Release\apollo\apollo.inf: run tools\build-all.cmd^).
exit /b 1
:have_build
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

echo === 3. apollo-helper (/W4 /WX, offline unit and defaults.json tests) ===
set "IOCTL_INC=%BUILD%\src\include"
if not exist "%IOCTL_INC%\apollo_ioctl.h" set "IOCTL_INC=%BUILD%\include"
if not exist "%IOCTL_INC%\apollo_ioctl.h" set "IOCTL_INC=%ROOT%\include"
echo IOCTL layout checked against %IOCTL_INC%\apollo_ioctl.h
set "DEFAULTS_JSON=%HERE%defaults.json"
if exist "%BUILD%\package\apollo.inf" if exist "%BUILD%\defaults.json" set "DEFAULTS_JSON=%BUILD%\defaults.json"
call "%HERE%helper\build.cmd" "%IOCTL_INC%" "%DEFAULTS_JSON%"
if errorlevel 1 goto fail

echo === 4. stage the payload (unsigned driver package) ===
if not exist "%OUT%" mkdir "%OUT%"
powershell -NoProfile -ExecutionPolicy Bypass -File "%HERE%stage.ps1" -Build "%BUILD%" -Stage "%STAGE%" -Helper "%HERE%helper\bin\apollo-helper.exe" -Info "%OUT%\stage-info.cmd" -Ewdk "%EWDK%"
if errorlevel 1 goto fail
call "%OUT%\stage-info.cmd"

echo === 5. compile ApolloSetup-%DRIVERVER%.exe ===
set "ISSLOG=%OUT%\iscc-%DRIVERVER%.log"
if exist "%OUT%\ApolloSetup-%DRIVERVER%.exe" del /q "%OUT%\ApolloSetup-%DRIVERVER%.exe"
"%ISCC%" "/DPayload=%STAGE%" "/DDriverVer=%DRIVERVER%" "/DOutDir=%OUT%" "%HERE%ApolloSetup.iss" > "%ISSLOG%" 2>&1
if errorlevel 1 (
    type "%ISSLOG%"
    echo Inno Setup compile FAILED: see %ISSLOG%
    goto fail
)
echo Compile log: %ISSLOG%

echo === 6. verify the output against the payload manifest ===
powershell -NoProfile -ExecutionPolicy Bypass -File "%HERE%verify.ps1" -Stage "%STAGE%" -Files "%OUT%\ApolloSetup-%DRIVERVER%-files.txt" -CompileLog "%ISSLOG%" -Exe "%OUT%\ApolloSetup-%DRIVERVER%.exe"
if errorlevel 1 goto fail

echo === 7. nothing local in the payload or Setup (scan-release.ps1) ===
powershell -NoProfile -ExecutionPolicy Bypass -File "%HERE%scan-release.ps1" -Paths "%STAGE%","%OUT%\ApolloSetup-%DRIVERVER%.exe"
if errorlevel 1 (
    if /i "%MODE%"=="release" (
        echo The release build embeds local identifiers ^(above^): FAILED.
        goto fail
    )
    echo WARNING: this Setup embeds local identifiers ^(above^). Fine for a test on this PC; never publish it.
    echo A public release is built by installer\release.cmd, in a folder outside C:\Users.
)
exit /b 0

:fail
echo BUILD FAILED
exit /b 1
