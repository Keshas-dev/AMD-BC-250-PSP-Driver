@echo off
setlocal enabledelayedexpansion

echo ===================================================
echo  AMD BC-250 PSP Driver - Install Script
echo ===================================================
echo.

:: Check for Admin rights
net session >nul 2>&1
if %errorlevel% neq 0 (
    echo ERROR: Must run as Administrator!
    pause
    exit /b 1
)

set "OUTPUT_DIR=C:\AMD-BC-250\AMD-BC-250-PSP-Windows-Driver\output"
set "FW_DEST=%SystemRoot%\System32\drivers\bc-250"

:: Step 1: Check for PSP device in Device Manager
echo [1] Checking for PSP device...
pnputil /enum-devices /class System | findstr /i "bc-250 psp" >nul 2>&1
if %errorlevel% neq 0 (
    echo WARNING: PSP device not found.
    echo Install via Device Manager:
    echo   Device Manager -^> Add legacy hardware -^> Have disk -^> %OUTPUT_DIR%\PspDriver.inf
    echo.
)

:: Step 2: Create firmware directory and copy .bin files
echo [2] Copying firmware to %FW_DEST%\
if not exist "%FW_DEST%" mkdir "%FW_DEST%"
xcopy /y "%OUTPUT_DIR%\firmware\*.bin" "%FW_DEST%\" >nul 2>&1
if %errorlevel% equ 0 (
    echo   Firmware copied OK
    dir /b "%FW_DEST%\*.bin"
) else (
    echo   WARNING: Firmware copy failed
)

:: Step 3: Prompt for driver install
echo.
echo [3] Install PSP driver via Device Manager:
echo    1. Open Device Manager
echo    2. Find "AMD BC-250 PSP" (or "Other devices" -^> "BC-250")
echo    3. Right-click -^> Update Driver -^> Browse my computer
echo    4. Select: %OUTPUT_DIR%
echo    5. Click Next, then Install
echo.
echo [4] After install: REBOOT
echo.
echo [5] Test: output\toc-load-test.exe
echo.

:: Try auto-install via pnputil
echo Attempting auto-install via pnputil...
pnputil /add-driver "%OUTPUT_DIR%\PspDriver.inf" /install 2>&1
if %errorlevel% equ 0 (
    echo   Auto-install initiated successfully
) else (
    echo   Use Device Manager for installation
)

echo.
echo Done.
pause
