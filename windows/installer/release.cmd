@echo off
rem release.cmd: builds a public release of ApolloSetup in a neutral folder, so nothing local ends up in it.
rem   installer\release.cmd [release folder] [EWDK drive letter]
rem   release folder: default C:\apollo-release (must be outside C:\Users and outside this source tree)
rem   EWDK drive:     default D:
rem
rem   1. Copies the source folders tools\build-all.cmd needs (driver include app asio tools dsp installer third_party
rem      docs) into <release folder>\src. Never copied: build outputs (bin, obj, driver\x64, installer\out), logs,
rem      per-PC captures (*.defaults.json, *.used, *.aggconfig.bin), tools\bin, and the folders outside that list
rem      (builds, reference, ua-files, plugins with its bundles, p1, store, downloads, work and specs). Of plugins only
rem      plugins\tools is copied ([PlugSetup]: the plug-in setup's and the mapper's source code).
rem   2. Copies the Inno Setup this source tree already fetched (installer\tools\inno-6.7.4): nothing is downloaded.
rem   3. Puts release-nosign.targets there as Directory.Build.targets: the WDK test-signs nothing in that tree.
rem   4. Runs tools\build-all.cmd there (every offline suite runs), then installer\build.cmd there in release mode:
rem      the unsigned payload check (stage.ps1) and the leak scan (scan-release.ps1: no C:\Users\, user name, PC
rem      name, test certificate, unit key or device instance ID in any payload file or in Setup) fail the build.
rem   5. Copies ApolloSetup-<DriverVer>.exe and its .sha256 to installer\out\release\ in this source tree.
rem Binaries embed their PDB paths: built in <release folder>\src, they name that folder, never a user folder.
setlocal EnableExtensions
set "HERE=%~dp0"
for %%I in ("%HERE%..") do set "SRC=%%~fI"
set "REL=%~1"
if "%REL%"=="" set "REL=C:\apollo-release"
for %%I in ("%REL%") do set "REL=%%~fI"
set "EWDK=%~2"
if "%EWDK%"=="" set "EWDK=D:"
set "INNO_VER=6.7.4"
set "MARK=%REL%\.apollo-release"

rem ---- where it may build (the paths reach PowerShell through the environment, never through its command line)
powershell -NoProfile -Command "$r = $env:REL.TrimEnd('\') + '\'; foreach ($bad in @('C:\Users\', ($env:USERPROFILE.TrimEnd('\') + '\'))) { if ($r.StartsWith($bad, [StringComparison]::OrdinalIgnoreCase)) { exit 2 } }; if ($r.StartsWith($env:SRC.TrimEnd('\') + '\', [StringComparison]::OrdinalIgnoreCase)) { exit 3 }; exit 0"
if errorlevel 3 (
    echo Refusing "%REL%": it is inside the source tree "%SRC%".
    exit /b 1
)
if errorlevel 2 (
    echo Refusing "%REL%": a release must be built outside C:\Users ^(its binaries name the folder they were built in^).
    exit /b 1
)
if errorlevel 1 (
    echo Could not check the release folder.
    exit /b 1
)
if exist "%REL%\" if not exist "%MARK%" (
    echo Refusing "%REL%": it exists and was not made by release.cmd ^(no .apollo-release in it^). Pick another folder.
    exit /b 1
)
if not exist "%EWDK%\BuildEnv\SetupBuildEnv.cmd" (
    echo EWDK not found at %EWDK%\ - mount the ISO, or pass its drive letter as the second argument.
    exit /b 1
)
if not exist "%SRC%\installer\tools\inno-%INNO_VER%\ISCC.exe" (
    echo No Inno Setup %INNO_VER% in %SRC%\installer\tools: run installer\get-inno.ps1 once ^(it downloads and verifies it^).
    exit /b 1
)

echo === 1. copy the sources to %REL%\src ===
if exist "%REL%\src" rmdir /s /q "%REL%\src"
if exist "%REL%\src" (
    echo Could not empty %REL%\src ^(a file in use?^).
    goto fail
)
mkdir "%REL%\src" || goto fail
echo made by installer\release.cmd; release.cmd deletes and rebuilds src\ here> "%MARK%"
for %%D in (driver include app asio tools dsp installer third_party docs LICENSES) do (
    robocopy "%SRC%\%%D" "%REL%\src\%%D" /E /NFL /NDL /NJH /NJS /NP ^
        /XD "%SRC%\driver\x64" "%SRC%\app\bin" "%SRC%\app\obj" "%SRC%\asio\bin" "%SRC%\asio\obj" "%SRC%\dsp\bin" ^
            "%SRC%\dsp\obj" "%SRC%\installer\helper\bin" "%SRC%\installer\helper\obj" "%SRC%\installer\out" ^
            "%SRC%\installer\tools" "%SRC%\tools\bin" node_modules shots __pycache__ .vs ^
        /XF *.log *.exe *.dll *.sys *.cat *.cer *.pfx *.pdb *.obj *.ilk *.exp *.res *.tlog *.defaults.json *.used ^
            *.aggconfig.bin >nul
    if errorlevel 8 (
        echo robocopy failed on %%D
        goto fail
    )
)
rem [PlugSetup] plugins\tools only: the plug-in setup's and the mapper's source (stage.ps1 takes source files only).
rem Never bundles, store, specs, downloads, work, p1 or .venv: they hold or come from the developer's UAD install.
robocopy "%SRC%\plugins\tools" "%REL%\src\plugins\tools" /E /NFL /NDL /NJH /NJS /NP /XD __pycache__ /XF *.pyc *.log >nul
if errorlevel 8 (
    echo robocopy failed on plugins\tools
    goto fail
)
robocopy "%SRC%\installer\tools\inno-%INNO_VER%" "%REL%\src\installer\tools\inno-%INNO_VER%" /E /NFL /NDL /NJH /NJS /NP >nul
if errorlevel 8 (
    echo robocopy failed on Inno Setup
    goto fail
)
copy /y "%HERE%release-nosign.targets" "%REL%\src\Directory.Build.targets" >nul || goto fail
echo Copied to %REL%\src

echo === 2. build everything there (tools\build-all.cmd %EWDK%) ===
call "%REL%\src\tools\build-all.cmd" %EWDK%
if errorlevel 1 (
    echo tools\build-all.cmd FAILED in %REL%\src
    goto fail
)

echo === 3. the installer, release mode ===
call "%REL%\src\installer\build.cmd" "%REL%\src" %EWDK% release
if errorlevel 1 goto fail
call "%REL%\src\installer\out\stage-info.cmd"
set "EXE=ApolloSetup-%DRIVERVER%.exe"
if not exist "%REL%\src\installer\out\%EXE%" (
    echo No %EXE% in %REL%\src\installer\out
    goto fail
)

echo === 4. copy the result to installer\out\release ===
if not exist "%SRC%\installer\out\release" mkdir "%SRC%\installer\out\release"
copy /y "%REL%\src\installer\out\%EXE%" "%SRC%\installer\out\release\%EXE%" >nul || goto fail
copy /y "%REL%\src\installer\out\%EXE%.sha256" "%SRC%\installer\out\release\%EXE%.sha256" >nul || goto fail
set "RELEXE=%SRC%\installer\out\release\%EXE%"
powershell -NoProfile -Command "$f = Get-Item -LiteralPath $env:RELEXE; $h = (Get-FileHash -Algorithm SHA256 -LiteralPath $f.FullName).Hash.ToLowerInvariant(); $want = ((Get-Content -LiteralPath ($f.FullName + '.sha256')) -split ' ')[0]; if ($h -ne $want) { Write-Host 'SHA-256 of the copy does not match'; exit 1 }; Write-Host ('Release:  ' + $f.FullName); Write-Host ('Size:     {0:N0} bytes' -f $f.Length); Write-Host ('SHA-256:  ' + $h)"
if errorlevel 1 goto fail
echo RELEASE BUILT
exit /b 0

:fail
echo RELEASE FAILED
exit /b 1
