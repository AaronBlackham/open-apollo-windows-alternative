@echo off
rem Builds apollo-plug.exe (docs/PLUGINS.md gate P1 host tool). Run from an EWDK prompt or tools\build-all.cmd.
setlocal

where cl.exe >nul 2>&1
if errorlevel 1 (
    echo ERROR: cl.exe is not on PATH. Run this from the EWDK prompt.
    exit /b 1
)
if defined EnterpriseWDK if defined WindowsSdkDir (
    set "INCLUDE=%INCLUDE%;%WindowsSdkDir%Include\%Version_Number%\ucrt;%WindowsSdkDir%Include\%Version_Number%\um;%WindowsSdkDir%Include\%Version_Number%\shared"
    set "LIB=%LIB%;%WindowsSdkDir%Lib\%Version_Number%\ucrt\x64;%WindowsSdkDir%Lib\%Version_Number%\um\x64"
)

pushd "%~dp0"
cl /nologo /W4 /WX /O2 /MT apollo-plug.c cfgmgr32.lib bcrypt.lib advapi32.lib /Fe:apollo-plug.exe
set BUILD_ERR=%ERRORLEVEL%
rem the offline selftest (effect measurements, page cut, readback groups; no device): .\ explicitly
set RUN_ERR=0
if "%BUILD_ERR%"=="0" .\apollo-plug.exe selftest
if "%BUILD_ERR%"=="0" set RUN_ERR=%ERRORLEVEL%
popd
if not "%BUILD_ERR%"=="0" (
    echo BUILD FAILED with exit code %BUILD_ERR%
    exit /b %BUILD_ERR%
)
if not "%RUN_ERR%"=="0" (
    echo SELFTEST FAILED
    exit /b %RUN_ERR%
)
echo Built apollo-plug.exe
exit /b 0
