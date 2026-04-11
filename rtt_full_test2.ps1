# Full autonomous test with RTT address
$JLINK     = 'C:\Program Files\SEGGER\JLink_V930a\JLink.exe'
$RTTLOGGER = 'C:\Program Files\SEGGER\JLink_V930a\JLinkRTTLogger.exe'
$rttLog    = 'C:\TouchGFXProjects\MyApplication\EWARM\SystemView\rtt_log.txt'
$RTT_ADDR  = '0x24000058'   # _SEGGER_RTT symbol from map file

# Kill any probe holders
Stop-Process -Name 'JLink'          -Force -ErrorAction SilentlyContinue
Stop-Process -Name 'JLinkRTTLogger' -Force -ErrorAction SilentlyContinue
Stop-Process -Name 'JLinkRTTViewer' -Force -ErrorAction SilentlyContinue
Start-Sleep -Seconds 2

# Clear old log
Remove-Item $rttLog -ErrorAction SilentlyContinue

# Reset board to running state
Write-Host "[1] Resetting board..."
& $JLINK -NoGui 1 -device STM32H747XI_M7 -if SWD -speed 4000 -autoconnect 1 -CommandFile 'C:\TouchGFXProjects\MyApplication\EWARM\_go.jlink' 2>&1 | Out-Null
Stop-Process -Name 'JLink' -Force -ErrorAction SilentlyContinue
Start-Sleep -Seconds 2

# Start RTTLogger with explicit RTT address
Write-Host "[2] Starting RTTLogger (RTT @ $RTT_ADDR)..."
$loggerArgs = "-device STM32H747XI_M7 -if SWD -speed 4000 -RTTChannel 0 -RTTSearchAddr $RTT_ADDR -OutputFile `"$rttLog`""
$loggerProc = Start-Process -FilePath $RTTLOGGER -ArgumentList $loggerArgs -PassThru -WindowStyle Hidden
Write-Host "RTTLogger PID: $($loggerProc.Id)"

# Wait for board to boot
Write-Host "[3] Waiting 10s for board boot..."
Start-Sleep -Seconds 10

# Check if log is being written
if (Test-Path $rttLog) {
    $sz = (Get-Item $rttLog).Length
    Write-Host "RTT capturing! $sz bytes so far"
} else {
    Write-Host "WARNING: still no log after 10s"
}

# Trigger button
Write-Host "[4] Triggering recording..."
& $JLINK -NoGui 1 -device STM32H747XI_M7 -if SWD -speed 4000 -autoconnect 1 -CommandFile 'C:\TouchGFXProjects\MyApplication\EWARM\jlink_trigger_button.jlink' 2>&1 | Out-Null
Stop-Process -Name 'JLink' -Force -ErrorAction SilentlyContinue
Write-Host "Button triggered."

# Wait for recording (3s) + SD write (0.5s) + upload + STT (8s) = ~15s
Write-Host "[5] Waiting 18s for recording + STT..."
Start-Sleep -Seconds 18

# Stop logger and read results
Stop-Process -Id $loggerProc.Id -Force -ErrorAction SilentlyContinue
Start-Sleep -Seconds 1

Write-Host "[6] Results:"
if (Test-Path $rttLog) {
    $size = (Get-Item $rttLog).Length
    Write-Host "=== RTT LOG ($size bytes) ==="
    Get-Content $rttLog
} else {
    Write-Host "No RTT log. RTTLogger did not connect."
    Write-Host "Try: open JLinkRTTViewer manually and check if board is outputting RTT."
}
