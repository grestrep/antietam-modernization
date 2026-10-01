# =====================================================================================
#  Antietam Modernization - uninstaller
# =====================================================================================
# Undoes install.ps1 using _modernization_backup\install.json in the game folder:
# removes the files it installed (ddraw.dll, smav2.ini if it installed it, Antietam_Win11.exe),
# restores anything it backed up or moved aside (including old setupapi.dll / cfgmgr32.dll),
# and deletes the desktop shortcuts it created.
# Your Antietam.exe and saved games are never touched.
# =====================================================================================
param([string]$GameDir)

$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
if (-not $GameDir) { $GameDir = $here }          # run from the game folder, like install.bat
$game     = $GameDir.Trim('"')
$backup   = Join-Path $game '_modernization_backup'
$manifest = Join-Path $backup 'install.json'
if (-not (Test-Path $manifest)) { Write-Host "No installation found in '$game'." -ForegroundColor Red; exit 1 }
if (Get-Process -Name 'Antietam_Win11' -ErrorAction SilentlyContinue) { Write-Host 'Please close the game first.' -ForegroundColor Red; exit 1 }

$m = Get-Content $manifest -Raw | ConvertFrom-Json
foreach ($f in $m.installed) {
    $p = Join-Path $game $f
    if (Test-Path $p) { Remove-Item $p -Force; Write-Host "Removed  $f" }
}
foreach ($f in $m.backedUp) {
    Copy-Item (Join-Path $backup $f) (Join-Path $game $f) -Force
    Write-Host "Restored $f"
}
foreach ($s in $m.shortcuts) { if (Test-Path $s) { Remove-Item $s -Force; Write-Host "Removed  shortcut $(Split-Path -Leaf $s)" } }
foreach ($f in 'smav2_ddraw.log', 'ddraw.pdb') { $p = Join-Path $game $f; if (Test-Path $p) { Remove-Item $p -Force } }
Remove-Item $backup -Recurse -Force
Write-Host 'Uninstalled. The game is back to its original state.' -ForegroundColor Green
