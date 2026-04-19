@echo off
echo ================================================================
echo   SCOPE CLOCK TEST — PE2 (SAI4_CK1 = 2.000 MHz PDM clock)
echo ================================================================
echo.
echo   Probe: PE2 / SB45
echo   Expected: 2.000 MHz square wave, 3.3V, 50%% duty cycle
echo.
echo   Flashing firmware and triggering recording in 500ms...
echo.

"C:\Program Files\SEGGER\JLink_V930a\JLink.exe" -device STM32H747XI_M7 -if SWD -speed 4000 -autoconnect 1 -CommandFile "%~dp0scope_clock.jlink"

echo.
echo   Clock should now be active on PE2.
echo   Recording will run until the firmware timeout.
echo   Press blue button on board to stop early.
echo.
pause
