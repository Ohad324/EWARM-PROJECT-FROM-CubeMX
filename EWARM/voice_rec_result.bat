@echo off
setlocal

set IAR_BIN=C:\TouchGFXProjects\IAR 9.70.2\ewarm-9.70.2\common\bin
set PROJECT=C:\TouchGFXProjects\MyApplication\EWARM\STM32H747I-DISCO.ewp
set TARGET=STM32H747I-DISCO_CM7
set FLASH_BAT=C:\TouchGFXProjects\MyApplication\EWARM\FlashSTM32 (Jlink+stlink).bat
set FLASH_JLINK_SCRIPT=C:\TouchGFXProjects\MyApplication\EWARM\_flash.jlink
set JLINK=C:\Program Files\SEGGER\JLink_V930a\JLink.exe
set RTTLOGGER=C:\Program Files\SEGGER\JLink_V930a\JLinkRTTLogger.exe
set TRIGGER_SCRIPT=C:\TouchGFXProjects\MyApplication\EWARM\jlink_trigger_button.jlink
set LOGDIR=C:\TouchGFXProjects\MyApplication\EWARM
set BUILDLOG=%LOGDIR%\voice_rec_build.log
set FLASHFALLBACKLOG=%LOGDIR%\voice_rec_flash_fallback.log
set TRIGGERLOG=%LOGDIR%\voice_rec_trigger.log
set RESULT_LOG=%LOGDIR%\VOICE_REC_RESULT.log

echo [TASK] VOICE_REC_RESULT
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
if exist "%RESULT_LOG%" del "%RESULT_LOG%" 2>nul

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

echo [5/6] Trigger EXTI13 to call VoiceRecTask...
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
echo.
echo === Trigger output ===
type "%TRIGGERLOG%"
echo.
echo === VOICE_REC_RESULT ===
type "%RESULT_LOG%" 2>nul || echo [WARN] No RTT result log generated.
echo.
echo [DONE] Result file: %RESULT_LOG%

endlocal
exit /b 0
