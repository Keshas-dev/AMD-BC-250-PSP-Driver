@echo off
setlocal enabledelayedexpansion

echo ===================================================
echo  AMD BC-250 PSP Driver - Build + Sign Script
echo ===================================================

set "PROJECT_DIR=%~dp0"
set "OUTPUT_DIR=%PROJECT_DIR%output"
set "CERT_NAME=AMD-BC250-Signer"

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
        echo Found VS2022 Community on %%D: drive ^(x86^)
        goto :SetupEnv
    )
    if exist "%%D:\Program Files (x86)\Microsoft Visual Studio\2022\Professional\VC\Auxiliary\Build\vcvars64.bat" (
        set "VSWHERE=%%D:\Program Files (x86)\Microsoft Visual Studio\2022\Professional\VC\Auxiliary\Build\vcvars64.bat"
        echo Found VS2022 Professional on %%D: drive ^(x86^)
        goto :SetupEnv
    )
    if exist "%%D:\Program Files (x86)\Microsoft Visual Studio\2022\Enterprise\VC\Auxiliary\Build\vcvars64.bat" (
        set "VSWHERE=%%D:\Program Files (x86)\Microsoft Visual Studio\2022\Enterprise\VC\Auxiliary\Build\vcvars64.bat"
        echo Found VS2022 Enterprise on %%D: drive ^(x86^)
        goto :SetupEnv
    )
)
echo ERROR: Visual Studio 2022 not found
echo Searched: C:, D:, E:, F:, G:, H: drives
echo Please install VS2022 with "Desktop development with C++" workload
exit /b 1

:SetupEnv
echo Setting up build environment...
call "%VSWHERE%"

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

echo.
echo ==========================================
echo  BUILDING WDM Driver (PspDriver.sys)
echo ==========================================
echo.

echo Building driver source files...
for %%S in (
  "%PROJECT_DIR%src\driver\PspDriver.c"
  "%PROJECT_DIR%src\driver\PspCore.c"
  "%PROJECT_DIR%src\driver\PspKiq.c"
  "%PROJECT_DIR%src\driver\PspSmu.c"
) do (
  cl.exe /c /kernel /GS- /W3 /Zi /Od /DAMD64 /D_AMD64_ /DAMD_BC250_PSP_DRIVER ^
    /I"%WDK_ROOT%\Include\%WDK_VERSION%\km" ^
    /I"%WDK_ROOT%\Include\%WDK_VERSION%\km\crt" ^
    /I"%WDK_ROOT%\Include\%WDK_VERSION%\shared" ^
    /I"%PROJECT_DIR%inc" ^
    "%%S"
  if errorlevel 1 (
    echo Compilation FAILED for %%S
    pause
    exit /b 1
  )
)

echo Linking WDM driver...
link.exe /DRIVER /NODEFAULTLIB /SUBSYSTEM:NATIVE /ENTRY:DriverEntry ^
  /OUT:"%OUTPUT_DIR%\PspDriver.sys" ^
  PspDriver.obj PspCore.obj PspKiq.obj PspSmu.obj ^
  ntoskrnl.lib wdm.lib hal.lib ^
  /LIBPATH:"%WDK_ROOT%\Lib\%WDK_VERSION%\km\x64" ^
  /SECTION:Shared,RW

if errorlevel 1 (
    echo WDM linking FAILED!
    pause
    exit /b 1
)

echo Copying INF file...
copy "%PROJECT_DIR%inf\PspDriver.inf" "%OUTPUT_DIR%\" >nul

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

:: --- Sign driver ---
if not "%SIGNTOOLS%"=="" (
    echo.
    echo ==========================================
    echo  SIGNING DRIVERS
    echo ==========================================
    echo.

    :: Sign driver (uses same cert as GPU driver)
    echo Signing PspDriver.sys...
    "%SIGNTOOLS%\signtool.exe" sign /fd SHA256 /a /s My /n "%CERT_NAME%" /v ^
      "%OUTPUT_DIR%\PspDriver.sys" > "%OUTPUT_DIR%\sign-kmd.log" 2>&1
    if errorlevel 1 (
        type "%OUTPUT_DIR%\sign-kmd.log"
        echo FATAL: PSP signing FAILED!
        echo.
        echo Try: Run build.bat as Administrator, or sign manually:
        echo   signtool sign /fd SHA256 /a /s My /n AMD-BC250-Signer output\PspDriver.sys
        pause
        exit /b 1
    ) else (
        echo   PSP signed OK
    )

    :: Verify signature
    "%SIGNTOOLS%\signtool.exe" verify /pa /v "%OUTPUT_DIR%\PspDriver.sys" > "%OUTPUT_DIR%\verify-kmd.log" 2>&1
    if errorlevel 1 (
        echo   Signature applied (test cert - verify may fail without testsigning)
    ) else (
        echo   PSP signature verified OK
    )

    :: Generate catalog
    if not "%INF2CAT%"=="" (
        echo Generating catalog file...
        "%INF2CAT%" /driver:"%OUTPUT_DIR%" /os:10_x64 /verbose >nul 2>&1
    )

    :: Sign catalog
    if exist "%OUTPUT_DIR%\PspDriver.cat" (
        "%SIGNTOOLS%\signtool.exe" sign /fd SHA256 /a /s My /n "%CERT_NAME%" ^
          "%OUTPUT_DIR%\PspDriver.cat" >nul 2>&1 && echo   Catalog signed OK
    )
    goto :DoneSigning
)

echo WARNING: No signing tools found - driver is UNSIGNED!
echo ----------------------------------------------------
echo To load this driver, run as Administrator:
echo   bcdedit /set testsigning on
echo Then REBOOT
echo ----------------------------------------------------

:DoneSigning

echo.
echo ==========================================
echo  EXTRACTING FIRMWARE .bin FILES
echo ==========================================
echo.

:: Run firmware extraction
python "%PROJECT_DIR%extract-firmware.py" >nul 2>&1
if %errorlevel% equ 0 (
    echo  Firmware extracted to %OUTPUT_DIR%\firmware\
    :: Copy additional firmware (ASD, TA) if available
    if exist "%OUTPUT_DIR%\asd_5.00.bin"  copy /y "%OUTPUT_DIR%\asd_5.00.bin"  "%OUTPUT_DIR%\firmware\Asd.bin" >nul
    if exist "%OUTPUT_DIR%\ta_5.00.bin"   copy /y "%OUTPUT_DIR%\ta_5.00.bin"   "%OUTPUT_DIR%\firmware\Ta.bin" >nul
    :: Copy firmware alongside INF for driver install
    copy /y "%OUTPUT_DIR%\firmware\*.bin" "%OUTPUT_DIR%\" >nul 2>&1
    dir /b "%OUTPUT_DIR%\firmware\*.bin"
) else (
    echo  WARNING: Firmware extraction failed
)

echo.
echo ==========================================
echo  BUILD COMPLETED!
echo ==========================================
echo.
echo  Output directory: %OUTPUT_DIR%
echo    PspDriver.sys   - WDM driver (kernel-mode)
echo    PspDriver.inf   - Device installation file
echo    PspDriver.cat   - Catalog file (if generated)
echo    firmware\       - Firmware .bin files
echo.
echo  Install (run as Administrator):
echo    1. Device Manager -^> Find "AMD BC-250 PSP"
echo    2. Update Driver -^> Browse -^> %OUTPUT_DIR%
echo    3. Reboot if prompted
echo.
echo  Post-install (copy firmware to system dir):
echo    mkdir "%SystemRoot%\System32\drivers\bc-250\" 2^>nul
echo    xcopy /y /i "%OUTPUT_DIR%\firmware\*.bin" "%SystemRoot%\System32\drivers\bc-250\"
echo.
echo  Test:
echo    output\test-psp-driver.exe
echo.
pause
