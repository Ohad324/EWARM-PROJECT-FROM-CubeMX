# run_2_tests.ps1 — auto-trigger 2 recording+upload cycles and report results

$JLINK  = "C:\Program Files\SEGGER\JLink\JLink.exe"
if (-not (Test-Path $JLINK)) { $JLINK = "C:\Program Files (x86)\SEGGER\JLink\JLink.exe" }

$SCRIPT = "$PSScriptRoot\uart_stream_diag.jlink"
$RUNS   = 2

$results = @()

for ($i = 1; $i -le $RUNS; $i++) {
    Write-Host "`n=== RUN $i / $RUNS ===" -ForegroundColor Cyan

    $tmp = "$env:TEMP\nora_run$i.txt"
    & $JLINK -nogui 1 -commandfile $SCRIPT > $tmp 2>&1
    $raw = Get-Content $tmp -Raw

    # Extract all 32-bit hex values in order from JLink output
    $hexValues = [System.Collections.Generic.List[long]]::new()
    foreach ($line in ($raw -split "`n")) {
        if ($line -match '\b(0x[0-9A-Fa-f]{8})\b') {
            $hexValues.Add([Convert]::ToInt64($Matches[1], 16))
        }
    }

    # Map order from uart_stream_diag.jlink:
    # 0=tx_retries 1=tx_hard_fails 2=tx_last_err_code 3=tx_last_declared
    # 4=tx_last_sent 5=tx_transfer_count 6=rx_overruns
    # 7=dfsdm_overruns 8=sd_write_max_ms 9=buffer_misses 10=total_bytes_written
    # 11=g_SampleCount 12=g_sdFreeKB 13=g_FileIndex 14=s_dmaQueueOverflow
    $r = [PSCustomObject]@{
        Run              = $i
        tx_retries       = if ($hexValues.Count -gt 0)  { $hexValues[0]  } else { -1 }
        tx_hard_fails    = if ($hexValues.Count -gt 1)  { $hexValues[1]  } else { -1 }
        tx_last_err_code = if ($hexValues.Count -gt 2)  { "0x{0:X2}" -f $hexValues[2] } else { "?" }
        tx_last_declared = if ($hexValues.Count -gt 3)  { $hexValues[3]  } else { -1 }
        tx_last_sent     = if ($hexValues.Count -gt 4)  { $hexValues[4]  } else { -1 }
        rx_overruns      = if ($hexValues.Count -gt 6)  { $hexValues[6]  } else { -1 }
        dfsdm_overruns   = if ($hexValues.Count -gt 7)  { $hexValues[7]  } else { -1 }
        sd_write_max_ms  = if ($hexValues.Count -gt 8)  { $hexValues[8]  } else { -1 }
        dmaQ_overflow    = if ($hexValues.Count -gt 14) { $hexValues[14] } else { -1 }
        bytes_ok         = if ($hexValues.Count -gt 4)  { $hexValues[3] -eq $hexValues[4] } else { $false }
    }
    $results += $r

    $status = if ($r.bytes_ok) { "PASS ✓" } else { "FAIL ✗  (declared=$($r.tx_last_declared) sent=$($r.tx_last_sent))" }
    Write-Host "  Result        : $status" -ForegroundColor $(if ($r.bytes_ok) { "Green" } else { "Red" })
    Write-Host "  tx_retries    : $($r.tx_retries)"
    Write-Host "  tx_hard_fails : $($r.tx_hard_fails)"
    Write-Host "  tx_err_code   : $($r.tx_last_err_code)"
    Write-Host "  rx_overruns   : $($r.rx_overruns)"
    Write-Host "  dfsdm_overruns: $($r.dfsdm_overruns)"
    Write-Host "  sd_write_max  : $($r.sd_write_max_ms) ms"
    Write-Host "  dmaQ_overflow : $($r.dmaQ_overflow)"

    if ($i -lt $RUNS) {
        Write-Host "`n  Waiting 10s before next run..." -ForegroundColor Gray
        Start-Sleep -Seconds 10
    }
}

Write-Host "`n========== SUMMARY ==========" -ForegroundColor Yellow
$results | Format-Table -AutoSize

$pass = ($results | Where-Object { $_.bytes_ok }).Count
Write-Host "Passed: $pass / $RUNS" -ForegroundColor $(if ($pass -eq $RUNS) { "Green" } else { "Red" })
