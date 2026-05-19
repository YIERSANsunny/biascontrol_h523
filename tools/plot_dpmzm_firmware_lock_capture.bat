@echo off
setlocal
set "SCRIPT_DIR=%~dp0"
python "%SCRIPT_DIR%plot_dpmzm_firmware_lock_capture.py" %*
echo.
echo Finished with exit code %ERRORLEVEL%.
pause
