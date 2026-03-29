@echo off
set IDF_PATH=C:\Espressif\frameworks\esp-idf-v5.5.3
set IDF_TOOLS_PATH=C:\Espressif
set PYTHON=C:\Espressif\python_env\idf5.5_py3.11_env\Scripts\python.exe

set TEMP_IDF_GIT_PATH=%TEMP%\idf-git-path.txt
C:\Espressif\idf-env.exe config get --property gitPath>%TEMP_IDF_GIT_PATH%
set /P IDF_GIT=<%TEMP_IDF_GIT_PATH%
for %%F in (%IDF_GIT%) do set IDF_GIT_DIR=%%~dpF

for %%F in (%PYTHON%) do set IDF_PYTHON_DIR=%%~dpF
set "PATH=%IDF_PYTHON_DIR%;%IDF_GIT_DIR%;%IDF_TOOLS_PATH%;%PATH%"

:: Add IDF tools to PATH (cmake, ninja, xtensa gcc, etc.)
for /f "tokens=*" %%i in ('"%PYTHON%" "%IDF_PATH%\tools\idf_tools.py" export --format key-value 2^>nul') do (
    set %%i
)

cd C:\NORA_BLE\nora_wifi_ble_translate
"%PYTHON%" "%IDF_PATH%\tools\idf.py" build > build_output.txt 2>&1
echo Exit code: %ERRORLEVEL%
type build_output.txt
