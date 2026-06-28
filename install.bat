@echo off
setlocal
echo ========================================================
echo   Multiple Replay Buffers - Portable OBS Installer
echo ========================================================
echo.
echo Please enter the folder path to your Portable OBS Studio.
echo (This is the folder that contains the 'bin', 'data', and 'obs-plugins' folders)
echo.
set /p OBS_PATH="Path: "

REM Remove quotes if the user dragged and dropped a folder
set OBS_PATH=%OBS_PATH:"=%

if not exist "%OBS_PATH%\obs-plugins\64bit" (
    echo.
    echo ERROR: Could not find the 'obs-plugins\64bit' folder in that directory!
    echo Are you sure you selected the main OBS Studio root folder?
    echo.
    pause
    exit /b 1
)

echo.
echo Installing plugin to: "%OBS_PATH%\obs-plugins\64bit"
copy /y "%~dp0multiple-replay-buffers.dll" "%OBS_PATH%\obs-plugins\64bit\"

if %ERRORLEVEL% equ 0 (
    echo.
    echo SUCCESS: Plugin installed successfully! You can now launch OBS.
) else (
    echo.
    echo ERROR: Failed to copy the plugin. Make sure OBS Studio is completely closed!
)

echo.
pause
