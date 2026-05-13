@echo off
setlocal

set IAR_BIN=C:\TouchGFXProjects\IAR 9.70.2\ewarm-9.70.2\common\bin
set PROJECT=C:\TouchGFXProjects\MyApplication\EWARM\STM32H747I-DISCO.ewp
set TARGET=STM32H747I-DISCO_CM7
set MACRO=C:\TouchGFXProjects\MyApplication\EWARM\lcd_fb_debug_test.mac
set LOGDIR=C:\TouchGFXProjects\MyApplication\EWARM\runs
set LOGFILE=%LOGDIR%\lcd_fb_debug_test.log
set GENERAL_XCL=C:\TouchGFXProjects\MyApplication\EWARM\settings\STM32H747I-DISCO.STM32H747I-DISCO_CM7.general.xcl
set DRIVER_XCL=C:\TouchGFXProjects\MyApplication\EWARM\settings\STM32H747I-DISCO.STM32H747I-DISCO_CM7.driver.xcl
set GENERAL_XCL_CLEAN=%LOGDIR%\STM32H747I-DISCO_CM7.general.nomacro.xcl

if not exist "%LOGDIR%" mkdir "%LOGDIR%"

powershell -NoProfile -ExecutionPolicy Bypass -Command ^
    "$lines = Get-Content '%GENERAL_XCL%'; $filtered = $lines | Where-Object { $_ -notmatch '^\s*--macro=' }; $filtered + (' --macro=' + '%MACRO%') | Set-Content '%GENERAL_XCL_CLEAN%'"
if %ERRORLEVEL% NEQ 0 (
    echo FAILED TO PREPARE CLEAN XCL
    exit /b 1
)

echo [1/3] Stopping running debugger tools...
taskkill /F /IM iarbuild.exe >nul 2>&1
taskkill /F /IM CSpyBat.exe >nul 2>&1
taskkill /F /IM IarIdePm.exe >nul 2>&1

echo [2/3] Rebuilding Debug firmware...
"%IAR_BIN%\iarbuild.exe" "%PROJECT%" -make "%TARGET%" -log all > "%LOGDIR%\lcd_fb_debug_build.log" 2>&1
if %ERRORLEVEL% NEQ 0 (
    echo BUILD FAILED - see %LOGDIR%\lcd_fb_debug_build.log
    type "%LOGDIR%\lcd_fb_debug_build.log"
    exit /b 1
)

echo [3/3] Flashing, waiting for boot, then attaching CSpyBat probe...
"%IAR_BIN%\CSpyBat.exe" --download_only --silent -f "%GENERAL_XCL_CLEAN%" --backend -f "%DRIVER_XCL%" > "%LOGFILE%" 2>&1
if %ERRORLEVEL% NEQ 0 (
    echo FLASH FAILED - see %LOGFILE%
    type "%LOGFILE%"
    exit /b 1
)

timeout /t 4 /nobreak > nul

"%IAR_BIN%\CSpyBat.exe" --attach_to_running_target --leave_target_running --silent ^
    --macro "%MACRO%" ^
    -f "%GENERAL_XCL_CLEAN%" --backend -f "%DRIVER_XCL%" >> "%LOGFILE%" 2>&1

echo.
echo ================================================
echo LCD framebuffer Debug probe finished.
echo Log: %LOGFILE%
echo ================================================
type "%LOGFILE%"

endlocal