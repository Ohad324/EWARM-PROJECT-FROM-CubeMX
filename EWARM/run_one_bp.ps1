param(
    [Parameter(Mandatory=$true)] [string]$Mac,
    [Parameter(Mandatory=$true)] [string]$Log,
    [int]$TimeoutSec = 20
)

$ErrorActionPreference = 'Continue'
$cspy = 'C:\TouchGFXProjects\IAR 9.70.2\ewarm-9.70.2\common\bin\CSpyBat.exe'
$genXcl = 'C:/TouchGFXProjects/MyApplication/EWARM/runs/verify_pfb.general.xcl'
$drvXcl = 'C:/TouchGFXProjects/MyApplication/EWARM/settings/STM32H747I-DISCO.STM32H747I-DISCO_CM7.driver.xcl'

Get-Process -ErrorAction SilentlyContinue -Name 'CSpyBat','JLink','JLinkRTTViewer','JLinkRTTLogger' |
    Stop-Process -Force -ErrorAction SilentlyContinue

$cspyArgs = @(
    '--macro', $Mac,
    '-f',      $genXcl,
    '--backend',
    '-f',      $drvXcl
)
$cmdLine = '"' + $cspy + '" ' + ($cspyArgs -join ' ') + ' > "' + $Log + '" 2>&1'

$job = Start-Job -ScriptBlock { param($c) cmd.exe /c $c } -ArgumentList $cmdLine
if (-not (Wait-Job -Job $job -Timeout $TimeoutSec)) {
    Stop-Job -Job $job
    Get-Process -EA SilentlyContinue -Name 'CSpyBat','JLink' | Stop-Process -Force -EA SilentlyContinue
    "[host] ${TimeoutSec}s watchdog fired, killed cspybat" | Add-Content $Log
}
Remove-Job -Job $job -Force
"done"
