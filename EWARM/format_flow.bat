@echo off
setlocal

:: ============================================================
::  format_flow.bat — Full SD-card format flow (no button press needed)
::
::  Order:
::    1. Close IAR + J-Link
::    2. Launch format_sd.py in a separate window (starts waiting immediately)
::    3. Flash firmware + write force-format magic flag
::       → Board boots directly into USB MSC mode
::       → format_sd.py (already running) detects the drive, formats, ejects
::       → STM32 fires FMT DONE in ITM and resets
:: ============================================================

set JLINK="C:\Program Files\SEGGER\JLink_V930a\JLink.exe"
set FORMAT_JLINK=C:\TouchGFXProjects\MyApplication\EWARM\format_sd_mode.jlink
set FORMAT_PY=C:\TouchGFXProjects\MyApplication\tools\format_sd.py
set FORMAT_LOG=C:\TouchGFXProjects\MyApplication\EWARM\format_result.log
set FLASH_LOG=C:\TouchGFXProjects\MyApplication\EWARM\format_flow.log

echo ============================================
echo  SD FORMAT FLOW — Build + Flash + Format
echo ============================================
echo.

:: Step 1: Release probes
echo [1/3] Closing IAR, J-Link and any previous formatter...
taskkill /F /IM "IarIdePm.exe"       >nul 2>&1
taskkill /F /IM "JLink.exe"          >nul 2>&1
taskkill /F /IM "JLinkRTTViewer.exe" >nul 2>&1
taskkill /F /IM "JLinkRTTLogger.exe" >nul 2>&1
taskkill /F /IM "JLinkGDBServer.exe" >nul 2>&1
taskkill /F /IM "python.exe"         >nul 2>&1
timeout /t 2 /nobreak >nul
echo     Done.
echo.

:: Step 2: Launch format_sd.py NOW — it waits for the board to appear
echo [2/3] Starting format_sd.py (waiting for board)...
start "NORA SD Formatter" "C:\TouchGFXProjects\MyApplication\EWARM\run_formatter.bat"
echo     Formatter window opened — it will detect the board automatically.
echo.

:: Step 3: Flash + write force-format magic flag → board enters MSC mode immediately
echo [3/3] Flashing firmware + setting force-format flag...
%JLINK% -NoGui 1 -device STM32H747II_M7 -if SWD -speed 4000 -autoconnect 1 -CommandFile "%FORMAT_JLINK%" > "%FLASH_LOG%" 2>&1
if %ERRORLEVEL% neq 0 (
    echo.
    echo     JLINK FAILED — see %FLASH_LOG%
    pause
    exit /b 1
)
taskkill /F /IM "JLink.exe" >nul 2>&1
echo     Flash + flag OK.
echo.
echo ============================================
echo  Board is now running in USB MSC format mode.
echo  Watch the "NORA SD Formatter" window for result.
echo  Flash log: %FLASH_LOG%
echo ============================================
echo.
pause
