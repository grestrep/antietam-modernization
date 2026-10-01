# =====================================================================================
#  Builds the player release zip:  dist\antietam-modernization-<version>.zip
# =====================================================================================
# Layout (unzipped INTO the game folder, next to Antietam.exe):
#   install.bat / install.ps1 / uninstall.bat / uninstall.ps1
#   README.txt  LICENSE.txt  CHANGELOG.txt
#   modernization\ddraw.dll  modernization\smav2.ini
# ddraw.dll and smav2.ini are in a subfolder so unzipping can never overwrite a ddraw.dll that is
# already in the game folder; install.ps1 backs that up first.
# Usage: scripts\package.ps1 [-Version 0.9.0]   (run ddraw\build.bat first)
# =====================================================================================
param([string]$Version = 'dev')
$ErrorActionPreference = 'Stop'
$root  = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$dll   = Join-Path $root 'ddraw\build\ddraw.dll'
if (-not (Test-Path $dll)) { throw "Build first: ddraw\build.bat ($dll missing)" }

$stage = Join-Path $root "dist\stage"
if (Test-Path $stage) { Remove-Item $stage -Recurse -Force }
New-Item -ItemType Directory -Force -Path (Join-Path $stage 'modernization') | Out-Null

Copy-Item $dll                                   (Join-Path $stage 'modernization\ddraw.dll')
Copy-Item (Join-Path $root 'ddraw\smav2.ini')    (Join-Path $stage 'modernization\smav2.ini')
Copy-Item (Join-Path $root 'installer\*')        $stage
Copy-Item (Join-Path $root 'LICENSE')            (Join-Path $stage 'LICENSE.txt')
Copy-Item (Join-Path $root 'CHANGELOG.md')       (Join-Path $stage 'CHANGELOG.txt')

@"
Antietam Modernization Project $Version
Unofficial Windows 10/11 fix for Sid Meier's Antietam! and South Mountain.
Not affiliated with Firaxis Games, 2K or Take-Two Interactive. You need your own copy of the game.

REQUIRES
  Sid Meier's Antietam! with the v3.0 Final Patch (as in the Civil War Collection):
  Antietam and South Mountain. With the v2.0 Beta Patch (used for battle packs): Antietam only.

INSTALL
  1. Unzip everything into your game folder - the folder that contains Antietam.exe
     (Civil War Collection: ...\Sid Meier's Civil War Collection\SMA).
  2. Double-click install.bat.
  3. Play with the desktop shortcut "Antietam (Win11)" (and "South Mountain (Win11)").

SETTINGS   smav2.ini in the game folder (Notepad) - every option is explained there.
PROBLEMS   attach smav2_ddraw.log from the game folder to an issue on the project page.
UNINSTALL  double-click uninstall.bat in the game folder.
"@ | Set-Content -Encoding UTF8 (Join-Path $stage 'README.txt')

$zip = Join-Path $root "dist\antietam-modernization-$Version.zip"
if (Test-Path $zip) { Remove-Item $zip -Force }
Compress-Archive -Path (Join-Path $stage '*') -DestinationPath $zip
$hash = (Get-FileHash $zip -Algorithm SHA256).Hash
"$hash  $(Split-Path -Leaf $zip)" | Set-Content -Encoding ASCII "$zip.sha256"
Write-Host "Created $zip"
Write-Host "SHA-256 $hash"
