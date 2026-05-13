# verify_pfb.ps1 -- Partial Framebuffer architecture verification orchestrator.
#
# Runs cspybat once per checkpoint, generating a per-iteration .mac from the
# verify_pfb_one_bp.mac.tpl template.  Each run targets ONE intended stop
# point (one useful BP hit per cspybat session is the reliable model in this
# environment).  Together, the runs map the full PFB boot/render path.
#
# 15 checkpoints, ordered: foundation -> LCD init -> first frame -> sustained.

$ErrorActionPreference = 'Continue'
$IAR_BIN  = 'C:\TouchGFXProjects\IAR 9.70.2\ewarm-9.70.2\common\bin'
$EWARM    = 'C:\TouchGFXProjects\MyApplication\EWARM'
$TEMPLATE = "$EWARM\verify_pfb_one_bp.mac.tpl"
$GEN_XCL  = "$EWARM\settings\STM32H747I-DISCO.STM32H747I-DISCO_CM7.general.xcl"
$DRV_XCL  = "$EWARM\settings\STM32H747I-DISCO.STM32H747I-DISCO_CM7.driver.xcl"
$LOGDIR   = "$EWARM\runs"
$PERDIR   = "$LOGDIR\verify_pfb"
$COMBINED = "$LOGDIR\verify_pfb.log"

# Per "stop-and-fix at first failing BP" rule: while pre-kernel boot is broken,
# run only BP-03 (osKernelInitialize). Restore the full 15-checkpoint list once
# BP-03 fires cleanly. Full list preserved below as comments.
$CHECKPOINTS = @(
    @{ Symbol = 'osKernelInitialize';            Skip = 0;  Tag = '03_pre_kernel' }
    # @{ Symbol = '{main.c}.267';                  Skip = 0;  Tag = '01_post_mpu' }
    # @{ Symbol = 'SystemClock_Config';            Skip = 0;  Tag = '02_post_sram1_clk' }
    # @{ Symbol = 'MX_LTDC_Init';                  Skip = 0;  Tag = '04_ltdc_init_entry' }
    # @{ Symbol = 'HAL_DSI_Start';                 Skip = 0;  Tag = '05_dsi_start_entry' }
    # @{ Symbol = 'TouchGFXHAL::initialize';       Skip = 0;  Tag = '06_touchgfx_init' }
    # @{ Symbol = 'HAL_DSI_Refresh';               Skip = 0;  Tag = '07_first_refresh' }
    # @{ Symbol = 'HAL_DSI_EndOfRefreshCallback';  Skip = 0;  Tag = '08_first_eor' }
    # @{ Symbol = 'HAL_DSI_EndOfRefreshCallback';  Skip = 1;  Tag = '09_second_eor' }
    # @{ Symbol = 'HAL_DSI_EndOfRefreshCallback';  Skip = 3;  Tag = '10_frame_complete' }
    # @{ Symbol = 'HAL_DSI_TearingEffectCallback'; Skip = 0;  Tag = '11_first_te' }
    # @{ Symbol = 'HAL_DSI_TearingEffectCallback'; Skip = 10; Tag = '12_te_sustained' }
    # @{ Symbol = 'HAL_DSI_Refresh';               Skip = 60; Tag = '13_refresh_sustained' }
    # @{ Symbol = 'HAL_DSI_EndOfRefreshCallback';  Skip = 60; Tag = '14_stain_test' }
    # @{ Symbol = 'LTDC_ER_IRQHandler';            Skip = 0;  Tag = '15_ltdc_err_should_never_fire' }
)

# Strip any --macro= line from general.xcl so our --macro is the only one
$GEN_XCL_CLEAN = "$LOGDIR\verify_pfb.general.xcl"
(Get-Content $GEN_XCL) | Where-Object { $_ -notmatch '^\s*--macro=' } | Set-Content $GEN_XCL_CLEAN

if (-not (Test-Path $LOGDIR)) { New-Item -ItemType Directory -Path $LOGDIR | Out-Null }
if (Test-Path $PERDIR) { Remove-Item -Recurse -Force $PERDIR }
New-Item -ItemType Directory -Path $PERDIR | Out-Null

$tpl = Get-Content -Raw $TEMPLATE

"=== verify_pfb  $(Get-Date -Format 'yyyy-MM-dd HH:mm:ss')  config=Debug ===" | Out-File -Encoding utf8 $COMBINED
"" | Add-Content $COMBINED

$index = 0
foreach ($cp in $CHECKPOINTS) {
    $index++
    $sym  = $cp.Symbol
    $skip = $cp.Skip
    $tag  = $cp.Tag
    Write-Host "[$index/$($CHECKPOINTS.Count)] verify_pfb: $tag ($sym, skip=$skip)"

    $perMac = "$PERDIR\$tag.mac"
    $perLog = "$PERDIR\$tag.log"
    $rendered = $tpl -replace '@@CHECKPOINT@@', $sym -replace '@@SKIP@@', $skip
    $rendered | Set-Content -Encoding ascii $perMac

    # Release any probes that the previous iteration may have held
    Get-Process -ErrorAction SilentlyContinue -Name 'CSpyBat','JLink','JLinkRTTViewer','JLinkRTTLogger' |
        Stop-Process -Force -ErrorAction SilentlyContinue

    $cspyArgs = @(
        '--macro', "`"$perMac`"",
        '-f',      "`"$GEN_XCL_CLEAN`"",
        '--backend',
        '-f',      "`"$DRV_XCL`""
    )
    $cmdLine = "`"$IAR_BIN\CSpyBat.exe`" " + ($cspyArgs -join ' ') + " > `"$perLog`" 2>&1"

    # 30-second per-iteration host watchdog: skip=60 hits in ~1 sec at 60 Hz
    # plus boot ~5 sec.  If cspybat doesn't return inside 30 s, kill it -- the
    # macro action probably already completed and we just need to advance.
    $job = Start-Job -ScriptBlock { param($c) cmd.exe /c $c } -ArgumentList $cmdLine
    if (-not (Wait-Job -Job $job -Timeout 30)) {
        Stop-Job -Job $job
        Get-Process -ErrorAction SilentlyContinue -Name 'CSpyBat','JLink' |
            Stop-Process -Force -ErrorAction SilentlyContinue
        "[orchestrator] iteration timed out after 30 s -- killed cspybat" | Add-Content $perLog
    }
    $rc = $job.State
    Remove-Job -Job $job -Force

    "================================================================" | Add-Content $COMBINED
    "  [$index/$($CHECKPOINTS.Count)]  $tag  ($sym, skip=$skip, state=$rc)" | Add-Content $COMBINED
    "================================================================" | Add-Content $COMBINED
    if (Test-Path $perLog) { Get-Content $perLog | Add-Content $COMBINED }
    "" | Add-Content $COMBINED
}

Write-Host ""
Write-Host "================================================================"
Write-Host "  verify_pfb complete -- $($CHECKPOINTS.Count) checkpoints"
Write-Host "  Combined log:    $COMBINED"
Write-Host "  Per-checkpoint:  $PERDIR\*.log"
Write-Host "================================================================"
