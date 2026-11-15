@echo off
rem Builds wasapi-loop.exe from an EWDK prompt (or tools\build-all.cmd).
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
cl /nologo /W4 /WX /O2 /MT /EHsc wasapi-loop.cpp ole32.lib avrt.lib cfgmgr32.lib /Fe:wasapi-loop.exe
set RC=%ERRORLEVEL%
popd
if not "%RC%"=="0" (echo BUILD FAILED & exit /b 1)
echo Built wasapi-loop.exe
exit /b 0
