@echo off
setlocal enabledelayedexpansion

echo Compiling PSP Driver Test Tool...

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
call "%VSWHERE%" >nul 2>&1
if errorlevel 1 (
    echo ERROR: Failed to setup VS build environment
    exit /b 1
)

set "SCRIPT_DIR=%~dp0"
set "PROJECT_DIR=%SCRIPT_DIR%.."
set "OUTPUT_DIR=%PROJECT_DIR%\output"
if not exist "%OUTPUT_DIR%" mkdir "%OUTPUT_DIR%"

cl.exe /W3 /Zi /O2 /D_AMD64_ /DWIN64 ^
  /I"%PROJECT_DIR%\inc" ^
  "%PROJECT_DIR%\src\test\test-psp-driver.c" ^
  /Fe"%OUTPUT_DIR%\test-psp-driver.exe"

if errorlevel 1 (
    echo Compilation FAILED!
    pause
    exit /b 1
)

echo.
echo Test tool compiled: %OUTPUT_DIR%\test-psp-driver.exe
echo.
pause
