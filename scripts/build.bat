@echo off
setlocal enabledelayedexpansion

echo ===================================================
echo  AMD BC-250 PSP Driver - Build + Sign Script
echo ===================================================

set "SCRIPT_DIR=%~dp0"
set "PROJECT_DIR=%SCRIPT_DIR%.."
set "OUTPUT_DIR=%PROJECT_DIR%\output"
set "CERT_NAME=PspTestCert"

:: --- Detect Visual Studio 2022 ---
:: Searches multiple drives and editions (Community, Professional, Enterprise)
set "VSWHERE="
for %%D in (C D E F G H) do (
    if exist "%%D:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" (
        set "VSWHERE=%%D:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
        echo Found VS2022 Community on %%D: drive
        goto :SetupEnv
    )
    if exist "%%D:\Program Files\Microsoft Visual Studio\2022\Professional\VC\Auxiliary\Build\vcvars64.bat" (
        set "VSWHERE=%%D:\Program Files\Microsoft Visual Studio\2022\Professional\VC\Auxiliary\Build\vcvars64.bat"
        echo Found VS2022 Professional on %%D: drive
        goto :SetupEnv
    )
    if exist "%%D:\Program Files\Microsoft Visual Studio\2022\Enterprise\VC\Auxiliary\Build\vcvars64.bat" (
        set "VSWHERE=%%D:\Program Files\Microsoft Visual Studio\2022\Enterprise\VC\Auxiliary\Build\vcvars64.bat"
        echo Found VS2022 Enterprise on %%D: drive
        goto :SetupEnv
    )
    if exist "%%D:\Program Files (x86)\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" (
        set "VSWHERE=%%D:\Program Files (x86)\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
        echo Found VS2022 Community on %%D: drive (x86)
        goto :SetupEnv
    )
    if exist "%%D:\Program Files (x86)\Microsoft Visual Studio\2022\Professional\VC\Auxiliary\Build\vcvars64.bat" (
        set "VSWHERE=%%D:\Program Files (x86)\Microsoft Visual Studio\2022\Professional\VC\Auxiliary\Build\vcvars64.bat"
        echo Found VS2022 Professional on %%D: drive (x86)
        goto :SetupEnv
    )
    if exist "%%D:\Program Files (x86)\Microsoft Visual Studio\2022\Enterprise\VC\Auxiliary\Build\vcvars64.bat" (
        set "VSWHERE=%%D:\Program Files (x86)\Microsoft Visual Studio\2022\Enterprise\VC\Auxiliary\Build\vcvars64.bat"
        echo Found VS2022 Enterprise on %%D: drive (x86)
        goto :SetupEnv
    )
)
echo ERROR: Visual Studio 2022 not found
echo Searched: C:, D:, E:, F:, G:, H: drives
echo Please install VS2022 with "Desktop development with C++" workload
exit /b 1

:SetupEnv
echo Setting up build environment...
call "%VSWHERE%" >nul 2>&1
if errorlevel 1 (
    echo ERROR: Failed to setup VS build environment
    exit /b 1
)

:: --- Detect Windows Kit (WDK) ---
:: Searches multiple drives for Windows Kits
set "WDK_ROOT="
for %%D in (C D E F G H) do (
    if exist "%%D:\Program Files (x86)\Windows Kits\10\Include" (
        set "WDK_ROOT=%%D:\Program Files (x86)\Windows Kits\10"
        echo Found Windows Kit on %%D: drive
        goto :FoundWDKRoot
    )
    if exist "%%D:\Program Files\Windows Kits\10\Include" (
        set "WDK_ROOT=%%D:\Program Files\Windows Kits\10"
        echo Found Windows Kit on %%D: drive
        goto :FoundWDKRoot
    )
)
echo ERROR: Windows Kit (WDK) not found
echo Searched: C:, D:, E:, F:, G:, H: drives
echo Please install Windows 11 SDK + WDK
exit /b 1

:FoundWDKRoot
set "WDK_VERSION="
for /f "delims=" %%V in ('dir /b /ad "!WDK_ROOT!\Include" ^| sort /r') do (
    if exist "!WDK_ROOT!\Include\%%V\km\ntddk.h" (
        set "WDK_VERSION=%%V"
        goto :FoundWDK
    )
)
echo ERROR: No kernel headers (ntddk.h) found in WDK
exit /b 1

:FoundWDK
echo Using Windows Kit version %WDK_VERSION%

:: --- Locate signing tools ---
set "SIGNTOOLS="
set "INF2CAT="
if exist "%WDK_ROOT%\bin\%WDK_VERSION%\x64\signtool.exe" (
    set "SIGNTOOLS=%WDK_ROOT%\bin\%WDK_VERSION%\x64"
)
if exist "%WDK_ROOT%\bin\%WDK_VERSION%\x64\Inf2Cat.exe" (
    set "INF2CAT=%WDK_ROOT%\bin\%WDK_VERSION%\x64\Inf2Cat.exe"
)
if "%SIGNTOOLS%"=="" (
    where signtool.exe >nul 2>&1 && set "SIGNTOOLS=." && echo Found signtool in PATH
)
if "%SIGNTOOLS%"=="" (
    echo WARNING: signtool.exe not found - will skip signing
)

:: --- Create output directory ---
if not exist "%OUTPUT_DIR%" mkdir "%OUTPUT_DIR%"
if exist "%PROJECT_DIR%*.obj" del /q "%PROJECT_DIR%*.obj" 2>nul

echo.
echo ==========================================
echo  BUILDING KMDF Driver (PspDriver.sys)
echo ==========================================
echo.

cl.exe /c /kernel /W3 /Zi /Od /DAMD64 /D_AMD64_ /DAMD_BC250_PSP_DRIVER ^
  /I"%WDK_ROOT%\Include\%WDK_VERSION%\km" ^
  /I"%WDK_ROOT%\Include\%WDK_VERSION%\km\crt" ^
  /I"%WDK_ROOT%\Include\%WDK_VERSION%\shared" ^
  /I"%PROJECT_DIR%\inc" ^
  "%PROJECT_DIR%\src\driver\PspDriver.c"

if errorlevel 1 (
    echo KMDF compilation FAILED!
    pause
    exit /b 1
)

echo Linking KMDF driver...
link.exe /DRIVER /SUBSYSTEM:NATIVE /ENTRY:DriverEntry ^
  /OUT:"%OUTPUT_DIR%\PspDriver.sys" ^
  PspDriver.obj ^
  ntoskrnl.lib wdm.lib hal.lib ^
  /LIBPATH:"%WDK_ROOT%\Lib\%WDK_VERSION%\km\x64"

if errorlevel 1 (
    echo KMDF linking FAILED!
    pause
    exit /b 1
)

echo Copying INF file...
copy "%PROJECT_DIR%\inf\PspDriver.inf" "%OUTPUT_DIR%\" >nul

:: --- Generate catalog file ---
if not "%INF2CAT%"=="" (
    echo Generating catalog file...
    "%INF2CAT%" /driver:"%OUTPUT_DIR%" /os:10_X64,11_X64 /verbose >nul 2>&1
    if exist "%OUTPUT_DIR%\PspDriver.cat" (
        echo   Catalog generated OK
    ) else (
        echo   Catalog generation failed (non-fatal)
    )
)

:: --- Create test certificate ---
echo.
echo ==========================================
echo  SIGNING DRIVER (Test Certificate)
echo ==========================================
echo.

set "CERT_PATH=%PROJECT_DIR%\%CERT_NAME%.cer"
set "PFX_PATH=%PROJECT_DIR%\%CERT_NAME%.pfx"

if not exist "%CERT_PATH%" (
    echo Creating test certificate for driver signing...
    
    :: Method 1: New-SelfSignedCertificate (PowerShell - modern Windows)
    echo   Trying PowerShell New-SelfSignedCertificate...
    powershell -Command "New-SelfSignedCertificate -Type Custom -Subject 'CN=%CERT_NAME%' -KeyUsage DigitalSignature -CertStoreLocation Cert:\\CurrentUser\\My -NotAfter (Get-Date).AddYears(10) -OutFile '%CERT_PATH%'" >nul 2>&1
    
    if not exist "%CERT_PATH%" (
        :: Method 2: makecert (legacy WDK tool)
        if exist "%SIGNTOOLS%\makecert.exe" (
            echo   Trying makecert (legacy)...
            "%SIGNTOOLS%\makecert.exe" /r /pe /ss PrivateCertStore /n "CN=%CERT_NAME%" "%CERT_PATH%" >nul 2>&1
        )
    )
    
    if exist "%CERT_PATH%" (
        echo   Test certificate created: %CERT_NAME%
    ) else (
        echo   WARNING: Failed to create test certificate automatically.
        echo            You can still use bcdedit /set testsigning on
    )
) else (
    echo   Using existing certificate: %CERT_NAME%
)

:: --- Check if testsigning is enabled ---
for /f "tokens=2* delims=" %%A in ('bcdedit /enum {current} ^| findstr "testsigning"') do (
    echo   Test Signing: %%A
)

:: --- Sign driver ---
if not "%SIGNTOOLS%"=="" (
    echo.
    echo Signing PspDriver.sys...
    
    :: Try with store certificate first
    "%SIGNTOOLS%\signtool.exe" sign /fd SHA256 /a /s My /n "%CERT_NAME%" ^
      "%OUTPUT_DIR%\PspDriver.sys" >nul 2>&1
    
    if errorlevel 1 (
        :: Try with test certificate file
        if exist "%CERT_PATH%" (
            "%SIGNTOOLS%\signtool.exe" sign /fd SHA256 /f "%CERT_PATH%" ^
              "%OUTPUT_DIR%\PspDriver.sys" >nul 2>&1
        )
    )
    
    if errorlevel 1 (
        echo   WARNING: Driver signing failed!
        echo   ----------------------------------------------------
        echo   To load unsigned drivers, run as Administrator:
        echo     bcdedit /set testsigning on
        echo   Then REBOOT
        echo   ----------------------------------------------------
    ) else (
        echo   Driver signed OK
        
        :: Verify signature
        "%SIGNTOOLS%\signtool.exe" verify /pa "%OUTPUT_DIR%\PspDriver.sys" >nul 2>&1
        if not errorlevel 1 (
            echo   Signature verified
        ) else (
            echo   Signature verification failed (may still work in test mode)
        )
    )
    
    if exist "%OUTPUT_DIR%\PspDriver.cat" (
        echo Signing catalog file...
        "%SIGNTOOLS%\signtool.exe" sign /fd SHA256 /a /s My /n "%CERT_NAME%" ^
          "%OUTPUT_DIR%\PspDriver.cat" >nul 2>&1
        if errorlevel 1 (
            if exist "%CERT_PATH%" (
                "%SIGNTOOLS%\signtool.exe" sign /fd SHA256 /f "%CERT_PATH%" ^
                  "%OUTPUT_DIR%\PspDriver.cat" >nul 2>&1
            )
        )
        if not errorlevel 1 echo   Catalog signed OK
    )
) else (
    echo WARNING: No signing tools found - driver is UNSIGNED!
    echo ----------------------------------------------------
    echo To load this driver, run as Administrator:
    echo   bcdedit /set testsigning on
    echo Then REBOOT
    echo ----------------------------------------------------
)

echo.
echo ==========================================
echo  BUILD COMPLETED!
echo ==========================================
echo.
echo  Output directory: %OUTPUT_DIR%
echo    PspDriver.sys   - KMDF driver (kernel-mode)
echo    PspDriver.inf   - Device installation file
echo    PspDriver.cat   - Catalog file (if generated)
echo.
echo  Install (run as Administrator):
echo    1. Device Manager -^> Find "AMD BC-250 PSP"
echo    2. Update Driver -^> Browse -^> %OUTPUT_DIR%
echo    3. Reboot if prompted
echo.
echo  Test:
echo    output\test-psp-driver.exe
echo.
pause
