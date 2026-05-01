# uart_stress_test.ps1
# Runs 20 recording cycles via J-Link DAP reads and summarises UART health.
# CPU stays free throughout — no halts.

$JLINK  = "C:\Program Files\SEGGER\JLink\JLink.exe"
$SCRIPT = "$PSScriptRoot\uart_stream_diag.jlink"
$LOG    = "$PSScriptRoot\uart_stress_results.txt"
$RUNS   = 20

if (-not (Test-Path $JLINK)) {
    $JLINK = "C:\Program Files (x86)\SEGGER\JLink\JLink.exe"
}
if (-not (Test-Path $JLINK)) {
    Write-Error "JLink.exe not found. Set path manually."
    exit 1
}

# Clear log
"" | Set-Content $LOG
$results = @()

for ($i = 1; $i -le $RUNS; $i++) {
    Write-Host "=== RUN $i / $RUNS ===" -ForegroundColor Cyan

    $tmp = "$env:TEMP\uart_diag_run$i.txt"
    & $JLINK -nogui 1 -commandfile $SCRIPT > $tmp 2>&1
    $raw = Get-Content $tmp -Raw

    # Parse key counters from JLink output
    function ParseU32 ($text, $label) {
        # mem32 output: "0x2402D71C = 0x00000003"
        $lines = $text -split "`n"
        $idx   = ($lines | Select-String -SimpleMatch $label).LineNumber
        if ($idx) {
            $valLine = $lines[$idx]   # line after the echo
            if ($valLine -match '=\s*(0x[0-9A-Fa-f]+)') { return [Convert]::ToInt64($Matches[1], 16) }
        }
        return -1
    }

    # Extract values by searching for hex patterns after each echo label
    $lines = $raw -split "`n"
    $vals  = @{}
    $keys  = @(
        'tx_retries','tx_hard_fails','tx_last_err_code',
        'tx_last_declared','tx_last_sent','tx_transfer_count','rx_overruns',
        'dfsdm_overruns','sd_write_max_ms','buffer_misses','total_bytes_written',
        'g_SampleCount','s_dmaQueueOverflow'
    )

    # Pull all hex values from the output in order
    $hexValues = [System.Collections.Generic.List[long]]::new()
    foreach ($line in $lines) {
        if ($line -match '\b(0x[0-9A-Fa-f]{8})\b') {
            $hexValues.Add([Convert]::ToInt64($Matches[1], 16))
        }
    }

    # Map known offsets — mem32 values appear in order after connect
    # Output order from script: tx_retries(0) tx_hard_fails(1) tx_last_err_code(2)
    #   tx_last_declared(3) tx_last_sent(4) tx_transfer_count(5) rx_overruns(6)
    #   dfsdm_overruns(7) sd_write_max_ms(8) buffer_misses(9) total_bytes_written(10)
    #   g_SampleCount(11) g_sdFreeKB(12) g_FileIndex(13) s_dmaQueueOverflow(14)
    $r = [PSCustomObject]@{
        Run              = $i
        tx_retries       = if ($hexValues.Count -gt 0)  { $hexValues[0]  } else { -1 }
        tx_hard_fails    = if ($hexValues.Count -gt 1)  { $hexValues[1]  } else { -1 }
        tx_last_err_code = if ($hexValues.Count -gt 2)  { $hexValues[2]  } else { -1 }
        tx_last_declared = if ($hexValues.Count -gt 3)  { $hexValues[3]  } else { -1 }
        tx_last_sent     = if ($hexValues.Count -gt 4)  { $hexValues[4]  } else { -1 }
        tx_transfer_cnt  = if ($hexValues.Count -gt 5)  { $hexValues[5]  } else { -1 }
        rx_overruns      = if ($hexValues.Count -gt 6)  { $hexValues[6]  } else { -1 }
        dfsdm_overruns   = if ($hexValues.Count -gt 7)  { $hexValues[7]  } else { -1 }
        sd_write_max_ms  = if ($hexValues.Count -gt 8)  { $hexValues[8]  } else { -1 }
        buffer_misses    = if ($hexValues.Count -gt 9)  { $hexValues[9]  } else { -1 }
        dmaQ_overflow    = if ($hexValues.Count -gt 14) { $hexValues[14] } else { -1 }
        bytes_match      = if ($hexValues.Count -gt 4)  { $hexValues[3] -eq $hexValues[4] } else { $false }
    }
    $results += $r

    # Print per-run summary
    $match = if ($r.bytes_match) { "OK" } else { "MISMATCH" }
    $err   = if ($r.tx_last_err_code -gt 0) { "ERR=0x{0:X2}" -f $r.tx_last_err_code } else { "no-err" }
    Write-Host ("  declared={0}  sent={1}  [{2}]  retries={3}  rx_overruns={4}  {5}" -f `
        $r.tx_last_declared, $r.tx_last_sent, $match, $r.tx_retries, $r.rx_overruns, $err)

    # Append raw log
    "=== RUN $i ===" | Add-Content $LOG
    $raw         | Add-Content $LOG

    # Small gap between runs so the system settles
    if ($i -lt $RUNS) { Start-Sleep -Seconds 5 }
}

# Summary table
Write-Host "`n========== SUMMARY ==========" -ForegroundColor Yellow
$results | Format-Table -AutoSize

$fails = $results | Where-Object { -not $_.bytes_match }
$errs  = $results | Where-Object { $_.tx_last_err_code -gt 0 }
$rovr  = $results | Where-Object { $_.rx_overruns -gt 0 }

Write-Host "Byte-count mismatches : $($fails.Count) / $RUNS"
Write-Host "UART error codes seen : $($errs.Count) / $RUNS"
Write-Host "RX overruns           : $($rovr.Count) / $RUNS"

Write-Host "`nFull raw log saved to: $LOG" -ForegroundColor Green
