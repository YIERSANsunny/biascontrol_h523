@echo off
setlocal

cd /d "%~dp0.."

set "DEFAULT_I=0.0"
set "DEFAULT_Q=0.0"
set "DEFAULT_P=0.0"
set "DEFAULT_PORT=COM9"
set "DEFAULT_CYCLES=100"
set "DEFAULT_DELTA=0.01"
set "DEFAULT_GAIN=0.003"
set "DEFAULT_MAX_STEP=0.003"
set "DEFAULT_Q_GAIN=0.003"
set "DEFAULT_Q_MAX_STEP=0.003"
set "DEFAULT_ADAPTIVE_HARD_MAX_STEP=0.010"
set "DEFAULT_DC_GUARD_THRESHOLD_DB=4.0"
set "DEFAULT_DC_GUARD_RELEASE_DB=2.0"
set "DEFAULT_DC_GUARD_AVG_WINDOW=5"
set "DEFAULT_DC_GUARD_COUNT=3"
set "DEFAULT_BOUNDARY_RECHECK_DEGRADE_DB=8.0"
set "DEFAULT_BOUNDARY_RECHECK_COUNT=3"
set "DEFAULT_BOUNDARY_RECHECK_WINDOW=0.15"
set "DEFAULT_BOUNDARY_RECHECK_STEP=0.01"
set "DEFAULT_BOUNDARY_RECHECK_COOLDOWN=12"
set "DEFAULT_BOUNDARY_RECHECK_EXPAND_MARGIN=0.10"
set "DEFAULT_BOUNDARY_RECHECK_FAST_REPEAT_STEPS=30"
set "DEFAULT_BOUNDARY_RECHECK_FAST_EXPAND=1.5"
set "DEFAULT_BOUNDARY_RECHECK_MAX_WINDOW=0.80"
set "DEFAULT_BOUNDARY_RECHECK_SHRINK_STABLE_STEPS=50"
set "DEFAULT_BOUNDARY_RECHECK_SHRINK=0.90"

echo DPMZM auto-find + PC-side gradient lock test
echo.
echo Flow:
echo   1. Firmware auto coarse
echo   2. Firmware auto fine
echo   3. Do NOT start firmware lock
echo   4. Use final I/Q/P as gradient-test anchor
echo   5. Run PC-side P -^> I -^> Q coordinate gradient descent
echo      Start from common I/Q/P step; adaptive tracking increases step only when needed
echo      PD DC rise guard triggers local I/Q/P recheck only when average optical power rises
echo      I/Q boundary guard recenters anchors when repeated clamps make the metric worse
echo.

echo Available serial ports:
powershell -NoProfile -Command "[System.IO.Ports.SerialPort]::GetPortNames() -join ', '"
echo.
set /p PORT="Serial port [default %DEFAULT_PORT%]: "
if "%PORT%"=="" set "PORT=%DEFAULT_PORT%"
echo Using serial port: %PORT%
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

set /p Q_GAIN="Q-axis gain V [default %DEFAULT_Q_GAIN%]: "
if "%Q_GAIN%"=="" set "Q_GAIN=%DEFAULT_Q_GAIN%"

set /p Q_MAX_STEP="Q-axis max step V [default %DEFAULT_Q_MAX_STEP%]: "
if "%Q_MAX_STEP%"=="" set "Q_MAX_STEP=%DEFAULT_Q_MAX_STEP%"

set /p ADAPTIVE_HARD_MAX_STEP="Adaptive hard max step V [default %DEFAULT_ADAPTIVE_HARD_MAX_STEP%]: "
if "%ADAPTIVE_HARD_MAX_STEP%"=="" set "ADAPTIVE_HARD_MAX_STEP=%DEFAULT_ADAPTIVE_HARD_MAX_STEP%"

set /p DC_GUARD_THRESHOLD_DB="PD DC rise trigger dB [default %DEFAULT_DC_GUARD_THRESHOLD_DB%]: "
if "%DC_GUARD_THRESHOLD_DB%"=="" set "DC_GUARD_THRESHOLD_DB=%DEFAULT_DC_GUARD_THRESHOLD_DB%"

set /p DC_GUARD_RELEASE_DB="PD DC release dB [default %DEFAULT_DC_GUARD_RELEASE_DB%]: "
if "%DC_GUARD_RELEASE_DB%"=="" set "DC_GUARD_RELEASE_DB=%DEFAULT_DC_GUARD_RELEASE_DB%"

set /p DC_GUARD_AVG_WINDOW="PD DC average window [default %DEFAULT_DC_GUARD_AVG_WINDOW%]: "
if "%DC_GUARD_AVG_WINDOW%"=="" set "DC_GUARD_AVG_WINDOW=%DEFAULT_DC_GUARD_AVG_WINDOW%"

set /p DC_GUARD_COUNT="PD DC consecutive trigger count [default %DEFAULT_DC_GUARD_COUNT%]: "
if "%DC_GUARD_COUNT%"=="" set "DC_GUARD_COUNT=%DEFAULT_DC_GUARD_COUNT%"

echo.
echo Step 1/2: firmware auto find, no lock at end.
echo.

python tools\run_dpmzm_firmware_auto_flow.py ^
  --port %PORT% ^
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
  --port %PORT% ^
  --output-dir "C:\Users\Administrator\Desktop\DPMZM_contral_bais\raw data" ^
  --plot-dir "C:\Users\Administrator\Desktop\DPMZM_contral_bais\simulation_image\lock_gradient" ^
  --startup-wait 0.5 ^
  --cycles %CYCLES% ^
  --sequence piq ^
  --delta %DELTA% ^
  --gain %GAIN% ^
  --max-step %MAX_STEP% ^
  --q-gain %Q_GAIN% ^
  --q-max-step %Q_MAX_STEP% ^
  --adaptive-step ^
  --adaptive-hard-max-step %ADAPTIVE_HARD_MAX_STEP% ^
  --dc-guard ^
  --dc-guard-threshold-db %DC_GUARD_THRESHOLD_DB% ^
  --dc-guard-release-db %DC_GUARD_RELEASE_DB% ^
  --dc-guard-avg-window %DC_GUARD_AVG_WINDOW% ^
  --dc-guard-trigger-count %DC_GUARD_COUNT% ^
  --dc-guard-recheck-sequence iqp ^
  --dc-guard-update-anchor ^
  --boundary-recheck ^
  --boundary-recheck-degrade-db %DEFAULT_BOUNDARY_RECHECK_DEGRADE_DB% ^
  --boundary-recheck-count %DEFAULT_BOUNDARY_RECHECK_COUNT% ^
  --boundary-recheck-window %DEFAULT_BOUNDARY_RECHECK_WINDOW% ^
  --boundary-recheck-step %DEFAULT_BOUNDARY_RECHECK_STEP% ^
  --boundary-recheck-cooldown-steps %DEFAULT_BOUNDARY_RECHECK_COOLDOWN% ^
  --boundary-recheck-expand-margin %DEFAULT_BOUNDARY_RECHECK_EXPAND_MARGIN% ^
  --boundary-recheck-fast-repeat-steps %DEFAULT_BOUNDARY_RECHECK_FAST_REPEAT_STEPS% ^
  --boundary-recheck-fast-expand %DEFAULT_BOUNDARY_RECHECK_FAST_EXPAND% ^
  --boundary-recheck-max-window %DEFAULT_BOUNDARY_RECHECK_MAX_WINDOW% ^
  --boundary-recheck-shrink-stable-steps %DEFAULT_BOUNDARY_RECHECK_SHRINK_STABLE_STEPS% ^
  --boundary-recheck-shrink %DEFAULT_BOUNDARY_RECHECK_SHRINK% ^
  --deadband 0.03 ^
  --iq-blocks 4 ^
  --p-blocks 10

set EXIT_CODE=%ERRORLEVEL%
echo.
echo Finished with exit code %EXIT_CODE%.
pause
exit /b %EXIT_CODE%
