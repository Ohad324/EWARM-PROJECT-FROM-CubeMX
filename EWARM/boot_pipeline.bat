@echo off
setlocal

REM boot_pipeline.bat -- Build Release, flash, run boot_pipeline.jlink probe.
REM
REM Build the Release configuration via iarbuild, flash via cspybat, wait for
REM boot to settle, then connect with J-Link Commander to dump LTDC / DSI /
REM framebuffer / MPU registers and re-sample LTDC->ISR after 2 s.
REM
REM Output: EWARM\runs\boot_pipeline.log    (J-Link mem32 dumps)
REM         EWARM\runs\boot_pipeline.txt    (parsed PASS/FAIL summary)

set IAR_BIN=C:\TouchGFXProjects\IAR 9.70.2\ewarm-9.70.2\common\bin
set JLINK=C:\Program Files\SEGGER\JLink_V934b\JLink.exe
set PROJECT=C:\TouchGFXProjects\MyApplication\EWARM\STM32H747I-DISCO.ewp
set TARGET=Release
set JLINK_SCRIPT=C:\TouchGFXProjects\MyApplication\EWARM\boot_pipeline.jlink
set LOGDIR=C:\TouchGFXProjects\MyApplication\EWARM\runs
set LOGFILE=%LOGDIR%\boot_pipeline.log
set REPORT=%LOGDIR%\boot_pipeline.txt
set GENERAL_XCL=C:\TouchGFXProjects\MyApplication\EWARM\settings\STM32H747I-DISCO.Release.general.xcl
set DRIVER_XCL=C:\TouchGFXProjects\MyApplication\EWARM\settings\STM32H747I-DISCO.Release.driver.xcl

if not exist "%LOGDIR%" mkdir "%LOGDIR%"

echo [1/4] Stopping any running debugger tools...
taskkill /F /IM iarbuild.exe >nul 2>&1
taskkill /F /IM CSpyBat.exe >nul 2>&1
taskkill /F /IM JLink.exe    >nul 2>&1

echo [2/4] Building Release configuration...
"%IAR_BIN%\iarbuild.exe" "%PROJECT%" -make "%TARGET%" -log warnings > "%LOGDIR%\boot_pipeline_build.log" 2>&1
if %ERRORLEVEL% NEQ 0 (
    echo BUILD FAILED  -  see %LOGDIR%\boot_pipeline_build.log
    type "%LOGDIR%\boot_pipeline_build.log"
    exit /b 1
)

echo [3/4] Flashing Release.out via J-Link (loadfile + reset + go)...
"%JLINK%" -device STM32H747XI_M7 -if SWD -speed 4000 -autoconnect 1 ^
    -CommanderScript "C:\TouchGFXProjects\MyApplication\EWARM\boot_pipeline_flash.jlink" > "%LOGFILE%" 2>&1
if %ERRORLEVEL% NEQ 0 (
    echo FLASH FAILED  -  see %LOGFILE%
    type "%LOGFILE%"
    exit /b 1
)

REM Wait for chip to come up through clocks/MPU/peripheral *_Init, kernel
REM start, guiTask boot, first frames painted.
timeout /t 5 /nobreak > nul

echo [4/4] Running boot_pipeline.jlink probe via J-Link Commander...
echo. >> "%LOGFILE%"
echo === J-Link probe at T+~5s after flash === >> "%LOGFILE%"
"%JLINK%" -device STM32H747XI_M7 -if SWD -speed 4000 -autoconnect 1 ^
    -CommanderScript "%JLINK_SCRIPT%" >> "%LOGFILE%" 2>&1

set JLINK_EXIT=%ERRORLEVEL%

echo.
echo ================================================================
echo   J-Link probe complete  (JLink exit = %JLINK_EXIT%)
echo   Raw log: %LOGFILE%
echo ================================================================

REM Parse the J-Link mem32 lines into a labelled PASS/FAIL report.
powershell -NoProfile -ExecutionPolicy Bypass -File ^
    "C:\TouchGFXProjects\MyApplication\EWARM\boot_pipeline_parse.ps1" ^
    "%LOGFILE%" "%REPORT%"

if exist "%REPORT%" type "%REPORT%"

endlocal
exit /b %JLINK_EXIT%
