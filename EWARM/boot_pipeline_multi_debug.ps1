# boot_pipeline_multi_debug.ps1 -- Debug-config variant of the 25-checkpoint
# multi-invocation cspybat probe.
#
# Identical logic to boot_pipeline_multi.ps1 except points at the CM7 Debug
# configuration. The Debug build has Music_InjectTestThumb compiled in (it is
# stripped by RELEASE_BUILD), so the framebuffer actually gets painted with
# the test JPEG and our LCD/DMA2D probes have something real to inspect.

# Don't abort on cspybat's stderr noise (e.g. "DMAC: DBGMCU_CR was changed"
# is a harmless connection note that PowerShell would otherwise escalate).
$ErrorActionPreference = 'Continue'
$IAR_BIN  = 'C:\TouchGFXProjects\IAR 9.70.2\ewarm-9.70.2\common\bin'
$EWARM    = 'C:\TouchGFXProjects\MyApplication\EWARM'
$TEMPLATE = "$EWARM\boot_pipeline_one_bp.mac.tpl"
$GEN_XCL  = "$EWARM\settings\STM32H747I-DISCO.STM32H747I-DISCO_CM7.general.xcl"
$DRV_XCL  = "$EWARM\settings\STM32H747I-DISCO.STM32H747I-DISCO_CM7.driver.xcl"
$LOGDIR   = "$EWARM\runs"
$PERDIR   = "$LOGDIR\boot_pipeline_multi_debug"
$COMBINED = "$LOGDIR\boot_pipeline_multi_debug.log"

# Same checkpoint list as the Release probe -- the symbols all exist in Debug
# too (Debug has even more debug info, no symbols are stripped).
$CHECKPOINTS = @(
    'SystemInit',
    'main',
    'HAL_Init',
    'MPU_Config',
    'SystemClock_Config',
    'HAL_RCC_OscConfig',
    'HAL_RCC_ClockConfig',
    'MX_GPIO_Init',
    'MX_MDMA_Init',
    'MX_FMC_Init',
    'MX_DMA2D_Init',
    'MX_CRC_Init',
    'MX_JPEG_Init',
    'MX_QUADSPI_Init',
    'MX_UART8_Init',
    'MX_TouchGFX_PreOSInit',
    'osKernelInitialize',
    'osThreadNew',
    'vTaskStartScheduler',
    'TouchGFX_Task',
    'MX_DSIHOST_DSI_Init',
    'MX_LTDC_Init',
    'MX_TouchGFX_Init',
    'touchgfx_taskEntry',
    'HAL_DSI_Refresh'
)

$GEN_XCL_CLEAN = "$LOGDIR\boot_pipeline_multi_debug.general.xcl"
(Get-Content $GEN_XCL) | Where-Object { $_ -notmatch '^\s*--macro=' } | Set-Content $GEN_XCL_CLEAN

if (-not (Test-Path $LOGDIR)) { New-Item -ItemType Directory -Path $LOGDIR | Out-Null }
if (Test-Path $PERDIR) { Remove-Item -Recurse -Force $PERDIR }
New-Item -ItemType Directory -Path $PERDIR | Out-Null

$tpl = Get-Content -Raw $TEMPLATE

"=== boot_pipeline_multi_debug  $(Get-Date -Format 'yyyy-MM-dd HH:mm:ss')  config=STM32H747I-DISCO_CM7 (Debug) ===" | Out-File -Encoding utf8 $COMBINED
"" | Add-Content $COMBINED

$index = 0
foreach ($cp in $CHECKPOINTS) {
    $index++
    Write-Host "[$index/$($CHECKPOINTS.Count)] cspybat probe (Debug) at: $cp"

    $perMac = "$PERDIR\boot_$cp.mac"
    $perLog = "$PERDIR\boot_$cp.log"
    $tpl -replace '@@CHECKPOINT@@', $cp | Set-Content -Encoding ascii $perMac

    $cspyArgs = @(
        '--macro', "`"$perMac`"",
        '-f',      "`"$GEN_XCL_CLEAN`"",
        '--backend',
        '-f',      "`"$DRV_XCL`""
    )
    $cmdLine = "`"$IAR_BIN\CSpyBat.exe`" " + ($cspyArgs -join ' ') + " > `"$perLog`" 2>&1"
    & cmd.exe /c $cmdLine
    $rc = $LASTEXITCODE

    "================================================================" | Add-Content $COMBINED
    "  [$index/$($CHECKPOINTS.Count)]  $cp  (cspybat exit=$rc)"          | Add-Content $COMBINED
    "================================================================" | Add-Content $COMBINED
    Get-Content $perLog | Add-Content $COMBINED
    "" | Add-Content $COMBINED
}

Write-Host ""
Write-Host "================================================================"
Write-Host "  boot_pipeline_multi_debug complete -- $($CHECKPOINTS.Count) checkpoints"
Write-Host "  Combined: $COMBINED"
Write-Host "  Per-checkpoint: $PERDIR\*.log"
Write-Host "================================================================"
