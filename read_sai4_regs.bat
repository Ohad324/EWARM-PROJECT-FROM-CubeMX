@echo off
REM Read SAI4 PDM + GPIO + BDMA registers from running STM32H747I-DISCO (CM7 core).
REM Attaches to running target (no halt/reset), reads registers, prints results.

set IAR=C:\TouchGFXProjects\IAR 9.70.2\ewarm-9.70.2
set EXE=%IAR%\common\bin\cspybat.exe
set OUT=C:\TouchGFXProjects\MyApplication\EWARM\STM32H747I-DISCO_CM7\Exe\STM32H747I-DISCO_CM7.out
set MAC=C:\TouchGFXProjects\MyApplication\EWARM\read_sai4_regs.mac

"%EXE%" ^
  "%IAR%\arm\bin\armproc.dll" ^
  "%IAR%\arm\bin\armjlink.dll" ^
  "%OUT%" ^
  --plugin="%IAR%\arm\bin\armLibSupportUniversal.dll" ^
  --device_macro="%IAR%\arm\config\debugger\ST\STM32H7xx.dmac" ^
  --device_macro="%IAR%\arm\config\debugger\ST\STM32H7x5_M7.dmac" ^
  --device_macro="%IAR%\arm\config\debugger\ST\STM32H7x5_DBG.dmac" ^
  --macro="%MAC%" ^
  --endian=little ^
  --cpu=Cortex-M7 ^
  --fpu=VFPv5_D16 ^
  -p "%IAR%\arm\config\debugger\ST\STM32H747XI_M7.ddf" ^
  --device=STM32H747XI_M7 ^
  --drv_communication=USB0 ^
  --drv_interface=SWD ^
  --drv_interface_speed=auto ^
  --jlink_initial_speed=1000 ^
  --jlink_reset_strategy=0,0 ^
  --drv_catch_exceptions=0x000 ^
  --attach_to_running_target ^
  --silent

echo.
echo Done.
pause
