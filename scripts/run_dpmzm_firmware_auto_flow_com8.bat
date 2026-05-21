@echo off
setlocal

cd /d "%~dp0.."

set "DEFAULT_I=0.0"
set "DEFAULT_Q=0.0"
set "DEFAULT_P=0.0"

echo DPMZM pure firmware auto flow on COM8
echo.
echo Flow:
echo   1. Stop lock, enable metrics dump, open onboard pilot
echo   2. Set initial I/Q/P bias
echo   3. Run firmware dpmzm auto coarse
echo   4. Run firmware dpmzm auto fine
echo   5. Start firmware closed-loop lock
echo   6. Save serial log, metrics CSV, and all-stages plot
echo.

set /p INIT_I="Initial I voltage [default %DEFAULT_I%]: "
if "%INIT_I%"=="" set "INIT_I=%DEFAULT_I%"

set /p INIT_Q="Initial Q voltage [default %DEFAULT_Q%]: "
if "%INIT_Q%"=="" set "INIT_Q=%DEFAULT_Q%"

set /p INIT_P="Initial P voltage [default %DEFAULT_P%]: "
if "%INIT_P%"=="" set "INIT_P=%DEFAULT_P%"

echo.
echo Starting firmware auto flow with I=%INIT_I% V, Q=%INIT_Q% V, P=%INIT_P% V.
echo Closed-loop lock will be started after auto fine.
echo.

python scripts\run_dpmzm_firmware_auto_flow.py ^
  --port COM8 ^
  --output-dir "C:\Users\Administrator\Desktop\DPMZM_contral_bais\raw data" ^
  --plot-dir "C:\Users\Administrator\Desktop\DPMZM_contral_bais\simulation_image\auto_flow" ^
  --startup-wait 0.5 ^
  --coarse-timeout 360 ^
  --fine-timeout 360 ^
  --initial-i %INIT_I% ^
  --initial-q %INIT_Q% ^
  --initial-p %INIT_P%

set EXIT_CODE=%ERRORLEVEL%
echo.
echo Finished with exit code %EXIT_CODE%.
pause
exit /b %EXIT_CODE%
