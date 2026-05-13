@echo off
setlocal

:: ═══════════════════════════════════════════════════════════════════════════
:: dfsdm_3dump_cycle.bat — Rebuild + Flash + CSpyBat 3-dump debug cycle
::
:: Steps:
::   1. Kill running debuggers
::   2. Rebuild CM7
::   3. Flash via JLink
::   4. Run CSpyBat with dfsdm_3dump.mac (3 register dumps)
::   5. Save output to timestamped log
:: ═══════════════════════════════════════════════════════════════════════════

:: ─── Paths ────────────────────────────────────────────────────────────────
set IAR_BIN=C:\TouchGFXProjects\IAR 9.70.2\ewarm-9.70.2\common\bin
set IAR_ARM=C:\TouchGFXProjects\IAR 9.70.2\ewarm-9.70.2\arm\bin
set JLINK=C:\Program Files\SEGGER\JLink_V930a\JLink.exe
set PROJECT=C:\TouchGFXProjects\MyApplication\EWARM\STM32H747I-DISCO.ewp
set TARGET=STM32H747I-DISCO_CM7
set EXE=C:\TouchGFXProjects\MyApplication\EWARM\STM32H747I-DISCO_CM7\Exe\STM32H747I-DISCO_CM7.out
set MACRODIR=C:\TouchGFXProjects\MyApplication\EWARM
set JLINK_SCRIPT=%MACRODIR%\_flash.jlink

:: ─── Timestamped log filename ─────────────────────────────────────────────
for /f "tokens=1-3 delims=/ " %%a in ("%DATE%") do set D=%%c%%b%%a
for /f "tokens=1-3 delims=:. " %%a in ("%TIME: =0%") do set T=%%a%%b%%c
set LOGFILE=%MACRODIR%\dfsdm_3dump_%D%_%T%.log

echo ══════════════════════════════════════════════════════════════
echo   DFSDM 3-Dump Cycle — %DATE% %TIME%
echo   Log: %LOGFILE%
echo ══════════════════════════════════════════════════════════════

:: ─── Step 1: Stop any running debuggers ───────────────────────────────────
echo [1/4] Stopping debuggers...
taskkill /F /IM CSpyBat.exe   >nul 2>&1
taskkill /F /IM iarbuild.exe  >nul 2>&1
taskkill /F /IM JLink.exe     >nul 2>&1
taskkill /F /IM IarIdePm.exe  >nul 2>&1
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
taskkill /F /IM JLink.exe >nul 2>&1
timeout /t 1 /nobreak >nul
echo Flash OK.

:: ─── Step 4: CSpyBat with 3-dump macro ──────────────────────────────────
echo [4/4] Running CSpyBat — 3 dumps (boot / 1.5s / 5s)...
echo   DUMP 1: at main (pre-init)
echo   SW trigger blue button
echo   DUMP 2: 1.5s after trigger (mid-recording)
echo   DUMP 3: 5.0s after trigger (post-recording)
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
    --macro="%MACRODIR%\dfsdm_3dump.mac" ^
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
echo   DEBUG SESSION COMPLETE
echo   Log saved to: %LOGFILE%
echo ══════════════════════════════════════════════════════════════
echo.
type "%LOGFILE%"

endlocal
