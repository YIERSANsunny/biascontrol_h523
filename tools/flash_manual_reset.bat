@echo off
setlocal
cd /d "%~dp0.."
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0flash_manual_reset.ps1" %*
set FLASH_EXIT=%ERRORLEVEL%
echo.
if %FLASH_EXIT% EQU 0 (
    echo [flash] Done.
) else (
    echo [flash] Failed with exit code %FLASH_EXIT%.
)
pause
exit /b %FLASH_EXIT%
