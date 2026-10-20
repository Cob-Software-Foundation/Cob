@echo off
setlocal enabledelayedexpansion

:: Check for the custom smartpass argument
if "%~1"=="--smartpass" (
    echo --- [SmartPass System Alert] ---
    echo Initiating 3-second countdown to exit the terminal...
    timeout /t 1 >nul
    echo Timer running: 2 seconds remaining...
    timeout /t 1 >nul
    echo Timer running: 1 second remaining...
    timeout /t 1 >nul
    echo [ERROR] OVERTIME DETECTED! 3 minutes is up!
    echo [ERROR] Configuration frozen. Turn your Chromebook around and go get a physical yellow paper pass.
    exit /b 1
)

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

:: 3. Case-Insensitive Vendor Directory Verification
set "VENDOR_DIR=vendor"
if not exist "%VENDOR_DIR%" (
    echo [ERROR] The vendor directory '%VENDOR_DIR%' does not exist in the root.
    exit /b 1
)

set MISSING_DEPENDENCY=0

:: Check for SQLite
if not exist "%VENDOR_DIR%\SQLite\" (
    echo [ERROR] Missing required vendor dependency: SQLite
    set MISSING_DEPENDENCY=1
)

:: Check for TCL
if not exist "%VENDOR_DIR%\tcl\" (
    echo [ERROR] Missing required vendor dependency: TCL
    set MISSING_DEPENDENCY=1
)

:: Check for TK
if not exist "%VENDOR_DIR%\tk\" (
    echo [ERROR] Missing required vendor dependency: TK
    set MISSING_DEPENDENCY=1
)

:: Check for MINIZ
if not exist "%VENDOR_DIR%\miniz\" (
    echo [ERROR] Missing required vendor dependency: MINIZ
    set MISSING_DEPENDENCY=1
)

if %MISSING_DEPENDENCY% neq 0 (
    echo [ERROR] Configuration failed due to missing vendor dependencies.
    exit /b 1
) else (
    echo [OK] All required vendor dependencies ^(SQLite, TCL, TK, MINIZ^) are present.
)

echo -----------------------------------
echo === Configuration Complete!     ===
echo -----------------------------------
echo.
echo Running Make...
echo.

:: 4. Execute Make
make
