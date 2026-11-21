@echo off
rem GYRO SNAKE desktop simulator - double-click launcher.
rem Builds desktop/build/gyro_snake.dll with Git Bash when it is missing,
rem then starts desktop/play.py from the gyro-snake project root.
setlocal EnableExtensions
cd /d "%~dp0.."

set "PY=python"
%PY% -c "import tkinter, PIL" >nul 2>nul
if errorlevel 1 set "PY=py -3"
%PY% -c "import tkinter, PIL" >nul 2>nul
if errorlevel 1 (
    echo [GYRO SNAKE] Python 3 with tkinter + Pillow not found.
    echo              Need Python 3.10+, Tk 8.6, Pillow 9+ on PATH.
    pause
    exit /b 1
)

if not exist "desktop\build\gyro_snake.dll" (
    echo [GYRO SNAKE] gyro_snake.dll missing; building with Git Bash ...
    if not exist "desktop\build.sh" (
        echo [GYRO SNAKE] desktop\build.sh is missing.
        pause
        exit /b 1
    )
    if exist "%ProgramFiles%\Git\bin\bash.exe" (
        "%ProgramFiles%\Git\bin\bash.exe" desktop/build.sh
    ) else (
        where bash >nul 2>nul
        if errorlevel 1 (
            echo [GYRO SNAKE] Git Bash not found.
            echo              Install Git for Windows, or run desktop\build.sh manually.
            pause
            exit /b 1
        )
        bash desktop/build.sh
    )
    if errorlevel 1 (
        echo [GYRO SNAKE] build failed.
        pause
        exit /b 1
    )
    if not exist "desktop\build\gyro_snake.dll" (
        echo [GYRO SNAKE] build finished but gyro_snake.dll is missing.
        pause
        exit /b 1
    )
)

%PY% desktop\play.py %*
if errorlevel 1 pause
endlocal