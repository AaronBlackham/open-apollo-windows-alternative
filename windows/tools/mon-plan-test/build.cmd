@echo off
rem Builds and runs mon-plan-test.exe (driver\monitor_plan.h: ALT, sources, cue mixes per model; the x8p's
rem monitor writes unchanged) and mon-cmds-test.exe (app\src\monitor_cmds.h). Run from an EWDK build prompt.
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
cl /nologo /W4 /WX /O2 /MT mon-plan-test.c /Fe:mon-plan-test.exe || goto fail
cl /nologo /W4 /WX /O2 /MT /EHsc /std:c++20 mon-cmds-test.cpp /Fe:mon-cmds-test.exe || goto fail
rem .\ explicitly: the EWDK prompt may set NoDefaultCurrentDirectoryInExePath
.\mon-plan-test.exe || goto fail
.\mon-cmds-test.exe || goto fail
popd
exit /b 0
:fail
popd
echo MON-PLAN-TEST FAILED
exit /b 1
