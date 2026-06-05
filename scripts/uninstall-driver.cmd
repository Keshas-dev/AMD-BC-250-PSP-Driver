@echo off
echo ==========================================
echo  AMD BC-250 PSP Driver - Uninstall
echo ==========================================
echo.

set "OUTPUT_DIR=%~dp0..\output"

echo Stopping driver service...
sc stop PspDriver >nul 2>&1

echo Deleting driver service...
sc delete PspDriver >nul 2>&1

echo Removing driver files...
if exist "%OUTPUT_DIR%\PspDriver.sys" del /f "%OUTPUT_DIR%\PspDriver.sys" >nul 2>&1
if exist "%OUTPUT_DIR%\PspDriver.inf" del /f "%OUTPUT_DIR%\PspDriver.inf" >nul 2>&1
if exist "%OUTPUT_DIR%\PspDriver.cat" del /f "%OUTPUT_DIR%\PspDriver.cat" >nul 2>&1

echo.
echo ==========================================
echo  Uninstall complete!
echo  Please reboot to fully remove driver.
echo ==========================================
echo.
pause
