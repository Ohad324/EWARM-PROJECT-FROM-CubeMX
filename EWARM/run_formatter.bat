@echo off
echo ============================================
echo  NORA SD Formatter — waiting for board...
echo ============================================
echo.

:: Verify Python is reachable
where python >nul 2>&1
if %ERRORLEVEL% neq 0 (
    echo ERROR: python.exe not found in PATH.
    echo        Install Python or add it to the system PATH.
    pause
    exit /b 1
)
echo Python: OK
python --version
echo.

:: Run the formatter
python "C:\TouchGFXProjects\MyApplication\tools\format_sd.py" --auto
echo.
echo Exit code: %ERRORLEVEL%
echo ============================================
echo  Done. Press any key to close.
echo ============================================
pause >nul
