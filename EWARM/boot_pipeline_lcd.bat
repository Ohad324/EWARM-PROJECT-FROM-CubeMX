@echo off
setlocal

REM boot_pipeline_lcd.bat -- LCD/DMA2D pipeline probe (Debug build).
REM
REM Targets the half-blue/half-green visual bug. Probes 9 different
REM rendering-pipeline points using skip-count breakpoints to catch the
REM Nth occurrence of HAL_DSI_Refresh / HAL_DSI_EndOfRefreshCallback /
REM LTDC_ER_IRQHandler / Music_InjectTestThumb. The Debug build has the
REM test thumbnail compiled in (gated by RELEASE_BUILD), so DMA2D and
REM LTDC are actually doing work.

set IAR_BIN=C:\TouchGFXProjects\IAR 9.70.2\ewarm-9.70.2\common\bin
set PROJECT=C:\TouchGFXProjects\MyApplication\EWARM\STM32H747I-DISCO.ewp
set TARGET=STM32H747I-DISCO_CM7
set LOGDIR=C:\TouchGFXProjects\MyApplication\EWARM\runs
set ORCHESTRATOR=C:\TouchGFXProjects\MyApplication\EWARM\boot_pipeline_lcd.ps1

if not exist "%LOGDIR%" mkdir "%LOGDIR%"

echo [1/3] Stopping any running debugger tools...
taskkill /F /IM iarbuild.exe >nul 2>&1
taskkill /F /IM CSpyBat.exe  >nul 2>&1
taskkill /F /IM IarIdePm.exe >nul 2>&1
taskkill /F /IM JLink.exe    >nul 2>&1

echo [2/3] Building Debug (CM7) configuration...
"%IAR_BIN%\iarbuild.exe" "%PROJECT%" -make "%TARGET%" -log warnings > "%LOGDIR%\boot_pipeline_lcd_build.log" 2>&1
if %ERRORLEVEL% NEQ 0 (
    echo BUILD FAILED  -  see %LOGDIR%\boot_pipeline_lcd_build.log
    type "%LOGDIR%\boot_pipeline_lcd_build.log"
    exit /b 1
)

echo [3/3] Orchestrating 9 LCD probes (skip-count BPs catch later refreshes)...
powershell -NoProfile -ExecutionPolicy Bypass -File "%ORCHESTRATOR%"
set PS_RC=%ERRORLEVEL%

echo.
echo ================================================================
echo   boot_pipeline_lcd complete  (orchestrator exit = %PS_RC%)
echo   Combined log: %LOGDIR%\boot_pipeline_lcd.log
echo ================================================================

endlocal
exit /b %PS_RC%
