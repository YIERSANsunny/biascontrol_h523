@echo off
setlocal

cd /d "%~dp0.."

set "DEFAULT_DURATION=300"

echo DPMZM firmware closed-loop capture on COM8
echo.
echo This captures the firmware lock loop itself:
echo   axis / bias / metric0,+,- / DC / error / applied step
echo.
echo Recommended flow:
echo   1. Finish auto coarse/fine or manually set a known good point
echo   2. Make sure RF and optical setup match the real experiment
echo   3. Run this script and observe the OSA carrier at the same time
echo.

set /p DURATION="Capture duration seconds [default %DEFAULT_DURATION%]: "
if "%DURATION%"=="" set "DURATION=%DEFAULT_DURATION%"

set /p START_LOCK="Send 'dpmzm lock start' before capture? [y/N]: "
set "START_LOCK_ARG="
if /I "%START_LOCK%"=="y" set "START_LOCK_ARG=--start-lock"
if /I "%START_LOCK%"=="yes" set "START_LOCK_ARG=--start-lock"

python scripts\capture_dpmzm_firmware_lock_data.py ^
  --port COM8 ^
  --duration %DURATION% ^
  --output-dir "C:\Users\Administrator\Desktop\DPMZM_contral_bais\raw data" ^
  --plot-dir "C:\Users\Administrator\Desktop\DPMZM_contral_bais\simulation_image\lock_firmware" ^
  %START_LOCK_ARG% ^
  %*

set EXIT_CODE=%ERRORLEVEL%
echo.
echo Finished with exit code %EXIT_CODE%.
pause
exit /b %EXIT_CODE%
