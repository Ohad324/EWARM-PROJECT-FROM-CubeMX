@echo off
REM pfb_dispatch.bat -- one-shot probe to count transmitBlock() dispatches.
REM Builds, flashes, then runs cspybat with pfb_dispatch_probe.mac.
REM BP fires at HAL_DSI_TearingEffectCallback skip=120 (~2 sec @60Hz).

setlocal
set IARBIN=C:\TouchGFXProjects\IAR 9.70.2\ewarm-9.70.2\common\bin
set EWARM=C:\TouchGFXProjects\MyApplication\EWARM
set LOG=%EWARM%\runs\pfb_dispatch.log

REM Release any held probes
taskkill /F /IM IarIdePm.exe   2>nul
taskkill /F /IM JLink.exe      2>nul
taskkill /F /IM CSpyBat.exe    2>nul

echo === Build CM7 ===
"%IARBIN%\iarbuild.exe" %EWARM%\STM32H747I-DISCO.ewp -make STM32H747I-DISCO_CM7 -log warnings
if errorlevel 1 (
    echo BUILD FAILED
    exit /b 1
)

REM Strip any --macro line from general.xcl
set GEN=%EWARM%\settings\STM32H747I-DISCO.STM32H747I-DISCO_CM7.general.xcl
set DRV=%EWARM%\settings\STM32H747I-DISCO.STM32H747I-DISCO_CM7.driver.xcl
set GENCLEAN=%EWARM%\runs\pfb_dispatch.general.xcl
findstr /v /b /c:"--macro=" "%GEN%" > "%GENCLEAN%"

echo === Run cspybat probe ===
"%IARBIN%\CSpyBat.exe" --macro "%EWARM%\pfb_dispatch_probe.mac" -f "%GENCLEAN%" --backend -f "%DRV%" > "%LOG%" 2>&1

echo === Done ===  log: %LOG%
type "%LOG%"
endlocal
