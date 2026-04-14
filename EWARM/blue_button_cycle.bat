@echo off
setlocal

:: Paths
set IAR_BIN=C:\TouchGFXProjects\IAR 9.70.2\ewarm-9.70.2\common\bin
set PROJECT=C:\TouchGFXProjects\MyApplication\EWARM\STM32H747I-DISCO.ewp
set TARGET=STM32H747I-DISCO_CM7
set FLASH_BAT=C:\TouchGFXProjects\MyApplication\EWARM\FlashSTM32 (Jlink+stlink).bat
set JLINK=C:\Program Files\SEGGER\JLink_V930a\JLink.exe
set TRIGGER_SCRIPT=C:\TouchGFXProjects\MyApplication\EWARM\jlink_trigger_button.jlink
set LOGDIR=C:\TouchGFXProjects\MyApplication\EWARM
set BUILDLOG=%LOGDIR%\blue_button_build.log
set TRIGGERLOG=%LOGDIR%\blue_button_trigger.log

echo [TASK] Build + Flash + Blue Button Trigger + RTT_with_results
echo [1/4] Killing IAR and debugger processes...
taskkill /F /IM iarbuild.exe >nul 2>&1
taskkill /F /IM CSpyBat.exe >nul 2>&1
taskkill /F /IM IarIdePm.exe >nul 2>&1
taskkill /F /IM JLink.exe >nul 2>&1
taskkill /F /IM JLinkRTTViewer.exe >nul 2>&1
taskkill /F /IM JLinkRTTLogger.exe >nul 2>&1
taskkill /F /IM ST-LinkServer.exe >nul 2>&1
taskkill /F /IM STLinkServer.exe >nul 2>&1
timeout /t 2 /nobreak >nul

echo [2/4] Rebuilding project...
"%IAR_BIN%\iarbuild.exe" "%PROJECT%" -make "%TARGET%" -log all > "%BUILDLOG%" 2>&1
if %ERRORLEVEL% NEQ 0 (
    echo [ERROR] Build failed. See %BUILDLOG%
    type "%BUILDLOG%"
    exit /b 1
)
echo [OK] Build succeeded.

echo [3/4] Flashing firmware...
call "%FLASH_BAT%"
if %ERRORLEVEL% NEQ 0 (
    echo [ERROR] Flash failed.
    exit /b 2
)
echo [OK] Flash complete.

echo [4/4] Triggering blue button EXTI13...
"%JLINK%" -NoGui 1 -CommandFile "%TRIGGER_SCRIPT%" > "%TRIGGERLOG%" 2>&1
if %ERRORLEVEL% NEQ 0 (
    echo [ERROR] Blue-button trigger failed. See %TRIGGERLOG%
    type "%TRIGGERLOG%"
    exit /b 3
)
echo [OK] Trigger command complete.
echo.
echo === Trigger output ===
type "%TRIGGERLOG%"
echo.

set RTTLOGGER="C:\Program Files\SEGGER\JLink_V930a\JLinkRTTLogger.exe"
set RTTLOG=%LOGDIR%\SystemView\rtt_log.txt
if exist "%RTTLOG%" del "%RTTLOG%" 2>nul
echo [5/5] Capturing RTT output...
start "RTTLogger" /B %RTTLOGGER% -device STM32H747XI_M7 -if SWD -speed 4000 -RTTChannel 0 -OutputFile "%RTTLOG%" -AutoConnect 1
timeout /t 10 /nobreak >nul
taskkill /F /IM JLinkRTTLogger.exe >nul 2>&1
echo [OK] RTT capture complete.
echo.
echo === RTT Log ===
type "%RTTLOG%" 2>nul || echo [WARN] No RTT log generated.
echo.
echo [DONE] Build, flash, trigger, and RTT capture finished.
endlocal
exit /b 0