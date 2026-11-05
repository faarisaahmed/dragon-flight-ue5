@echo off
REM ---------------------------------------------------------------------------
REM Dragon Flight - Windows build script. Normally run by Install.ps1 (see
REM Docs/SETUP_Windows.md); double-clicking it works too.
REM
REM   1. checks for Unreal Engine 5.8 and Visual Studio's C++ tools (installs
REM      Visual Studio Community + the C++ game workload with winget if missing)
REM   2. downloads the terrain, the dragon, the village and the CC0 art
REM   3. compiles the C++ and builds the island
REM   4. packages the game into %USERPROFILE%\Games\Dragon Flight (~4 GB) and puts a
REM      "Dragon Flight" shortcut on the desktop. After that the project, the engine
REM      and Visual Studio can be deleted to get the space back.
REM
REM Usage:  Setup.bat              the game, ready to play (default)
REM         Setup.bat editor       stop after building the island and open the editor
REM         Setup.bat max          16k terrain + 8k ground (untested; 32 GB+ RAM)
REM         set UE_ROOT=D:\Epic\UE_5.8 ^& Setup.bat
REM
REM It skips whatever is already done, so re-running after a failure is safe.
REM After pulling a new version: delete Saved\IslandBuilt.txt to rebuild the island.
REM ---------------------------------------------------------------------------
setlocal EnableExtensions
cd /d "%~dp0"
set "PROJECT=%~dp0DragonFlight.uproject"
if not exist "%~dp0Saved\Logs" mkdir "%~dp0Saved\Logs"

set MODE=game
set RES=8129
set TEX=4k
set MAXFLAG=
for %%A in (%*) do (
  if /i "%%A"=="editor" set MODE=editor
  if /i "%%A"=="max" (set RES=16321& set TEX=8k& set MAXFLAG=--max)
)

echo.
echo  ==============================================
echo    Dragon Flight - Windows setup
echo    First run takes 1-2 hours (mostly shaders).
echo  ==============================================
echo.

REM --- Unreal Engine -----------------------------------------------------------
if not "%UE_ROOT%"=="" goto :have_ue
if exist "C:\Program Files\Epic Games\UE_5.8\Engine\Binaries\Win64\UnrealEditor.exe" set "UE_ROOT=C:\Program Files\Epic Games\UE_5.8"
if not "%UE_ROOT%"=="" goto :have_ue
if exist "C:\Program Files\Epic Games\UE_5.9\Engine\Binaries\Win64\UnrealEditor.exe" set "UE_ROOT=C:\Program Files\Epic Games\UE_5.9"
if not "%UE_ROOT%"=="" goto :have_ue
REM Installed somewhere else: the Epic launcher keeps a list.
set "LID=C:\ProgramData\Epic\UnrealEngineLauncher\LauncherInstalled.dat"
if exist "%LID%" for /f "usebackq delims=" %%P in (`powershell -NoProfile -Command "$j = Get-Content -Raw '%LID%' | ConvertFrom-Json; ($j.InstallationList | Where-Object { $_.AppName -match '^UE_5\.(8|9)$' } | Sort-Object AppName | Select-Object -First 1).InstallLocation"`) do set "UE_ROOT=%%P"
if not "%UE_ROOT%"=="" goto :have_ue

echo Unreal Engine 5.8 is not installed.
echo.
echo   1. Install the Epic Games Launcher (installing it now if winget can).
echo   2. Sign in, go to Unreal Engine ^> Library ^> the + next to "Engine Versions",
echo      pick 5.8 and Install (about 40 GB).
echo   3. When it has finished, double-click Setup.bat again.
echo.
where winget >nul 2>&1 && winget install --id EpicGames.EpicGamesLauncher -e --accept-package-agreements --accept-source-agreements
if not defined DF_NOPAUSE pause
exit /b 2

:have_ue
if not exist "%UE_ROOT%\Engine\Binaries\Win64\UnrealEditor.exe" (
  echo UE_ROOT is set to "%UE_ROOT%" but there is no Unreal Editor there.
  goto :fail
)
echo Unreal Engine: %UE_ROOT%

REM --- Visual Studio C++ tools ---------------------------------------------------
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
set VSOK=
if exist "%VSWHERE%" for /f "usebackq delims=" %%P in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set VSOK=1
if defined VSOK goto :have_vs
echo.
echo Visual Studio's C++ tools are missing - installing Visual Studio 2022 Community
echo with "Game development with C++" - free, about 10 GB. Say Yes if Windows asks.
where winget >nul 2>&1 || goto :no_winget
winget install --id Microsoft.VisualStudio.2022.Community -e --accept-package-agreements --accept-source-agreements --override "--passive --wait --add Microsoft.VisualStudio.Workload.NativeGame --add Microsoft.VisualStudio.Workload.NativeDesktop --includeRecommended"
if exist "%VSWHERE%" for /f "usebackq delims=" %%P in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set VSOK=1
if defined VSOK goto :have_vs
echo Visual Studio did not finish installing. Restart the PC and run Setup.bat again.
goto :fail
:no_winget
echo winget is not available. Install Visual Studio 2022 Community from
echo https://visualstudio.microsoft.com/ with the workload "Game development with C++",
echo then run Setup.bat again.
goto :fail
:have_vs
echo Visual Studio C++ tools: found

set PY="%UE_ROOT%\Engine\Binaries\ThirdParty\Python3\Win64\python.exe"

echo.
echo === 1/4 Downloading terrain, dragon, village and art - a few GB ===
%PY% Tools\fetch_world.py %MAXFLAG% || goto :fail
%PY% Tools\fetch_assets.py --tex %TEX% || goto :fail

echo.
echo === 2/4 Compiling ===
call "%UE_ROOT%\Engine\Build\BatchFiles\Build.bat" DragonFlightEditor Win64 Development -Project="%PROJECT%" -WaitMutex || goto :fail

echo.
echo === 3/4 Building the island - shaders and Nanite, the long part ===
if not exist "Content\Maps\Archipelago.umap" goto :build_island
if not exist "Saved\IslandBuilt.txt" goto :build_island
echo already built - delete Saved\IslandBuilt.txt to rebuild
goto :built
:build_island
"%UE_ROOT%\Engine\Binaries\Win64\UnrealEditor-Cmd.exe" "%PROJECT%" -run=BuildArchipelago -Res=%RES% -unattended -nop4 -nosplash || goto :fail
echo %RES%> "Saved\IslandBuilt.txt"
:built

if /i not "%MODE%"=="editor" goto :package
echo.
echo === 4/4 Packaging the game ===
call "%UE_ROOT%\Engine\Build\BatchFiles\RunUAT.bat" BuildCookRun -project="%PROJECT%" -platform=Win64 -clientconfig=Shipping -build -cook -stage -pak -archive -archivedirectory="%~dp0Packaged" -unattended -nop4 -utf8output || goto :fail

set "PKG=%~dp0Packaged\Windows"
if not exist "%PKG%\DragonFlight.exe" set "PKG=%~dp0Saved\StagedBuilds\Windows"
if not exist "%PKG%\DragonFlight.exe" goto :no_exe
REM The game is copied out of the project, so the project, the engine and Visual
REM Studio can all be deleted afterwards and the game still runs.
set "GAME=%USERPROFILE%\Games\Dragon Flight"
robocopy "%PKG%" "%GAME%" /MIR /NFL /NDL /NJH /NJS /NP >nul
if errorlevel 8 goto :fail
set "EXE=%GAME%\DragonFlight.exe"
powershell -NoProfile -Command "$s = (New-Object -ComObject WScript.Shell).CreateShortcut([Environment]::GetFolderPath('Desktop') + '\Dragon Flight.lnk'); $s.TargetPath = '%EXE%'; $s.WorkingDirectory = '%GAME%'; $s.Save()"
echo.
echo Game installed: %GAME%
REM Run by Install.ps1: it cleans up and starts the game itself.
if defined DF_NOPAUSE exit /b 0
start "" "%EXE%"
pause
exit /b 0

:no_exe
echo Packaging finished but DragonFlight.exe was not found.
:fail
echo.
echo Setup stopped at the step above. Running it again skips the finished steps.
echo If it fails the same way, send Faaris a screenshot of this window and the
echo newest file in Saved\Logs.
if not defined DF_NOPAUSE pause
exit /b 1
