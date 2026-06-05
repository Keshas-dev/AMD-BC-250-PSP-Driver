@echo off
echo Exporting PspTestSigner certificate...
certutil -exportPFX -p "" My PspTestSigner "C:\AMD-BC-250\AMD-BC-250-PSP-Windows-Driver\output\PspTestSigner.pfx" >nul 2>&1
if errorlevel 1 (
    echo Failed to export PFX
    pause
    exit /b 1
)

echo Adding to Trusted Root store...
certutil -addstore Root "C:\AMD-BC-250\AMD-BC-250-PSP-Windows-Driver\output\PspTestSigner.pfx" >nul 2>&1
if errorlevel 1 (
    echo Failed to add to Root store
    pause
    exit /b 1
)

echo.
echo Certificate PspTestSigner is now trusted.
echo.
echo Now re-sign and verify the driver:
echo   signtool sign /fd SHA256 /a /s My /n PspTestSigner output\PspDriver.sys
echo   signtool verify /pa /v output\PspDriver.sys
echo.
pause
