$cspybat = "C:\TouchGFXProjects\IAR 9.70.2\ewarm-9.70.2\common\bin\cspybat.exe"
$general  = "C:\TouchGFXProjects\MyApplication\EWARM\settings\STM32H747I-DISCO.STM32H747I-DISCO_CM7.general.xcl"
$driver   = "C:\TouchGFXProjects\MyApplication\EWARM\settings\STM32H747I-DISCO.STM32H747I-DISCO_CM7.driver.xcl"
$trigMac  = "C:\TouchGFXProjects\MyApplication\EWARM\trigger_and_dump_session.mac"
$log      = "C:\TouchGFXProjects\MyApplication\EWARM\runs\attach_trigger2.log"

New-Item -ItemType Directory -Force -Path "C:\TouchGFXProjects\MyApplication\EWARM\runs" | Out-Null

Write-Host "=== Attach + EXTI trigger + mid-recording dump ==="
& $cspybat --attach_to_running_target --leave_target_running --macro $trigMac -f $general --backend -f $driver 2>&1 | Tee-Object -FilePath $log
Write-Host "Exit: $LASTEXITCODE"
