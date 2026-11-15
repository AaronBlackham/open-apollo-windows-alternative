@echo off
rem Builds and runs tools\bringup-plan-test (driver\bringup_plan.h: the x8p's bring-up plan is the
rem pre-G3 path, the Twin X Gen 1's matches docs/spec-twinx.md section 2; gate G3) from an EWDK
rem prompt or tools\build-all.cmd.
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
cl /nologo /O2 /W4 /WX /MT bringup-plan-test.c /Fe:bringup-plan-test.exe || goto fail
"%~dp0bringup-plan-test.exe" || goto fail
popd
exit /b 0
:fail
popd
echo BRINGUP-PLAN-TEST FAILED
exit /b 1
