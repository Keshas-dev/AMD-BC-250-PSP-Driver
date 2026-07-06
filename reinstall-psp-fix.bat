@echo off
echo === Reinstalling PSP Driver with KIQ/HQD fix ===
echo.

copy /y "C:\AMD-BC-250\AMD-BC-250-PSP-Windows-Driver\output\PspDriver.sys" "C:\Windows\System32\drivers\PspDriver.sys"
if errorlevel 1 (
    echo FAIL: Could not copy PspDriver.sys
    echo Make sure no process is using the old driver.
    pause
    exit /b 1
)
echo PspDriver.sys copied OK

mkdir "C:\Windows\System32\drivers\bc-250\" 2>nul
xcopy /y /i "C:\AMD-BC-250\AMD-BC-250-PSP-Windows-Driver\output\firmware\*.bin" "C:\Windows\System32\drivers\bc-250\"
echo Firmware copied OK

echo.
echo Now restarting PSP device...
pnputil /disable-device "PCI\VEN_1022&DEV_143E" 2>nul
pnputil /enable-device "PCI\VEN_1022&DEV_143E" 2>nul
if errorlevel 1 (
    echo pnputil restart failed, trying devcon...
    pnputil /restart-device "PCI\VEN_1022&DEV_143E" 2>nul
)
if errorlevel 1 (
    echo.
    echo Could not restart device. REBOOT required.
    echo After reboot, run from repo root: output\psp-gpu-pm4-submit-test.exe
)
if errorlevel 1 (
    echo.
    echo Could not restart device. REBOOT required.
    echo After reboot, run: output\psp-gpu-pm4-submit-test.exe
)
echo.
echo Done. If PSP driver restarted, test now: output\psp-gpu-pm4-submit-test.exe
pause
