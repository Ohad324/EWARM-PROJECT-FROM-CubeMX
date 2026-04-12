@echo off
setlocal
:: ============================================================================
:: DFSDM Debug Cycle — Full automation (JLink ONLY, no ST-Link)
:: Steps:
::   1. Kill IAR + JLink processes (release probe)
::   2. Build CM7 with iarbuild.exe
::   3. Flash firmware via JLink
::   4. Wait for MCU boot (4s)
::   5. Trigger recording + live register monitor via JLink
::
:: Usage: run from any directory, or via VSCode task "DFSDM Debug Cycle"
:: ============================================================================

set IARBUILD="C:\TouchGFXProjects\IAR 9.70.2\ewarm-9.70.2\common\bin\iarbuild.exe"
set JLINK="C:\Program Files\SEGGER\JLink\JLink.exe"
if not exist %JLINK% (
    set JLINK="C:\Program Files\SEGGER\JLink_V930a\JLink.exe"
)

set EWP=C:\TouchGFXProjects\MyApplication\EWARM\STM32H747I-DISCO.ewp
set OUT=C:\TouchGFXProjects\MyApplication\EWARM\STM32H747I-DISCO_CM7\Exe\STM32H747I-DISCO_CM7.out
set FLASH_SCRIPT=C:\TouchGFXProjects\MyApplication\EWARM\_flash.jlink
set MONITOR_SCRIPT=C:\TouchGFXProjects\MyApplication\EWARM\jlink_dfsdm_monitor.jlink
set LOG=C:\TouchGFXProjects\MyApplication\EWARM\dfsdm_debug_cycle.log
set JLINK_OPTS=-NoGui 1 -device STM32H747XI_M7 -if SWD -speed 4000 -autoconnect 1

echo ============================================================
echo  DFSDM DEBUG CYCLE — JLink only
echo ============================================================
echo.

:: ────────────────────────────────────────────────────────────
:: [1/5] Release all probes
:: ────────────────────────────────────────────────────────────
echo [1/5] Killing IAR + JLink processes...
taskkill /F /IM "IarIdePm.exe"        >nul 2>&1
taskkill /F /IM "JLink.exe"           >nul 2>&1
taskkill /F /IM "JLinkRTTViewer.exe"  >nul 2>&1
taskkill /F /IM "JLinkRTTLogger.exe"  >nul 2>&1
taskkill /F /IM "JLinkGDBServer.exe"  >nul 2>&1
timeout /t 2 /nobreak >nul
echo     Done.
echo.

:: ────────────────────────────────────────────────────────────
:: [2/5] Build CM7 target
:: ────────────────────────────────────────────────────────────
echo [2/5] Building CM7 target...
%IARBUILD% "%EWP%" -make "STM32H747I-DISCO_CM7" -log errors
if %ERRORLEVEL% neq 0 (
    echo.
    echo [FAIL] BUILD FAILED — fix errors before flashing.
    exit /b 1
)
echo     BUILD OK
echo.

:: ────────────────────────────────────────────────────────────
:: [3/5] Flash via JLink
:: ────────────────────────────────────────────────────────────
echo [3/5] Flashing via JLink...
if not exist "%OUT%" (
    echo [FAIL] Output file not found: %OUT%
    exit /b 1
)
%JLINK% %JLINK_OPTS% -CommandFile "%FLASH_SCRIPT%"
if %ERRORLEVEL% neq 0 (
    echo [FAIL] JLink flash failed.
    exit /b 1
)
taskkill /F /IM "JLink.exe" >nul 2>&1
echo     FLASH OK
echo.

:: ────────────────────────────────────────────────────────────
:: [4/5] Wait for MCU to boot fully
:: ────────────────────────────────────────────────────────────
echo [4/5] Waiting 4s for MCU boot (FreeRTOS + peripherals init)...
timeout /t 4 /nobreak >nul
echo     Boot wait complete.
echo.

:: ────────────────────────────────────────────────────────────
:: [5/5] Trigger recording + live register monitor
:: ────────────────────────────────────────────────────────────
echo [5/5] Running DFSDM monitor (trigger + snapshots)...
echo       This takes ~6 seconds (3s recording window + overhead).
echo.
%JLINK% %JLINK_OPTS% -CommandFile "%MONITOR_SCRIPT%" > "%LOG%" 2>&1
taskkill /F /IM "JLink.exe" >nul 2>&1

echo ============================================================
echo  DFSDM MONITOR OUTPUT:
echo ============================================================
type "%LOG%"
echo.
echo ============================================================
echo  Saved to: %LOG%
echo  INTERPRETATION GUIDE:
echo    FLTRDATAR snap1/2/3 all == 0x00EE6B03 → DFSDM input stuck high (all-1s PDM)
echo    FLTRDATAR changes between snaps        → DFSDM receiving live data
echo    NDTR stuck at 32 after trigger         → DMA not started (RDMAEN issue)
echo    Ch0_CHCFGR1 != 0x80180000             → CKOUTDIV write order bug still present
echo    SAI4 CR1 MCKDIV field != 12 (bits[25:20]) → CR1 override not applied
echo    SLOTR != 0x00010000                    → SlotActive=0 bug still present
echo ============================================================
pause
endlocal
