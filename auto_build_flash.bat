@echo off
setlocal

set IARBUILD="C:\TouchGFXProjects\IAR 9.70.2\ewarm-9.70.2\common\bin\iarbuild.exe"
set EWP=C:\TouchGFXProjects\MyApplication\EWARM\STM32H747I-DISCO.ewp
set JLINK="C:\Program Files\SEGGER\JLink_V930a\JLink.exe"
set CSPYBAT="C:\TouchGFXProjects\IAR 9.70.2\ewarm-9.70.2\common\bin\cspybat.exe"
set GENERAL_XCL=C:\TouchGFXProjects\MyApplication\EWARM\settings\STM32H747I-DISCO.STM32H747I-DISCO_CM7.general.xcl
set DRIVER_XCL=C:\TouchGFXProjects\MyApplication\EWARM\settings\STM32H747I-DISCO.STM32H747I-DISCO_CM7.driver.xcl
set OUTPUT_FILE=C:\TouchGFXProjects\MyApplication\EWARM\STM32H747I-DISCO_CM7\Exe\STM32H747I-DISCO_CM7.out
set FLASH_SCRIPT=C:\TouchGFXProjects\MyApplication\EWARM\_flash.jlink
set LOG=C:\TouchGFXProjects\MyApplication\EWARM\auto_flash.log

echo ============================================
echo  AUTO BUILD + FLASH
echo ============================================

:: Kill any probe holders
echo [1/4] Releasing probes...
taskkill /F /IM "IarIdePm.exe"       >nul 2>&1
taskkill /F /IM "JLink.exe"          >nul 2>&1
taskkill /F /IM "JLinkRTTViewer.exe" >nul 2>&1
taskkill /F /IM "JLinkRTTLogger.exe" >nul 2>&1
timeout /t 2 /nobreak >nul

:: Build
echo [2/4] Building CM7 target...
%IARBUILD% "%EWP%" -make "STM32H747I-DISCO_CM7" -log errors
if %ERRORLEVEL% neq 0 (
    echo BUILD FAILED
    exit /b 1
)
echo BUILD OK

:: Flash via JLink
echo [3/4] Flashing via J-Link...
%JLINK% -NoGui 1 -device STM32H747II_M7 -if SWD -speed 4000 -autoconnect 1 -CommandFile "%FLASH_SCRIPT%"
if %ERRORLEVEL% neq 0 (
    echo JLINK FLASH FAILED
    exit /b 1
)
echo JLINK FLASH OK

taskkill /F /IM "JLink.exe" >nul 2>&1
timeout /t 2 /nobreak >nul

:: cspybat reset+run
echo [4/4] cspybat reset+run...
%CSPYBAT% --download_only --silent -f "%GENERAL_XCL%" --backend -f "%DRIVER_XCL%"
if %ERRORLEVEL% neq 0 (
    echo CSPYBAT FAILED
    exit /b 1
)

echo ============================================
echo  FLASH COMPLETE — board running new firmware
echo ============================================
endlocal
