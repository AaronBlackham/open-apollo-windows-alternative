@echo off
rem Builds and runs plugin-plan-test.exe (driver\plugin_plan.h, docs/PLUGINS.md gate P0). Run from an
rem EWDK build prompt or tools\build-all.cmd.
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
cl /nologo /W4 /WX /O2 /MT plugin-plan-test.c /Fe:plugin-plan-test.exe
set BUILD_ERR=%ERRORLEVEL%
rem .\ explicitly: the EWDK prompt may set NoDefaultCurrentDirectoryInExePath
if "%BUILD_ERR%"=="0" .\plugin-plan-test.exe
set RUN_ERR=%ERRORLEVEL%
popd
if not "%BUILD_ERR%"=="0" (
    echo BUILD FAILED with exit code %BUILD_ERR%
    exit /b %BUILD_ERR%
)
exit /b %RUN_ERR%
