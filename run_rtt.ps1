$log = "C:\TouchGFXProjects\MyApplication\EWARM\SystemView\rtt_log.txt"
$rtt = "C:\Program Files\SEGGER\JLink_V930a\JLinkRTTLogger.exe"
$args = @("-device", "STM32H747XI_M7", "-if", "SWD", "-speed", "4000", "-RTTChannel", "0", "-OutputFile", $log)
$proc = Start-Process -FilePath $rtt -ArgumentList $args -PassThru -NoNewWindow
Start-Sleep -Seconds 12
Stop-Process -Id $proc.Id -Force -ErrorAction SilentlyContinue
Write-Host "RTT capture done. Lines: $((Get-Content $log).Count)"
