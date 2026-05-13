@echo off
setlocal

set CSPYBAT="C:\TouchGFXProjects\IAR 9.70.2\ewarm-9.70.2\common\bin\cspybat.exe"
set GENERAL_XCL=C:\TouchGFXProjects\MyApplication\EWARM\settings\STM32H747I-DISCO.STM32H747I-DISCO_CM7.general.xcl
set DRIVER_XCL=C:\TouchGFXProjects\MyApplication\EWARM\settings\STM32H747I-DISCO.STM32H747I-DISCO_CM7.driver.xcl
set TRIGGER_MAC=C:\TouchGFXProjects\MyApplication\EWARM\trigger_and_dump_session.mac
set LOG=C:\TouchGFXProjects\MyApplication\EWARM\runs\trigger_dump.log

if not exist "C:\TouchGFXProjects\MyApplication\EWARM\runs" mkdir "C:\TouchGFXProjects\MyApplication\EWARM\runs"

:: ── Step 1: Flash firmware + reset + leave running ───────────────────────────
echo.
echo [1/2] Flashing firmware...
%CSPYBAT% --download_only --silent -f "%GENERAL_XCL%" --backend -f "%DRIVER_XCL%" > "%LOG%" 2>&1
if %ERRORLEVEL% neq 0 (
    echo FLASH FAILED. Log:
    type "%LOG%"
    pause & exit /b 1
)
echo Flash OK. Waiting 4s for app to boot...
timeout /t 4 /nobreak > nul

:: ── Step 2: Attach to running app, fire trigger, dump mid-recording ──────────
echo [2/2] Attaching + firing EXTI13 trigger...
%CSPYBAT% --attach_to_running_target --leave_target_running --silent ^
    --macro "%TRIGGER_MAC%" ^
    -f "%GENERAL_XCL%" --backend -f "%DRIVER_XCL%" >> "%LOG%" 2>&1

echo.
echo ================================================================
echo  LOG:
echo ================================================================
type "%LOG%"
echo.
pause
endlocal
