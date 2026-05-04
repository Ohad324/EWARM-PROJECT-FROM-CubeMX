@echo off
setlocal

REM boot_pipeline_multi_debug.bat -- 25-checkpoint cspybat probe against
REM the CM7 Debug configuration (NOT Release).
REM
REM The Debug build has Music_InjectTestThumb compiled in (gated out of
REM Release by #ifndef RELEASE_BUILD), so the framebuffer is actually
REM painted with the flasher test JPEG. Use this when you want to probe
REM the LCD pipeline with real visual content vs Release's blank state.

set IAR_BIN=C:\TouchGFXProjects\IAR 9.70.2\ewarm-9.70.2\common\bin
set PROJECT=C:\TouchGFXProjects\MyApplication\EWARM\STM32H747I-DISCO.ewp
set TARGET=STM32H747I-DISCO_CM7
set LOGDIR=C:\TouchGFXProjects\MyApplication\EWARM\runs
set ORCHESTRATOR=C:\TouchGFXProjects\MyApplication\EWARM\boot_pipeline_multi_debug.ps1

if not exist "%LOGDIR%" mkdir "%LOGDIR%"

echo [1/3] Stopping any running debugger tools...
taskkill /F /IM iarbuild.exe >nul 2>&1
taskkill /F /IM CSpyBat.exe  >nul 2>&1
taskkill /F /IM IarIdePm.exe >nul 2>&1
taskkill /F /IM JLink.exe    >nul 2>&1

echo [2/3] Building Debug (CM7) configuration...
"%IAR_BIN%\iarbuild.exe" "%PROJECT%" -make "%TARGET%" -log warnings > "%LOGDIR%\boot_pipeline_multi_debug_build.log" 2>&1
if %ERRORLEVEL% NEQ 0 (
    echo BUILD FAILED  -  see %LOGDIR%\boot_pipeline_multi_debug_build.log
    type "%LOGDIR%\boot_pipeline_multi_debug_build.log"
    exit /b 1
)

echo [3/3] Orchestrating 25 cspybat invocations against Debug build...
powershell -NoProfile -ExecutionPolicy Bypass -File "%ORCHESTRATOR%"
set PS_RC=%ERRORLEVEL%

echo.
echo ================================================================
echo   boot_pipeline_multi_debug complete  (orchestrator exit = %PS_RC%)
echo   Combined log: %LOGDIR%\boot_pipeline_multi_debug.log
echo ================================================================

endlocal
exit /b %PS_RC%
