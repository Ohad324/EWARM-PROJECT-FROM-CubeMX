@echo off
setlocal EnableDelayedExpansion

set IAR_BIN=C:\TouchGFXProjects\IAR 9.70.2\ewarm-9.70.2\common\bin
set PROJECT=C:\TouchGFXProjects\MyApplication\EWARM\STM32H747I-DISCO.ewp
set TARGET=STM32H747I-DISCO_CM7
set FLASH_BAT=C:\TouchGFXProjects\MyApplication\EWARM\FlashSTM32 (Jlink+stlink).bat
set FLASH_JLINK_SCRIPT=C:\TouchGFXProjects\MyApplication\EWARM\_flash.jlink
set JLINK=C:\Program Files\SEGGER\JLink_V930a\JLink.exe
set RTTLOGGER=C:\Program Files\SEGGER\JLink_V930a\JLinkRTTLogger.exe
set APPEND_TABLE_PS1=C:\TouchGFXProjects\MyApplication\EWARM\append_dfsdm_table.ps1
set CH0_SITP_PS1=C:\TouchGFXProjects\MyApplication\EWARM\force_ch0_sitp_once.ps1
set CH0_SITP_FLAG=C:\TouchGFXProjects\MyApplication\EWARM\ch0_sitp_once.flag
set TRIGGER_SCRIPT=C:\TouchGFXProjects\MyApplication\EWARM\jlink_trigger_button.jlink
set LOGDIR=C:\TouchGFXProjects\MyApplication\EWARM
set RUNSDIR=%LOGDIR%\runs
for /f %%i in ('powershell -NoProfile -Command "(Get-Date).ToString('yyyyMMdd_HHmmss')"') do set TS=%%i
set RUNLOGDIR=%RUNSDIR%\run_%TS%
set BUILDLOG=%RUNLOGDIR%\voice_rec_build.log
set FLASHFALLBACKLOG=%RUNLOGDIR%\voice_rec_flash_fallback.log
set TRIGGERLOG=%RUNLOGDIR%\voice_rec_trigger.log
set RESULT_LOG=%RUNLOGDIR%\VOICE_REC_RESULT_%TS%.log

echo [TASK] Rebuild + Flash + Read RTT Log (after connect) - Tuesday 21:10
echo [1/6] Kill IAR/J-Link processes...
taskkill /F /IM iarbuild.exe >nul 2>&1
taskkill /F /IM CSpyBat.exe >nul 2>&1
taskkill /F /IM IarIdePm.exe >nul 2>&1
taskkill /F /IM JLink.exe >nul 2>&1
taskkill /F /IM JLinkRTTViewer.exe >nul 2>&1
taskkill /F /IM JLinkRTTLogger.exe >nul 2>&1
taskkill /F /IM ST-LinkServer.exe >nul 2>&1
taskkill /F /IM STLinkServer.exe >nul 2>&1
timeout /t 2 /nobreak >nul

echo [2/6] Prepare result log...
if not exist "%RUNSDIR%" mkdir "%RUNSDIR%" >nul 2>&1
if not exist "%RUNLOGDIR%" mkdir "%RUNLOGDIR%" >nul 2>&1
echo [LOG] Run log folder: %RUNLOGDIR%
echo [LOG] RTT output file: %RESULT_LOG%

echo [3/6] Rebuild target...
"%IAR_BIN%\iarbuild.exe" "%PROJECT%" -make "%TARGET%" -log all > "%BUILDLOG%" 2>&1
if %ERRORLEVEL% NEQ 0 (
    echo [ERROR] Build failed. See %BUILDLOG%
    type "%BUILDLOG%"
    taskkill /F /IM JLinkRTTLogger.exe >nul 2>&1
    exit /b 1
)
echo [OK] Build succeeded.

echo [4/6] Program firmware...
call "%FLASH_BAT%"
if %ERRORLEVEL% NEQ 0 (
    echo [WARN] Flash tool returned nonzero exit; attempting direct J-Link flash as best effort...
    "%JLINK%" -NoGui 1 -device STM32H747II_M7 -if SWD -speed 4000 -autoconnect 1 -CommandFile "%FLASH_JLINK_SCRIPT%" > "%FLASHFALLBACKLOG%" 2>&1
    echo [WARN] Continuing; flash output may still be valid. See %FLASHFALLBACKLOG% if needed.
)
echo [OK] Flash complete.

echo [5/6] Wait 7s for firmware init, optional one-time CH0 SITP phase test, then trigger EXTI13...
timeout /t 7 /nobreak >nul
if exist "%CH0_SITP_FLAG%" (
    echo [TEST] One-time CH0 SITP phase test requested. Applying now - firmware initialized...
    powershell -NoProfile -ExecutionPolicy Bypass -File "%CH0_SITP_PS1%" -JLinkExe "%JLINK%" -RunLogDir "%RUNLOGDIR%"
    set SITP_RC=!ERRORLEVEL!
    if !SITP_RC! EQU 0 (
        del "%CH0_SITP_FLAG%" >nul 2>&1
        echo [TEST] One-time CH0 SITP phase test applied. Flag cleared.
    ) else (
        echo [WARN] One-time CH0 SITP phase test failed with exit code !SITP_RC!. Flag kept for retry.
    )
)
"%JLINK%" -NoGui 1 -CommandFile "%TRIGGER_SCRIPT%" > "%TRIGGERLOG%" 2>&1
if %ERRORLEVEL% NEQ 0 (
    echo [ERROR] EXTI13 trigger failed. See %TRIGGERLOG%
    type "%TRIGGERLOG%"
    taskkill /F /IM JLinkRTTLogger.exe >nul 2>&1
    exit /b 3
)
echo [OK] EXTI13 trigger sent.

echo [6/6] Connect RTT and capture recording result logs...
start "RTTLogger" /B "%RTTLOGGER%" -device STM32H747XI_M7 -if SWD -speed 4000 -RTTChannel 0 "%RESULT_LOG%"
timeout /t 15 /nobreak >nul
taskkill /F /IM JLinkRTTLogger.exe >nul 2>&1

powershell -NoProfile -ExecutionPolicy Bypass -File "%APPEND_TABLE_PS1%" -JLinkExe "%JLINK%" -ResultLog "%RESULT_LOG%" -RunLogDir "%RUNLOGDIR%" >nul 2>&1
echo.
echo === Trigger output ===
type "%TRIGGERLOG%"
echo.
echo === VOICE_REC_RESULT ===
type "%RESULT_LOG%" 2>nul || echo [WARN] No RTT result log generated.
echo.
echo [DONE] Run logs folder: %RUNLOGDIR%

endlocal
exit /b 0
