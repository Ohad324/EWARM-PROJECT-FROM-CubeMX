@echo off
setlocal

:: ═══════════════════════════════════════════════════════════════════════════
:: dfsdm_runtime_check.bat — CSpyBat headless DFSDM register checker
::
:: Rebuilds, flashes, runs firmware, takes 4 timed register snapshots:
::   SNAP 1: Boot (at main)
::   SNAP 2: Idle (8s — firmware initialized)
::   SNAP 3: Mid-recording (2s after SW trigger)
::   SNAP 4: Post-recording (7s after trigger)
::
:: No breakpoints. Pure register reads via __readMemory32.
:: Output saved to EWARM\dfsdm_runtime_check.log
:: ═══════════════════════════════════════════════════════════════════════════

:: ─── Paths ────────────────────────────────────────────────────────────────
set IAR_BIN=C:\TouchGFXProjects\IAR 9.70.2\ewarm-9.70.2\common\bin
set IAR_ARM=C:\TouchGFXProjects\IAR 9.70.2\ewarm-9.70.2\arm\bin
set JLINK=C:\Program Files\SEGGER\JLink_V930a\JLink.exe
set PROJECT=C:\TouchGFXProjects\MyApplication\EWARM\STM32H747I-DISCO.ewp
set TARGET=STM32H747I-DISCO_CM7
set EXE=C:\TouchGFXProjects\MyApplication\EWARM\STM32H747I-DISCO_CM7\Exe\STM32H747I-DISCO_CM7.out
set MACRODIR=C:\TouchGFXProjects\MyApplication\EWARM
set LOGFILE=%MACRODIR%\dfsdm_runtime_check.log
set JLINK_SCRIPT=%MACRODIR%\_flash.jlink

echo ══════════════════════════════════════════════════════════════
echo   DFSDM Runtime Register Check — %DATE% %TIME%
echo ══════════════════════════════════════════════════════════════

:: ─── Step 1: Stop any running debuggers ───────────────────────────────────
echo [1/4] Stopping debuggers...
taskkill /F /IM CSpyBat.exe   >nul 2>&1
taskkill /F /IM iarbuild.exe  >nul 2>&1
taskkill /F /IM JLink.exe     >nul 2>&1
timeout /t 2 /nobreak >nul

:: ─── Step 2: Rebuild ─────────────────────────────────────────────────────
echo [2/4] Rebuilding %TARGET%...
"%IAR_BIN%\iarbuild.exe" "%PROJECT%" -make "%TARGET%" -log errors > "%MACRODIR%\build.log" 2>&1
if %ERRORLEVEL% NEQ 0 (
    echo BUILD FAILED — see build.log
    type "%MACRODIR%\build.log"
    pause
    exit /b 1
)
echo Build OK.

:: ─── Step 3: Flash via JLink ─────────────────────────────────────────────
echo [3/4] Flashing via JLink...
"%JLINK%" -device STM32H747XI -if SWD -speed 4000 -autoconnect 1 -CommandFile "%JLINK_SCRIPT%" > "%MACRODIR%\flash.log" 2>&1
if %ERRORLEVEL% NEQ 0 (
    echo FLASH FAILED — see flash.log
    type "%MACRODIR%\flash.log"
    pause
    exit /b 1
)
echo Flash OK.

:: ─── Step 4: CSpyBat with runtime check macro ───────────────────────────
echo [4/4] Running CSpyBat — 4 snapshots over ~20s...
echo   SNAP 1: boot       (T+0s)
echo   SNAP 2: idle       (T+8s)
echo   SW trigger button  (T+8s)
echo   SNAP 3: recording  (T+10s)
echo   SNAP 4: post-rec   (T+15s)
echo.
echo Output: %LOGFILE%
echo.

"%IAR_BIN%\CSpyBat.exe" ^
    "%IAR_ARM%\armproc.dll" ^
    "%IAR_ARM%\armjlink.dll" ^
    "%EXE%" ^
    --plugin="%IAR_ARM%\armLibSupportUniversal.dll" ^
    --device_macro="%IAR_ARM:bin=config/debugger/ST%\STM32H7xx.dmac" ^
    --device_macro="%IAR_ARM:bin=config/debugger/ST%\STM32H7x5_M7.dmac" ^
    --device_macro="%IAR_ARM:bin=config/debugger/ST%\STM32H7x5_DBG.dmac" ^
    --device_macro="%IAR_ARM:bin=config/debugger/ST%\STM32H7xx_OB.dmac" ^
    --device_macro="%IAR_ARM:bin=config/debugger/ST%\STM32H7xx_TRACE.dmac" ^
    --macro="%MACRODIR%\dfsdm_runtime_check.mac" ^
    --backend ^
    --endian=little ^
    --cpu=Cortex-M7 ^
    --fpu=VFPv5_D16 ^
    -p "%IAR_ARM:bin=config/debugger/ST%\STM32H747XI_M7.ddf" ^
    --semihosting ^
    --device=STM32H747XI_M7 ^
    --drv_communication=USB0 ^
    --drv_interface_speed=auto ^
    --jlink_initial_speed=1000 ^
    --jlink_reset_strategy=0,0 ^
    --drv_interface=SWD ^
    --drv_catch_exceptions=0x000 ^
    --drv_swo_clock_setup=400000000,1,2000000 ^
    > "%LOGFILE%" 2>&1

echo.
echo ══════════════════════════════════════════════════════════════
echo   COMPLETE — Results:
echo ══════════════════════════════════════════════════════════════
echo.
type "%LOGFILE%"
echo.
pause
