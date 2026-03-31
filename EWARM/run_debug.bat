@echo off
REM ============================================================================
REM  run_debug.bat — launch CSpyBat with pipeline trace breakpoints
REM  Uses exact settings from IAR IDE session (C:\iar\ewarm-9.70.2)
REM ============================================================================

set IAR=C:\iar\ewarm-9.70.2
set PROJ=C:\TouchGFXProjects\MyApplication
set SETTINGS=%PROJ%\EWARM\settings
set OUT=%PROJ%\EWARM\STM32H747I-DISCO_CM7\Exe\STM32H747I-DISCO_CM7.out
set MACRO=%PROJ%\debug_pipeline.mac
set LOG=C:\TouchGFXProjects\cspy_stdout.txt
set ERR=C:\TouchGFXProjects\cspy_stderr.txt

echo.
echo === CSpyBat pipeline trace ===
echo.

"%IAR%\common\bin\CSpyBat.exe" ^
  -f "%SETTINGS%\cspy_cm7_general.xcl" ^
  --macro "%MACRO%" ^
  --timeout 30000 ^
  --backend ^
  -f "%SETTINGS%\cspy_cm7_driver.xcl" ^
  > "%LOG%" 2>"%ERR%"

echo Exit: %ERRORLEVEL%
echo.
echo === STDOUT ===
type "%LOG%"
echo.
echo === STDERR ===
type "%ERR%"
echo.
pause
