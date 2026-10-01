# =====================================================================================
#  Antietam Modernization - installer
# =====================================================================================
# Unzip the release into the game folder (next to Antietam.exe) and run install.bat there.
# The release keeps ddraw.dll and smav2.ini in a "modernization" subfolder, so unzipping never
# overwrites a ddraw.dll that is already in the game folder (another wrapper, an older version):
# the installer can back it up first.
# What it does, all inside that folder:
#   1. checks that Antietam.exe is there and identifies the build
#   2. backs up any existing ddraw.dll / smav2.ini to _modernization_backup\
#   3. copies our ddraw.dll and smav2.ini (keeps an existing smav2.ini of ours)
#   4. copies YOUR Antietam.exe to Antietam_Win11.exe - unchanged, only renamed: Windows applies
#      a crashing compatibility fix to any program called "Antietam.exe"
#   5. creates desktop shortcuts: Antietam, plus South Mountain for v12.10 (v3.0 Final Patch)
#   6. writes _modernization_backup\install.json so uninstall.ps1 can undo everything
# Nothing outside the game folder is changed except the desktop shortcuts.
# =====================================================================================
param([string]$GameDir)

$ErrorActionPreference = 'Stop'
$here    = Split-Path -Parent $MyInvocation.MyCommand.Path
$src     = Join-Path $here 'modernization'                   # where the release's DLL/ini are
$newName = 'Antietam_Win11.exe'
if (-not (Test-Path (Join-Path $src 'ddraw.dll'))) {
    Write-Host "The folder 'modernization' (with ddraw.dll) is missing next to install.bat." -ForegroundColor Red
    Write-Host 'Unzip the complete release into the game folder.'
    exit 1
}

# The game folder is the folder this script is in: unzip the release INTO the game folder
# (the one with Antietam.exe, usually ...\SMA). That way several copies of the game can each
# get their own installation. -GameDir overrides it (for testing).
$game = if ($GameDir) { $GameDir.Trim('"') } else { $here }
$exe  = Join-Path $game 'Antietam.exe'
if (-not (Test-Path $exe)) {
    Write-Host "Antietam.exe not found in '$game'." -ForegroundColor Red
    Write-Host 'Unzip this release into your game folder (the folder that contains Antietam.exe,'
    Write-Host 'usually ...\Sid Meier''s Civil War Collection\SMA) and run install.bat from there.'
    exit 1
}
if (Get-Process -Name 'Antietam','Antietam_Win11' -ErrorAction SilentlyContinue) {
    Write-Host 'Please close the game first.' -ForegroundColor Red; exit 1
}

# --- identify the build (PE timestamp at offset e_lfanew+8) --------------------------------------
$bytes = [IO.File]::ReadAllBytes($exe)
$pe    = [BitConverter]::ToInt32($bytes, 0x3C)
$ts    = [BitConverter]::ToUInt32($bytes, $pe + 8)
# Supported: the exes of the two official patches. v3.0 Final Patch w/ South Mountain (exe v12.10,
# what the Civil War Collection installs) and v2.0 Beta Patch (exe v9.84, which the community uses
# for player-made battle packs). Anything else needs the player to accept the risk.
$build = switch ($ts) {
    0x398EF5B0 { 'v12.10 (v3.0 Final Patch w/ South Mountain, as in the Civil War Collection)'; break }
    0x38A45BF6 { 'v9.84 (v2.0 Beta Patch)'; break }
    default    { ('unknown (timestamp 0x{0:X8}, {1} bytes)' -f $ts, $bytes.Length) }
}
Write-Host "Game folder : $game"
Write-Host "Game build  : $build"
if ($ts -ne 0x398EF5B0 -and $ts -ne 0x38A45BF6) {
    Write-Host ''
    Write-Host 'This project needs the Antietam.exe of the v3.0 Final Patch (as in the Civil War Collection)' -ForegroundColor Yellow
    Write-Host 'or of the v2.0 Beta Patch. This Antietam.exe is neither.' -ForegroundColor Yellow
    Write-Host 'v2.0 Beta Patch: https://www.moddb.com/games/sid-meiers-gettysburg/downloads/antietam-beta-patch-v2-0'
    if ((Read-Host 'Install anyway, at your own risk? (y/n)') -ne 'y') { exit 1 }
}

# --- backup + copy --------------------------------------------------------------------------------
$backup = Join-Path $game '_modernization_backup'
New-Item -ItemType Directory -Force -Path $backup | Out-Null
$manifest = [ordered]@{ version = 1; game = $game; installed = @(); backedUp = @(); shortcuts = @() }

foreach ($f in 'ddraw.dll', 'smav2.ini') {
    $dst = Join-Path $game $f
    if ($f -eq 'smav2.ini' -and (Test-Path $dst) -and (Select-String -Path $dst -Pattern 'smav2' -Quiet)) {
        Write-Host "Keeping your existing $f"
        continue
    }
    if (Test-Path $dst) {
        Copy-Item $dst (Join-Path $backup $f) -Force
        $manifest.backedUp += $f
        Write-Host "Backed up existing $f"
    }
    Copy-Item (Join-Path $src $f) $dst -Force
    $manifest.installed += $f
}

# The original setup put Windows 98 copies of these system DLLs in the game folder. Windows loads
# DLLs from the game folder first, so they replace the real ones (no sound, instability; see
# dvwjr's "Antietam! on XP" guide). Move them into the backup; the uninstaller puts them back.
foreach ($f in 'setupapi.dll', 'cfgmgr32.dll') {
    $p = Join-Path $game $f
    if (Test-Path $p) {
        Move-Item $p (Join-Path $backup $f) -Force
        $manifest.backedUp += $f
        Write-Host "Moved old Windows 98 $f into _modernization_backup"
    }
}

Copy-Item $exe (Join-Path $game $newName) -Force
$manifest.installed += $newName
Write-Host "Created $newName (copy of your Antietam.exe, unchanged)"

# --- shortcuts ----------------------------------------------------------------------------------
$desktop = [Environment]::GetFolderPath('Desktop')
$shell   = New-Object -ComObject WScript.Shell
# v12.10 (v3.0 Final Patch) picks the campaign from its -campaign switch: 1 = Antietam,
# 3 = South Mountain. v9.84 (v2.0 Beta Patch) has no switch and reads Campaign=N from antietam.ini,
# which is where JSGME battle packs set their own campaign; so its shortcut passes nothing and plays
# whatever is installed (plain Antietam, or the active battle pack). v9.84 can't run South Mountain:
# it lists the scenarios but loads the Antietam map and quits when a battle starts (tested).
if ($ts -eq 0x398EF5B0) {
    $links = @(@{ name = 'Antietam (Win11)'; args = '-campaign 1' },
               @{ name = 'South Mountain (Win11)'; args = '-campaign 3' })
} else {
    $links = @(@{ name = 'Antietam (Win11)'; args = '' })
    Write-Host 'Note: v2.0 Beta Patch: the shortcut plays Antietam, or the battle pack enabled in JSGME.' -ForegroundColor Yellow
    Write-Host '      South Mountain needs the v3.0 Final Patch exe (v12.10).' -ForegroundColor Yellow
}
$folderName = Split-Path -Leaf $game                         # "SMA" says little: use the parent's name
if ($folderName -eq 'SMA') { $folderName = Split-Path -Leaf (Split-Path -Parent $game) }
foreach ($l in $links) {
    # Several game copies? If a shortcut with this name already points to another folder,
    # add this folder's name so both stay usable, e.g. "Antietam (Win11) - SMA test".
    $path = Join-Path $desktop ($l.name + '.lnk')
    if ((Test-Path $path) -and ($shell.CreateShortcut($path).WorkingDirectory -ne $game)) {
        $path = Join-Path $desktop ("{0} - {1}.lnk" -f $l.name, $folderName)
    }
    $s = $shell.CreateShortcut($path)
    $s.TargetPath = Join-Path $game $newName
    $s.Arguments = $l.args
    $s.WorkingDirectory = $game
    $s.IconLocation = (Join-Path $game $newName) + ',0'
    $s.Save()
    $manifest.shortcuts += $path
    Write-Host "Shortcut   : $([IO.Path]::GetFileNameWithoutExtension($path))"
}

$manifest | ConvertTo-Json | Set-Content -Encoding UTF8 (Join-Path $backup 'install.json')
Write-Host ''
Write-Host 'Done. Start the game with the desktop shortcut(s). Settings: smav2.ini in the game folder.' -ForegroundColor Green
