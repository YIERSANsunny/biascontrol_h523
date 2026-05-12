@echo off
setlocal

cd /d "%~dp0.."

set "DEFAULT_CYCLES=30"
set "DEFAULT_DELTA=0.01"
set "DEFAULT_GAIN=0.003"
set "DEFAULT_MAX_STEP=0.003"

echo DPMZM PC-side gradient lock test on COM8
echo.
echo Flow:
echo   1. Stop firmware lock, enable metrics dump, open onboard pilot
echo   2. Read current I/Q/P bias as anchor from dpmzm status
echo   3. Run PC-side coordinate gradient descent: P -^> I -^> Q -^> ...
echo   4. Save serial log, gradient CSV, and summary plot
echo.

set /p CYCLES="Gradient updates [default %DEFAULT_CYCLES%]: "
if "%CYCLES%"=="" set "CYCLES=%DEFAULT_CYCLES%"

set /p DELTA="Probe delta V [default %DEFAULT_DELTA%]: "
if "%DELTA%"=="" set "DELTA=%DEFAULT_DELTA%"

set /p GAIN="Gain V [default %DEFAULT_GAIN%]: "
if "%GAIN%"=="" set "GAIN=%DEFAULT_GAIN%"

set /p MAX_STEP="Max step V [default %DEFAULT_MAX_STEP%]: "
if "%MAX_STEP%"=="" set "MAX_STEP=%DEFAULT_MAX_STEP%"

echo.
echo Starting gradient test: cycles=%CYCLES%, delta=%DELTA% V, gain=%GAIN% V, max_step=%MAX_STEP% V.
echo.

python tools\run_dpmzm_gradient_lock_test.py ^
  --port COM8 ^
  --output-dir "C:\Users\Administrator\Desktop\DPMZM_contral_bais\raw data" ^
  --plot-dir "C:\Users\Administrator\Desktop\DPMZM_contral_bais\simulation_image\lock_gradient" ^
  --cycles %CYCLES% ^
  --sequence piq ^
  --delta %DELTA% ^
  --gain %GAIN% ^
  --max-step %MAX_STEP% ^
  --deadband 0.03 ^
  --iq-blocks 4 ^
  --p-blocks 10 ^
  %*

set EXIT_CODE=%ERRORLEVEL%
echo.
echo Finished with exit code %EXIT_CODE%.
pause
exit /b %EXIT_CODE%
