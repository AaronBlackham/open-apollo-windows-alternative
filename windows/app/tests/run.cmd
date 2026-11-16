@echo off
rem UI checks for Apollo Control (demo model and hardware message path). Needs Node + Playwright.
rem Set PLAYWRIGHT_DIR if Playwright is not in the Avium Invoid repo.
node "%~dp0interact.js" %1 || exit /b 1
node "%~dp0host-path.js" %1 || exit /b 1
node "%~dp0status.js" %1 || exit /b 1
node "%~dp0units.js" %1 || exit /b 1
node "%~dp0control-units.js" %1 || exit /b 1
node "%~dp0cues.js" %1 || exit /b 1
node "%~dp0mix.js" %1 || exit /b 1
node "%~dp0correction.js" %1 || exit /b 1
node "%~dp0plugins.js" %1 || exit /b 1
node "%~dp0looks.js" %1 || exit /b 1
node "%~dp0rate.js" %1 || exit /b 1
node "%~dp0signatures.js" %1 || exit /b 1
node "%~dp0endpoints.js" %1 || exit /b 1
node "%~dp0sessions.js" %1 || exit /b 1
node "%~dp0undo.js" %1 || exit /b 1
