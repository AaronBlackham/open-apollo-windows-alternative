@echo off
rem Builds and runs tools\rate-test ([Rate] driver\rate_plan.h: the rate table, the buffer math per rate,
rem the strip words per rate, the combined device's clock loop and ASRC per rate) from an EWDK prompt or build-all.
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
cl /nologo /O2 /W4 /WX /MT rate-test.c /Fe:rate-test.exe || goto fail
"%~dp0rate-test.exe" || goto fail
popd
exit /b 0
:fail
popd
echo RATE-TEST FAILED
exit /b 1
