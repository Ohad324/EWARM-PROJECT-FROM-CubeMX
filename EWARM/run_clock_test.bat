@echo off
setlocal EnableDelayedExpansion
set JLINK="C:\Program Files\SEGGER\JLink_V930a\JLink.exe"
set ARGS=-device STM32H747XI_M7 -if SWD -speed 4000 -autoconnect 1
set SCRIPTS=%~dp0

:: ── timestamped log file in runs\ ────────────────────────────────────────────
if not exist "%SCRIPTS%runs" mkdir "%SCRIPTS%runs"
set D=%DATE%
set T=%TIME%
set YY=%D:~10,4%
set MM=%D:~4,2%
set DD=%D:~7,2%
set HH=%T:~0,2%
set MI=%T:~3,2%
set SS=%T:~6,2%
set HH=%HH: =0%
set LOG=%SCRIPTS%runs\clock_test_%YY%%MM%%DD%_%HH%%MI%%SS%.log

:: helper macro: write a line to screen AND log
:: Usage: call :tee "text"
goto :main

:tee
    echo %~1
    echo %~1 >> "%LOG%"
    goto :eof

:tee_blank
    echo.
    echo. >> "%LOG%"
    goto :eof

:main

call :tee "================================================================"
call :tee "  CLOCK-TO-MIC AUTOMATED TEST"
call :tee "  Log: %LOG%"
call :tee "================================================================"
call :tee_blank

:: ── PHASE 1: flash ───────────────────────────────────────────────────────────
call :tee "[PHASE 1] Flashing firmware..."
call :tee "----------------------------------------------------------------"

%JLINK% %ARGS% -CommandFile "%SCRIPTS%clock_test_flash.jlink" >> "%LOG%" 2>&1
if errorlevel 1 (
    call :tee "FLASH FAILED - check J-Link connection"
    pause & exit /b 1
)
call :tee "Flash OK."
call :tee_blank

:: ── PHASE 2+3: live DAP monitoring ───────────────────────────────────────────
call :tee "[PHASE 2] DAP live monitoring — 20 pre-recording + 10 recording samples"
call :tee "----------------------------------------------------------------"
call :tee_blank
call :tee "REGISTER LEGEND (12 values per sample, in order):"
call :tee "  --- D3AMR (read once at startup — autonomous mode gate for D3 peripherals) ---"
call :tee "  [S1] 0x580244A8 RCC_D3AMR      bit4=GPIOEEN want 1 | bit28=SAI4EN want 1"
call :tee "  --- CLOCK ENABLES (if 0, peripheral registers return 0 or bus hangs) ---"
call :tee "  [01] 0x580244F4 RCC_APB4ENR    bit21=SAI4EN want 1"
call :tee "  [02] 0x580244E0 RCC_AHB4ENR    bit4=GPIOEEN want 1 | bit21=BDMAEN want 1"
call :tee "  --- CLOCK SOURCE ---"
call :tee "  [03] 0x58024458 RCC D3CCIPR    SAI4ASEL bits[23:21] want 4 = per_ck/HSI64"
call :tee "  --- SAI4 (PE2 clock generator) ---"
call :tee "  [04] 0x58005404 SAI4 CR1       MCKDIV[24:20] want 16 | MODE[1:0] want 01 | SAIEN[16] want 1"
call :tee "  [05] 0x58005444 SAI4 PDMCR     want 0x00000101"
call :tee "  --- GPIO PE2 (physical clock pin) ---"
call :tee "  [06] 0x58021000 GPIOE MODER    PE2 bits[5:4] want 10 (AF mode)"
call :tee "  [07] 0x58021020 GPIOE AFRL     PE2 bits[11:8] want A  (AF10 = SAI4_CK1)"
call :tee "  [08] 0x58021008 GPIOE OSPEEDR  PE2 bits[5:4] want 11 (Very High Speed)"
call :tee "  --- BDMA Ch1 (keeps SAI4 FIFO drained so SAIEN stays latched) ---"
call :tee "  [09] 0x5802541C BDMA Ch1 CCR   bit0=EN want 1 | bit5=CIRC want 1"
call :tee "  [10] 0x58025420 BDMA Ch1 CNDTR live count 1-8 = active"
call :tee "  [11] 0x58025424 BDMA Ch1 CPAR  want 0x58005420 (SAI4 Block_A DR)"
call :tee "  [12] 0x58025428 BDMA Ch1 CM0AR want 0x38000000 (D3 SRAM4)"
call :tee_blank
call :tee "--- PRE-RECORDING (20 samples x 250ms = 5s after main) ---"
echo --- PRE-RECORDING (20 samples x 250ms = 5s after main) --- >> "%LOG%"

:: run JLink — pipe to PowerShell Tee so output is live on screen AND saved
%JLINK% %ARGS% -CommandFile "%SCRIPTS%clock_test_check.jlink" 2>&1 | powershell -Command "& { $input | Tee-Object -FilePath '%LOG%' -Append }"

call :tee_blank
call :tee "================================================================"
call :tee "  QUICK DIAGNOSIS GUIDE"
call :tee "================================================================"
call :tee_blank
call :tee "  PRE-RECORDING samples (first 20 x 12 values):"
call :tee "    [01] APB4ENR bit21 = 1   -> SAI4 clock gate ON         (0 = peripheral invisible)"
call :tee "    [02] AHB4ENR bit4  = 1   -> GPIOE clock gate ON        (0 = PE2 pin invisible)"
call :tee "    [02] AHB4ENR bit21 = 1   -> BDMA clock gate ON         (0 = BDMA invisible)"
call :tee "    [04] SAI4 CR1 bit16 = 0  -> SAIEN off at idle          (FAIL if already 1)"
call :tee "    [09] BDMA CCR bit0  = 0  -> BDMA not running at idle   (FAIL if already 1)"
call :tee_blank
call :tee "  RECORDING samples (last 10 x 12 values, after button trigger):"
call :tee "    [04] SAI4 CR1 bit16 = 1  -> PE2 clock active           (FAIL = PE2 flat)"
call :tee "    [09] BDMA CCR bit0  = 1  -> FIFO drain running         (FAIL = PE2 goes flat)"
call :tee "    [09] BDMA CCR bit5  = 1  -> circular mode active       (FAIL = one-shot only)"
call :tee "    [10] BDMA CNDTR 1-8 live -> DMA is transferring        (frozen = stalled)"
call :tee "    [11] BDMA CPAR  = 58005420 -> source = SAI4 DR         (FAIL = wrong source)"
call :tee "    [12] BDMA CM0AR = 38000000 -> dest = D3 SRAM4          (FAIL = wrong domain)"
call :tee_blank
call :tee "  Key rule: if [09] BDMA EN=0, SAI4 FIFO fills and hardware kills SAIEN."
call :tee "  If SAIEN=1 but PE2 still flat, check [06]-[08] GPIO config."
call :tee_blank
call :tee "================================================================"
call :tee "  Log saved to: %LOG%"
call :tee "================================================================"

if /i "%1"=="auto" goto :eof
pause
