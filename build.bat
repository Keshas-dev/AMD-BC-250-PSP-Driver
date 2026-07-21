@echo off
setlocal enabledelayedexpansion

echo ===================================================
echo  AMD BC-250 PSP Test Tool — Build Script
echo  (No kernel driver needed — uses GPU driver IOCTLs)
echo ===================================================

set "PROJECT_DIR=%~dp0"
set "OUTPUT_DIR=%PROJECT_DIR%output"

:: Detect VS2022 + SDK (always on E: for this setup)
set "VS_DIR=E:\Program Files\Microsoft Visual Studio\2022\Community"
set "VC_TOOLS=%VS_DIR%\VC\Tools\MSVC"

:: Find MSVC toolchain version
set "MSVC_VER="
for /f "delims=" %%V in ('dir /b /ad "!VC_TOOLS!" 2^>nul ^| sort /r') do (
    set "MSVC_VER=%%V"
    goto :FoundMsvc
)
:FoundMsvc
if "!MSVC_VER!"=="" echo ERROR: MSVC not found & exit /b 1
echo MSVC version: !MSVC_VER!

:: Find Windows SDK version
set "SDK_ROOT=E:\Program Files (x86)\Windows Kits\10"
set "SDK_VER="
for /f "delims=" %%V in ('dir /b /ad "!SDK_ROOT!\Include" 2^>nul ^| sort /r') do (
    if not "%%V"=="wdf" set "SDK_VER=%%V" & goto :FoundSdk
)
:FoundSdk
if "!SDK_VER!"=="" echo ERROR: SDK not found & exit /b 1
echo SDK version: !SDK_VER!

:: Set paths
set "VC_BIN=!VC_TOOLS!\!MSVC_VER!\bin\Hostx64\x64"
set "VC_INC=!VC_TOOLS!\!MSVC_VER!\include"
set "VC_ATL_INC=!VC_TOOLS!\!MSVC_VER!\ATLMFC\include"
set "SDK_INC=!SDK_ROOT!\Include\!SDK_VER!"
set "VC_LIB=!VC_TOOLS!\!MSVC_VER!\lib\x64"
set "SDK_LIB=!SDK_ROOT!\Lib\!SDK_VER!\um\x64"
set "UCRT_LIB=!SDK_ROOT!\Lib\!SDK_VER!\ucrt\x64"

set "CL_EXE=!VC_BIN!\cl.exe"
if not exist "!CL_EXE!" (
    echo ERROR: cl.exe not found at !CL_EXE!
    exit /b 1
)
echo CL: !CL_EXE!

if not exist "%OUTPUT_DIR%" mkdir "%OUTPUT_DIR%"

echo.
echo ==========================================
echo  BUILDING USER-MODE Test Tool (EXE)
echo ==========================================
echo.

"!CL_EXE!" /nologo /O2 /MT /W3 /Fe"%OUTPUT_DIR%\psp-test-tool.exe" ^
  /I"%PROJECT_DIR%inc" ^
  /I"!VC_INC!" /I"!VC_ATL_INC!" ^
  /I"!SDK_INC!\um" /I"!SDK_INC!\shared" /I"!SDK_INC!\winrt" /I"!SDK_INC!\ucrt" ^
  "%PROJECT_DIR%src\test\test-psp-driver.c" ^
  /link /LIBPATH:"!VC_LIB!" /LIBPATH:"!SDK_LIB!" /LIBPATH:"!UCRT_LIB!" user32.lib

if errorlevel 1 (
    echo  BUILD FAILED!
    exit /b 1
)

:: Copy firmware files for -A / -L commands
if not exist "%OUTPUT_DIR%\firmware" mkdir "%OUTPUT_DIR%\firmware"
copy /Y "%PROJECT_DIR%firmware\cyan_skillfish2_*.bin" "%OUTPUT_DIR%\firmware\" >nul 2>&1
echo  Firmware files copied to output\firmware\

echo.
echo ==========================================
echo  BUILD COMPLETED!
echo ==========================================
echo.
echo  Output: %OUTPUT_DIR%\psp-test-tool.exe
echo.
echo  Usage (requires atikmdag.sys loaded + BAR5 mapped):
echo    psp-test-tool.exe -i 0xFE800000 0x80000        Init HW
echo    psp-test-tool.exe -t                            Quick test
echo    psp-test-tool.exe -L 8 cyan_skillfish2_rlc.bin  Load RLC FW
echo    psp-test-tool.exe -S 0x02 0                     Get SMU version
echo.
echo  NOTE: No INF needed. No kernel driver installation required.
echo  The GPU driver (atikmdag.sys) provides all PSP mailbox access.
echo.

pause
