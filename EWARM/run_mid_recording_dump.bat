@echo off
echo.
echo ================================================================
echo  Mid-recording dump
echo.
echo  INSTRUCTIONS:
echo    1. Press the BLUE BUTTON on the board NOW
echo    2. This script will connect and dump after 500ms
echo    3. You have ~3 seconds from button press
echo ================================================================
echo.
pause

"C:\Program Files\SEGGER\JLink_V934b\JLink.exe" -NoGui 1 -device STM32H747XI_M7 -if SWD -speed 4000 -autoconnect 1 -CommandFile "%~dp0mid_recording_dump.jlink" > "%~dp0runs\last_dump.txt" 2>&1

echo.
type "%~dp0runs\last_dump.txt"
echo.
pause
