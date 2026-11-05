# Dragon Flight - one-command installer for Windows.
#
# Paste this into PowerShell (Start menu -> type "PowerShell" -> Enter):
#
#   irm https://raw.githubusercontent.com/faarisaahmed/dragon-flight-ue5/main/Install.ps1 | iex
#
# It downloads the project into C:\DragonFlightBuild, builds the game (Setup.bat),
# installs it to %USERPROFILE%\Games\Dragon Flight with a desktop shortcut, then
# deletes the build folder. It can also remove Unreal Engine and Visual Studio
# afterwards. If anything stops it, pasting the same command again carries on
# where it left off.

$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'   # Invoke-WebRequest is very slow with the progress bar

$Repo  = 'faarisaahmed/dragon-flight-ue5'
$Build = 'C:\DragonFlightBuild'            # short path: Unreal hits Windows' path-length limit in deep folders
$Game  = Join-Path $env:USERPROFILE 'Games\Dragon Flight'
$Mark  = Join-Path $env:LOCALAPPDATA 'DragonFlightSetup'
New-Item -ItemType Directory -Force -Path $Mark | Out-Null

function Say($t, $c = 'White') { Write-Host $t -ForegroundColor $c }
function Remove-Tree($Path) {
    # robocopy /MIR from an empty folder copes with paths longer than 260 characters.
    if (-not (Test-Path -LiteralPath $Path)) { return }
    $empty = Join-Path $env:TEMP ('df_empty_' + [guid]::NewGuid())
    New-Item -ItemType Directory -Path $empty | Out-Null
    robocopy $empty $Path /MIR /NFL /NDL /NJH /NJS /NP | Out-Null
    Remove-Item -LiteralPath $Path -Recurse -Force -ErrorAction SilentlyContinue
    Remove-Item -LiteralPath $empty -Force -ErrorAction SilentlyContinue
}
function Find-VS {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (-not (Test-Path $vswhere)) { return $null }
    $p = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if ($p) { return $p } else { return $null }
}
function Find-UE {
    foreach ($v in '5.9', '5.8') {
        $d = "C:\Program Files\Epic Games\UE_$v"
        if (Test-Path "$d\Engine\Binaries\Win64\UnrealEditor.exe") { return $d }
    }
    $lid = 'C:\ProgramData\Epic\UnrealEngineLauncher\LauncherInstalled.dat'
    if (Test-Path $lid) {
        $j = Get-Content -Raw $lid | ConvertFrom-Json
        $e = $j.InstallationList | Where-Object { $_.AppName -match '^UE_5\.(8|9)' } | Sort-Object AppName -Descending | Select-Object -First 1
        if ($e -and (Test-Path "$($e.InstallLocation)\Engine\Binaries\Win64\UnrealEditor.exe")) { return $e.InstallLocation }
    }
    return $null
}

Say ''
Say '  ==============================================' Cyan
Say '    Dragon Flight - installer' Cyan
Say '    First time: 1-2 hours. Leave this window open.' Cyan
Say '  ==============================================' Cyan
Say ''

# --- Room on the disk: ~45 GB while building (+40 GB if Unreal still has to be installed).
$free = [math]::Round((Get-PSDrive C).Free / 1GB)
if ($free -lt 50) {
    Say "Only $free GB free on C:. Building needs about 50 GB for a while (it is all deleted at the end)." Yellow
    if ((Read-Host 'Try anyway? (Y/N)') -notmatch '^[Yy]') { return }
}

# Remember whether Visual Studio was already here, so we only ever remove what we installed.
if (-not (Test-Path "$Mark\vs-state.txt")) {
    if (Find-VS) { 'had' | Set-Content "$Mark\vs-state.txt" } else { 'installed-by-us' | Set-Content "$Mark\vs-state.txt" }
}

# --- Get (or update) the project.
Say '=== Downloading the project ===' Green
$zip = Join-Path $env:TEMP 'dragon-flight.zip'
$src = Join-Path $env:TEMP 'dragon-flight-src'
Invoke-WebRequest "https://github.com/$Repo/archive/refs/heads/main.zip" -OutFile $zip -UseBasicParsing
Remove-Tree $src
Expand-Archive $zip -DestinationPath $src -Force
$top = Get-ChildItem $src | Select-Object -First 1
robocopy $top.FullName $Build /E /NFL /NDL /NJH /NJS /NP | Out-Null   # over an old build: keeps finished steps
Remove-Item $zip -Force
Remove-Tree $src

# --- Build it.
$env:DF_NOPAUSE = '1'
& cmd.exe /c "$Build\Setup.bat"
$code = $LASTEXITCODE

if ($code -eq 2) {
    Say ''
    Say 'One thing to do by hand first: install Unreal Engine 5.8.' Yellow
    Say '  1. Open the Epic Games Launcher (just installed if you did not have it) and sign in.'
    Say '  2. Unreal Engine -> Library -> the + next to "Engine Versions" -> 5.8 -> Install.'
    Say '     (Options: untick Starter Content, Templates, Engine Source, debug symbols and the'
    Say '      Android / iOS / Linux platforms - it saves about 20 GB.)'
    Say '  3. When it has finished, paste the same command in PowerShell again.'
    return
}
if ($code -ne 0) {
    Say ''
    Say 'Something went wrong (see above). Paste the same command again - it picks up where it stopped.' Red
    Say "If it fails the same way, send Faaris a screenshot of this window and the newest file in $Build\Saved\Logs." Red
    return
}

# --- Done: clean up.
Say ''
Say '=== The game is installed. Cleaning up the build files ===' Green
Remove-Tree $Build
Remove-Tree (Join-Path $env:LOCALAPPDATA 'UnrealEngine\Common\DerivedDataCache')
Say "Done. Dragon Flight is on your desktop (the game is in $Game, about 4 GB)." Green
Say ''
$ans = Read-Host 'Also remove Unreal Engine and Visual Studio to free about 50 GB? You only need them to build a newer version. (Y/N)'
if ($ans -match '^[Yy]') {
    $ue = Find-UE
    if ($ue) {
        Say "Removing Unreal Engine ($ue) - say Yes if Windows asks."
        Start-Process powershell -Verb RunAs -Wait -ArgumentList "-NoProfile -Command Remove-Item -LiteralPath '$ue' -Recurse -Force"
        Say '  (If the Epic launcher still lists 5.8, click its little arrow -> Remove.)'
    }
    if ((Get-Content "$Mark\vs-state.txt") -eq 'installed-by-us') {
        Say 'Removing Visual Studio - say Yes if Windows asks.'
        winget uninstall --id Microsoft.VisualStudio.2022.Community -e --silent
    } else {
        Say 'Visual Studio was already on this PC before, so it stays.'
    }
}
Remove-Item -LiteralPath $Mark -Recurse -Force -ErrorAction SilentlyContinue

Say ''
Say 'Starting Dragon Flight. F1 shows the controls, F2 graphics, F5 resolution.' Cyan
Start-Process (Join-Path $Game 'DragonFlight.exe')
