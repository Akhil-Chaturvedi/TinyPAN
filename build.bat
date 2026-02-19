@echo off
REM TinyPAN Build Script for TCC (Tiny C Compiler)
REM Usage: build.bat [path-to-tcc]
REM Example: build.bat C:\tcc\tcc.exe

setlocal

REM Set TCC path - modify this or pass as argument
if "%1"=="" (
    set TCC=C:\Users\Electrobot\tcc\tcc.exe
) else (
    set TCC=%1
)

echo TinyPAN Build Script
echo ====================
echo Using compiler: %TCC%
echo.

REM Create build directory
if not exist build mkdir build

REM Compile core library
echo [1/6] Compiling TinyPAN core...
%TCC% -c -o build\tinypan.o -I include src\tinypan.c
%TCC% -c -o build\tinypan_bnep.o -I include src\tinypan_bnep.c
%TCC% -c -o build\tinypan_supervisor.o -I include src\tinypan_supervisor.c
if errorlevel 1 (
    echo FAILED: Core library
    exit /b 1
)
echo      OK

REM Compile mock HAL
echo [2/6] Compiling Mock HAL...
%TCC% -c -o build\hal_mock.o -I include hal\mock\tinypan_hal_mock.c
if errorlevel 1 (
    echo FAILED: Mock HAL
    exit /b 1
)
echo      OK

REM Compile BNEP tests
echo [3/6] Compiling BNEP tests...
%TCC% -c -o build\test_bnep.o -I include -I src tests\test_bnep.c
%TCC% -o build\test_bnep.exe build\test_bnep.o build\tinypan_bnep.o build\hal_mock.o
if errorlevel 1 (
    echo FAILED: test_bnep.exe
    exit /b 1
)
echo      OK: build\test_bnep.exe

REM Compile Supervisor tests
echo [4/6] Compiling Supervisor tests...
%TCC% -c -o build\test_supervisor.o -I include -I src tests\test_supervisor.c
%TCC% -o build\test_supervisor.exe build\test_supervisor.o build\tinypan.o build\tinypan_bnep.o build\tinypan_supervisor.o build\hal_mock.o
if errorlevel 1 (
    echo FAILED: test_supervisor.exe
    exit /b 1
)
echo      OK: build\test_supervisor.exe

REM Compile Integration test
echo [5/6] Compiling Integration tests...
%TCC% -c -o build\dhcp_sim.o -I include -I src tests\dhcp_sim.c
%TCC% -c -o build\test_integration.o -I include -I src -I tests tests\test_integration.c
%TCC% -o build\test_integration.exe build\test_integration.o build\dhcp_sim.o build\tinypan.o build\tinypan_bnep.o build\tinypan_supervisor.o build\hal_mock.o
if errorlevel 1 (
    echo FAILED: test_integration.exe
    exit /b 1
)
echo      OK: build\test_integration.exe

REM Compile demo
echo [6/6] Compiling Demo...
%TCC% -c -o build\demo.o -I include -I src examples\demo\main.c
%TCC% -o build\demo.exe build\demo.o build\tinypan.o build\tinypan_bnep.o build\tinypan_supervisor.o build\hal_mock.o
if errorlevel 1 (
    echo FAILED: demo.exe
    exit /b 1
)
echo      OK: build\demo.exe

echo.
echo ====================================
echo Build complete!
echo ====================================
echo.
echo Executables:
echo   build\test_bnep.exe        - BNEP unit tests
echo   build\test_supervisor.exe  - Supervisor tests  
echo   build\test_integration.exe - Integration tests
echo   build\demo.exe             - Connection demo
echo.
echo Run all tests:
echo   build\test_bnep.exe ^&^& build\test_supervisor.exe
echo.
