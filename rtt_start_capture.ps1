$rttLog = 'C:\TouchGFXProjects\MyApplication\EWARM\SystemView\rtt_log.txt'
Remove-Item $rttLog -ErrorAction SilentlyContinue

$psi = New-Object System.Diagnostics.ProcessStartInfo
$psi.FileName = 'C:\Program Files\SEGGER\JLink_V930a\JLinkRTTLogger.exe'
$psi.Arguments = "-OutputFile `"$rttLog`""
$psi.RedirectStandardInput = $true
$psi.UseShellExecute = $false
$psi.CreateNoWindow = $true
$proc = [System.Diagnostics.Process]::Start($psi)

Start-Sleep -Milliseconds 800
$proc.StandardInput.WriteLine('STM32H747XI_M7')
Start-Sleep -Milliseconds 800
$proc.StandardInput.WriteLine('SWD')
Start-Sleep -Milliseconds 800
$proc.StandardInput.WriteLine('4000')
Start-Sleep -Milliseconds 800
$proc.StandardInput.WriteLine('0x24000058')
Start-Sleep -Milliseconds 800
$proc.StandardInput.WriteLine('0')
Start-Sleep -Milliseconds 800
$proc.StandardInput.WriteLine($rttLog)
$proc.StandardInput.Flush()

Start-Sleep -Seconds 5
if (Test-Path $rttLog) {
    Write-Host 'RTT CAPTURING — board ready!'
} else {
    Write-Host 'RTT not connected yet...'
}

# Capture for 90 seconds — enough for boot + button press + recording + STT
Write-Host 'Waiting 90s for your test...'
Start-Sleep -Seconds 90
Stop-Process -Id $proc.Id -Force -ErrorAction SilentlyContinue
Write-Host 'Capture complete.'
