@echo off
setlocal
set CSPYBAT=C:\TouchGFXProjects\IAR 9.70.2\ewarm-9.70.2\common\bin\cspybat.exe
set GENERAL=C:\TouchGFXProjects\MyApplication\EWARM\settings\STM32H747I-DISCO.STM32H747I-DISCO_CM7.general.xcl
set DRIVER=C:\TouchGFXProjects\MyApplication\EWARM\settings\STM32H747I-DISCO.STM32H747I-DISCO_CM7.driver.xcl
set TRIGMAC=C:\TouchGFXProjects\MyApplication\EWARM\trigger_and_dump_session.mac
set LOG=C:\TouchGFXProjects\MyApplication\EWARM\runs\attach_trigger.log

echo === Attach + trigger ===
"%CSPYBAT%" --attach_to_running_target --leave_target_running --macro "%TRIGMAC%" -f "%GENERAL%" --backend -f "%DRIVER%" > "%LOG%" 2>&1
echo Exit: %ERRORLEVEL%
echo.
type "%LOG%"
endlocal
