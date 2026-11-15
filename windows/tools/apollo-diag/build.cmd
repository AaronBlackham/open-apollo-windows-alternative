@echo off
rem Builds apollo-diag.exe. Run from an EWDK build prompt:
rem   mount the EWDK ISO, run LaunchBuildEnv.cmd, then run this file from that window.
setlocal

where cl.exe >nul 2>&1
if errorlevel 1 (
    echo ERROR: cl.exe is not on PATH.
    echo Mount the EWDK ISO, run LaunchBuildEnv.cmd from it, then run build.cmd from that prompt.
    exit /b 1
)

rem The EWDK prompt sets up the compiler with -winsdk=none: add the user-mode SDK.
if defined EnterpriseWDK if defined WindowsSdkDir (
    set "INCLUDE=%INCLUDE%;%WindowsSdkDir%Include\%Version_Number%\ucrt;%WindowsSdkDir%Include\%Version_Number%\um;%WindowsSdkDir%Include\%Version_Number%\shared"
    set "LIB=%LIB%;%WindowsSdkDir%Lib\%Version_Number%\ucrt\x64;%WindowsSdkDir%Lib\%Version_Number%\um\x64"
)

pushd "%~dp0"
cl /nologo /W4 /WX /O2 /MT apollo-diag.c cfgmgr32.lib bcrypt.lib advapi32.lib /Fe:apollo-diag.exe
set BUILD_ERR=%ERRORLEVEL%
popd

if not "%BUILD_ERR%"=="0" (
    echo BUILD FAILED with exit code %BUILD_ERR%
    exit /b %BUILD_ERR%
)
echo Built apollo-diag.exe
exit /b 0
