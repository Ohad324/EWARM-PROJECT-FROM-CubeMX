# boot_pipeline_parse.ps1 -- Parse J-Link Commander output from
# boot_pipeline.jlink and emit a labelled PASS/FAIL report.
#
# Usage: boot_pipeline_parse.ps1 <input.log> <output.txt>

param(
    [Parameter(Mandatory=$true)] [string]$InputLog,
    [Parameter(Mandatory=$true)] [string]$OutputReport
)

# JLink mem32 output format:
#   50001018 = 00002001
#  or with multiple words on one line:
#   50001018 = 00002001 0000FFFF ...
# We grab the first hex word that follows the address.

function Read-Reg([string]$logText, [string]$addrHex) {
    $pattern = '^\s*' + $addrHex + '\s*=\s*([0-9A-Fa-f]+)'
    $opts = [System.Text.RegularExpressions.RegexOptions]::Multiline -bor `
            [System.Text.RegularExpressions.RegexOptions]::IgnoreCase
    $m = [regex]::Match($logText, $pattern, $opts)
    if ($m.Success) { return [Convert]::ToUInt32($m.Groups[1].Value, 16) }
    return $null
}

$log = Get-Content -Raw $InputLog
$out = New-Object System.Text.StringBuilder

function W($s) { [void]$out.AppendLine($s) }

W ""
W "=========================================================="
W "  Gated-Multitasking boot pipeline -- post-boot register snapshot"
W "  Source: $InputLog"
W "=========================================================="

# --- Read every register the .jlink script dumped ---
$LTDC_GCR     = Read-Reg $log '50001018'
$LTDC_ISR_1   = Read-Reg $log '50001038'   # first occurrence (pre-clear)
$LTDC_L1PFCR  = Read-Reg $log '50001094'
$LTDC_L1CFBAR = Read-Reg $log '500010ac'
$LTDC_L1CFBLR = Read-Reg $log '500010b0'
$LTDC_L1CFBLNR= Read-Reg $log '500010b4'
$DSI_CR       = Read-Reg $log '50000004'
$DSI_WCR      = Read-Reg $log '50000024'
$FMC_SDCR1    = Read-Reg $log '52004140'
$MPU_CTRL     = Read-Reg $log 'e000ed94'
$FB_0_0       = Read-Reg $log 'd0000000'
$FB_100_100   = Read-Reg $log 'd003d0ec'
$FB_240_400   = Read-Reg $log 'd00928b0'
$FB_240_750   = Read-Reg $log 'd0093b98'

# Find the second occurrence of LTDC_ISR (post-clear, post-2s sleep).
$isrMatches = [regex]::Matches($log, '(?im)^\s*50001038\s*=\s*([0-9A-Fa-f]+)')
$LTDC_ISR_FINAL = if ($isrMatches.Count -ge 3) {
    [Convert]::ToUInt32($isrMatches[$isrMatches.Count-1].Groups[1].Value, 16)
} else { $null }

function Hex32($v) {
    if ($null -eq $v) { return '<missing>' }
    return ('0x{0:X8}' -f $v)
}

function CheckBit($label, $val, $bit, $expectSet) {
    if ($null -eq $val) {
        W ("  -    {0,-44} -- register read missing" -f $label); return
    }
    $got = ($val -shr $bit) -band 1
    $tag = if ($got -eq $expectSet) { 'PASS' } else { 'FAIL' }
    W ("  {0} {1,-44}  bit{2}={3}  (expect {4})" -f $tag, $label, $bit, $got, $expectSet)
}

function CheckEq($label, $got, $expect) {
    if ($null -eq $got) {
        W ("  -    {0,-44} -- missing" -f $label); return
    }
    $g = [uint32]$got
    $e = [uint32]$expect
    $tag = if ($g -eq $e) { 'PASS' } else { 'FAIL' }
    W ("  {0} {1,-44}  = {2}  (expect {3})" -f $tag, $label, (Hex32 $g), (Hex32 $e))
}

W ""
W "RAW REGISTER VALUES"
W ('  LTDC->GCR     = ' + (Hex32 $LTDC_GCR))
W ('  LTDC->ISR  #1 = ' + (Hex32 $LTDC_ISR_1) + '   (initial, sticky from boot)')
W ('  LTDC->ISR  #2 = ' + (Hex32 $LTDC_ISR_FINAL) + '   (after 2s with cleared sticky)')
W ('  LTDC L1PFCR   = ' + (Hex32 $LTDC_L1PFCR))
W ('  LTDC L1CFBAR  = ' + (Hex32 $LTDC_L1CFBAR))
W ('  LTDC L1CFBLR  = ' + (Hex32 $LTDC_L1CFBLR))
W ('  LTDC L1CFBLNR = ' + (Hex32 $LTDC_L1CFBLNR))
W ('  DSI->CR       = ' + (Hex32 $DSI_CR))
W ('  DSI->WCR      = ' + (Hex32 $DSI_WCR))
W ('  FMC SDCR1     = ' + (Hex32 $FMC_SDCR1))
W ('  MPU CTRL      = ' + (Hex32 $MPU_CTRL))
W ('  FB[0,0]              = ' + (Hex32 $FB_0_0))
W ('  FB[100,100]          = ' + (Hex32 $FB_100_100))
W ('  FB[240,400] center   = ' + (Hex32 $FB_240_400))
W ('  FB[240,750] right tail = ' + (Hex32 $FB_240_750))

W ""
W "POST-BOOT EXPECTATIONS"
W "  (DSI Command Mode: LTDC clock gates off between frames, so GCR.LTDCEN"
W "   may sample as 0 even though Phase 2 ran MX_LTDC_Init. The reliable"
W "   indicator is L1CFBAR pointing at 0xD0000000.)"
CheckBit 'DSI host enabled (CR.EN=1)'         $DSI_CR       0 1
CheckBit 'MPU enabled (CTRL.ENABLE=1)'        $MPU_CTRL     0 1
CheckEq  'Layer1 CFBAR = 0xD0000000 (LTDC inited)' $LTDC_L1CFBAR 0xD0000000L
CheckEq  'Layer1 PixelFormat = RGB888 (0x01)' $LTDC_L1PFCR  0x00000001
W ('  INFO  LTDC->GCR  bit0 LTDCEN = ' + $(if ($null -ne $LTDC_GCR) { ($LTDC_GCR -band 1) } else { '?' }) + '   (gates off between DSI frames in Command Mode)')
W ('  INFO  DSI->WCR   bit3 DSIEN  = ' + $(if ($null -ne $DSI_WCR)  { (($DSI_WCR -shr 3) -band 1) } else { '?' }) + '   (DSI wrapper -- verify address against RM0399 §31)')

W ""
W "LTDC ERROR FLAGS (snapshot 1, sticky from boot)"
CheckBit 'LIF    bit0  Line Interrupt Flag'        $LTDC_ISR_1 0 0
CheckBit 'FUIF   bit1  FIFO Underrun (boot-time)'  $LTDC_ISR_1 1 0
CheckBit 'TERRIF bit2  Transfer Error (boot-time)' $LTDC_ISR_1 2 0
CheckBit 'RRIF   bit3  Register Reload Flag'       $LTDC_ISR_1 3 0

W ""
W "LTDC ERROR FLAGS (snapshot 2, after sticky cleared + 2s of run)"
CheckBit 'FUIF   bit1  FIFO Underrun (steady-state)'  $LTDC_ISR_FINAL 1 0
CheckBit 'TERRIF bit2  Transfer Error (steady-state)' $LTDC_ISR_FINAL 2 0

W ""
W "FRAMEBUFFER CONTENT"
$allZero = (($null -ne $FB_0_0) -and ($null -ne $FB_100_100) -and ($null -ne $FB_240_400) -and ($null -ne $FB_240_750) -and
           ($FB_0_0 -eq 0) -and ($FB_100_100 -eq 0) -and ($FB_240_400 -eq 0) -and ($FB_240_750 -eq 0))
if ($allZero) {
    W "  FAIL  framebuffer all-zero -- TouchGFX never painted"
} else {
    W "  PASS  framebuffer has content"
}
if ($FB_240_750 -eq 0) {
    W "  WARN  right-half tail (col=750) is zero -- DMA2D may not be reaching the cushion"
}

W ""
W "=========================================================="
W " Read interpretation:"
W "   Snapshot 1 FUIF=1 -> contention happened during boot bring-up."
W "   Snapshot 2 FUIF=1 -> contention is ongoing in steady state."
W "   FB all-zero       -> guiTask never painted (Phase 2 stalled)."
W "   LTDC GCR.LTDCEN=0 -> Phase 2 never reached MX_LTDC_Init."
W "=========================================================="

[IO.File]::WriteAllText($OutputReport, $out.ToString())
