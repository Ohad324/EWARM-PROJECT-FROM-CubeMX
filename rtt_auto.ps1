Stop-Process -Name 'JLink' -Force -ErrorAction SilentlyContinue
Stop-Process -Name 'JLinkRTTLogger' -Force -ErrorAction SilentlyContinue
Start-Sleep -Seconds 2

$rttLog = 'C:\TouchGFXProjects\MyApplication\EWARM\SystemView\rtt_log.txt'
Remove-Item $rttLog -ErrorAction SilentlyContinue

$p = Start-Process -FilePath 'C:\Program Files\SEGGER\JLink_V930a\JLinkRTTLogger.exe' `
    -ArgumentList "-device STM32H747XI_M7 -if SWD -speed 4000 -RTTChannel 0 -OutputFile `"$rttLog`" -AutoConnect 1" `
    -PassThru -WindowStyle Hidden

Write-Host "RTTLogger PID: $($p.Id)"
Write-Host "Waiting 20s for board boot + settle..."
Start-Sleep -Seconds 20

if (Test-Path $rttLog) {
    $size = (Get-Item $rttLog).Length
    Write-Host "LOG EXISTS: $size bytes"
    Get-Content $rttLog -Tail 30
} else {
    Write-Host "FAIL: no log file. Process still running: $(-not $p.HasExited)"
}

Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue
