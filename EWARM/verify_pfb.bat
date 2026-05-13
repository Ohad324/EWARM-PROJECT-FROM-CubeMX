@echo off
setlocal

REM verify_pfb.bat -- Partial Framebuffer architecture verification driver.
REM
REM Builds the Debug CM7 target then orchestrates 15 cspybat runs (one BP
REM per run) via verify_pfb.ps1 to map the full PFB boot/render path.
REM
REM Per-iteration logs:    EWARM\runs\verify_pfb\<NN_tag>.log
REM Combined log:          EWARM\runs\verify_pfb.log
REM Pass criterion:        BP-15 (LTDC_ER_IRQHandler) NEVER fires AND
REM                        BP-14 reports IRQ-ring LTDC_ER count == 0.

set IAR_BIN=C:\TouchGFXProjects\IAR 9.70.2\ewarm-9.70.2\common\bin
set PROJECT=C:\TouchGFXProjects\MyApplication\EWARM\STM32H747I-DISCO.ewp
set TARGET=STM32H747I-DISCO_CM7
set LOGDIR=C:\TouchGFXProjects\MyApplication\EWARM\runs
set ORCHESTRATOR=C:\TouchGFXProjects\MyApplication\EWARM\verify_pfb.ps1

if not exist "%LOGDIR%" mkdir "%LOGDIR%"

echo [1/3] Releasing probes (kill IDE / J-Link / cspybat)...
taskkill /F /IM iarbuild.exe       >nul 2>&1
taskkill /F /IM CSpyBat.exe        >nul 2>&1
taskkill /F /IM IarIdePm.exe       >nul 2>&1
taskkill /F /IM JLink.exe          >nul 2>&1
taskkill /F /IM JLinkRTTViewer.exe >nul 2>&1

echo [2/3] Building Debug (CM7) configuration...
"%IAR_BIN%\iarbuild.exe" "%PROJECT%" -make "%TARGET%" -log warnings > "%LOGDIR%\verify_pfb_build.log" 2>&1
if %ERRORLEVEL% NEQ 0 (
    echo BUILD FAILED  -  see %LOGDIR%\verify_pfb_build.log
    type "%LOGDIR%\verify_pfb_build.log"
    exit /b 1
)
echo Build OK

echo [3/3] Orchestrating 15 verify_pfb probes (one BP per cspybat run)...
powershell -NoProfile -ExecutionPolicy Bypass -File "%ORCHESTRATOR%"
set PS_RC=%ERRORLEVEL%

echo.
echo ================================================================
echo   verify_pfb complete  (orchestrator exit = %PS_RC%)
echo   Combined log: %LOGDIR%\verify_pfb.log
echo   Per-checkpoint logs: %LOGDIR%\verify_pfb\*.log
echo.
echo   PASS criterion:
echo     1. Every BP-01..BP-14 fires and prints register dumps.
echo     2. BP-15 (LTDC_ER_IRQHandler) NEVER fires.
echo     3. BP-14 (stain test) reports IRQ-ring LTDC_ER count == 0.
echo ================================================================

endlocal
exit /b %PS_RC%
