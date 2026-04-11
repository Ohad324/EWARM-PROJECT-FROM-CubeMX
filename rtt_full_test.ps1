# Full autonomous test: RTT capture + button trigger + read results
$JLINK = 'C:\Program Files\SEGGER\JLink_V930a\JLink.exe'
$rttLog = 'C:\TouchGFXProjects\MyApplication\EWARM\SystemView\rtt_log.txt'
$triggerScript = 'C:\TouchGFXProjects\MyApplication\EWARM\jlink_trigger_button.jlink'

# Kill any probe holders
Stop-Process -Name 'JLink' -Force -ErrorAction SilentlyContinue
Stop-Process -Name 'JLinkRTTLogger' -Force -ErrorAction SilentlyContinue
Stop-Process -Name 'JLinkRTTViewer' -Force -ErrorAction SilentlyContinue
Start-Sleep -Seconds 2

# Clear old log
Remove-Item $rttLog -ErrorAction SilentlyContinue

# Phase 1: Reset board to running state
Write-Host "[1] Resetting board to run state..."
$resetScript = 'C:\TouchGFXProjects\MyApplication\EWARM\_go.jlink'
& $JLINK -NoGui 1 -device STM32H747XI_M7 -if SWD -speed 4000 -autoconnect 1 -CommandFile $resetScript 2>&1 | Out-Null
Stop-Process -Name 'JLink' -Force -ErrorAction SilentlyContinue
Start-Sleep -Seconds 2

# Phase 2: Start RTT capture session (20s) in background via JLink commander
Write-Host "[2] Starting RTT capture (25s window)..."
$rttCaptureScript = 'C:\TouchGFXProjects\MyApplication\EWARM\rtt_session.jlink'

# JLink rttstart dumps to stdout — capture it
$rttJob = Start-Job -ScriptBlock {
    param($jlink, $script, $log)
    & $jlink -NoGui 1 -device STM32H747XI_M7 -if SWD -speed 4000 -autoconnect 1 -CommandFile $script 2>&1 | Out-File -FilePath $log -Encoding UTF8
} -ArgumentList $JLINK, $rttCaptureScript, $rttLog

# Phase 3: Wait for board to boot (8s), then trigger button
Write-Host "[3] Waiting 8s for board boot..."
Start-Sleep -Seconds 8

Write-Host "[4] Triggering recording (button press)..."
if (Test-Path $triggerScript) {
    & $JLINK -NoGui 1 -device STM32H747XI_M7 -if SWD -speed 4000 -autoconnect 1 -CommandFile $triggerScript 2>&1 | Out-Null
    Stop-Process -Name 'JLink' -Force -ErrorAction SilentlyContinue
    Write-Host "Button triggered."
} else {
    Write-Host "WARNING: trigger script not found at $triggerScript"
}

# Phase 4: Wait for recording + upload to complete
Write-Host "[5] Waiting 20s for recording + STT..."
Start-Sleep -Seconds 20

# Phase 5: Read results
Write-Host "[6] Reading log..."
Stop-Job $rttJob -ErrorAction SilentlyContinue
Remove-Job $rttJob -ErrorAction SilentlyContinue
Stop-Process -Name 'JLink' -Force -ErrorAction SilentlyContinue

if (Test-Path $rttLog) {
    $size = (Get-Item $rttLog).Length
    Write-Host "=== RTT LOG ($size bytes) ==="
    Get-Content $rttLog
} else {
    Write-Host "No RTT log captured."
}
