# boot_pipeline_lcd.ps1 -- LCD/DMA2D pipeline probe orchestrator.
#
# Targets the CM7 Debug build (which has Music_InjectTestThumb compiled in,
# unlike Release where it's stripped by RELEASE_BUILD). Runs cspybat once
# per LCD checkpoint, generating a per-iteration .mac from the LCD template.
#
# Each entry in $CHECKPOINTS is @{ Symbol=...; Skip=N; Tag=... }, where Skip
# is the BP skip count (0 = first hit, N = N-th hit).

$ErrorActionPreference = 'Continue'
$IAR_BIN  = 'C:\TouchGFXProjects\IAR 9.70.2\ewarm-9.70.2\common\bin'
$EWARM    = 'C:\TouchGFXProjects\MyApplication\EWARM'
$TEMPLATE = "$EWARM\boot_pipeline_lcd_one_bp.mac.tpl"
$GEN_XCL  = "$EWARM\settings\STM32H747I-DISCO.STM32H747I-DISCO_CM7.general.xcl"
$DRV_XCL  = "$EWARM\settings\STM32H747I-DISCO.STM32H747I-DISCO_CM7.driver.xcl"
$LOGDIR   = "$EWARM\runs"
$PERDIR   = "$LOGDIR\boot_pipeline_lcd"
$COMBINED = "$LOGDIR\boot_pipeline_lcd.log"

# LCD/DMA2D probe checkpoints, ordered from "boot baseline" -> "first refresh"
# -> "after test thumb inject" -> "fault-time" -> "sustained rendering".
$CHECKPOINTS = @(
    @{ Symbol = 'HAL_DSI_Refresh';                Skip = 0;    Tag = 'first_refresh_baseline' }
    @{ Symbol = 'HAL_DSI_Refresh';                Skip = 60;   Tag = 'refresh_60_early_render' }
    @{ Symbol = 'HAL_DSI_Refresh';                Skip = 200;  Tag = 'refresh_200_post_thumb_inject' }
    @{ Symbol = 'HAL_DSI_Refresh';                Skip = 300;  Tag = 'refresh_300_sustained' }
    @{ Symbol = 'HAL_DSI_EndOfRefreshCallback';   Skip = 0;    Tag = 'first_EOR' }
    @{ Symbol = 'HAL_DSI_EndOfRefreshCallback';   Skip = 200;  Tag = 'EOR_200_post_thumb' }
    @{ Symbol = 'LTDC_ER_IRQHandler';             Skip = 0;    Tag = 'first_LTDC_error' }
    @{ Symbol = 'Music_InjectTestThumb';          Skip = 0;    Tag = 'first_thumb_inject' }
    @{ Symbol = 'Music_InjectTestThumb';          Skip = 1;    Tag = 'second_thumb_inject' }
)

$GEN_XCL_CLEAN = "$LOGDIR\boot_pipeline_lcd.general.xcl"
(Get-Content $GEN_XCL) | Where-Object { $_ -notmatch '^\s*--macro=' } | Set-Content $GEN_XCL_CLEAN

if (-not (Test-Path $LOGDIR)) { New-Item -ItemType Directory -Path $LOGDIR | Out-Null }
if (Test-Path $PERDIR) { Remove-Item -Recurse -Force $PERDIR }
New-Item -ItemType Directory -Path $PERDIR | Out-Null

$tpl = Get-Content -Raw $TEMPLATE

"=== boot_pipeline_lcd  $(Get-Date -Format 'yyyy-MM-dd HH:mm:ss')  config=Debug ===" | Out-File -Encoding utf8 $COMBINED
"" | Add-Content $COMBINED

$index = 0
foreach ($cp in $CHECKPOINTS) {
    $index++
    $sym  = $cp.Symbol
    $skip = $cp.Skip
    $tag  = $cp.Tag
    Write-Host "[$index/$($CHECKPOINTS.Count)] LCD probe: $tag ($sym, skip=$skip)"

    $perMac = "$PERDIR\$tag.mac"
    $perLog = "$PERDIR\$tag.log"
    $rendered = $tpl -replace '@@CHECKPOINT@@', $sym -replace '@@SKIP@@', $skip
    $rendered | Set-Content -Encoding ascii $perMac

    $cspyArgs = @(
        '--macro', "`"$perMac`"",
        '-f',      "`"$GEN_XCL_CLEAN`"",
        '--backend',
        '-f',      "`"$DRV_XCL`""
    )
    # 30s host-side timeout: skip=300 with 60Hz refresh = ~5 sec to reach BP.
    # cspybat doesn't have a host watchdog so just run it; user CTRL+C if hung.
    $cmdLine = "`"$IAR_BIN\CSpyBat.exe`" " + ($cspyArgs -join ' ') + " > `"$perLog`" 2>&1"
    & cmd.exe /c $cmdLine
    $rc = $LASTEXITCODE

    "================================================================" | Add-Content $COMBINED
    "  [$index/$($CHECKPOINTS.Count)]  $tag  ($sym, skip=$skip, exit=$rc)" | Add-Content $COMBINED
    "================================================================" | Add-Content $COMBINED
    Get-Content $perLog | Add-Content $COMBINED
    "" | Add-Content $COMBINED
}

Write-Host ""
Write-Host "================================================================"
Write-Host "  boot_pipeline_lcd complete -- $($CHECKPOINTS.Count) LCD probes"
Write-Host "  Combined: $COMBINED"
Write-Host "  Per-checkpoint: $PERDIR\*.log"
Write-Host "================================================================"
