@echo off
set LOG=C:\TouchGFXProjects\MyApplication\EWARM\SystemView\rtt_log.txt
del "%LOG%" 2>nul
"C:\Program Files\SEGGER\JLink_V930a\JLinkRTTLogger.exe" -device STM32H747XI_M7 -if SWD -speed 4000 -RTTChannel 0 -OutputFile "%LOG%"
