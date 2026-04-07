@echo off
setlocal

:: =============================================================================
:: FlashSTM32 (Jlink+stlink).bat
:: Step 1 — JLink.exe    : program Flash memory
:: Step 2 — cspybat      : download active application via ST-Link
:: =============================================================================

set JLINK_EXE=C:\Program Files\SEGGER\JLink_V930a\JLink.exe
set CSPYBAT=C:\TouchGFXProjects\IAR 9.70.2\ewarm-9.70.2\common\bin\cspybat.exe
set GENERAL_XCL=C:\TouchGFXProjects\MyApplication\EWARM\settings\STM32H747I-DISCO.STM32H747I-DISCO_CM7.general.xcl
set DRIVER_XCL=C:\TouchGFXProjects\MyApplication\EWARM\settings\STM32H747I-DISCO.STM32H747I-DISCO_CM7.driver.xcl
set OUTPUT_FILE=C:\TouchGFXProjects\MyApplication\EWARM\STM32H747I-DISCO_CM7\Exe\STM32H747I-DISCO_CM7.out
set DEVICE=STM32H747II_M7
set INTERFACE=SWD
set SPEED=4000

if not exist "%OUTPUT_FILE%" (
    echo.
    echo [ERROR] Firmware not found: %OUTPUT_FILE%
    echo         Build the CM7 target in IAR first.
    echo.
    exit /b 1
)

:: ─────────────────────────────────────────────────────────────────────────────
:: [1/3] Release all probes
:: ─────────────────────────────────────────────────────────────────────────────
echo.
echo [1/3] Releasing probes...
taskkill /F /IM "IarIdePm.exe"       >nul 2>&1
taskkill /F /IM "JLink.exe"          >nul 2>&1
taskkill /F /IM "JLinkRTTViewer.exe" >nul 2>&1
taskkill /F /IM "JLinkRTTLogger.exe" >nul 2>&1
taskkill /F /IM "ST-LinkServer.exe"  >nul 2>&1
taskkill /F /IM "STLinkServer.exe"   >nul 2>&1
timeout /t 1 /nobreak >nul

:: ─────────────────────────────────────────────────────────────────────────────
:: [2/3] JLink — program Flash
:: ─────────────────────────────────────────────────────────────────────────────
echo [2/3] Flashing firmware via J-Link...
echo       %OUTPUT_FILE%
echo.

"%JLINK_EXE%" -device %DEVICE% -if %INTERFACE% -speed %SPEED% -autoconnect 1 -CommandFile "%~dp0_flash.jlink"

if %ERRORLEVEL% neq 0 (
    echo.
    echo [ERROR] J-Link flash FAILED  (exit code %ERRORLEVEL%)
    echo.
    exit /b 1
)

echo [OK] Flash programmed.

:: Release JLink before ST-Link takes over
taskkill /F /IM "JLink.exe" >nul 2>&1
timeout /t 1 /nobreak >nul

:: ─────────────────────────────────────────────────────────────────────────────
:: [3/3] cspybat — download active application via ST-Link
:: ─────────────────────────────────────────────────────────────────────────────
echo [3/3] Downloading active application via ST-Link...
echo.

"%CSPYBAT%" --download_only --silent -f "%GENERAL_XCL%" --backend -f "%DRIVER_XCL%"

if %ERRORLEVEL% neq 0 (
    echo.
    echo [ERROR] cspybat download FAILED  (exit code %ERRORLEVEL%)
    echo.
    exit /b 1
)

echo.
echo [OK] Done. Board is running new firmware.
echo.

:: Relaunch RTT Viewer so boot logs are captured immediately
timeout /t 1 /nobreak >nul
start "" "C:\Program Files\SEGGER\JLink_V930a\JLinkRTTViewer.exe"

endlocal
