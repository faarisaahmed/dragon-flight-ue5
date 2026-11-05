# Dragon Flight - one-command installer for Windows.
#
# Paste this into PowerShell (Start menu -> type "PowerShell" -> Enter):
#
#   irm https://raw.githubusercontent.com/faarisaahmed/dragon-flight-ue5/main/Install.ps1 | iex
#
# It downloads the project into C:\DragonFlightBuild, builds the game (Setup.bat)
# and installs it to %USERPROFILE%\Games\Dragon Flight with a desktop shortcut.
# The build folder is kept, so running the same command again for a newer
# version only rebuilds what changed (minutes, not hours). If anything stops it,
# pasting the same command again carries on where it left off.

$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'   # Invoke-WebRequest is very slow with the progress bar

$Repo  = 'faarisaahmed/dragon-flight-ue5'
$Build = 'C:\DragonFlightBuild'            # short path: Unreal hits Windows' path-length limit in deep folders
$Game  = Join-Path $env:USERPROFILE 'Games\Dragon Flight'

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
    foreach ($v in '5.8', '5.9') {
        $d = "C:\Program Files\Epic Games\UE_$v"
        if (Test-Path "$d\Engine\Binaries\Win64\UnrealEditor.exe") { return $d }
    }
    $lid = 'C:\ProgramData\Epic\UnrealEngineLauncher\LauncherInstalled.dat'
    if (Test-Path $lid) {
        $j = Get-Content -Raw $lid | ConvertFrom-Json
        foreach ($v in '5.8', '5.9') {
            $e = $j.InstallationList | Where-Object { $_.AppName -eq "UE_$v" } | Select-Object -First 1
            if ($e -and (Test-Path "$($e.InstallLocation)\Engine\Binaries\Win64\UnrealEditor.exe")) { return $e.InstallLocation }
        }
    }
    return $null
}

function Send-SetupReport($OutFile, $BuildId) {
    $user = $env:USERNAME
    function Clean($t) { if ($user.Length -ge 2) { $t -replace [regex]::Escape($user), '<user>' } else { $t } }
    $lines = @(); if (Test-Path $OutFile) { $lines = Get-Content $OutFile }
    # Error lines first (compiler, UAT, Python), then how the output ended.
    $errs = $lines | Where-Object { $_ -match '(error|Error|ERROR|failed|Failed|Exception|Traceback|not found|cannot|denied)' } | Select-Object -Last 40
    $tail = $lines | Select-Object -Last 40
    $gpu = (Get-CimInstance Win32_VideoController | ForEach-Object { "$($_.Name) (driver $($_.DriverVersion))" }) -join '; '
    $os  = (Get-CimInstance Win32_OperatingSystem).Caption
    $ram = [math]::Round((Get-CimInstance Win32_ComputerSystem).TotalPhysicalMemory / 1GB)
    $cpu = (Get-CimInstance Win32_Processor | Select-Object -First 1).Name
    $ue  = Find-UE; $vs = Find-VS
    $free = [math]::Round((Get-PSDrive C).Free / 1GB)
    $body = @"
**Setup failed** (sent from the installer)

- Build: ``$BuildId``
- OS: $os
- CPU: $cpu, RAM: $ram GB, free on C: $free GB
- GPU: $gpu
- Unreal: $ue
- Visual Studio: $vs

**Errors**
``````
$(($errs -join "`n"))
``````
**End of the output**
``````
$(($tail -join "`n"))
``````
"@
    $body = Clean $body
    # GitHub refuses very long links: trim until it fits.
    $enc = [uri]::EscapeDataString($body)
    while ($enc.Length -gt 7000 -and $body.Length -gt 300) {
        $body = $body.Substring(0, [int]($body.Length * 0.85)) + "`n...(cut to fit)`n``````"
        $enc = [uri]::EscapeDataString($body)
    }
    $url = "https://github.com/$Repo/issues/new?labels=setup&title=" + [uri]::EscapeDataString('Setup failed') + "&body=$enc"
    Start-Process $url
    # Also on the desktop in full, in case the page does not open.
    $full = Join-Path ([Environment]::GetFolderPath('Desktop')) 'Dragon Flight setup log.txt'
    Clean (($lines) -join "`r`n") | Set-Content $full
    Say "The page is opening in your browser. (The full log is also on your desktop: $full)" Green
}

Say ''
Say '  ==============================================' Cyan
Say '    Dragon Flight - installer' Cyan
if (Test-Path "$Build\Saved\IslandBuilt.txt") { Say '    Updating: usually 10-30 minutes.' Cyan } else { Say '    First time: 1-2 hours. Leave this window open.' Cyan }
Say '  ==============================================' Cyan
Say ''

# --- Room on the disk: ~45 GB for the build folder and the game.
$free = [math]::Round((Get-PSDrive C).Free / 1GB)
if ((-not (Test-Path $Build)) -and $free -lt 50) {
    Say "Only $free GB free on C:. The build needs about 50 GB." Yellow
    if ((Read-Host 'Try anyway? (Y/N)') -notmatch '^[Yy]') { return }
}

# --- Get (or update) the project.
Say '=== Downloading the project ===' Green
$zip = Join-Path $env:TEMP 'dragon-flight.zip'
$src = Join-Path $env:TEMP 'dragon-flight-src'
# The exact commit, so reports can say which version this is.
$sha = 'main'
try { $sha = (Invoke-RestMethod "https://api.github.com/repos/$Repo/commits/main" -UseBasicParsing).sha } catch { }
Invoke-WebRequest "https://github.com/$Repo/archive/$sha.zip" -OutFile $zip -UseBasicParsing
Remove-Tree $src
Expand-Archive $zip -DestinationPath $src -Force
$top = Get-ChildItem $src | Select-Object -First 1
robocopy $top.FullName $Build /E /NFL /NDL /NJH /NJS /NP | Out-Null   # over an old build: keeps finished steps
Remove-Item $zip -Force
Remove-Tree $src

# --- Rebuild the island only when what it is made from has changed.
$inputs = @('Source\DragonFlightEditor', 'Config', 'Data\World', 'Data\Defaults', 'Tools\fetch_world.py', 'Tools\fetch_assets.py') |
    ForEach-Object { Join-Path $Build $_ } | Where-Object { Test-Path $_ } |
    ForEach-Object { Get-ChildItem $_ -Recurse -File | Where-Object { $_.Extension -notin '.r16', '.zip' -and $_.Name -ne 'Heightmap.release' } } |
    Sort-Object FullName | Get-FileHash -Algorithm MD5 | ForEach-Object { $_.Hash }
$stamp = (Get-FileHash -InputStream ([IO.MemoryStream]::new([Text.Encoding]::UTF8.GetBytes(($inputs -join ''))))).Hash
$stampFile = "$Build\Saved\IslandInputs.txt"
if ((Test-Path $stampFile) -and ((Get-Content $stampFile) -ne $stamp)) {
    Say 'The island has changed in this version - it will be rebuilt.' Yellow
    Remove-Item "$Build\Saved\IslandBuilt.txt" -ErrorAction SilentlyContinue
}

# --- Build it.
$env:DF_NOPAUSE = '1'
$env:DF_BUILD = $sha.Substring(0, [math]::Min(12, $sha.Length))
$out = Join-Path $env:TEMP 'dragon-flight-setup.txt'
# (Windows PowerShell turns a program's error output into errors; with 'Stop' the first one
# would end the script, so relax it while the build runs.)
$ErrorActionPreference = 'Continue'
& cmd.exe /c "$Build\Setup.bat" 2>&1 | ForEach-Object { "$_" } | Tee-Object -FilePath $out
$code = $LASTEXITCODE
$ErrorActionPreference = 'Stop'

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
    Say 'Something went wrong (see above).' Red
    Say 'Pasting the same command again picks up where it stopped - worth one retry.' Yellow
    if ((Read-Host 'Send an error report to Faaris? A GitHub page opens with it filled in - just press "Submit new issue". (Y/N)') -match '^[Yy]') {
        Send-SetupReport $out $env:DF_BUILD
    }
    return
}

# --- Done.
New-Item -ItemType Directory -Force -Path "$Build\Saved" | Out-Null
$stamp | Set-Content $stampFile
Say ''
Say "Done. Dragon Flight is on your desktop (the game is in $Game)." Green
Say 'For a newer version later, paste the same command again.' Green
Say ''
Say 'Starting Dragon Flight. F1 shows the controls, F2 graphics, F5 resolution.' Cyan
Start-Process (Join-Path $Game 'DragonFlight.exe')
