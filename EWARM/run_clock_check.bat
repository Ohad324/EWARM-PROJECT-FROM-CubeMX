@echo off
echo Press the RECORD button on the board first, then press any key here...
pause
echo.
echo Reading clock registers (board must be RECORDING now)...
echo.
"C:\Program Files\SEGGER\JLink_V930a\JLink.exe" -device STM32H747XI_M7 -if SWD -speed 4000 -autoconnect 1 -CommandFile "%~dp0clock_check.jlink"
echo.
echo EXPECTED (during recording):
echo   0x58005004 SAI4 ACR1    : bit16=SAIEN=1, bits[1:0]=01
echo   0x58005044 SAI4 PDMCR  : 0x00000101
echo   0x58024490 RCC D3CCIPR : bits[23:21]=100 (per_ck)
echo   0x58025408 BDMA Ch1 CCR: bit0=1 (EN), bit5=1 (CIRC)
echo   0x5802540C BDMA Ch1 CNDTR: 8
echo   0x40017420 DFSDM Ch1 CFG1: 0x00000085
echo   0x40026028 DMA1 S1 CR  : bit0=1 (EN)
echo   0x40026034 DMA1 S1 M0AR: 0x30004000
echo   0x58021020 GPIOE AFRL  : bits[11:8]=0xA (AF10)
pause
