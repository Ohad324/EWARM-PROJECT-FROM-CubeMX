param(
    [Parameter(Mandatory = $true)]
    [string]$JLinkExe,

    [Parameter(Mandatory = $true)]
    [string]$ResultLog,

    [Parameter(Mandatory = $true)]
    [string]$RunLogDir
)

$ErrorActionPreference = 'Stop'

$fields = @(
    'ch0_cfg1',
    'ch0_cfg2',
    'flt0_cr1',
    'flt0_cr2',
    'flt0_fcr',
    'flt0_isr',
    'sai4_pdm',
    'dma_cr',
    'dma_ndtr',
    'dma_m0ar',
    'sitp',
    'spicksel',
    'dtrbs',
    'last_raw',
    'last_val',
    'uptime_ticks',
    'ref_lr_low',
    'ref_lr_high'
)

if (-not (Test-Path -LiteralPath $RunLogDir)) {
    New-Item -ItemType Directory -Path $RunLogDir | Out-Null
}

$jlinkScript = Join-Path $RunLogDir 'read_dfsdm_struct_24000050.jlink'
$jlinkOut    = Join-Path $RunLogDir 'dfsdm_struct_read.log'

@(
    'si SWD',
    'speed 4000',
    'device STM32H747XI_M7',
    'connect',
    'h',
    'mem32 0x24000050 18',
    'g',
    'exit'
) | Set-Content -LiteralPath $jlinkScript -Encoding ASCII

& $JLinkExe -NoGui 1 -CommandFile $jlinkScript | Set-Content -LiteralPath $jlinkOut -Encoding ASCII

$hexWords = New-Object System.Collections.Generic.List[string]
$regex = '^[0-9A-Fa-f]{8}\s*=\s*(.+)$'

Get-Content -LiteralPath $jlinkOut | ForEach-Object {
    if ($_ -match $regex) {
        $tail = $Matches[1].Trim()
        foreach ($token in ($tail -split '\s+')) {
            if ($token -match '^[0-9A-Fa-f]{8}$') {
                [void]$hexWords.Add($token.ToUpperInvariant())
            }
        }
    }
}

if ($hexWords.Count -lt 18) {
    $line = "[WARN] DFSDM struct parse incomplete: expected 18 words, got $($hexWords.Count). See $jlinkOut"
    Add-Content -LiteralPath $ResultLog -Value ""
    Add-Content -LiteralPath $ResultLog -Value $line
    return
}

$hexWords = $hexWords.GetRange(0, 18)
$timestamp = Get-Date -Format 'yyyy-MM-dd HH:mm:ss'

$table = New-Object System.Collections.Generic.List[string]
$table.Add('')
$table.Add('==== DFSDM PARAM SNAPSHOT (0x24000050, 18 words) ====')
$table.Add("Timestamp: $timestamp")
$table.Add('Index  Field         Value')
$table.Add('-----  ------------  ----------')

for ($i = 0; $i -lt 18; $i++) {
    $idx = "[$i]".PadRight(6)
    $name = $fields[$i].PadRight(12)
    $value = ('0x' + $hexWords[$i])
    $table.Add("$idx$name  $value")
}

$table.Add('')
$table.Add('Decoded quick bits:')

$ch0cfg1 = [Convert]::ToUInt32($hexWords[0], 16)
$ch0cfg2 = [Convert]::ToUInt32($hexWords[1], 16)
$sitp = $ch0cfg1 -band 0x3
$spicksel = ($ch0cfg1 -shr 2) -band 0x3
$dtrbs = ($ch0cfg2 -shr 3) -band 0x1F

$table.Add(("SITP(from ch0_cfg1): {0}" -f $sitp))
$table.Add(("SPICKSEL(from ch0_cfg1): {0}" -f $spicksel))
$table.Add(("DTRBS(from ch0_cfg2): {0}" -f $dtrbs))
$table.Add('===============================================')

Add-Content -LiteralPath $ResultLog -Value ($table -join [Environment]::NewLine)
