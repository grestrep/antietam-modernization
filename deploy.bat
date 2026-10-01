@echo off
rem ---------------------------------------------------------------------------
rem  Developer helper: copies the freshly built ddraw.dll (+ .pdb) into a game
rem  folder that was set up with installer\install.bat.
rem    usage: deploy.bat ["game folder"]   (default: %ANTIETAM_GAME_DIR%)
rem  smav2.ini is only copied if the folder has none (your settings stay).
rem ---------------------------------------------------------------------------
setlocal
set "GAME=%~1"
if "%GAME%"=="" set "GAME=%ANTIETAM_GAME_DIR%"
if "%GAME%"=="" (echo Set ANTIETAM_GAME_DIR or pass the game folder. & exit /b 1)
cd /d "%~dp0"
tasklist /fi "imagename eq Antietam_Win11.exe" | "%SystemRoot%\System32\find.exe" /i "Antietam_Win11.exe" >nul && (echo The game is running - close it first. & exit /b 1)
if not exist "ddraw\build\ddraw.dll" (echo Build first: ddraw\build.bat & exit /b 1)
copy /y "ddraw\build\ddraw.dll" "%GAME%\ddraw.dll" >nul || exit /b 1
if exist "ddraw\build\ddraw.pdb" copy /y "ddraw\build\ddraw.pdb" "%GAME%\ddraw.pdb" >nul
if not exist "%GAME%\smav2.ini" copy /y "ddraw\smav2.ini" "%GAME%\smav2.ini" >nul
echo Deployed ddraw.dll to "%GAME%"
