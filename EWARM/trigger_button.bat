@echo off
REM Simulate blue button press on STM32H747I-DISCO via J-Link SWIER1
REM Triggers EXTI line 13 (PC13) software interrupt → VoiceRecTask starts recording

set JLINK="C:\Program Files\SEGGER\JLink\JLink.exe"
set SCRIPT=C:\TouchGFXProjects\MyApplication\EWARM\jlink_trigger_button.jlink
set LOG=C:\TouchGFXProjects\MyApplication\EWARM\trigger_button.log

echo Triggering blue button (EXTI13 SWIER) ...
%JLINK% -NoGui 1 -CommandFile "%SCRIPT%" > "%LOG%" 2>&1

echo.
echo === J-Link output ===
type "%LOG%"
echo.
echo Done. Check SEGGER RTT Viewer or RTT log for recording activity.
pause
