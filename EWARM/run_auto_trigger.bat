@echo off
setlocal

set JLINK=C:\Program Files\SEGGER\JLink_V934b\JLink.exe
set SCRIPT=%~dp0auto_trigger_dump.jlink
set LOG=%~dp0runs\last_dump.txt

if not exist "%~dp0runs" mkdir "%~dp0runs"

echo.
echo ================================================================
echo  Auto-trigger + mid-recording dump
echo  Firing EXTI13 (blue button), waiting 1500ms, dumping...
echo  Log: %LOG%
echo ================================================================
echo.

"%JLINK%" -NoGui 1 -device STM32H747XI_M7 -if SWD -speed 4000 -autoconnect 1 -CommandFile "%SCRIPT%" > "%LOG%" 2>&1

echo.
echo ================================================================
echo  DONE
echo ================================================================
echo.
type "%LOG%"
echo.
pause
endlocal
