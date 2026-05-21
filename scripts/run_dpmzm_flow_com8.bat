@echo off
setlocal

cd /d "%~dp0.."

set "DEFAULT_P=0.0"

echo DPMZM positive-branch flow on COM8
echo.
echo Flow:
echo   1. Set initial P bias
echo   2. Run I/Q MATP coarse scans and choose high-power plateau seeds
echo   3. Run P-QTP full scan
echo   4. Run I-MITP full scan
echo   5. Run Q-MITP full scan
echo   6. Re-run P-QTP full scan after I/Q MITP
echo   7. Run P/I/Q/P turning-point refinement
echo   8. Run pre-lock I/Q turning recheck and start lock
echo.

set /p INIT_P="Initial P voltage [default %DEFAULT_P%]: "
if "%INIT_P%"=="" set "INIT_P=%DEFAULT_P%"

echo.
echo Starting with P=%INIT_P% V; I/Q will be selected by MATP coarse scans.
echo.

python scripts\run_dpmzm_positive_branch_flow.py ^
  --port COM8 ^
  --output-dir "C:\Users\Administrator\Desktop\DPMZM_contral_bais\raw data" ^
  --plot-dir "C:\Users\Administrator\Desktop\DPMZM_contral_bais\simulation_image\auto_flow" ^
  --startup-wait 0.5 ^
  --coarse-timeout 300 ^
  --fine-timeout 240 ^
  --manual-p-qtp-seed ^
  --iq-matp-seed ^
  --initial-p %INIT_P% ^
  --manual-matp-start -9.0 ^
  --manual-matp-stop 9.0 ^
  --manual-matp-step 0.5 ^
  --manual-p-start -9.0 ^
  --manual-p-stop 9.0 ^
  --manual-p-step 0.5 ^
  --manual-mitp-start -9.0 ^
  --manual-mitp-stop 9.0 ^
  --manual-mitp-step 0.5 ^
  --p-blocks 10 ^
  --iq-blocks 4 ^
  %*

set EXIT_CODE=%ERRORLEVEL%
echo.
echo Finished with exit code %EXIT_CODE%.
pause
exit /b %EXIT_CODE%
