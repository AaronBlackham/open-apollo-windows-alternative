@echo off
rem Builds dsp\bin\dsp-test.exe and dsp\bin\dsp-render.exe, then runs the checks.
rem Run from an EWDK prompt (or tools\build-all.cmd, which sets one up).
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
if not exist bin mkdir bin
if not exist obj mkdir obj
set CFLAGS=/nologo /std:c11 /W4 /WX /O2 /MT /Fo:obj\
cl %CFLAGS% avdsp.c tools\dsp-test.c /Fe:bin\dsp-test.exe || goto fail
cl %CFLAGS% avdsp.c tools\wav.c tools\dsp-render.c /Fe:bin\dsp-render.exe || goto fail
popd
"%~dp0bin\dsp-test.exe" || exit /b 1
exit /b 0
:fail
popd
echo BUILD FAILED
exit /b 1
