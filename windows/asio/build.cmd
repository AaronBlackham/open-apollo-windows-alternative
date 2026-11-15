@echo off
rem Builds asio\bin\avium-asio.dll and asio\bin\asio-test.exe from an EWDK prompt
rem (or tools\build-all.cmd). Register the DLL elevated: regsvr32 asio\bin\avium-asio.dll
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
set SDK=..\third_party\asiosdk\common
set COMMON=/nologo /EHsc /O2 /MT /DUNICODE /D_UNICODE /I %SDK% /Fo:obj\

rem Steinberg's SDK files: their own warning level, not ours. register.cpp is ANSI-only.
cl %COMMON% /W1 /c %SDK%\combase.cpp %SDK%\dllentry.cpp || goto fail
cl /nologo /EHsc /O2 /MT /I %SDK% /Fo:obj\ /W1 /c %SDK%\register.cpp || goto fail
rem Our code: /W4 /WX.
cl %COMMON% /W4 /WX /c avium-asio.cpp || goto fail
link /nologo /DLL /DEF:avium-asio.def /OUT:bin\avium-asio.dll obj\avium-asio.obj obj\combase.obj obj\dllentry.obj obj\register.obj ^
     ole32.lib oleaut32.lib uuid.lib advapi32.lib user32.lib shell32.lib cfgmgr32.lib avrt.lib || goto fail
cl %COMMON% /W4 /WX asio-test.cpp /Fe:bin\asio-test.exe /link ole32.lib uuid.lib user32.lib cfgmgr32.lib || goto fail
popd
echo Built asio\bin\avium-asio.dll and asio\bin\asio-test.exe
exit /b 0
:fail
popd
echo BUILD FAILED
exit /b 1
