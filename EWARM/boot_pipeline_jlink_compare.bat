@echo off
setlocal

REM boot_pipeline_jlink_compare.bat -- apples-to-apples cspybat-vs-JLink probe.
REM
REM Same checkpoint (MX_TouchGFX_Init entry, 0x08026EA8), same LTDC regs, same
REM DAP path -- only difference is the probe-side software stack.
REM
REM cspybat snapshot at this halt is in EWARM\runs\boot_pipeline_multi.log
REM (search "[BP-HIT] MX_TouchGFX_Init" -- all 11 LTDC regs alias to 0xC0002220).
REM
REM This script writes its results to EWARM\runs\boot_pipeline_jlink_compare.log
REM where each "mem32 0xADDR 1" line is followed by "ADDR = VALUE". If those
REM values are distinct (L1CFBAR=0xD0000000, L1PFCR=0x01, etc.), cspybat is the
REM culprit. If they all read 0xC0002220 too, the LTDC peripheral aliases at
REM this halt and cspybat is exonerated.

set IAR_BIN=C:\TouchGFXProjects\IAR 9.70.2\ewarm-9.70.2\common\bin
set JLINK=C:\Program Files\SEGGER\JLink_V934b\JLink.exe
set PROJECT=C:\TouchGFXProjects\MyApplication\EWARM\STM32H747I-DISCO.ewp
set TARGET=Release
set JLINK_SCRIPT=C:\TouchGFXProjects\MyApplication\EWARM\boot_pipeline_jlink_compare.jlink
set LOGDIR=C:\TouchGFXProjects\MyApplication\EWARM\runs
set LOGFILE=%LOGDIR%\boot_pipeline_jlink_compare.log

if not exist "%LOGDIR%" mkdir "%LOGDIR%"

echo [1/3] Stopping any running debugger tools...
taskkill /F /IM iarbuild.exe >nul 2>&1
taskkill /F /IM CSpyBat.exe  >nul 2>&1
taskkill /F /IM IarIdePm.exe >nul 2>&1
taskkill /F /IM JLink.exe    >nul 2>&1

echo [2/3] Building Release configuration (no-op if up to date)...
"%IAR_BIN%\iarbuild.exe" "%PROJECT%" -make "%TARGET%" -log warnings > "%LOGDIR%\boot_pipeline_jlink_compare_build.log" 2>&1
if %ERRORLEVEL% NEQ 0 (
    type "%LOGDIR%\boot_pipeline_jlink_compare_build.log"
    exit /b 1
)

echo [3/3] J-Link: flash + setbp 0x08026EA8 + go + read LTDC at halt...
"%JLINK%" -device STM32H747XI_M7 -if SWD -speed 4000 -autoconnect 1 ^
    -CommanderScript "%JLINK_SCRIPT%" > "%LOGFILE%" 2>&1

set JLINK_RC=%ERRORLEVEL%

echo.
echo ================================================================
echo   J-Link halt-mode LTDC probe complete  (J-Link exit = %JLINK_RC%)
echo   Log: %LOGFILE%
echo ================================================================
type "%LOGFILE%"

endlocal
exit /b %JLINK_RC%
