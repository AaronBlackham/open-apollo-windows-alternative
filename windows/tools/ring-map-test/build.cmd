@echo off
rem Builds and runs tools\ring-map-test (driver\ring_map.h: the audio rings' SG entry i and linear
rem view page i are the same memory, contiguous or one common buffer per page; the contiguous SG
rem table is the pre-G4b one; G4b) from an EWDK prompt or tools\build-all.cmd.
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
cl /nologo /O2 /W4 /WX /MT ring-map-test.c /Fe:ring-map-test.exe || goto fail
"%~dp0ring-map-test.exe" || goto fail
popd
exit /b 0
:fail
popd
echo RING-MAP-TEST FAILED
exit /b 1
