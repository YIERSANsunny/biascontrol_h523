@echo off
setlocal

cd /d "%~dp0.."

set "DEFAULT_PORT=COM9"
set "DEFAULT_SCAN=dpmzm scan matp i -9.0 9.0 0.5 4"

echo DPMZM single scan + save data + plot
echo.
echo Examples:
echo   dpmzm scan matp i -9.0 9.0 0.5 4
echo   dpmzm scan matp q -9.0 9.0 0.5 4
echo   dpmzm scan qtp p -9.0 9.0 0.5 10
echo   dpmzm scan mitp i -9.0 9.0 0.1 6
echo.

set /p PORT="Serial port [default %DEFAULT_PORT%]: "
if "%PORT%"=="" set "PORT=%DEFAULT_PORT%"

set /p SCAN_CMD="Scan command [default %DEFAULT_SCAN%]: "
if "%SCAN_CMD%"=="" set "SCAN_CMD=%DEFAULT_SCAN%"

echo.
echo Port: %PORT%
echo Scan: %SCAN_CMD%
echo.

python scripts\run_dpmzm_scan_and_plot.py ^
  --port "%PORT%" ^
  --scan "%SCAN_CMD%" ^
  --output-dir "C:\Users\Administrator\Desktop\DPMZM_contral_bais\raw data" ^
  --plot-dir "C:\Users\Administrator\Desktop\DPMZM_contral_bais\simulation_image\manual_scan" ^
  %*

set EXIT_CODE=%ERRORLEVEL%
echo.
echo Finished with exit code %EXIT_CODE%.
pause
exit /b %EXIT_CODE%
