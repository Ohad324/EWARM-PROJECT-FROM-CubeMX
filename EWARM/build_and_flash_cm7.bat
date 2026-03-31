@echo off
setlocal

REM ── Build and flash STM32H747I-DISCO CM7 ─────────────────────────────────────
REM
REM Usage:   build_and_flash_cm7.bat          (build + flash)
REM          build_and_flash_cm7.bat --flash   (flash only, skip build)
REM
REM Requires IAR EWARM 9.70.2 at C:\iar\ewarm-9.70.2
REM J-Link must be connected via USB.
REM ─────────────────────────────────────────────────────────────────────────────

set IAR=C:\iar\ewarm-9.70.2
set IARBUILD=%IAR%\common\bin\iarbuild.exe
set CSPYBAT=%IAR%\common\bin\cspybat.exe

set PROJECT=%~dp0STM32H747I-DISCO.ewp
set CONFIG=STM32H747I-DISCO_CM7
set OUT=%~dp0STM32H747I-DISCO_CM7\Exe\STM32H747I-DISCO_CM7.out

set GENERAL_XCL=%~dp0settings\cspy_cm7_general.xcl
set DRIVER_XCL=%~dp0settings\cspy_cm7_driver.xcl

REM ── Step 1: Build (skip if --flash argument given) ────────────────────────────
if /i "%~1"=="--flash" goto flash

echo.
echo [BUILD] Building %CONFIG% ...
echo.

"%IARBUILD%" "%PROJECT%" -build "%CONFIG%" -log all
if errorlevel 1 (
    echo.
    echo [BUILD] FAILED. Fix errors before flashing.
    exit /b 1
)

echo.
echo [BUILD] OK.

REM ── Step 2: Flash ─────────────────────────────────────────────────────────────
:flash
echo.
echo [FLASH] Flashing %OUT% ...
echo.

"%CSPYBAT%" -f "%GENERAL_XCL%" --download_only --backend -f "%DRIVER_XCL%"

if errorlevel 1 (
    echo.
    echo [FLASH] FAILED. Check J-Link connection.
    exit /b 1
)

echo.
echo [FLASH] Done. Board is running.
echo.

endlocal
