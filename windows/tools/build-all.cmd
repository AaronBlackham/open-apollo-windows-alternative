@echo off
rem Builds apollo.sys, apollo-diag.exe and apollo-control.exe from a plain prompt.
rem Needs the EWDK ISO mounted (default D:). Usage: build-all.cmd [EWDK drive letter]
setlocal
set EWDK=%~1
if "%EWDK%"=="" set EWDK=D:
if not exist "%EWDK%\BuildEnv\SetupBuildEnv.cmd" (
    echo EWDK not found at %EWDK%\ - mount the ISO from Downloads\EWDK first.
    exit /b 1
)
call "%EWDK%\BuildEnv\SetupBuildEnv.cmd" amd64 >nul
set ROOT=%~dp0..

echo === sat-test (playback ring saturating add) ===
call "%ROOT%\tools\sat-test\build.cmd" || exit /b 1
echo === ident-test (model identification, Twin X G0) ===
call "%ROOT%\tools\ident-test\build.cmd" || exit /b 1
echo === strip-plan-test (input strip transactions, STRIPS.md T0) ===
call "%ROOT%\tools\strip-plan-test\build.cmd" || exit /b 1
echo === plugin-plan-test (plug-in hosting P0: paging, ledger, checks, encodings; PLUGINS.md) ===
call "%ROOT%\tools\plugin-plan-test\build.cmd" || exit /b 1
echo === bringup-plan-test (bring-up plans: x8p unchanged, Twin X G3 = spec-twinx 2) ===
call "%ROOT%\tools\bringup-plan-test\build.cmd" || exit /b 1
echo === ring-map-test (audio ring pages: SG entry i = view page i, contiguous table unchanged, G4b) ===
call "%ROOT%\tools\ring-map-test\build.cmd" || exit /b 1
echo === preamp-plan-test (preamps per model: x8p unchanged, Twin X G5 = spec-twinx 4.1) ===
call "%ROOT%\tools\preamp-plan-test\build.cmd" || exit /b 1
echo === mon-plan-test (monitor features per model: ALT, sources, cues; x8p monitor writes unchanged) ===
call "%ROOT%\tools\mon-plan-test\build.cmd" || exit /b 1
echo === agg-test (combined device G6: ASRC, clock loop, layout, Apollo Control JSON; G7: combined Windows endpoints, Align/trims vs G6, start ratios, INF; plus the hardware tools) ===
call "%ROOT%\tools\agg-test\build.cmd" || exit /b 1
echo === rate-test ([Rate] the six sample rates: tables, buffer math, strip words, combined clock loop and ASRC per rate; 48 kHz unchanged) ===
call "%ROOT%\tools\rate-test\build.cmd" || exit /b 1
echo === corr-test (speaker correction: engine vs Avium Measure, bit-identical off/bypass, crossfades, driver check, cost; the host's validate() port) ===
call "%ROOT%\tools\corr-test\build.cmd" || exit /b 1
echo === driver ===
call "%ROOT%\driver\build.cmd" Release || exit /b 1
echo === infverif ===
"%EWDK%\Program Files\Windows Kits\10\Tools\10.0.28000.0\x64\infverif.exe" /w "%ROOT%\driver\x64\Release\apollo\apollo.inf" || exit /b 1
echo === apollo-diag ===
call "%ROOT%\tools\apollo-diag\build.cmd" || exit /b 1
echo === apollo-plug (plug-in host tool, gate P1) ===
call "%ROOT%\tools\plugin-host\build.cmd" || exit /b 1
echo === plugin-recipe-test (Apollo Control's recipe builder = p1_recipe.py's, saved inserts; PLUGINS.md 5) ===
call "%ROOT%\tools\plugin-recipe-test\build.cmd" || exit /b 1
echo === loopback-test ===
call "%ROOT%\tools\loopback-test\build.cmd" || exit /b 1
echo === wasapi-loop ===
call "%ROOT%\tools\wasapi-loop\build.cmd" || exit /b 1
echo === asio driver + test host ===
call "%ROOT%\asio\build.cmd" || exit /b 1
rem A work copy under builds\ may not carry dsp\ (it is not part of the driver): skip it there.
if exist "%ROOT%\dsp\build.cmd" (
    echo === dsp ^(build + checks^) ===
    call "%ROOT%\dsp\build.cmd" || exit /b 1
)
if exist "%ROOT%\app\build.cmd" (
    echo === apollo-control ===
    call "%ROOT%\app\build.cmd" || exit /b 1
)
echo === tools manifest (SHA-256 for the elevated scripts, tools\phase1\safe-tools.ps1) ===
powershell -NoProfile -ExecutionPolicy Bypass -File "%ROOT%\tools\write-manifest.ps1" || exit /b 1
echo === all built ===
exit /b 0
