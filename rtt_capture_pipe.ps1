# Pipe answers to RTTLogger prompts and capture output to file
$JLINK     = 'C:\Program Files\SEGGER\JLink_V930a\JLink.exe'
$RTTLOGGER = 'C:\Program Files\SEGGER\JLink_V930a\JLinkRTTLogger.exe'
$rttLog    = 'C:\TouchGFXProjects\MyApplication\EWARM\SystemView\rtt_log.txt'

Stop-Process -Name 'JLink'          -Force -ErrorAction SilentlyContinue
Stop-Process -Name 'JLinkRTTLogger' -Force -ErrorAction SilentlyContinue
Start-Sleep -Seconds 2

Remove-Item $rttLog -ErrorAction SilentlyContinue

# Reset board to running state
Write-Host "[1] Resetting board to run state..."
& $JLINK -NoGui 1 -device STM32H747XI_M7 -if SWD -speed 4000 -autoconnect 1 -CommandFile 'C:\TouchGFXProjects\MyApplication\EWARM\_go.jlink' 2>&1 | Out-Null
Stop-Process -Name 'JLink' -Force -ErrorAction SilentlyContinue
Start-Sleep -Seconds 1

Write-Host "[2] Launching RTTLogger with piped answers..."
# Answers to prompts: Device=STM32H747XI_M7, Interface=SWD, Speed=4000
$answers = "STM32H747XI_M7`nSWD`n4000`n"

$psi = New-Object System.Diagnostics.ProcessStartInfo
$psi.FileName  = $RTTLOGGER
$psi.Arguments = "-OutputFile `"$rttLog`""
$psi.RedirectStandardInput  = $true
$psi.RedirectStandardOutput = $false
$psi.RedirectStandardError  = $false
$psi.UseShellExecute = $false
$psi.CreateNoWindow  = $true

$proc = [System.Diagnostics.Process]::Start($psi)
Write-Host "RTTLogger PID: $($proc.Id)"

# Send answers to prompts
Start-Sleep -Milliseconds 500
$proc.StandardInput.WriteLine("STM32H747XI_M7")
Start-Sleep -Milliseconds 800
$proc.StandardInput.WriteLine("SWD")
Start-Sleep -Milliseconds 800
$proc.StandardInput.WriteLine("4000")
Start-Sleep -Milliseconds 800
$proc.StandardInput.WriteLine("0x24000058")   # RTT Control Block address from map file
Start-Sleep -Milliseconds 800
$proc.StandardInput.WriteLine("0")            # RTT channel 0
Start-Sleep -Milliseconds 800
$proc.StandardInput.WriteLine($rttLog)        # output file path
Start-Sleep -Milliseconds 1500
$proc.StandardInput.Flush()

Write-Host "[3] Waiting 10s for board boot..."
Start-Sleep -Seconds 10

if (Test-Path $rttLog) {
    $sz = (Get-Item $rttLog).Length
    Write-Host "RTT capturing: $sz bytes so far"
} else {
    Write-Host "Still no log after 10s"
}

# Trigger button
Write-Host "[4] Triggering recording..."
& $JLINK -NoGui 1 -device STM32H747XI_M7 -if SWD -speed 4000 -autoconnect 1 -CommandFile 'C:\TouchGFXProjects\MyApplication\EWARM\jlink_trigger_button.jlink' 2>&1 | Out-Null
Stop-Process -Name 'JLink' -Force -ErrorAction SilentlyContinue
Write-Host "Button triggered."

Write-Host "[5] Waiting 20s for recording + STT..."
Start-Sleep -Seconds 20

# Stop logger
Stop-Process -Id $proc.Id -Force -ErrorAction SilentlyContinue
Start-Sleep -Seconds 1

Write-Host "[6] Reading results..."
if (Test-Path $rttLog) {
    $size = (Get-Item $rttLog).Length
    Write-Host "=== RTT LOG ($size bytes) ==="
    Get-Content $rttLog
} else {
    Write-Host "No RTT log captured."
}
