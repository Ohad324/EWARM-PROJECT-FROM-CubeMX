@echo off
set JLINK="C:\Program Files\SEGGER\JLink\JLink.exe"
if not exist %JLINK% set JLINK="C:\Program Files\SEGGER\JLink_V930a\JLink.exe"
%JLINK% -NoGui 1 -device STM32H747XI_M7 -if SWD -speed 4000 -autoconnect 1 -CommandFile "C:\TouchGFXProjects\MyApplication\EWARM\jlink_dfsdm_monitor.jlink"
