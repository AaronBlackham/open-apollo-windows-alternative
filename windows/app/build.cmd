@echo off
rem Builds apollo-control.exe into app\bin\ (with ui\ next to it).
rem Run from an EWDK prompt (or tools\build-all.cmd, which sets one up).
setlocal
where cl.exe >nul 2>&1
if errorlevel 1 (
    echo cl.exe is not on PATH: run tools\build-all.cmd, or this file from an EWDK prompt.
    exit /b 1
)
rem The EWDK prompt sets the compiler up with -winsdk=none: add the user-mode SDK.
if defined EnterpriseWDK if defined WindowsSdkDir (
    set "INCLUDE=%INCLUDE%;%WindowsSdkDir%Include\%Version_Number%\ucrt;%WindowsSdkDir%Include\%Version_Number%\um;%WindowsSdkDir%Include\%Version_Number%\shared;%WindowsSdkDir%Include\%Version_Number%\winrt"
    set "LIB=%LIB%;%WindowsSdkDir%Lib\%Version_Number%\ucrt\x64;%WindowsSdkDir%Lib\%Version_Number%\um\x64"
    set "PATH=%PATH%;%WindowsSdkDir%bin\%Version_Number%\x64"
)

pushd "%~dp0"
if not exist bin mkdir bin
if not exist obj mkdir obj

rc /nologo /fo obj\app.res res\app.rc || goto fail
rem A running Apollo Control keeps bin\apollo-control.exe open, so the link cannot overwrite it. Windows does let a
rem running .exe be renamed: move it aside to apollo-control.old.exe (the running app carries on from that file),
rem link a fresh apollo-control.exe, and the next start from the same path (Start menu, tray autostart) runs the new
rem build. The .old file is deleted here once nothing runs from it any more.
if exist bin\apollo-control.old.exe del /q bin\apollo-control.old.exe >nul 2>&1
if exist bin\apollo-control.exe move /y bin\apollo-control.exe bin\apollo-control.old.exe >nul 2>&1
if exist bin\apollo-control.exe (
    echo bin\apollo-control.exe is in use and could not be moved aside ^(an older apollo-control.old.exe still runs^).
    goto fail
)
cl /nologo /std:c++20 /EHsc /W4 /WX /O2 /MT /DUNICODE /D_UNICODE ^
   /I third_party\webview2\include /Fo:obj\ src\main.cpp obj\app.res ^
   /Fe:bin\apollo-control.exe ^
   /link /SUBSYSTEM:WINDOWS /MANIFEST:EMBED /MANIFESTINPUT:res\app.manifest ^
   third_party\webview2\x64\WebView2LoaderStatic.lib ^
   user32.lib gdi32.lib shell32.lib ole32.lib oleaut32.lib advapi32.lib cfgmgr32.lib setupapi.lib ^
   dwmapi.lib shlwapi.lib version.lib bcrypt.lib dbghelp.lib || goto linkfail
if exist bin\apollo-control.old.exe del /q bin\apollo-control.old.exe >nul 2>&1
if exist bin\apollo-control.old.exe echo Apollo Control is running from the previous build: the new one starts next time it is opened.

if exist bin\ui rmdir /s /q bin\ui
xcopy /e /i /q /y ui bin\ui >nul || goto fail
rem [Plug-ins] the parameter planner Apollo Control starts on demand (our code; it reads the local maps)
copy /y ..\tools\plugin-host\plughelper.py bin\plughelper.py >nul || goto fail
popd
echo Built app\bin\apollo-control.exe
exit /b 0

:linkfail
rem a failed compile or link: put the previous build back so bin\ always has an apollo-control.exe
set RC=%ERRORLEVEL%
if not exist bin\apollo-control.exe if exist bin\apollo-control.old.exe move /y bin\apollo-control.old.exe bin\apollo-control.exe >nul 2>&1
popd
echo BUILD FAILED (%RC%)
exit /b 1

:fail
set RC=%ERRORLEVEL%
popd
echo BUILD FAILED (%RC%)
exit /b 1
