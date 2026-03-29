$ErrorActionPreference = "Stop"
if (-not (Test-Path -LiteralPath "C:\Program Files (x86)\Bauhaus\absvars.ps1")) {
Write-Error "Axivion Suite setup script "C:\Program Files (x86)\Bauhaus\absvars.ps1" not found."
exit 1
}
. "C:\Program Files (x86)\Bauhaus\absvars.ps1"
$env:BAUHAUS_CONFIG = Split-Path -Parent $MyInvocation.MyCommand.Path
# Toolchain setup command: "C:/Program Files (x86)/Bauhaus/bin/iarsetup" --cc "iccarm --no_cse --no_unroll --no_inline --no_code_motion --no_tbaa --no_clustering --no_scheduling --debug --endian=little --cpu=Cortex-M7 -e --fpu=VFPv5_d16 --dlib_config C:\iar\ewarm-9.60.3\arm\inc\c\DLib_Config_Full.h" --cxx "iccarm --no_cse --no_unroll --no_inline --no_code_motion --no_tbaa --no_clustering --no_scheduling --debug --endian=little --cpu=Cortex-M7 -e --fpu=VFPv5_d16 --dlib_config C:\iar\ewarm-9.60.3\arm\inc\c\DLib_Config_Full.h --c++" --config C:/TouchGFXProjects/MyApplication/axivion/compiler_config.json
axivion_ci @args
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
