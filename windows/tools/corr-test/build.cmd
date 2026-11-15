@echo off
rem Builds and runs tools\corr-test (speaker correction, include\apollo_corr.h):
rem   corr-test.exe         driver\corr_dsp.h offline: the engine against Avium Measure's reference (ref\),
rem                         bit-identical off/bypass, crossfades, the driver's check, the pump, the cost per tick
rem   corrprofile-test.exe  app\src\corr_profile.h: the host's port of validate() against Avium Measure's own
rem                         verdicts (ref\cases.json), and what it compiles passing the driver's check
rem ref\make-ref.py regenerates ref\ with Avium Measure (not needed to build). Run from an EWDK prompt.
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
rem /fp:precise and no /arch: SSE2 doubles, no FMA contraction, as the driver is built
cl /nologo /O2 /W4 /WX /MT /fp:precise corr-test.c /Fe:corr-test.exe || goto fail
if exist corrprofile-test.cpp (
    cl /nologo /O2 /W4 /WX /MT /EHsc /std:c++20 /fp:precise corrprofile-test.cpp /Fe:corrprofile-test.exe || goto fail
)
"%~dp0corr-test.exe" "%~dp0ref" || goto fail
if exist corrprofile-test.exe (
    "%~dp0corrprofile-test.exe" "%~dp0ref" || goto fail
)
popd
exit /b 0
:fail
popd
echo CORR-TEST FAILED
exit /b 1
