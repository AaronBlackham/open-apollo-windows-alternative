@echo off
rem Builds and runs tools\ident-test (driver\apollo_model.h + apollo_unit.h: model identification and
rem per-unit Parameters, docs/spec-twinx.md gate G0) from an EWDK prompt or tools\build-all.cmd.
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
cl /nologo /O2 /W4 /WX /MT ident-test.c /Fe:ident-test.exe || goto fail
"%~dp0ident-test.exe" || goto fail
popd
exit /b 0
:fail
popd
echo IDENT-TEST FAILED
exit /b 1
