@echo off
setlocal EnableExtensions

rem One-click build + flash helper for STM32H523 biascontrol firmware.
rem Usage:
rem   scripts\flash_current_firmware.bat [delay_seconds] [connect_mode] [frequency_hz] [reset_option]
rem Examples:
rem   scripts\flash_current_firmware.bat
rem   scripts\flash_current_firmware.bat 3 under-reset 100000
rem   scripts\flash_current_firmware.bat 8 under-reset 100000 --no-reset

if /I "%~1"=="help" goto :help
if /I "%~1"=="--help" goto :help
if /I "%~1"=="/?" goto :help

set "REPO=%~dp0.."
set "DELAY=%~1"
set "CONNECT=%~2"
set "FREQ=%~3"
set "RESET_OPTION=%~4"

if "%DELAY%"=="" set "DELAY=2"
if "%CONNECT%"=="" set "CONNECT=under-reset"
if "%FREQ%"=="" set "FREQ=1000000"

pushd "%REPO%" || exit /b 1

echo [flash] repo: %CD%
echo [flash] building firmware...
cmake --build build -j 8
if errorlevel 1 (
    echo [flash] build failed.
    popd
    exit /b 1
)

if not exist "build\biascontrol.hex" (
    echo [flash] missing build\biascontrol.hex
    popd
    exit /b 1
)

where pyocd >nul 2>nul
if errorlevel 1 (
    echo [flash] pyocd was not found in PATH.
    echo [flash] Install pyocd or add it to PATH, then retry.
    popd
    exit /b 1
)

echo [flash] target: stm32h523cetx
echo [flash] image:  build\biascontrol.hex
echo [flash] delay:  %DELAY%s
echo [flash] connect=%CONNECT% frequency=%FREQ% %RESET_OPTION%
echo [flash] If you are holding RESET manually, release it when pyocd starts connecting.
timeout /t %DELAY% /nobreak >nul

pyocd flash -t stm32h523cetx --connect %CONNECT% --frequency %FREQ% %RESET_OPTION% build\biascontrol.hex
set "FLASH_RC=%ERRORLEVEL%"

if not "%FLASH_RC%"=="0" (
    echo [flash] flash failed, exit code %FLASH_RC%.
    popd
    exit /b %FLASH_RC%
)

echo [flash] flash completed.
popd
exit /b 0

:help
echo One-click build + flash helper for STM32H523 biascontrol firmware.
echo.
echo Usage:
echo   scripts\flash_current_firmware.bat [delay_seconds] [connect_mode] [frequency_hz] [reset_option]
echo.
echo Defaults:
echo   delay_seconds = 2
echo   connect_mode  = under-reset
echo   frequency_hz  = 1000000
echo   reset_option  = empty
echo.
echo Examples:
echo   scripts\flash_current_firmware.bat
echo   scripts\flash_current_firmware.bat 3 under-reset 100000
echo   scripts\flash_current_firmware.bat 8 under-reset 100000 --no-reset
exit /b 0
