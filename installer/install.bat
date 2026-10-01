@echo off
rem Double-click to install. Unzip the whole release into the game folder first
rem (the folder with Antietam.exe, usually ...\Sid Meier's Civil War Collection\SMA).
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0install.ps1" %*
pause
