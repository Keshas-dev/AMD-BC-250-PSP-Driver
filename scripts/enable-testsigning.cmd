@echo off
echo ==========================================
echo  AMD BC-250 PSP Driver - Test Signing Setup
echo ==========================================
echo.
echo This script enables Windows Test Mode to
echo allow loading unsigned/test-signed drivers.
echo.
echo WARNING: Test Mode should ONLY be used on
echo development/test systems, NEVER on production.
echo.
pause

echo [1] Enabling test signing...
bcdedit /set testsigning on
if errorlevel 1 (
    echo FAILED - Run this script as Administrator!
    pause
    exit /b 1
)

echo [2] Checking Secure Boot status...
for /f "tokens=2 delims=" %%A in ('bcdedit /enum {current} ^| findstr "secureboot"') do (
    echo    Secure Boot: %%A
)

echo [3] Status:
bcdedit /enum {current} | findstr "testsigning"

echo.
echo ==========================================
echo  REBOOT REQUIRED!
echo ==========================================
echo After reboot, you should see "Test Mode"
echo watermark in bottom-right corner.
echo.
pause
