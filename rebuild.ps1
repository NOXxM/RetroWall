# rebuild.ps1 — stop any running instance, rebuild, and relaunch.
# Run from the "Developer PowerShell for VS 2022":   .\rebuild.ps1
$ErrorActionPreference = "Continue"
Set-Location $PSScriptRoot

Write-Host "Stopping any running instance..." -ForegroundColor Cyan
taskkill /IM RetroWall.exe /F 2>$null | Out-Null
Start-Sleep -Milliseconds 500

Write-Host "Building (Release x64)..." -ForegroundColor Cyan
cmake --build build --config Release
if ($LASTEXITCODE -ne 0) {
    Write-Host "BUILD FAILED — see the first 'error' line above." -ForegroundColor Red
    exit 1
}

Write-Host "Launching..." -ForegroundColor Green
Start-Process "build\Release\RetroWall.exe"
Write-Host "Running. Wait ~10 seconds (don't pause it), then tell Claude 'done'." -ForegroundColor Green
