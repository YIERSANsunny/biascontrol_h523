@echo off
setlocal

cd /d "%~dp0.."

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

echo DPMZM PC-side gradient lock test on COM8
echo.
echo Flow:
echo   1. Stop firmware lock, enable metrics dump, open onboard pilot
echo   2. Read current I/Q/P bias as anchor from dpmzm status
echo   3. Run PC-side coordinate gradient descent: P -^> I -^> Q -^> ...
echo   4. Use adaptive per-axis gain/max-step when one axis keeps moving in the same direction
echo   5. Trigger local I/Q/P recheck only when PD DC rises beyond threshold
echo   6. Save serial log, gradient CSV, and summary plot
echo.

set /p CYCLES="Gradient updates [default %DEFAULT_CYCLES%]: "
if "%CYCLES%"=="" set "CYCLES=%DEFAULT_CYCLES%"

set /p DELTA="Probe delta V [default %DEFAULT_DELTA%]: "
if "%DELTA%"=="" set "DELTA=%DEFAULT_DELTA%"

set /p GAIN="Gain V [default %DEFAULT_GAIN%]: "
if "%GAIN%"=="" set "GAIN=%DEFAULT_GAIN%"

set /p MAX_STEP="Max step V [default %DEFAULT_MAX_STEP%]: "
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
echo Starting gradient test: cycles=%CYCLES%, delta=%DELTA% V, gain=%GAIN% V, max_step=%MAX_STEP% V, q_gain=%Q_GAIN% V, q_max_step=%Q_MAX_STEP% V, adaptive_hard_max=%ADAPTIVE_HARD_MAX_STEP% V, dc_guard=%DC_GUARD_THRESHOLD_DB%dB release=%DC_GUARD_RELEASE_DB%dB avg=%DC_GUARD_AVG_WINDOW% count=%DC_GUARD_COUNT%.
echo.

python scripts\run_dpmzm_gradient_lock_test.py ^
  --port COM8 ^
  --output-dir "C:\Users\Administrator\Desktop\DPMZM_contral_bais\raw data" ^
  --plot-dir "C:\Users\Administrator\Desktop\DPMZM_contral_bais\simulation_image\lock_gradient" ^
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
  --deadband 0.03 ^
  --iq-blocks 4 ^
  --p-blocks 10 ^
  %*

set EXIT_CODE=%ERRORLEVEL%
echo.
echo Finished with exit code %EXIT_CODE%.
pause
exit /b %EXIT_CODE%
