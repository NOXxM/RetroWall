# Configure + build (Visual Studio 2022 / MSVC toolset).
# Usage:  ./build.ps1            # Release
#         ./build.ps1 -Config Debug
param([string]$Config = "Release")

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $MyInvocation.MyCommand.Definition

cmake -S $root -B "$root/build" -G "Visual Studio 17 2022" -A x64
cmake --build "$root/build" --config $Config

Write-Host "Binary: $root/build/$Config/LiveWallpaperEngine.exe"
