@echo off
rem Double-click to uninstall. Run it from the game folder where install.bat was run.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0uninstall.ps1" %*
pause
