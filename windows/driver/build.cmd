@echo off
rem Build apollo.sys (test-signed) from an EWDK prompt.
rem   1. Mount the EWDK ISO and run LaunchBuildEnv.cmd from its root.
rem   2. In that prompt: cd to this folder and run build.cmd [Debug|Release]
setlocal
set CFG=%~1
if "%CFG%"=="" set CFG=Release

where msbuild >nul 2>nul
if errorlevel 1 (
    echo msbuild.exe is not on PATH.
    echo Run this from the EWDK prompt: mount the ISO, run LaunchBuildEnv.cmd, then build.cmd again.
    exit /b 1
)

pushd "%~dp0"
msbuild apollo.vcxproj /nologo /m /p:Configuration=%CFG% /p:Platform=x64 /p:RunCodeAnalysis=true /v:minimal
set RC=%ERRORLEVEL%
popd
if not "%RC%"=="0" (
    echo BUILD FAILED ^(%RC%^)
    exit /b %RC%
)
echo.
echo Driver package: %~dp0x64\%CFG%\apollo\
endlocal
