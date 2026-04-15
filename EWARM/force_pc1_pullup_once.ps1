param(
    [Parameter(Mandatory = $true)]
    [string]$JLinkExe,

    [Parameter(Mandatory = $true)]
    [string]$RunLogDir
)

$ErrorActionPreference = 'Stop'

$gpioCPupdrAddr = 0x5802080C
$maskClear = [Convert]::ToUInt32('FFFFFFF3', 16)
$maskSet = [Convert]::ToUInt32('00000004', 16)

if (-not (Test-Path -LiteralPath $RunLogDir)) {
    New-Item -ItemType Directory -Path $RunLogDir | Out-Null
}

$readScript = Join-Path $RunLogDir 'pc1_pullup_read.jlink'
$writeScript = Join-Path $RunLogDir 'pc1_pullup_write.jlink'
$readLog = Join-Path $RunLogDir 'pc1_pullup_read.log'
$writeLog = Join-Path $RunLogDir 'pc1_pullup_write.log'

@(
    'si SWD',
    'speed 4000',
    'device STM32H747XI_M7',
    'connect',
    'h',
    ('mem32 0x{0:X8} 1' -f $gpioCPupdrAddr),
    'exit'
) | Set-Content -LiteralPath $readScript -Encoding ASCII

& $JLinkExe -NoGui 1 -CommandFile $readScript | Set-Content -LiteralPath $readLog -Encoding ASCII
if ($LASTEXITCODE -ne 0) {
    throw "J-Link read command failed with exit code $LASTEXITCODE"
}

$oldVal = $null
foreach ($line in Get-Content -LiteralPath $readLog) {
    if ($line -match '^5802080C\s*=\s*([0-9A-Fa-f]{8})') {
        $oldVal = [Convert]::ToUInt32($Matches[1], 16)
        break
    }
}

if ($null -eq $oldVal) {
    throw "Could not read GPIOC->PUPDR from $readLog"
}

$newVal = [uint32]((([uint64]$oldVal) -band ([uint64]$maskClear)) -bor ([uint64]$maskSet))

@(
    'si SWD',
    'speed 4000',
    'device STM32H747XI_M7',
    'connect',
    'h',
    ('mem32 0x{0:X8} 1' -f $gpioCPupdrAddr),
    ('w4 0x{0:X8} 0x{1:X8}' -f $gpioCPupdrAddr, $newVal),
    ('mem32 0x{0:X8} 1' -f $gpioCPupdrAddr),
    'go',
    'exit'
) | Set-Content -LiteralPath $writeScript -Encoding ASCII

& $JLinkExe -NoGui 1 -CommandFile $writeScript | Set-Content -LiteralPath $writeLog -Encoding ASCII
if ($LASTEXITCODE -ne 0) {
    throw "J-Link write command failed with exit code $LASTEXITCODE"
}

Write-Output ('[TEST] GPIOC->PUPDR old=0x{0:X8} new=0x{1:X8} (PC1 bits[3:2] forced to pull-up=01)' -f $oldVal, $newVal)
Write-Output ('[TEST] Check last_raw in live watch at 0x24000084 (0x24000050 + 52) after resume.')
exit 0

