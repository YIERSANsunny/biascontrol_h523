@echo off
setlocal

cd /d "%~dp0.."

set "DEFAULT_I=0.0"
set "DEFAULT_Q=0.0"
set "DEFAULT_P=0.0"
set "DEFAULT_CYCLES=30"
set "DEFAULT_DELTA=0.01"
set "DEFAULT_GAIN=0.003"
set "DEFAULT_MAX_STEP=0.003"

echo DPMZM auto-find + PC-side gradient lock test on COM8
echo.
echo Flow:
echo   1. Firmware auto coarse
echo   2. Firmware auto fine
echo   3. Do NOT start firmware lock
echo   4. Use final I/Q/P as gradient-test anchor
echo   5. Run PC-side P -^> I -^> Q coordinate gradient descent
echo.

set /p INIT_I="Initial I voltage before auto [default %DEFAULT_I%]: "
if "%INIT_I%"=="" set "INIT_I=%DEFAULT_I%"

set /p INIT_Q="Initial Q voltage before auto [default %DEFAULT_Q%]: "
if "%INIT_Q%"=="" set "INIT_Q=%DEFAULT_Q%"

set /p INIT_P="Initial P voltage before auto [default %DEFAULT_P%]: "
if "%INIT_P%"=="" set "INIT_P=%DEFAULT_P%"

set /p CYCLES="Gradient updates after auto [default %DEFAULT_CYCLES%]: "
if "%CYCLES%"=="" set "CYCLES=%DEFAULT_CYCLES%"

set /p DELTA="Gradient probe delta V [default %DEFAULT_DELTA%]: "
if "%DELTA%"=="" set "DELTA=%DEFAULT_DELTA%"

set /p GAIN="Gradient gain V [default %DEFAULT_GAIN%]: "
if "%GAIN%"=="" set "GAIN=%DEFAULT_GAIN%"

set /p MAX_STEP="Gradient max step V [default %DEFAULT_MAX_STEP%]: "
if "%MAX_STEP%"=="" set "MAX_STEP=%DEFAULT_MAX_STEP%"

echo.
echo Step 1/2: firmware auto find, no lock at end.
echo.

python tools\run_dpmzm_firmware_auto_flow.py ^
  --port COM8 ^
  --output-dir "C:\Users\Administrator\Desktop\DPMZM_contral_bais\raw data" ^
  --plot-dir "C:\Users\Administrator\Desktop\DPMZM_contral_bais\simulation_image\auto_flow" ^
  --startup-wait 0.5 ^
  --coarse-timeout 360 ^
  --fine-timeout 360 ^
  --initial-i %INIT_I% ^
  --initial-q %INIT_Q% ^
  --initial-p %INIT_P% ^
  --no-lock-at-end

if errorlevel 1 (
  echo.
  echo Firmware auto flow failed. Gradient test will not start.
  pause
  exit /b 1
)

echo.
echo Step 2/2: PC-side gradient descent from the firmware auto result.
echo.

python tools\run_dpmzm_gradient_lock_test.py ^
  --port COM8 ^
  --output-dir "C:\Users\Administrator\Desktop\DPMZM_contral_bais\raw data" ^
  --plot-dir "C:\Users\Administrator\Desktop\DPMZM_contral_bais\simulation_image\lock_gradient" ^
  --startup-wait 0.5 ^
  --cycles %CYCLES% ^
  --sequence piq ^
  --delta %DELTA% ^
  --gain %GAIN% ^
  --max-step %MAX_STEP% ^
  --deadband 0.03 ^
  --iq-blocks 4 ^
  --p-blocks 10

set EXIT_CODE=%ERRORLEVEL%
echo.
echo Finished with exit code %EXIT_CODE%.
pause
exit /b %EXIT_CODE%
