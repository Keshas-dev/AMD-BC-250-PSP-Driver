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

if not exist "%PROJECT_DIR%%CERT_NAME%.cer" (
    echo Creating test certificate...
    if exist "%SIGNTOOLS%\makecert.exe" (
        "%SIGNTOOLS%\makecert.exe" /r /pe /ss PrivateCertStore /n "CN=%CERT_NAME%" "%PROJECT_DIR%%CERT_NAME%.cer" >nul 2>&1
        if exist "%PROJECT_DIR%%CERT_NAME%.cer" (
            echo   Test certificate created
        ) else (
            echo   WARNING: Failed to create test certificate
        )
    ) else (
        echo   WARNING: makecert.exe not found, cannot create certificate
    )
)

:: --- Sign driver ---
if not "%SIGNTOOLS%"=="" (
    echo Signing PspDriver.sys...
    "%SIGNTOOLS%\signtool.exe" sign /fd SHA256 /a /s PrivateCertStore /n "%CERT_NAME%" ^
      "%OUTPUT_DIR%\PspDriver.sys" >nul 2>&1
    if errorlevel 1 (
        echo   WARNING: Driver signing failed (driver may not load without test signing)
        echo   Make sure test signing is enabled: bcdedit /set testsigning on
    ) else (
        echo   Driver signed OK
    )
    
    if exist "%OUTPUT_DIR%\PspDriver.cat" (
        echo Signing catalog file...
        "%SIGNTOOLS%\signtool.exe" sign /fd SHA256 /a /s PrivateCertStore /n "%CERT_NAME%" ^
          "%OUTPUT_DIR%\PspDriver.cat" >nul 2>&1
        if not errorlevel 1 echo   Catalog signed OK
    )
) else (
    echo WARNING: No signing tools found - driver is UNSIGNED
    echo          Enable testsigning or sign manually before installation
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
