# TinyPAN Build Script for TCC (Tiny C Compiler)
# Usage: .\build.ps1 [-TccPath "C:\tcc\tcc.exe"]

param(
    [string]$TccPath = "tcc"
)

Write-Host "TinyPAN Build Script" -ForegroundColor Cyan
Write-Host "====================" -ForegroundColor Cyan
Write-Host "Using compiler: $TccPath"
Write-Host ""

# Check if TCC exists
$tccCmd = Get-Command $TccPath -ErrorAction SilentlyContinue
if (-not $tccCmd) {
    Write-Host "ERROR: TCC not found at '$TccPath'" -ForegroundColor Red
    Write-Host ""
    Write-Host "Please download TCC from:" -ForegroundColor Yellow
    Write-Host "  http://download.savannah.gnu.org/releases/tinycc/tcc-0.9.27-win64-bin.zip" -ForegroundColor Yellow
    Write-Host ""
    Write-Host "Then run:" -ForegroundColor Yellow
    Write-Host "  .\build.ps1 -TccPath 'C:\path\to\tcc\tcc.exe'" -ForegroundColor Yellow
    exit 1
}

# Create build directory
if (-not (Test-Path "build")) {
    New-Item -ItemType Directory -Path "build" | Out-Null
}

# Compile BNEP tests
Write-Host "[1/2] Compiling BNEP unit tests..." -ForegroundColor Yellow
$result = & $TccPath -o build/test_bnep.exe `
    -I include `
    -I src `
    tests/test_bnep.c `
    src/tinypan_bnep.c `
    hal/mock/tinypan_hal_mock.c 2>&1

if ($LASTEXITCODE -ne 0) {
    Write-Host "FAILED: test_bnep.exe" -ForegroundColor Red
    Write-Host $result
    exit 1
}
Write-Host "      OK: build/test_bnep.exe" -ForegroundColor Green

# Compile library objects
Write-Host "[2/2] Compiling TinyPAN library..." -ForegroundColor Yellow

& $TccPath -c -o build/tinypan.o -I include src/tinypan.c
& $TccPath -c -o build/tinypan_bnep.o -I include src/tinypan_bnep.c  
& $TccPath -c -o build/tinypan_supervisor.o -I include src/tinypan_supervisor.c
& $TccPath -c -o build/tinypan_hal_mock.o -I include hal/mock/tinypan_hal_mock.c

if ($LASTEXITCODE -ne 0) {
    Write-Host "FAILED: Library compilation" -ForegroundColor Red
    exit 1
}
Write-Host "      OK: Library objects compiled" -ForegroundColor Green

Write-Host ""
Write-Host "Build complete!" -ForegroundColor Green
Write-Host ""
Write-Host "To run tests:" -ForegroundColor Cyan
Write-Host "  .\build\test_bnep.exe"
Write-Host ""

# Optionally run tests
$runTests = Read-Host "Run tests now? (y/N)"
if ($runTests -eq "y" -or $runTests -eq "Y") {
    Write-Host ""
    Write-Host "Running BNEP tests..." -ForegroundColor Cyan
    Write-Host "---------------------"
    & .\build\test_bnep.exe
}
