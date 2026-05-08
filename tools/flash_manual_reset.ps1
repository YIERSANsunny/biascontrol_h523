param(
    [string]$Firmware,
    [string]$Target = "stm32h523cetx",
    [int]$Frequency = 100000,
    [int]$Attempts = 8,
    [int]$RetryDelayMs = 350,
    [switch]$Build,
    [switch]$NoReset,
    [switch]$DryRun,
    [switch]$Help
)

$ErrorActionPreference = "Stop"

$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$repoRoot = Resolve-Path (Join-Path $scriptDir "..")

if ($Help) {
    Write-Host "Manual RESET flashing helper"
    Write-Host ""
    Write-Host "Typical use:"
    Write-Host "  1. Hold the board RESET button."
    Write-Host "  2. Run: tools\flash_manual_reset.bat"
    Write-Host "  3. Release RESET as soon as this script starts connecting."
    Write-Host ""
    Write-Host "PowerShell examples:"
    Write-Host "  powershell -ExecutionPolicy Bypass -File tools\flash_manual_reset.ps1"
    Write-Host "  powershell -ExecutionPolicy Bypass -File tools\flash_manual_reset.ps1 -Build"
    Write-Host "  powershell -ExecutionPolicy Bypass -File tools\flash_manual_reset.ps1 -NoReset"
    exit 0
}

if (-not $Firmware) {
    $Firmware = Join-Path $repoRoot "build\biascontrol.hex"
}

$Firmware = [System.IO.Path]::GetFullPath($Firmware)

if ($Build) {
    Write-Host "[flash] Building firmware first..."
    Push-Location $repoRoot
    try {
        & cmake --build build -j 8
        if ($LASTEXITCODE -ne 0) {
            throw "cmake build failed with exit code $LASTEXITCODE"
        }
    } finally {
        Pop-Location
    }
}

if (-not (Test-Path -LiteralPath $Firmware)) {
    throw "Firmware file not found: $Firmware"
}

$pyocd = Get-Command pyocd -ErrorAction Stop
$flashArgs = @(
    "flash",
    "-t", $Target,
    "--connect", "halt",
    "--frequency", $Frequency.ToString()
)

if ($NoReset) {
    $flashArgs += "--no-reset"
}

$flashArgs += $Firmware

Write-Host "[flash] Firmware : $Firmware"
Write-Host "[flash] Target   : $Target"
Write-Host "[flash] Frequency: $Frequency Hz"
Write-Host "[flash] Attempts : $Attempts"
Write-Host ""
Write-Host "Hold RESET before launching this script, then release RESET now/when pyOCD starts connecting."
Write-Host "The script will retry automatically if the first timing misses."
Write-Host ""

if ($DryRun) {
    Write-Host "[flash] Dry run command:"
    Write-Host "  pyocd $($flashArgs -join ' ')"
    exit 0
}

for ($attempt = 1; $attempt -le $Attempts; $attempt++) {
    Write-Host "[flash] Attempt $attempt/$Attempts..."
    & $pyocd.Path @flashArgs
    $exitCode = $LASTEXITCODE

    if ($exitCode -eq 0) {
        Write-Host ""
        Write-Host "[flash] Success. If the board does not start automatically, tap RESET once."
        exit 0
    }

    Write-Host "[flash] Attempt $attempt failed with exit code $exitCode."
    if ($attempt -lt $Attempts) {
        Start-Sleep -Milliseconds $RetryDelayMs
    }
}

Write-Host ""
Write-Host "[flash] All attempts failed."
Write-Host "[flash] If this keeps happening, check target 3.3V/VREF, GND, SWDIO, SWCLK, and RESET button state."
exit 1
