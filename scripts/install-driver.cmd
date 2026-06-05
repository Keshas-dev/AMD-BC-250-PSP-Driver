@echo off
echo ==========================================
echo  AMD BC-250 PSP Driver - Manual Install
echo ==========================================
echo.
echo NOTE: Make sure test signing is enabled first!
echo   Run: scripts\enable-testsigning.cmd
echo   Then REBOOT
echo.

set "OUTPUT_DIR=%~dp0..\output"

if not exist "%OUTPUT_DIR%\PspDriver.sys" (
    echo ERROR: Driver not found!
    echo Please run build.bat first.
    pause
    exit /b 1
)

if not exist "%OUTPUT_DIR%\PspDriver.inf" (
    echo ERROR: INF file not found!
    pause
    exit /b 1
)

echo Installing driver from: %OUTPUT_DIR%
echo.

:: Use pnputil for modern Windows (Win10+)
echo Method 1: Using pnputil (recommended)...
pnputil /add-driver "%OUTPUT_DIR%\PspDriver.inf" /install 2>nul
if errorlevel 1 (
    echo pnputil failed, trying Device Manager method...
    goto :DevMgrMethod
)

echo.
echo ==========================================
echo  Install initiated!
echo ==========================================
echo Driver should appear in Device Manager.
echo Reboot if prompted.
echo.
goto :End

:DevMgrMethod
echo.
echo ==========================================
echo  Manual Install Instructions:
echo ==========================================
echo 1. Open Device Manager
echo 2. Find "AMD BC-250 PSP" (or unknown device)
echo 3. Right-click -^> Update Driver
echo 4. Choose "Browse my computer for drivers"
echo 5. Navigate to: %OUTPUT_DIR%
echo 6. Click Next and follow prompts
echo 7. Reboot if asked
echo.

:End
pause
