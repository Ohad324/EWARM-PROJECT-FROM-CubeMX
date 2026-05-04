@echo off
setlocal

REM boot_pipeline_multi.bat -- 10-checkpoint headless boot probe.
REM
REM Workaround for cspybat 9.4.6.1706 (in EWARM 9.70.2) firing only one
REM code BP per invocation. Runs 10 cspybat sessions, one BP each, then
REM concatenates the logs into a single combined report.
REM
REM Steps:
REM   1. Stop any debugger processes still holding J-Link USB
REM   2. iarbuild Release
REM   3. PowerShell orchestrator boot_pipeline_multi.ps1 loops 10 cspybat
REM      invocations, generating a per-iteration .mac from the template
REM   4. Combined log printed at the end
REM
REM Output:
REM   EWARM\runs\boot_pipeline_multi.log         -- combined boot-stage report
REM   EWARM\runs\boot_pipeline_multi\*.log       -- per-checkpoint raw logs
REM   EWARM\runs\boot_pipeline_multi\*.mac       -- per-checkpoint generated macros

set IAR_BIN=C:\TouchGFXProjects\IAR 9.70.2\ewarm-9.70.2\common\bin
set PROJECT=C:\TouchGFXProjects\MyApplication\EWARM\STM32H747I-DISCO.ewp
set TARGET=Release
set LOGDIR=C:\TouchGFXProjects\MyApplication\EWARM\runs
set ORCHESTRATOR=C:\TouchGFXProjects\MyApplication\EWARM\boot_pipeline_multi.ps1

if not exist "%LOGDIR%" mkdir "%LOGDIR%"

echo [1/3] Stopping any running debugger tools...
taskkill /F /IM iarbuild.exe >nul 2>&1
taskkill /F /IM CSpyBat.exe  >nul 2>&1
taskkill /F /IM IarIdePm.exe >nul 2>&1
taskkill /F /IM JLink.exe    >nul 2>&1

echo [2/3] Building Release configuration...
"%IAR_BIN%\iarbuild.exe" "%PROJECT%" -make "%TARGET%" -log warnings > "%LOGDIR%\boot_pipeline_multi_build.log" 2>&1
if %ERRORLEVEL% NEQ 0 (
    echo BUILD FAILED  -  see %LOGDIR%\boot_pipeline_multi_build.log
    type "%LOGDIR%\boot_pipeline_multi_build.log"
    exit /b 1
)

echo [3/3] Orchestrating 10 cspybat invocations (one BP each)...
powershell -NoProfile -ExecutionPolicy Bypass -File "%ORCHESTRATOR%"
set PS_RC=%ERRORLEVEL%

echo.
echo ================================================================
echo   boot_pipeline_multi complete  (orchestrator exit = %PS_RC%)
echo   Combined log: %LOGDIR%\boot_pipeline_multi.log
echo ================================================================

endlocal
exit /b %PS_RC%
