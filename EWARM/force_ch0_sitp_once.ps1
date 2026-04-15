param(
    [Parameter(Mandatory = $true)]
    [string]$JLinkExe,

    [Parameter(Mandatory = $true)]
    [string]$RunLogDir
)

$ErrorActionPreference = 'Stop'

# Firmware uses Channel 1 as data channel (RCSEL=1 in FLTCR1)
# Channel 0 is clock master only (DFSDMEN+CKOUTDIV)
$gpioCPupdrAddr = 0x5802080C
$ch1Cfg1Addr    = 0x40017420          # DFSDM1_Channel1->CHCFGR1
$flt0IsrAddr    = 0x40017508
$flt0IcrAddr    = 0x4001750C
$flt0RdataAddr  = 0x4001751C

$gDbgBase       = 0x24000050
$gDbgCh0Cfg1    = $gDbgBase + 0x00
$gDbgFlt0Isr    = $gDbgBase + 0x14
$gDbgLastRaw    = $gDbgBase + 0x34

$maskPullNone   = [Convert]::ToUInt32('FFFFFFF3', 16)

if (-not (Test-Path -LiteralPath $RunLogDir)) {
    New-Item -ItemType Directory -Path $RunLogDir | Out-Null
}

$writeScript  = Join-Path $RunLogDir 'ch1_sitp_write.jlink'
$storeScript  = Join-Path $RunLogDir 'ch1_sitp_store.jlink'
$writeLog     = Join-Path $RunLogDir 'ch1_sitp_write.log'
$storeLog     = Join-Path $RunLogDir 'ch1_sitp_store.log'

# Read current values first so we can apply surgical bit changes.
$readScript = Join-Path $RunLogDir 'ch1_sitp_read_pre.jlink'
$readLog    = Join-Path $RunLogDir 'ch1_sitp_read_pre.log'

@(
    'si SWD',
    'speed 4000',
    'device STM32H747XI_M7',
    'connect',
    'h',
    ('mem32 0x{0:X8} 1' -f $gpioCPupdrAddr),
    ('mem32 0x{0:X8} 1' -f $ch1Cfg1Addr),
    ('mem32 0x{0:X8} 1' -f $flt0IsrAddr),
    ('mem32 0x{0:X8} 1' -f $flt0RdataAddr),
    'go',
    'exit'
) | Set-Content -LiteralPath $readScript -Encoding ASCII

& $JLinkExe -NoGui 1 -CommandFile $readScript | Set-Content -LiteralPath $readLog -Encoding ASCII
if ($LASTEXITCODE -ne 0) {
    throw "J-Link pre-read failed with exit code $LASTEXITCODE"
}

$pupdrOld = $null
$cfg1Old = $null

foreach ($line in Get-Content -LiteralPath $readLog) {
    if (($null -eq $pupdrOld) -and ($line -match '^5802080C\s*=\s*([0-9A-Fa-f]{8})')) {
        $pupdrOld = [Convert]::ToUInt32($Matches[1], 16)
    }
    if (($null -eq $cfg1Old) -and ($line -match '^40017420\s*=\s*([0-9A-Fa-f]{8})')) {
        $cfg1Old = [Convert]::ToUInt32($Matches[1], 16)
    }
}

if ($null -eq $pupdrOld) { throw "Could not parse GPIOC->PUPDR from $readLog" }
if ($null -eq $cfg1Old)  { throw "Could not parse DFSDM1_Channel1->CHCFGR1 from $readLog (expected 40017420)" }

# Sanity: if cfg1Old is 0, firmware hasn't initialized — abort
if ($cfg1Old -eq 0) {
    throw "DFSDM1_Channel1->CHCFGR1 = 0x00000000 — firmware not initialized yet! This script must run AFTER boot."
}

$maskSitpClear = [Convert]::ToUInt32('FFFFFFFC', 16)
$maskSitpSet   = [Convert]::ToUInt32('00000001', 16)

$pupdrNew = [uint32](([uint64]$pupdrOld) -band ([uint64]$maskPullNone))
$cfg1New  = [uint32]((([uint64]$cfg1Old) -band ([uint64]$maskSitpClear)) -bor ([uint64]$maskSitpSet))

@(
    'si SWD',
    'speed 4000',
    'device STM32H747XI_M7',
    'connect',
    'h',
    ('w4 0x{0:X8} 0x{1:X8}' -f $gpioCPupdrAddr, $pupdrNew),
    ('w4 0x{0:X8} 0x{1:X8}' -f $ch1Cfg1Addr,    $cfg1New),
    ('w4 0x{0:X8} 0xFFFFFFFF' -f $flt0IcrAddr),
    'sleep 20',
    ('mem32 0x{0:X8} 1' -f $gpioCPupdrAddr),
    ('mem32 0x{0:X8} 1' -f $ch1Cfg1Addr),
    ('mem32 0x{0:X8} 1' -f $flt0IsrAddr),
    ('mem32 0x{0:X8} 1' -f $flt0RdataAddr),
    'go',
    'exit'
) | Set-Content -LiteralPath $writeScript -Encoding ASCII

& $JLinkExe -NoGui 1 -CommandFile $writeScript | Set-Content -LiteralPath $writeLog -Encoding ASCII
if ($LASTEXITCODE -ne 0) {
    throw "J-Link write phase failed with exit code $LASTEXITCODE"
}

$pupdrAfter = $null
$cfg1After  = $null
$isrAfter   = $null
$rawAfter   = $null

foreach ($line in Get-Content -LiteralPath $writeLog) {
    if (($null -eq $pupdrAfter) -and ($line -match '^5802080C\s*=\s*([0-9A-Fa-f]{8})')) {
        $pupdrAfter = [Convert]::ToUInt32($Matches[1], 16)
    }
    if (($null -eq $cfg1After) -and ($line -match '^40017420\s*=\s*([0-9A-Fa-f]{8})')) {
        $cfg1After = [Convert]::ToUInt32($Matches[1], 16)
    }
    if (($null -eq $isrAfter) -and ($line -match '^40017508\s*=\s*([0-9A-Fa-f]{8})')) {
        $isrAfter = [Convert]::ToUInt32($Matches[1], 16)
    }
    if (($null -eq $rawAfter) -and ($line -match '^4001751C\s*=\s*([0-9A-Fa-f]{8})')) {
        $rawAfter = [Convert]::ToUInt32($Matches[1], 16)
    }
}

if ($null -eq $cfg1After) { throw "Could not parse CH1 CHCFGR1 post-value from $writeLog" }
if ($null -eq $isrAfter)  { throw "Could not parse FLT0 ISR post-value from $writeLog" }
if ($null -eq $rawAfter)  { throw "Could not parse FLTRDATAR post-value from $writeLog" }

@(
    'si SWD',
    'speed 4000',
    'device STM32H747XI_M7',
    'connect',
    'h',
    ('w4 0x{0:X8} 0x{1:X8}' -f $gDbgCh0Cfg1, $cfg1After),
    ('w4 0x{0:X8} 0x{1:X8}' -f $gDbgFlt0Isr, $isrAfter),
    ('w4 0x{0:X8} 0x{1:X8}' -f $gDbgLastRaw, $rawAfter),
    ('mem32 0x{0:X8} 1' -f $gDbgCh0Cfg1),
    ('mem32 0x{0:X8} 1' -f $gDbgFlt0Isr),
    ('mem32 0x{0:X8} 1' -f $gDbgLastRaw),
    'go',
    'exit'
) | Set-Content -LiteralPath $storeScript -Encoding ASCII

& $JLinkExe -NoGui 1 -CommandFile $storeScript | Set-Content -LiteralPath $storeLog -Encoding ASCII
if ($LASTEXITCODE -ne 0) {
    throw "J-Link g_dbg store failed with exit code $LASTEXITCODE"
}

Write-Output ('[21:10 TEST] GPIOC->PUPDR old=0x{0:X8} new=0x{1:X8} (PC1 pull-up cleared)' -f $pupdrOld, $pupdrAfter)
Write-Output ('[21:10 TEST] DFSDM1_Channel1->CHCFGR1 old=0x{0:X8} new=0x{1:X8} (SITP forced to 01=falling)' -f $cfg1Old, $cfg1After)
Write-Output ('[21:10 TEST] DFSDM1_Filter0->FLTISR=0x{0:X8} FLTRDATAR=0x{1:X8}' -f $isrAfter, $rawAfter)
Write-Output ('[21:10 TEST] g_dbg updated: ch0_cfg1, flt0_isr, last_raw')
exit 0
exit 0
