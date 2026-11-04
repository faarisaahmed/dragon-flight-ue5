@echo off
REM ---------------------------------------------------------------------------
REM Dragon Flight - one-click setup on Windows.
REM   1. downloads the baked terrain and the CC0 textures/models
REM   2. compiles the C++ (needs Visual Studio 2022 with "Game development with C++")
REM   3. builds the Archipelago map (landscape, water, sky, forests)
REM   4. opens the editor - press Play (Alt+P)
REM
REM Usage:  Setup.bat                (UE 5.8 in the default folder, 8k terrain)
REM         Setup.bat max            (16k terrain + 8k ground textures: for a big GPU)
REM         set UE_ROOT=D:\Epic\UE_5.8 & Setup.bat
REM ---------------------------------------------------------------------------
setlocal
cd /d "%~dp0"
set PROJECT=%~dp0DragonFlight.uproject

if "%UE_ROOT%"=="" (
  for %%V in (5.9 5.8) do (
    if exist "C:\Program Files\Epic Games\UE_%%V\Engine\Binaries\Win64\UnrealEditor.exe" if "%UE_ROOT%"=="" set "UE_ROOT=C:\Program Files\Epic Games\UE_%%V"
  )
)
if "%UE_ROOT%"=="" (
  echo Could not find Unreal Engine. Install UE 5.8 from the Epic Games Launcher, or set UE_ROOT.
  exit /b 1
)
echo Using %UE_ROOT%
set PY="%UE_ROOT%\Engine\Binaries\ThirdParty\Python3\Win64\python.exe"

set RES=8129
set TEX=4k
set MAXFLAG=
if /i "%1"=="max" ( set RES=16321& set TEX=8k& set MAXFLAG=--max )

echo === 1/4 Downloading terrain and art ===
%PY% Tools\fetch_world.py %MAXFLAG% || goto :fail
%PY% Tools\fetch_assets.py --tex %TEX% || goto :fail

echo === 2/4 Compiling ===
call "%UE_ROOT%\Engine\Build\BatchFiles\Build.bat" DragonFlightEditor Win64 Development -Project="%PROJECT%" -WaitMutex || goto :fail

echo === 3/4 Building the archipelago (first time takes a while: shaders + Nanite) ===
"%UE_ROOT%\Engine\Binaries\Win64\UnrealEditor-Cmd.exe" "%PROJECT%" -run=BuildArchipelago -Res=%RES% -unattended -nop4 -nosplash || goto :fail

echo === 4/4 Opening the editor - press Play (Alt+P), or File ^> Package Project ^> Windows ===
start "" "%UE_ROOT%\Engine\Binaries\Win64\UnrealEditor.exe" "%PROJECT%"
exit /b 0

:fail
echo.
echo Setup failed at the step above. See Docs\SETUP_Windows.md, or send the log in Saved\Logs.
exit /b 1
