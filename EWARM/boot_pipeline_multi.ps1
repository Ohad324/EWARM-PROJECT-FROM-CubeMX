# boot_pipeline_multi.ps1 -- 10-checkpoint boot probe orchestrator.
#
# Workaround for cspybat 9.4.6.1706 (EWARM 9.70.2) firing only the first
# code BP per invocation. We run cspybat 10 times, one BP per call,
# generating a per-iteration .mac from the template.
#
# Usage (called by boot_pipeline_multi.bat after iarbuild succeeds):
#   pwsh -NoProfile -ExecutionPolicy Bypass -File boot_pipeline_multi.ps1
#
# Output:
#   EWARM\runs\boot_pipeline_multi.log     -- combined report
#   EWARM\runs\boot_pipeline_multi\*.log   -- per-checkpoint raw cspybat logs

# Don't abort on cspybat's stderr noise -- e.g. "DMAC: DBGMCU_CR was changed"
# is a harmless connection note that PowerShell would otherwise escalate.
$ErrorActionPreference = 'Continue'
$IAR_BIN  = 'C:\TouchGFXProjects\IAR 9.70.2\ewarm-9.70.2\common\bin'
$EWARM    = 'C:\TouchGFXProjects\MyApplication\EWARM'
$TEMPLATE = "$EWARM\boot_pipeline_one_bp.mac.tpl"
$GEN_XCL  = "$EWARM\settings\STM32H747I-DISCO.Release.general.xcl"
$DRV_XCL  = "$EWARM\settings\STM32H747I-DISCO.Release.driver.xcl"
$LOGDIR   = "$EWARM\runs"
$PERDIR   = "$LOGDIR\boot_pipeline_multi"
$COMBINED = "$LOGDIR\boot_pipeline_multi.log"

# Twenty boot-path checkpoints in chronological order.  Phase 1 = pre-
# osKernelStart, Phase 2 = inside guiTask after the scheduler dispatches.
# A BP that fails to install (handle 0) likely means the symbol was
# inlined or stripped in Release; the orchestrator logs and keeps going.
$CHECKPOINTS = @(
    # --- Phase 1: pre-kernel ---
    'SystemInit',                   #  1. before main, Reset_Handler -> SystemInit
    'main',                         #  2. C entry point
    'HAL_Init',                     #  3. ST HAL bring-up
    'MPU_Config',                   #  4. MPU regions configured
    'SystemClock_Config',           #  5. about to switch to PLL
    'HAL_RCC_OscConfig',            #  6. HSE/PLL setup
    'HAL_RCC_ClockConfig',          #  7. SYSCLK source switch
    'MX_GPIO_Init',                 #  8. GPIO config
    'MX_MDMA_Init',                 #  9. MDMA enabled
    'MX_FMC_Init',                  # 10. SDRAM controller up
    'MX_DMA2D_Init',                # 11. DMA2D enabled
    'MX_CRC_Init',                  # 12. CRC peripheral
    'MX_JPEG_Init',                 # 13. JPEG codec enabled
    'MX_QUADSPI_Init',              # 14. QSPI peripheral
    'MX_UART8_Init',                # 15. UART8 (NORA bridge)
    'MX_TouchGFX_PreOSInit',        # 16. last pre-RTOS step
    'osKernelInitialize',           # 17. CMSIS-RTOS init
    'osThreadNew',                  # 18. task creation (guiTask)
    'vTaskStartScheduler',          # 19. END OF PHASE 1
    # --- Phase 2: inside guiTask after scheduler dispatches ---
    'TouchGFX_Task'                 # 20. PHASE 2 BEGIN -- guiTask entry
)

# Strip --macro= from the IDE-generated general.xcl (we pass --macro on cmdline,
# having it in both produces "Macro X already defined" warnings).
$GEN_XCL_CLEAN = "$LOGDIR\boot_pipeline_multi.general.xcl"
(Get-Content $GEN_XCL) | Where-Object { $_ -notmatch '^\s*--macro=' } | Set-Content $GEN_XCL_CLEAN

if (-not (Test-Path $LOGDIR)) { New-Item -ItemType Directory -Path $LOGDIR | Out-Null }
if (Test-Path $PERDIR) { Remove-Item -Recurse -Force $PERDIR }
New-Item -ItemType Directory -Path $PERDIR | Out-Null

$tpl = Get-Content -Raw $TEMPLATE

# Reset combined log
"=== boot_pipeline_multi  $(Get-Date -Format 'yyyy-MM-dd HH:mm:ss') ===" | Out-File -Encoding utf8 $COMBINED
"" | Add-Content $COMBINED

$index = 0
foreach ($cp in $CHECKPOINTS) {
    $index++
    Write-Host "[$index/$($CHECKPOINTS.Count)] cspybat probe at checkpoint: $cp"

    # Render per-iteration .mac
    $perMac = "$PERDIR\boot_$cp.mac"
    $perLog = "$PERDIR\boot_$cp.log"
    $tpl -replace '@@CHECKPOINT@@', $cp | Set-Content -Encoding ascii $perMac

    # Run cspybat (full path -- CWD-independent). Capture stdout AND stderr
    # via cmd.exe redirect so the file ends up plain ANSI, not UTF-16, and
    # PowerShell doesn't try to escalate stderr lines to exceptions.
    $cspyArgs = @(
        '--macro', "`"$perMac`"",
        '-f',      "`"$GEN_XCL_CLEAN`"",
        '--backend',
        '-f',      "`"$DRV_XCL`""
    )
    $cmdLine = "`"$IAR_BIN\CSpyBat.exe`" " + ($cspyArgs -join ' ') + " > `"$perLog`" 2>&1"
    & cmd.exe /c $cmdLine
    $rc = $LASTEXITCODE

    # Append a labelled section to the combined log
    "================================================================" | Add-Content $COMBINED
    "  [$index/$($CHECKPOINTS.Count)]  $cp  (cspybat exit=$rc)"          | Add-Content $COMBINED
    "================================================================" | Add-Content $COMBINED
    Get-Content $perLog | Add-Content $COMBINED
    "" | Add-Content $COMBINED
}

Write-Host ""
Write-Host "================================================================"
Write-Host "  boot_pipeline_multi complete -- $($CHECKPOINTS.Count) checkpoints"
Write-Host "  Combined: $COMBINED"
Write-Host "  Per-checkpoint: $PERDIR\*.log"
Write-Host "================================================================"
