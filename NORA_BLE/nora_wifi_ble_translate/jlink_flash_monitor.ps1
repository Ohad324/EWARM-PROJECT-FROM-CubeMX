# NORA - Flash via J-Link Ultra + open monitor
# Usage: .\jlink_flash_monitor.ps1
# Requires: J-Link Ultra wired to GPIO9/10/12/13, COM5 for monitor

$OPENOCD  = 'C:\Espressif\tools\openocd-esp32\v0.12.0-esp32-20251215\openocd-esp32\bin\openocd.exe'
$PYTHON   = 'C:\Espressif\python_env\idf5.5_py3.11_env\Scripts\python.exe'
$IDF_PY   = 'C:\Espressif\frameworks\esp-idf-v5.5.3\tools\idf.py'
$PROJECT  = 'C:\NORA_BLE\nora_wifi_ble_translate'
$BUILD    = "$PROJECT\build"
$PORT     = 'COM5'
$BAUD     = '115200'

$OPENOCD_CMDS = '-f interface/jlink.cfg -f target/esp32s3.cfg'

# ── Step 1: Start OpenOCD in background ─────────────────────────────────────
Write-Host "`n[1/3] Starting OpenOCD with J-Link Ultra..." -ForegroundColor Cyan

$oocd = Start-Process -FilePath $OPENOCD `
    -ArgumentList $OPENOCD_CMDS `
    -PassThru -NoNewWindow `
    -RedirectStandardOutput "$PROJECT\openocd_out.txt" `
    -RedirectStandardError  "$PROJECT\openocd_err.txt"

Start-Sleep -Seconds 2

if ($oocd.HasExited) {
    Write-Host "[ERROR] OpenOCD failed to start. Check wiring / J-Link connection." -ForegroundColor Red
    Get-Content "$PROJECT\openocd_err.txt"
    exit 1
}
Write-Host "        OpenOCD running (PID $($oocd.Id))" -ForegroundColor Green

# ── Step 2: Flash via OpenOCD ────────────────────────────────────────────────
Write-Host "`n[2/3] Flashing..." -ForegroundColor Cyan

$flashArgs = @(
    "$IDF_PY"
    "-C", "$PROJECT"
    "-p", $PORT
    "--openocd-commands", $OPENOCD_CMDS
    "flash"
)

$flashProc = Start-Process -FilePath $PYTHON `
    -ArgumentList $flashArgs `
    -PassThru -NoNewWindow -Wait

if ($flashProc.ExitCode -ne 0) {
    Write-Host "[ERROR] Flash failed (exit code $($flashProc.ExitCode))" -ForegroundColor Red
    Stop-Process -Id $oocd.Id -Force -ErrorAction SilentlyContinue
    exit 1
}
Write-Host "        Flash complete!" -ForegroundColor Green

# ── Step 3: Stop OpenOCD, open monitor ──────────────────────────────────────
Write-Host "`n[3/3] Stopping OpenOCD, opening serial monitor..." -ForegroundColor Cyan
Stop-Process -Id $oocd.Id -Force -ErrorAction SilentlyContinue
Start-Sleep -Seconds 1

# Open monitor (blocking — Ctrl+] to exit)
& $PYTHON $IDF_PY -C $PROJECT -p $PORT -b $BAUD monitor
