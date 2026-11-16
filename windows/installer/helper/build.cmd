@echo off
rem Builds installer\helper\bin\apollo-helper.exe at /W4 /WX. Run from an EWDK prompt, or let
rem installer\build.cmd call it. Arguments: the folder holding the packaged driver's apollo_ioctl.h
rem (ioctl_check.cpp fails the build if the IOCTLs the helper reads have changed there), and
rem optionally the defaults.json the release ships (default: installer\defaults.json), which the
rem offline test parses and prints.
setlocal
set IOCTL_INC=%~1
if "%IOCTL_INC%"=="" (
    echo usage: build.cmd ^<folder with apollo_ioctl.h^> [defaults.json]
    exit /b 1
)
if not exist "%IOCTL_INC%\apollo_ioctl.h" (
    echo No apollo_ioctl.h in %IOCTL_INC%
    exit /b 1
)
where cl.exe >nul 2>&1
if errorlevel 1 (
    echo cl.exe is not on PATH: run installer\build.cmd, or this file from an EWDK prompt.
    exit /b 1
)
rem The EWDK prompt sets the compiler up with -winsdk=none: add the user-mode SDK.
if defined EnterpriseWDK if defined WindowsSdkDir (
    set "INCLUDE=%INCLUDE%;%WindowsSdkDir%Include\%Version_Number%\ucrt;%WindowsSdkDir%Include\%Version_Number%\um;%WindowsSdkDir%Include\%Version_Number%\shared"
    set "LIB=%LIB%;%WindowsSdkDir%Lib\%Version_Number%\ucrt\x64;%WindowsSdkDir%Lib\%Version_Number%\um\x64"
    set "PATH=%PATH%;%WindowsSdkDir%bin\%Version_Number%\x64"
)

pushd "%~dp0"
if exist obj rmdir /s /q obj
if exist bin rmdir /s /q bin
mkdir obj
mkdir bin

set CFLAGS=/nologo /std:c++17 /permissive- /EHsc /W4 /WX /O2 /MT /GS /guard:cf /sdl /Zi /DUNICODE /D_UNICODE /D_WIN32_WINNT=0x0A00 /DWINVER=0x0A00 /DNTDDI_VERSION=0x0A00000C /Fo:obj\ /Fd:obj\

rc /nologo /fo obj\apollo-helper.res apollo-helper.rc || goto fail
cl %CFLAGS% /I "%IOCTL_INC%" /c ioctl_check.cpp || goto fail
cl %CFLAGS% /c main.cpp pnp.cpp audio.cpp sys.cpp units.cpp defaults_plan.cpp || goto fail

rem The unit logic (which Apollo is which, the multi-unit decisions) and defaults.json (the
rem release's own file too) run offline first: no hardware, no registry. A failure stops the
rem build. The test exe stays in obj\, never shipped.
cl %CFLAGS% /c test_units.cpp || goto fail
link /nologo /OUT:obj\test_units.exe /SUBSYSTEM:CONSOLE /DYNAMICBASE /NXCOMPAT /GUARD:CF /INCREMENTAL:NO /WX ^
     obj\test_units.obj obj\units.obj obj\defaults_plan.obj || goto fail
set "DEFAULTS_JSON=%~2"
if "%DEFAULTS_JSON%"=="" set "DEFAULTS_JSON=%~dp0..\defaults.json"
"%~dp0obj\test_units.exe" "%DEFAULTS_JSON%" || goto fail

rem /DEPENDENTLOADFLAG:0x800: the loader resolves the imported DLLs from System32 only.
rem /PDBALTPATH:%%_PDB%%: the exe names its PDB by file name only, never by this folder's full path.
link /nologo /OUT:bin\apollo-helper.exe /SUBSYSTEM:CONSOLE /DYNAMICBASE /NXCOMPAT /HIGHENTROPYVA /GUARD:CF /DEPENDENTLOADFLAG:0x800 ^
     /DEBUG /INCREMENTAL:NO /OPT:REF /OPT:ICF /PDB:obj\apollo-helper.pdb /PDBALTPATH:%%_PDB%% ^
     /MANIFEST:EMBED /MANIFESTINPUT:apollo-helper.manifest /WX ^
     obj\main.obj obj\pnp.obj obj\audio.obj obj\sys.obj obj\units.obj obj\defaults_plan.obj obj\apollo-helper.res ^
     setupapi.lib cfgmgr32.lib newdev.lib ole32.lib oleaut32.lib wbemuuid.lib crypt32.lib ncrypt.lib advapi32.lib ^
     user32.lib shell32.lib || goto fail
popd
echo Built installer\helper\bin\apollo-helper.exe
exit /b 0

:fail
set RC=%ERRORLEVEL%
popd
echo HELPER BUILD FAILED (%RC%)
exit /b 1
