@echo off
rem Builds and runs tools\sat-test (driver\apollo_sat.h) from an EWDK prompt or tools\build-all.cmd.
setlocal
where cl.exe >nul 2>&1
if errorlevel 1 (
    echo cl.exe is not on PATH: run tools\build-all.cmd, or this file from an EWDK prompt.
    exit /b 1
)
if defined EnterpriseWDK if defined WindowsSdkDir (
    set "INCLUDE=%INCLUDE%;%WindowsSdkDir%Include\%Version_Number%\ucrt;%WindowsSdkDir%Include\%Version_Number%\um;%WindowsSdkDir%Include\%Version_Number%\shared"
    set "LIB=%LIB%;%WindowsSdkDir%Lib\%Version_Number%\ucrt\x64;%WindowsSdkDir%Lib\%Version_Number%\um\x64"
)
pushd "%~dp0"
cl /nologo /O2 /W4 /WX sat-test.c /Fe:sat-test.exe || goto fail
"%~dp0sat-test.exe" || goto fail
popd
exit /b 0
:fail
popd
echo SAT-TEST FAILED
exit /b 1
