@echo off
setlocal

REM boot_pipeline_debug.bat -- same probe as boot_pipeline.bat, against the
REM CM7 Debug configuration (with the test-thumbnail injection compiled in).
REM
REM Use this to verify Phase 2 actually paints pixels: in Debug, the test
REM JPEG (Music_InjectTestThumb) drives the thumbnail pipeline so the four
REM sampled framebuffer positions should hold real RGB888 content.
REM
REM Use boot_pipeline.bat (Release) to verify FUIF/contention -- Release has
REM RTT/ITM stripped so it produces minimal AXI traffic outside TouchGFX.

set IAR_BIN=C:\TouchGFXProjects\IAR 9.70.2\ewarm-9.70.2\common\bin
set JLINK=C:\Program Files\SEGGER\JLink_V934b\JLink.exe
set PROJECT=C:\TouchGFXProjects\MyApplication\EWARM\STM32H747I-DISCO.ewp
set TARGET=STM32H747I-DISCO_CM7
set FLASH_SCRIPT=C:\TouchGFXProjects\MyApplication\EWARM\boot_pipeline_debug_flash.jlink
set PROBE_SCRIPT=C:\TouchGFXProjects\MyApplication\EWARM\boot_pipeline.jlink
set LOGDIR=C:\TouchGFXProjects\MyApplication\EWARM\runs
set LOGFILE=%LOGDIR%\boot_pipeline_debug.log
set REPORT=%LOGDIR%\boot_pipeline_debug.txt

if not exist "%LOGDIR%" mkdir "%LOGDIR%"

echo [1/4] Stopping any running debugger tools...
taskkill /F /IM iarbuild.exe >nul 2>&1
taskkill /F /IM CSpyBat.exe >nul 2>&1
taskkill /F /IM JLink.exe    >nul 2>&1

echo [2/4] Building Debug (CM7) configuration...
"%IAR_BIN%\iarbuild.exe" "%PROJECT%" -make "%TARGET%" -log warnings > "%LOGDIR%\boot_pipeline_debug_build.log" 2>&1
if %ERRORLEVEL% NEQ 0 (
    echo BUILD FAILED  -  see %LOGDIR%\boot_pipeline_debug_build.log
    type "%LOGDIR%\boot_pipeline_debug_build.log"
    exit /b 1
)

echo [3/4] Flashing CM7 Debug .out via J-Link (loadfile + reset + go)...
"%JLINK%" -device STM32H747XI_M7 -if SWD -speed 4000 -autoconnect 1 ^
    -CommanderScript "%FLASH_SCRIPT%" > "%LOGFILE%" 2>&1
if %ERRORLEVEL% NEQ 0 (
    echo FLASH FAILED  -  see %LOGFILE%
    type "%LOGFILE%"
    exit /b 1
)

REM Test thumb is injected at T+3s and re-injected every 1s after that.
REM Wait 7s so the first re-inject + paint cycle has fully landed.
timeout /t 7 /nobreak > nul

echo [4/4] Running boot_pipeline.jlink probe via J-Link Commander...
echo. >> "%LOGFILE%"
echo === J-Link probe at T+~7s after flash === >> "%LOGFILE%"
"%JLINK%" -device STM32H747XI_M7 -if SWD -speed 4000 -autoconnect 1 ^
    -CommanderScript "%PROBE_SCRIPT%" >> "%LOGFILE%" 2>&1

set JLINK_EXIT=%ERRORLEVEL%

echo.
echo ================================================================
echo   J-Link probe complete  (JLink exit = %JLINK_EXIT%)
echo   Raw log: %LOGFILE%
echo ================================================================

powershell -NoProfile -ExecutionPolicy Bypass -File ^
    "C:\TouchGFXProjects\MyApplication\EWARM\boot_pipeline_parse.ps1" ^
    "%LOGFILE%" "%REPORT%"

if exist "%REPORT%" type "%REPORT%"

endlocal
exit /b %JLINK_EXIT%
