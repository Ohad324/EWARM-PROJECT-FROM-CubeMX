setlocal
@echo off
if not exist "C:\Program Files (x86)\Bauhaus\absvars.bat" (
echo ERROR: Axivion Suite setup script "C:\Program Files (x86)\Bauhaus\absvars.bat" not found.>&2
exit /b 1
)
@echo on
call "C:\Program Files (x86)\Bauhaus\absvars.bat"
SET AXIVION_USERNAME=admin
SET AXIVION_PASSWORD=password
set BAUHAUS_CONFIG=%~dp0.
@REM Toolchain setup command: "C:/Program Files (x86)/Bauhaus/bin/iarsetup" --cc "\"C:\TouchGFXProjects\IAR 9.70.2\ewarm-9.70.2\arm\bin\iccarm.exe\"  -D CORE_CM7 -D USE_HAL_DRIVER -D STM32H747xx -D USE_BPP=24 -D USE_PWR_DIRECT_SMPS_SUPPLY --no_cse --no_unroll --no_inline --no_code_motion --no_tbaa --no_clustering --no_scheduling --debug --endian=little --cpu=Cortex-M7 --fpu=VFPv5_d16 --dlib_config \"C:\TouchGFXProjects\IAR 9.70.2\ewarm-9.70.2\arm\inc\c\DLib_Config_Full.h\"" --cxx "\"C:\TouchGFXProjects\IAR 9.70.2\ewarm-9.70.2\arm\bin\iccarm.exe\"  -D CORE_CM7 -D USE_HAL_DRIVER -D STM32H747xx -D USE_BPP=24 -D USE_PWR_DIRECT_SMPS_SUPPLY --no_cse --no_unroll --no_inline --no_code_motion --no_tbaa --no_clustering --no_scheduling --debug --endian=little --cpu=Cortex-M7 --fpu=VFPv5_d16 --dlib_config \"C:\TouchGFXProjects\IAR 9.70.2\ewarm-9.70.2\arm\inc\c\DLib_Config_Full.h\"" --config C:/TouchGFXProjects/MyApplication/EWARM/axivionEWARM/compiler_config.json
axivion_ci %* || exit /b
