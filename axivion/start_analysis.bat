setlocal
@echo off
if not exist "C:\Program Files (x86)\Bauhaus\absvars.bat" (
echo ERROR: Axivion Suite setup script "C:\Program Files (x86)\Bauhaus\absvars.bat" not found.>&2
exit /b 1
)
@echo on

SET AXIVION_USERNAME=admin
SET AXIVION_PASSWORD=password
call "C:\Program Files (x86)\Bauhaus\absvars.bat"
set BAUHAUS_CONFIG=%~dp0.
@REM Toolchain setup command: "C:/Program Files (x86)/Bauhaus/bin/iarsetup" --cc "iccarm --no_cse --no_unroll --no_inline --no_code_motion --no_tbaa --no_clustering --no_scheduling --debug --endian=little --cpu=Cortex-M7 -e --fpu=VFPv5_d16 --dlib_config C:\iar\ewarm-9.60.3\arm\inc\c\DLib_Config_Full.h" --cxx "iccarm --no_cse --no_unroll --no_inline --no_code_motion --no_tbaa --no_clustering --no_scheduling --debug --endian=little --cpu=Cortex-M7 -e --fpu=VFPv5_d16 --dlib_config C:\iar\ewarm-9.60.3\arm\inc\c\DLib_Config_Full.h --c++" --config C:/TouchGFXProjects/MyApplication/axivion/compiler_config.json
axivion_ci %* || exit /b
