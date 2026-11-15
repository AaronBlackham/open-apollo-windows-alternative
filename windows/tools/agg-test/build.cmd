@echo off
rem Builds tools\agg-test (gate G6, the combined device) from an EWDK prompt or tools\build-all.cmd:
rem   asrc-test.exe  offline checks of driver\apollo_agg.h (ASRC, clock loop, layout); run here
rem   agg-drift.exe  measures the drift between the units (hardware; not run here)
rem   agg-test.exe   the combined device's IOCTLs: state, layout, config, clock probe (hardware)
rem   aggjson-test.exe  offline checks of app\src\agg_json.h (Apollo Control's config JSON); run here
rem   aggep-test.exe  [G7] offline checks of the combined Windows endpoints, Align/trims (against the frozen
rem                  G6 header, ref\apollo_agg_g6.h), start ratios, and driver\apollo.inf; run here
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
cl /nologo /O2 /W4 /WX /MT asrc-test.c /Fe:asrc-test.exe || goto fail
cl /nologo /O2 /W4 /WX /MT agg-drift.c /Fe:agg-drift.exe /link cfgmgr32.lib winmm.lib || goto fail
cl /nologo /O2 /W4 /WX /MT agg-test.c /Fe:agg-test.exe /link cfgmgr32.lib || goto fail
cl /nologo /O2 /W4 /WX /MT /EHsc /std:c++20 aggjson-test.cpp /Fe:aggjson-test.exe || goto fail
cl /nologo /O2 /W4 /WX /MT aggep-test.c ref-g6.c /Fe:aggep-test.exe || goto fail
"%~dp0asrc-test.exe" || goto fail
"%~dp0aggjson-test.exe" || goto fail
"%~dp0aggep-test.exe" "%~dp0..\..\driver\apollo.inf" || goto fail
popd
exit /b 0
:fail
popd
echo AGG-TEST FAILED
exit /b 1
