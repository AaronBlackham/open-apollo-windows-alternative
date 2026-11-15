@echo off
rem Builds and runs plugin-recipe-test.exe: Apollo Control's recipe builder (app\src\plugin_host.h)
rem against p1_recipe.py's golden text for synthetic bundles (no UA data). Run from an EWDK prompt
rem or tools\build-all.cmd. The comparison on this PC's real bundles is compare.ps1 (local only).
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
cl /nologo /std:c++20 /EHsc /W4 /WX /O2 /MT plugin-recipe-test.cpp bcrypt.lib /Fe:plugin-recipe-test.exe
set BUILD_ERR=%ERRORLEVEL%
set RUN_ERR=0
if "%BUILD_ERR%"=="0" .\plugin-recipe-test.exe selftest
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
exit /b 0
