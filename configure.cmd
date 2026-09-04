@echo off
setlocal enabledelayedexpansion

echo ===================================
echo === Starting Configuration      ===
echo ===================================

:: 1. Check for GCC on PATH
where gcc >nul 2>nul
if %errorlevel% neq 0 (
    echo [ERROR] GCC compiler could not be found on your PATH.
    exit /b 1
) else (
    for /f "tokens=*" %%i in ('gcc -dumpversion') do set GCC_VER=%%i
    echo [OK] Found GCC version: !GCC_VER!
)

:: 2. Check for Make on PATH
where make >nul 2>nul
if %errorlevel% neq 0 (
    echo [ERROR] GNU Make could not be found on your PATH.
    exit /b 1
) else (
    for /f "tokens=*" %%i in ('make --version') do (
        set MAKE_VER=%%i
        goto :show_make
    )
    :show_make
    echo [OK] Found Make: !MAKE_VER!
)

echo ===================================
echo === Configuration Complete!     ===
echo ===================================
echo.
echo Running Make...
echo.

:: 3. Execute Make
make
