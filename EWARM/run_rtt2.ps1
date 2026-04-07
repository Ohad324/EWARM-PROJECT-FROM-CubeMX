$log = "C:\TouchGFXProjects\MyApplication\EWARM\SystemView\rtt_log.txt"
$rtt = "C:\Program Files\SEGGER\JLink_V930a\JLinkRTTLogger.exe"
# Use positional args — device, interface, speed, then output file
$proc = Start-Process -FilePath $rtt -ArgumentList "STM32H747XI_M7", "SWD", "4000", $log -PassThru -NoNewWindow -RedirectStandardOutput "C:\TouchGFXProjects\MyApplication\EWARM\SystemView\rtt_stdout.txt" -RedirectStandardError "C:\TouchGFXProjects\MyApplication\EWARM\SystemView\rtt_stderr.txt"
Start-Sleep -Seconds 18
Stop-Process -Id $proc.Id -Force -ErrorAction SilentlyContinue
Write-Host "Done. Log size: $((Get-Item $log -ErrorAction SilentlyContinue).Length) bytes"
