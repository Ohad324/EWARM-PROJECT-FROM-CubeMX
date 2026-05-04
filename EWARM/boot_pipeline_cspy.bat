@echo off
setlocal

REM boot_pipeline_cspy.bat -- BP-driven C-SPY probe.
REM
REM Single cspybat invocation in download flow:
REM   1. iarbuild builds Release
REM   2. cspybat downloads .out (no --attach_to_running_target)
REM   3. execUserSetup fires -> installs 4 code breakpoints at peripheral
REM      init entries (vTaskStartScheduler, MX_DSIHOST_DSI_Init,
REM      MX_LTDC_Init, MX_TouchGFX_Init)
REM   4. cspybat lets the CPU run; each BP fires its action macro inline
REM      (HitXxx()) which dumps the current peripheral state, then clears
REM      the BP and continues
REM   5. execUserExit prints final banner when session ends
REM
REM Output: EWARM\runs\boot_pipeline_cspy.log

set IAR_BIN=C:\TouchGFXProjects\IAR 9.70.2\ewarm-9.70.2\common\bin
set PROJECT=C:\TouchGFXProjects\MyApplication\EWARM\STM32H747I-DISCO.ewp
set TARGET=Release
set MACRO=C:\TouchGFXProjects\MyApplication\EWARM\boot_pipeline.mac
set LOGDIR=C:\TouchGFXProjects\MyApplication\EWARM\runs
set LOGFILE=%LOGDIR%\boot_pipeline_cspy.log
set GENERAL_XCL=C:\TouchGFXProjects\MyApplication\EWARM\settings\STM32H747I-DISCO.Release.general.xcl
set DRIVER_XCL=C:\TouchGFXProjects\MyApplication\EWARM\settings\STM32H747I-DISCO.Release.driver.xcl
set GENERAL_XCL_CLEAN=%LOGDIR%\boot_pipeline_cspy.general.xcl

if not exist "%LOGDIR%" mkdir "%LOGDIR%"

powershell -NoProfile -ExecutionPolicy Bypass -Command ^
    "$lines = Get-Content '%GENERAL_XCL%'; $filtered = $lines | Where-Object { $_ -notmatch '^\s*--macro=' }; $filtered | Set-Content '%GENERAL_XCL_CLEAN%'"
if %ERRORLEVEL% NEQ 0 exit /b 1

echo [1/3] Stopping any running debugger tools...
taskkill /F /IM iarbuild.exe >nul 2>&1
taskkill /F /IM CSpyBat.exe >nul 2>&1
taskkill /F /IM IarIdePm.exe >nul 2>&1
taskkill /F /IM JLink.exe    >nul 2>&1

echo [2/3] Building Release configuration...
"%IAR_BIN%\iarbuild.exe" "%PROJECT%" -make "%TARGET%" -log warnings > "%LOGDIR%\boot_pipeline_cspy_build.log" 2>&1
if %ERRORLEVEL% NEQ 0 (
    type "%LOGDIR%\boot_pipeline_cspy_build.log"
    exit /b 1
)

echo [3/3] cspybat: download .out + execUserSetup installs BPs + run...
REM --leave_target_running keeps cspybat alive so subsequent BPs fire,
REM not just the first one. Without this, cspybat exits after BP1.
"%IAR_BIN%\CSpyBat.exe" --leave_target_running --macro "%MACRO%" ^
    -f "%GENERAL_XCL_CLEAN%" --backend -f "%DRIVER_XCL%" > "%LOGFILE%" 2>&1

set CSPY_EXIT=%ERRORLEVEL%

echo.
echo ================================================================
echo   cspybat boot probe complete  (cspybat exit = %CSPY_EXIT%)
echo   Log: %LOGFILE%
echo ================================================================
type "%LOGFILE%"

endlocal
exit /b %CSPY_EXIT%
